#include "analysis/jumptable.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include <llvm/ADT/APInt.h>
#include <llvm/Analysis/LazyValueInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/ConstantRange.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/KnownBits.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/SROA.h>

namespace nxaot::analysis {

namespace {

// The x86-64 data layout the lifter's modules have (lift/jit.h gives the JIT's); no target machine is needed, nothing is compiled
constexpr const char* kDataLayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128";

struct Graph {
    std::map<uint64_t, std::vector<std::pair<uint64_t, Edge>>> preds;  // block -> (pred, kind)
    std::set<uint64_t> external;  // blocks entered from outside the function's CFG: the entry, landing pads, labels
    std::map<uint64_t, uint64_t> idom;
};

// excluded: blocks left out of the graph (an optimistic region's speculative code)
Graph BuildGraph(const Function& fn, const std::set<uint64_t>& excluded) {
    Graph g;
    for (const auto& [start, b] : fn.blocks) {
        g.preds[start];
        if (excluded.count(start)) continue;
        for (const auto& [t, kind] : b.succ) {
            if (fn.blocks.count(t) && !excluded.count(t)) {
                g.preds[t].emplace_back(start, kind);
            }
        }
    }
    g.external.insert(fn.entry);
    for (const uint64_t r : fn.roots) {
        // A speculative root that a known edge reaches is no entry; neither is a root a jump table of the function names (its table's block is
        // its predecessor); the others are entered from outside
        if (fn.speculative.count(r) && !fn.spec_live.count(r)) continue;
        if (excluded.count(r)) continue;
        if (g.preds[r].empty() || std::none_of(g.preds[r].begin(), g.preds[r].end(), [](const auto& p) { return p.second == Edge::Table; })) {
            g.external.insert(r);
        }
    }
    // Immediate dominators (Cooper, Harvey and Kennedy), with a virtual root above every external entry
    std::vector<uint64_t> order;  // reverse postorder
    std::set<uint64_t> seen;
    std::function<void(uint64_t)> dfs = [&](uint64_t n) {
        seen.insert(n);
        for (const auto& [t, kind] : fn.blocks.at(n).succ) {
            if (fn.blocks.count(t) && !seen.count(t) && !excluded.count(t)) {
                dfs(t);
            }
        }
        order.push_back(n);
    };
    for (const uint64_t e : g.external) {
        if (fn.blocks.count(e) && !seen.count(e)) {
            dfs(e);
        }
    }
    std::reverse(order.begin(), order.end());
    constexpr uint64_t kRoot = ~0ull;
    std::map<uint64_t, size_t> rpo;
    rpo[kRoot] = 0;
    for (size_t i = 0; i < order.size(); ++i) {
        rpo[order[i]] = i + 1;
    }
    std::map<uint64_t, uint64_t> dom;
    dom[kRoot] = kRoot;
    auto intersect = [&](uint64_t a, uint64_t b) {
        while (a != b) {
            while (rpo[a] > rpo[b]) a = dom[a];
            while (rpo[b] > rpo[a]) b = dom[b];
        }
        return a;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (const uint64_t n : order) {
            std::vector<uint64_t> ps;
            if (g.external.count(n)) {
                ps.push_back(kRoot);
            }
            for (const auto& [p, kind] : g.preds[n]) {
                if (dom.count(p)) {
                    ps.push_back(p);
                }
            }
            if (ps.empty()) {
                continue;
            }
            uint64_t d = ps[0];
            for (size_t i = 1; i < ps.size(); ++i) {
                d = intersect(ps[i], d);
            }
            if (!dom.count(n) || dom[n] != d) {
                dom[n] = d;
                changed = true;
            }
        }
    }
    for (const auto& [n, d] : dom) {
        if (n != kRoot && d != kRoot) {
            g.idom[n] = d;
        }
    }
    return g;
}

// An integer expression of the optimised IR evaluated with some values given (the index, the loaded entry); nullopt for anything else
std::optional<llvm::APInt> Eval(llvm::Value* v, const std::map<llvm::Value*, llvm::APInt>& given, int depth = 0) {
    if (const auto it = given.find(v); it != given.end()) {
        return it->second;
    }
    if (depth > 64) {
        return std::nullopt;
    }
    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(v)) {
        return c->getValue();
    }
    const unsigned bits = v->getType()->isIntegerTy() ? v->getType()->getIntegerBitWidth() : 0;
    if (!bits) {
        return std::nullopt;
    }
    if (auto* call = llvm::dyn_cast<llvm::IntrinsicInst>(v);
        call && (call->getIntrinsicID() == llvm::Intrinsic::fshl || call->getIntrinsicID() == llvm::Intrinsic::fshr)) {
        // Funnel shifts (the lifter's form of the shifts and bit-field extracts by an immediate): concatenate, shift, take one half
        const auto a = Eval(call->getArgOperand(0), given, depth + 1), b = Eval(call->getArgOperand(1), given, depth + 1),
                   c = Eval(call->getArgOperand(2), given, depth + 1);
        if (!a || !b || !c) return std::nullopt;
        const unsigned n = static_cast<unsigned>(c->getZExtValue() % bits);
        if (!n) return call->getIntrinsicID() == llvm::Intrinsic::fshl ? *a : *b;
        return call->getIntrinsicID() == llvm::Intrinsic::fshl ? (a->shl(n) | b->lshr(bits - n)) : (b->lshr(n) | a->shl(bits - n));
    }
    auto* u = llvm::dyn_cast<llvm::Operator>(v);
    if (!u) {
        return std::nullopt;
    }
    switch (u->getOpcode()) {
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
    case llvm::Instruction::Trunc: {
        const auto a = Eval(u->getOperand(0), given, depth + 1);
        if (!a) return std::nullopt;
        return u->getOpcode() == llvm::Instruction::ZExt ? a->zext(bits) : u->getOpcode() == llvm::Instruction::SExt ? a->sext(bits) : a->trunc(bits);
    }
    case llvm::Instruction::Add:
    case llvm::Instruction::Sub:
    case llvm::Instruction::Mul:
    case llvm::Instruction::Shl:
    case llvm::Instruction::LShr:
    case llvm::Instruction::AShr:
    case llvm::Instruction::And:
    case llvm::Instruction::Or:
    case llvm::Instruction::Xor: {
        const auto a = Eval(u->getOperand(0), given, depth + 1), b = Eval(u->getOperand(1), given, depth + 1);
        if (!a || !b) return std::nullopt;
        const uint64_t sh = b->getLimitedValue(bits);
        switch (u->getOpcode()) {
        case llvm::Instruction::Add: return *a + *b;
        case llvm::Instruction::Sub: return *a - *b;
        case llvm::Instruction::Mul: return *a * *b;
        case llvm::Instruction::Shl: return sh >= bits ? llvm::APInt(bits, 0) : a->shl(static_cast<unsigned>(sh));
        case llvm::Instruction::LShr: return sh >= bits ? llvm::APInt(bits, 0) : a->lshr(static_cast<unsigned>(sh));
        case llvm::Instruction::AShr: return a->ashr(static_cast<unsigned>(std::min<uint64_t>(sh, bits - 1)));
        case llvm::Instruction::And: return *a & *b;
        case llvm::Instruction::Or: return *a | *b;
        default: return *a ^ *b;
        }
    }
    default:
        return std::nullopt;
    }
}

// The operands an expression node of Eval depends on
std::vector<llvm::Value*> Operands(llvm::Value* v) {
    if (auto* call = llvm::dyn_cast<llvm::IntrinsicInst>(v)) {
        return {call->getArgOperand(0), call->getArgOperand(1), call->getArgOperand(2)};
    }
    std::vector<llvm::Value*> out;
    for (llvm::Value* op : llvm::cast<llvm::Operator>(v)->operands()) out.push_back(op);
    return out;
}

bool IsArith(llvm::Value* v) {
    if (!v->getType()->isIntegerTy()) {
        return false;
    }
    if (auto* call = llvm::dyn_cast<llvm::IntrinsicInst>(v)) {
        return call->getIntrinsicID() == llvm::Intrinsic::fshl || call->getIntrinsicID() == llvm::Intrinsic::fshr;
    }
    auto* u = llvm::dyn_cast<llvm::Operator>(v);
    if (!u) {
        return false;
    }
    switch (u->getOpcode()) {
    case llvm::Instruction::ZExt: case llvm::Instruction::SExt: case llvm::Instruction::Trunc: case llvm::Instruction::Add:
    case llvm::Instruction::Sub: case llvm::Instruction::Mul: case llvm::Instruction::Shl: case llvm::Instruction::LShr:
    case llvm::Instruction::AShr: case llvm::Instruction::And: case llvm::Instruction::Or: case llvm::Instruction::Xor:
        return true;
    default:
        return false;
    }
}

// The leaves of an expression that are neither constants nor arithmetic: values the expression depends on
void Leaves(llvm::Value* v, std::set<llvm::Value*>& out, std::set<llvm::Value*>& seen) {
    if (!seen.insert(v).second || llvm::isa<llvm::ConstantInt>(v)) {
        return;
    }
    if (IsArith(v)) {
        for (llvm::Value* op : Operands(v)) {
            Leaves(op, out, seen);
        }
        return;
    }
    out.insert(v);
}

bool DependsOn(llvm::Value* v, llvm::Value* leaf, std::map<llvm::Value*, bool>& memo) {
    if (v == leaf) return true;
    if (const auto it = memo.find(v); it != memo.end()) return it->second;
    bool d = false;
    if (IsArith(v)) {
        for (llvm::Value* op : Operands(v)) {
            d = d || DependsOn(op, leaf, memo);
        }
    }
    memo[v] = d;
    return d;
}

// A guest load: a volatile load from inttoptr of a 64-bit address
llvm::Value* GuestAddress(llvm::LoadInst* l) {
    if (!l->isVolatile() || !l->getType()->isIntegerTy()) {
        return nullptr;
    }
    if (auto* p = llvm::dyn_cast<llvm::Operator>(l->getPointerOperand()); p && p->getOpcode() == llvm::Instruction::IntToPtr) {
        return p->getOperand(0);
    }
    return nullptr;
}

struct Analysed {
    std::vector<uint64_t> targets;
    JumpTable info;
    std::string why;
};

// The range of v where `at` runs, sharper than LazyValueInfo's own when v is a cast or constant arithmetic of a value a condition bounds, or a
// phi of such values: LazyValueInfo bounds a value by the conditions on that same value, and the lifted IR often compares x and indexes with
// zext (trunc x), or with a phi of zext x and a constant. Casts and constant operations map the operand's range; a phi takes the union of its
// incoming values' ranges on their edges. Each step is sound on its own; the result is intersected with LazyValueInfo's
llvm::ConstantRange RangeOf(llvm::Value* v, llvm::Instruction* at, llvm::LazyValueInfo& lvi, std::set<llvm::Value*>& active, int depth);

llvm::ConstantRange RangeOnEdge(llvm::Value* v, llvm::BasicBlock* from, llvm::BasicBlock* to, llvm::Instruction* at, llvm::LazyValueInfo& lvi,
                                std::set<llvm::Value*>& active, int depth) {
    const unsigned bits = v->getType()->getIntegerBitWidth();
    llvm::ConstantRange base = lvi.getConstantRangeOnEdge(v, from, to, at);
    if (depth > 8 || active.count(v)) return base;
    if (auto* cast = llvm::dyn_cast<llvm::CastInst>(v); cast && cast->getOperand(0)->getType()->isIntegerTy()) {
        active.insert(v);
        const auto r = RangeOnEdge(cast->getOperand(0), from, to, at, lvi, active, depth + 1).castOp(cast->getOpcode(), bits);
        active.erase(v);
        return base.intersectWith(r);
    }
    if (auto* bin = llvm::dyn_cast<llvm::BinaryOperator>(v); bin && llvm::isa<llvm::ConstantInt>(bin->getOperand(1))) {
        active.insert(v);
        const auto r = RangeOnEdge(bin->getOperand(0), from, to, at, lvi, active, depth + 1)
                           .binaryOp(bin->getOpcode(), llvm::ConstantRange(llvm::cast<llvm::ConstantInt>(bin->getOperand(1))->getValue()));
        active.erase(v);
        return base.intersectWith(r);
    }
    (void)bits;
    return base;
}

llvm::ConstantRange RangeOf(llvm::Value* v, llvm::Instruction* at, llvm::LazyValueInfo& lvi, std::set<llvm::Value*>& active, int depth) {
    const unsigned bits = v->getType()->getIntegerBitWidth();
    llvm::ConstantRange base = lvi.getConstantRange(v, at, false);
    if (depth > 8 || active.count(v)) return base;
    active.insert(v);
    llvm::ConstantRange r = llvm::ConstantRange::getFull(bits);
    auto* cast = llvm::dyn_cast<llvm::CastInst>(v);
    if (cast && cast->getOperand(0)->getType()->isIntegerTy()) {
        // Each lifted instruction reads its registers anew, so `cmp w13` and the index w13 are two trunc x13: the compares of the predecessors
        // are on casts of their own. An equal cast of x on the straight line that ends at an edge (the edge's block and its single
        // predecessors: the lifter's if_then / if_else blocks) has the value of this cast on that edge
        const auto same_cast = [&](llvm::Value* x, llvm::BasicBlock* from) -> llvm::Value* {
            std::set<llvm::BasicBlock*> line{from};
            for (llvm::BasicBlock* b = from; line.size() < 8 && b->getSinglePredecessor(); line.insert(b)) b = b->getSinglePredecessor();
            for (llvm::User* user : x->users()) {
                auto* c = llvm::dyn_cast<llvm::CastInst>(user);
                if (c && c != cast && c->getOpcode() == cast->getOpcode() && c->getType() == cast->getType() && line.count(c->getParent())) return c;
            }
            return nullptr;
        };
        r = RangeOf(cast->getOperand(0), at, lvi, active, depth + 1).castOp(cast->getOpcode(), bits);
        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(cast->getOperand(0)); phi && phi->getParent() == cast->getParent()) {
            // A cast of a phi of its own block: per edge, the same cast of the incoming value
            llvm::ConstantRange u = llvm::ConstantRange::getEmpty(bits);
            for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
                llvm::Value* in = phi->getIncomingValue(i);
                llvm::BasicBlock* from = phi->getIncomingBlock(i);
                llvm::Value* same = same_cast(in, from);
                u = u.unionWith(same ? RangeOnEdge(same, from, phi->getParent(), at, lvi, active, depth + 1)
                                     : RangeOnEdge(in, from, phi->getParent(), at, lvi, active, depth + 1).castOp(cast->getOpcode(), bits));
            }
            r = r.intersectWith(u);
        } else if (cast->getParent() == at->getParent() && !llvm::pred_empty(cast->getParent())) {
            // A cast in the block of `at` of a value from before it: per predecessor edge, an equal cast of the value there
            llvm::BasicBlock* home = cast->getParent();
            llvm::ConstantRange u = llvm::ConstantRange::getEmpty(bits);
            bool all = true;
            for (llvm::BasicBlock* from : llvm::predecessors(home)) {
                llvm::Value* same = same_cast(cast->getOperand(0), from);
                if (!same) {
                    all = false;
                    break;
                }
                u = u.unionWith(RangeOnEdge(same, from, home, at, lvi, active, depth + 1));
            }
            if (all) r = r.intersectWith(u);
        }
    } else if (auto* bin = llvm::dyn_cast<llvm::BinaryOperator>(v); bin && llvm::isa<llvm::ConstantInt>(bin->getOperand(1))) {
        r = RangeOf(bin->getOperand(0), at, lvi, active, depth + 1)
                .binaryOp(bin->getOpcode(), llvm::ConstantRange(llvm::cast<llvm::ConstantInt>(bin->getOperand(1))->getValue()));
    } else if (auto* phi = llvm::dyn_cast<llvm::PHINode>(v)) {
        r = llvm::ConstantRange::getEmpty(bits);
        for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
            r = r.unionWith(RangeOnEdge(phi->getIncomingValue(i), phi->getIncomingBlock(i), phi->getParent(), at, lvi, active, depth + 1));
        }
    }
    active.erase(v);
    return base.intersectWith(r);
}

Analysed AnalyseTarget(const module::Image& image, llvm::CallInst* marker, llvm::LazyValueInfo& lvi, const llvm::DataLayout& dl, unsigned max_table) {
    Analysed out;
    llvm::Value* t = marker->getArgOperand(1);
    std::set<llvm::Value*> leaves, seen;
    Leaves(t, leaves, seen);
    if (leaves.empty()) {
        out.why = "a constant target (no table)";
        return out;
    }
    if (leaves.size() != 1 || !llvm::isa<llvm::LoadInst>(*leaves.begin())) {
        out.why = "the target is not a function of one guest load";
        return out;
    }
    auto* load = llvm::cast<llvm::LoadInst>(*leaves.begin());
    llvm::Value* addr = GuestAddress(load);
    if (!addr) {
        out.why = "the target comes from a load that is not a guest memory read";
        return out;
    }
    std::set<llvm::Value*> index_leaves, seen2;
    Leaves(addr, index_leaves, seen2);
    if (index_leaves.size() != 1) {
        out.why = index_leaves.empty() ? "a constant entry address (no index)" : "the entry address depends on more than one value";
        return out;
    }
    llvm::Value* leaf = *index_leaves.begin();
    // The index: of the nodes on the address's path to its one variable, the one with the smallest range at the br
    std::vector<llvm::Value*> nodes;
    std::map<llvm::Value*, bool> memo;
    std::set<llvm::Value*> visited;
    std::function<void(llvm::Value*)> collect = [&](llvm::Value* v) {
        if (!visited.insert(v).second || !DependsOn(v, leaf, memo)) return;
        nodes.push_back(v);
        if (IsArith(v)) {
            for (llvm::Value* op : Operands(v)) collect(op);
        }
    };
    collect(addr);
    // The number of values of a range that is neither full nor empty (upper - lower, modulo the width), saturated to 64 bits
    auto set_size = [](const llvm::ConstantRange& r) { return (r.getUpper() - r.getLower()).getLimitedValue(); };
    llvm::Value* best = nullptr;
    std::optional<llvm::ConstantRange> best_range;
    for (llvm::Value* n : nodes) {
        if (!n->getType()->isIntegerTy()) continue;
        std::set<llvm::Value*> active;
        llvm::ConstantRange r = RangeOf(n, marker, lvi, active, 0);
        const llvm::KnownBits kb = llvm::computeKnownBits(n, dl);
        r = r.intersectWith(llvm::ConstantRange::fromKnownBits(kb, false));
        if (r.isFullSet() || r.isEmptySet()) continue;
        if (!best_range || set_size(r) <= set_size(*best_range)) {  // ties: the node nearest the variable, the index itself
            best = n;
            best_range = r;
        }
    }
    if (!best || set_size(*best_range) > max_table) {
        out.why = best ? "the index range has more than " + std::to_string(max_table) + " values" : "the index is not bounded at the br";
        return out;
    }
    const unsigned entry_bits = load->getType()->getIntegerBitWidth();
    const uint64_t count = set_size(*best_range);
    const unsigned width = best->getType()->getIntegerBitWidth();
    for (uint64_t i = 0; i < count; ++i) {
        const llvm::APInt index = best_range->getLower() + llvm::APInt(width, i);
        const auto a = Eval(addr, {{best, index}});
        if (!a) {
            out.why = "the entry address cannot be evaluated";
            return out;
        }
        const uint64_t at = a->getZExtValue();
        if (!image.InImage(at, entry_bits / 8)) {
            out.why = "an entry lies outside the image";
            return out;
        }
        uint64_t raw = 0;
        std::memcpy(&raw, image.bytes.data() + at, entry_bits / 8);
        if (entry_bits == 64) {
            // A table of pointers in data: the loader relocates its slots, so an entry is its relocation's target
            if (const auto it = image.slot_targets.find(at); it != image.slot_targets.end()) raw = it->second;
        }
        const auto target = Eval(t, {{load, llvm::APInt(entry_bits, raw)}});
        if (!target) {
            out.why = "the target cannot be evaluated from the entry";
            return out;
        }
        out.targets.push_back(target->getZExtValue());
        if (i == 0) {
            out.info.table = at;
            out.info.index_lo = index.getZExtValue();
        }
    }
    out.info.width = entry_bits / 8;
    out.info.count = count;
    out.info.writable = out.info.table >= image.data_off;  // in the data segment: the guest may rewrite it (a pointer table)
    const auto t0 = Eval(t, {{load, llvm::APInt(entry_bits, 0)}}), t1 = Eval(t, {{load, llvm::APInt(entry_bits, 1)}}),
               tm = Eval(t, {{load, llvm::APInt::getAllOnes(entry_bits)}});
    if (t0 && t1 && tm) {
        out.info.base = t0->getZExtValue();
        out.info.scale = static_cast<int64_t>(t1->getZExtValue() - t0->getZExtValue());
        out.info.is_signed = tm->getZExtValue() < t0->getZExtValue();  // an all-ones entry goes backwards: sign-extended
    }
    return out;
}

}  // namespace

TableResult ResolveJumpTable(const module::Image& image, const Function& fn, uint64_t br, const DiscoverOptions& options,
                             const std::function<bool(uint64_t)>& valid, const CallEffects& calls, bool optimistic) {
    TableResult res;
    // NXAOT_DEBUG_REGION=1: why each region tried was not used (README.md, "Module loader and discovery")
    static const bool debug = std::getenv("NXAOT_DEBUG_REGION") != nullptr;
    const auto trace = [&](uint64_t from, const std::string& what) {
        if (debug) {
            std::fprintf(stderr, "nxaot: the br at %#llx, region from %#llx%s%s: %s\n", static_cast<unsigned long long>(br),
                         static_cast<unsigned long long>(from), optimistic ? ", optimistic" : "", calls ? ", past calls" : "", what.c_str());
        }
    };
    uint64_t home = 0;
    bool found = false;
    for (const auto& [start, b] : fn.blocks) {
        if (b.end == br + 4 && (b.term == Term::Indirect || b.term == Term::Table)) {
            home = start;
            found = true;
        }
    }
    if (!found) {
        res.why = "the br ends no block of the function";
        return res;
    }
    // Optimistic: the blocks that only the speculative roots reach are left out, as if not there (their real entries are not known yet)
    std::set<uint64_t> excluded;
    if (optimistic) {
        std::set<uint64_t> known{fn.entry};
        std::deque<uint64_t> w{fn.entry};
        for (const uint64_t r : fn.roots) {
            if (!fn.speculative.count(r) && fn.blocks.count(r) && known.insert(r).second) w.push_back(r);
        }
        while (!w.empty()) {
            const uint64_t n = w.front();
            w.pop_front();
            for (const auto& [t, kind] : fn.blocks.at(n).succ) {
                if (fn.blocks.count(t) && known.insert(t).second) w.push_back(t);
            }
        }
        for (const auto& [start, b] : fn.blocks) {
            if (!known.count(start)) excluded.insert(start);
        }
        if (excluded.count(home)) {
            res.why = "the br lies in code only speculative roots reach";
            return res;
        }
    }
    const Graph g = BuildGraph(fn, excluded);
    auto followable = [&](uint64_t pred, Edge kind) {
        if (kind == Edge::Branch || kind == Edge::Fall || kind == Edge::Table) return true;
        if (kind != Edge::Return || !calls) return false;
        const Block& b = fn.blocks.at(pred);
        return b.end >= 4 && calls(b.end - 4).has_value();
    };
    const decode::CodeReader read = [&image](uint64_t pc) -> std::optional<uint32_t> {
        if (!image.InText(pc) || image.InData(pc)) return std::nullopt;
        return image.Word(pc);
    };
    std::string shape_why = "no closed region reaches the br", lifted_why;  // the reason of the last region lifted wins
    uint64_t e = home;
    for (unsigned depth = 0; depth <= options.region_depth; ++depth) {
        if (depth) {
            const auto it = g.idom.find(e);
            if (it == g.idom.end()) {
                trace(e, "no dominator above it");
                break;
            }
            e = it->second;
        }
        // The region: blocks reachable from e by followable edges, then those of them that reach the br
        std::set<uint64_t> reach{e};
        std::deque<uint64_t> work{e};
        while (!work.empty()) {
            const uint64_t n = work.front();
            work.pop_front();
            for (const auto& [t, kind] : fn.blocks.at(n).succ) {
                if (fn.blocks.count(t) && followable(n, kind) && reach.insert(t).second) work.push_back(t);
            }
        }
        if (!reach.count(home)) {
            shape_why = "the br is not reachable from its dominator without leaving the region";
            trace(e, shape_why);
            continue;
        }
        std::set<uint64_t> slice{home};
        work.assign(1, home);
        while (!work.empty()) {
            const uint64_t n = work.front();
            work.pop_front();
            if (n == e) continue;
            for (const auto& [p, kind] : g.preds.at(n)) {
                if (!excluded.count(p) && reach.count(p) && followable(p, kind) && slice.insert(p).second) work.push_back(p);
            }
        }
        bool closed = true;
        for (const uint64_t n : slice) {
            if (n == e) continue;
            if (g.external.count(n)) closed = false;
            for (const auto& [p, kind] : g.preds.at(n)) {
                if (!excluded.count(p) && (!slice.count(p) || !followable(p, kind))) closed = false;
            }
        }
        if (!closed) {
            shape_why = "the region is entered from outside it";
            trace(e, shape_why);
            continue;
        }
        llvm::LLVMContext ctx;
        auto module = std::make_unique<llvm::Module>("nxaot.jumptable", ctx);
        module->setTargetTriple("x86_64-unknown-linux-gnu");
        module->setDataLayout(kDataLayout);
        lift::Region region;
        region.entry = e;
        region.lo = fn.entry;
        region.hi = fn.limit;
        region.max_blocks = 4 * slice.size() + 16;
        region.follow = [&slice](uint64_t pc) { return slice.count(pc) > 0; };
        region.calls = calls;
        region.mark_indirect = true;
        region.tables = [&fn](uint64_t pc) -> const std::vector<uint64_t>* {
            const auto it = fn.tables.find(pc);
            return it == fn.tables.end() ? nullptr : &it->second.targets;
        };
        lift::LiftOptions lo;
        lo.fpsr_flags = false;
        lo.nan_mode = lift::NaNMode::Unchecked;  // the region is analysed, never run: floating point needs no exact NaNs here
        const lift::Lifted lifted = lift::LiftRegion(*module, "region", region, read, lo);
        ++res.regions;
        if (!lifted.function) {
            lifted_why = "the region could not be lifted (" + (lifted.error.empty() ? "unlowered opcodes" : lifted.error) + ")";
            trace(e, lifted_why);
            continue;
        }
        llvm::LoopAnalysisManager lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager cgam;
        llvm::ModuleAnalysisManager mam;
        llvm::PassBuilder pb;
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        // SSA and common subexpressions only: InstCombine would rewrite the index's zext (trunc x) as and x, mask, a value other than the
        // trunc x the bound check compares, and LazyValueInfo bounds a value by the conditions on that same value
        llvm::FunctionPassManager fpm;
        fpm.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
        fpm.addPass(llvm::EarlyCSEPass(true));
        fpm.run(*lifted.function, fam);
        if (const char* dump = std::getenv("NXAOT_DUMP_REGION"); dump && std::strtoull(dump, nullptr, 16) == br) {
            // Debugging: the optimised IR of every region tried for this br (NXAOT_DUMP_REGION=<hex pc of the br>)
            std::fprintf(stderr, "---- region from %#llx for the br at %#llx (%zu blocks)\n", static_cast<unsigned long long>(e),
                         static_cast<unsigned long long>(br), slice.size());
            lifted.function->print(llvm::errs());
        }
        auto& lvi = fam.getResult<llvm::LazyValueAnalysis>(*lifted.function);
        std::vector<llvm::CallInst*> markers;
        for (auto& bb : *lifted.function) {
            for (auto& inst : bb) {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                if (!call || !call->getCalledFunction() || call->getCalledFunction()->getName() != "nxaot.indirect") continue;
                if (auto* pc = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(0)); pc && pc->getZExtValue() == br) markers.push_back(call);
            }
        }
        if (markers.empty()) {
            lifted_why = "the lifted region does not reach the br";
            trace(e, lifted_why);
            continue;
        }
        Analysed first;
        std::set<uint64_t> all;
        bool ok = true;
        for (size_t i = 0; i < markers.size() && ok; ++i) {
            Analysed a = AnalyseTarget(image, markers[i], lvi, module->getDataLayout(), options.max_table);
            if (a.targets.empty()) {
                lifted_why = a.why;
                trace(e, lifted_why);
                ok = false;
                break;
            }
            for (const uint64_t t : a.targets) {
                if (!valid(t)) {
                    char buf[96];
                    std::snprintf(buf, sizeof buf, "a target (%#llx) is not a decoded instruction of the function", static_cast<unsigned long long>(t));
                    lifted_why = buf;
                    trace(e, lifted_why);
                    ok = false;
                    break;
                }
                all.insert(t);
            }
            if (i == 0) first = std::move(a);
        }
        if (!ok) continue;
        JumpTable jt = first.info;
        jt.br = br;
        jt.targets = first.targets;
        for (const uint64_t t : all) {
            if (std::find(jt.targets.begin(), jt.targets.end(), t) == jt.targets.end()) jt.targets.push_back(t);
        }
        jt.region = e;
        jt.across_calls = static_cast<bool>(calls);
        res.table = jt;
        return res;
    }
    res.why = lifted_why.empty() ? shape_why : lifted_why;
    return res;
}

}  // namespace nxaot::analysis
