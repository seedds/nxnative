#include "analysis/jumptable.h"

#include <algorithm>
#include <cstdint>
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

#include "analysis/insn.h"
#include "analysis/memory.h"

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

// Whether v depends on one of the leaves
bool DependsOn(llvm::Value* v, const std::set<llvm::Value*>& leaves, std::map<llvm::Value*, bool>& memo) {
    if (leaves.count(v)) return true;
    if (const auto it = memo.find(v); it != memo.end()) return it->second;
    bool d = false;
    if (IsArith(v)) {
        for (llvm::Value* op : Operands(v)) {
            d = d || DependsOn(op, leaves, memo);
        }
    }
    memo[v] = d;
    return d;
}

// Whether v is a function of n alone: v is n, a constant, or arithmetic whose operands are each a function of n alone. Then every path of v's
// expression to a leaf passes through n (n cuts v from its leaves), and Eval(v, {{n, x}}) gives v's value for n = x
bool FunctionOf(llvm::Value* v, llvm::Value* n, std::map<llvm::Value*, bool>& memo) {
    if (v == n || llvm::isa<llvm::ConstantInt>(v)) return true;
    if (const auto it = memo.find(v); it != memo.end()) return it->second;
    bool f = IsArith(v);
    if (f) {
        for (llvm::Value* op : Operands(v)) {
            f = f && FunctionOf(op, n, memo);
        }
    }
    memo[v] = f;
    return f;
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
        const llvm::APInt& k = llvm::cast<llvm::ConstantInt>(bin->getOperand(1))->getValue();
        r = RangeOf(bin->getOperand(0), at, lvi, active, depth + 1).binaryOp(bin->getOpcode(), llvm::ConstantRange(k));
        if (bin->getOpcode() == llvm::Instruction::And && k.isMask() && k.countr_one() < bits) {
            // and x, 2^n - 1 is zext (trunc x to in), and and x, M with M < 2^n is zext (and (trunc x to in), M): the index of `and x8, x20,
            // #0xffffffff` after `cmp w20, #0x10; b.hi`, or of `and x9, x20, #0xff` after `and w8, w20, #0xff; cmp w8, #7; b.hi` (libpng's
            // png_set_filter), whose compares are on narrower values of their own. Such a value on the straight line that ends at `at` (as
            // same_cast's) bounds the index there: both are the same function of the same x
            std::set<llvm::BasicBlock*> line{at->getParent()};
            for (llvm::BasicBlock* b = at->getParent(); line.size() < 8 && b->getSinglePredecessor(); line.insert(b)) b = b->getSinglePredecessor();
            for (llvm::User* user : bin->getOperand(0)->users()) {
                auto* tr = llvm::dyn_cast<llvm::TruncInst>(user);
                if (!tr || !line.count(tr->getParent())) continue;
                const unsigned n = tr->getType()->getIntegerBitWidth();
                if (n == k.countr_one()) {
                    r = r.intersectWith(RangeOf(tr, at, lvi, active, depth + 1).zeroExtend(bits));
                } else if (n > k.countr_one()) {
                    for (llvm::User* u2 : tr->users()) {
                        auto* narrow = llvm::dyn_cast<llvm::BinaryOperator>(u2);
                        auto* m2 = narrow && narrow->getOpcode() == llvm::Instruction::And ? llvm::dyn_cast<llvm::ConstantInt>(narrow->getOperand(1)) : nullptr;
                        if (m2 && m2->getValue() == k.trunc(n) && line.count(narrow->getParent())) {
                            r = r.intersectWith(RangeOf(narrow, at, lvi, active, depth + 1).zeroExtend(bits));
                        }
                    }
                }
            }
        }
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
    if (index_leaves.empty()) {
        out.why = "a constant entry address (no index)";
        return out;
    }
    // The index: a node of the address's expression of which the address is a function alone (FunctionOf: it cuts the address from every
    // variable the address depends on), with the smallest range at the br. The variables may be several: an index computed from more than
    // one value (a bit field of a product, a sum of two registers: (a * b) >> 37 & 31) is bounded by its own compare or mask whatever
    // computed it, and the address is then a function of the index alone. A node that does not cut every variable off (an index modified
    // after its compare by another value: idx + y) is no index: the address takes other values for one value of it
    std::vector<llvm::Value*> nodes;
    std::map<llvm::Value*, bool> memo;
    std::set<llvm::Value*> visited;
    std::function<void(llvm::Value*)> collect = [&](llvm::Value* v) {
        if (!visited.insert(v).second || !DependsOn(v, index_leaves, memo)) return;
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
        std::map<llvm::Value*, bool> cut_memo;
        if (!FunctionOf(addr, n, cut_memo)) continue;
        std::set<llvm::Value*> active;
        llvm::ConstantRange r = RangeOf(n, marker, lvi, active, 0);
        const llvm::KnownBits kb = llvm::computeKnownBits(n, dl);
        r = r.intersectWith(llvm::ConstantRange::fromKnownBits(kb, false));
        if (r.isFullSet() || r.isEmptySet()) continue;
        if (!best_range || set_size(r) <= set_size(*best_range)) {  // ties: the node nearest the variables, the index itself
            best = n;
            best_range = r;
        }
    }
    // No bounded index, and no over-approximated table either. Scanning the table from index 0 while its entries land on instruction
    // boundaries inside the function's extent, and lowering the br as a switch over those targets with the dispatcher as its default, would
    // run correctly: the switch compares the computed address, so any other value still reaches the dispatcher. But a table's targets become
    // edges of the function's CFG, and the function then counts as closed: its summary (PASS, WRITES, preserving) covers only the blocks the
    // targets reach, and its callers keep registers in SSA across it on that word. That holds only if every real entry lies in the scanned
    // prefix: the index never negative nor past the scan, and no real target outside the extent or off the decoded code (the scan stops at
    // the first such entry and misses the ones after it; a case jumping to another function's entry or into a .cold part placed after the next
    // function does). Neither is known for an index no compare, mask or byte load bounds: that is exactly what the code does not show. The data
    // of Mario vs. Donkey Kong's three modules (README.md, "Unbounded brs by form") give no case to take that risk for: after the bound forms
    // above, the tables whose index no compare, mask or byte load bounds are 14 brs of sdk (6 in nlib's memchr variants, whose index is
    // end - (end & ~3), bounded only algebraically), 1 of subsdk0 and 12 of main, in about as many functions of 125,000; a wrong closure
    // there would be a silent miscompilation (a callee-saved register a missed case changes), a dispatcher miss costs one lookup. So those
    // tables stay with the dispatcher and their functions open.
    // The target may be a 64-bit word read at the sum of variables (each with coefficient 1) and a constant, and be that
    // word itself: a function pointer or a vtable slot (ldr x8, [x0, #0x28]; br x8), or a C++ member function pointer's vtable slot (ldr x8,
    // [x0]; ldr x1, [x8, x1]; br x1), no table: a table's index is scaled by its entries' size. Told apart from a table's unbounded index only
    // so that the census counts tables (README.md, "Unbounded brs by form"); the br goes to the dispatcher either way
    const auto pointer_sum = [&]() {
        if (!load->getType()->isIntegerTy(64)) return false;
        std::map<llvm::Value*, llvm::APInt> zeros;
        for (llvm::Value* leaf : index_leaves) {
            if (!leaf->getType()->isIntegerTy(64)) return false;
            zeros.emplace(leaf, llvm::APInt(64, 0));
        }
        const auto t0 = Eval(t, {{load, llvm::APInt(64, 0)}}), t1 = Eval(t, {{load, llvm::APInt(64, 0x10000)}});
        const auto a0 = Eval(addr, zeros);
        if (!a0 || !t0 || !t1 || !t0->isZero() || t1->getZExtValue() != 0x10000) return false;
        for (llvm::Value* leaf : index_leaves) {
            auto one = zeros;
            one.at(leaf) = llvm::APInt(64, 0x10000);
            const auto a1 = Eval(addr, one);
            if (!a1 || *a1 - *a0 != 0x10000) return false;
        }
        return true;
    };
    if (!best || set_size(*best_range) > max_table) {
        out.why = pointer_sum()              ? (index_leaves.size() == 1 ? "a pointer loaded from a register plus a constant (a function pointer or vtable slot, no table)"
                                                                         : "a pointer loaded from a sum of registers (a member function pointer's vtable slot, no table)")
                  : best                     ? "the index range has more than " + std::to_string(max_table) + " values"
                  : index_leaves.size() == 1 ? "the index is not bounded at the br"
                                             : "the entry address depends on more than one value, and no value it is a function of is bounded at the br";
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

// A region lifted and analysed: whether it lifted, whether it reaches the br, and the analysis of each marker of the br up to the first that
// found no table (the targets' validity, which can change, is checked by the caller)
struct RegionMemo::Outcome {
    bool lifted = false;
    std::string lift_error;
    bool reached = false;
    std::vector<Analysed> markers;
};

uint64_t BytesOf(const RegionMemo& memo) {
    // Each key, and each outcome once (a shared_ptr's control block and the object in one allocation: make_shared)
    uint64_t b = memory::Of(memo.regions);
    std::set<const RegionMemo::Outcome*> seen;
    for (const auto& [key, outcome] : memo.regions) {
        b += memory::Of(key);
        if (!outcome || !seen.insert(outcome.get()).second) continue;
        b += memory::Chunk(16 + sizeof(RegionMemo::Outcome)) + memory::Of(outcome->lift_error) + memory::Of(outcome->markers);
        for (const Analysed& a : outcome->markers) b += memory::Of(a.targets) + memory::Of(a.info.targets) + memory::Of(a.why);
    }
    return b;
}

namespace {

// The Z flag of a compare whose flags reach their branch through a block boundary (the lifter keeps N, Z, C and V apart there: a tbnz between
// `cmp w10, #0x32` and its `b.hi`) is `icmp eq (sub x, C), 0`, and b.hi's condition C & !Z. LazyValueInfo bounds x by the conditions on x
// itself, and `sub x, C` is another value: on the default's false edge it then knew x < C or nothing, not x <= C, and the switch's index
// stayed unbounded (Mario vs. Donkey Kong's sdk+0x105f20; README.md, "Unbounded brs by form"). (x - C == 0) is (x == C) in modular
// arithmetic: the compare is rewritten so, and add x, C likewise (x + C == 0 is x == -C). InstCombine would do it, and also rewrite the
// index's zext (trunc x) the bound compares (the pass list's comment above), so only this rewrite runs
void CompareOperands(llvm::Function& f) {
    for (auto& bb : f) {
        for (auto& inst : bb) {
            auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst);
            if (!cmp || !cmp->isEquality()) continue;
            auto* zero = llvm::dyn_cast<llvm::ConstantInt>(cmp->getOperand(1));
            auto* bin = llvm::dyn_cast<llvm::BinaryOperator>(cmp->getOperand(0));
            if (!zero || !zero->isZero() || !bin) continue;
            auto* c = llvm::dyn_cast<llvm::ConstantInt>(bin->getOperand(1));
            if (!c) continue;
            if (bin->getOpcode() == llvm::Instruction::Sub) {
                cmp->setOperand(0, bin->getOperand(0));
                cmp->setOperand(1, c);
            } else if (bin->getOpcode() == llvm::Instruction::Add) {
                cmp->setOperand(0, bin->getOperand(0));
                cmp->setOperand(1, llvm::ConstantInt::get(c->getType(), -c->getValue()));
            }
        }
    }
}

std::shared_ptr<const RegionMemo::Outcome> AnalyseRegion(const module::Image& image, const Function& fn, uint64_t br, uint64_t e,
                                                         const std::set<uint64_t>& slice, const CallEffects& calls,
                                                         const decode::CodeReader& read, unsigned max_table) {
    auto out = std::make_shared<RegionMemo::Outcome>();
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("nxaot.jumptable", ctx);
    module->setTargetTriple("x86_64-unknown-linux-gnu");
    module->setDataLayout(kDataLayout);
    lift::Region region;
    region.entry = e;
    region.lo = fn.entry;
    region.hi = fn.limit;
    if (image.aux_size) {
        region.extra.emplace_back(image.aux_off, image.aux_off + image.aux_size);  // the frame step's stubs: code of the functions that reach them
    }
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
    if (!lifted.function) {
        out->lift_error = lifted.error.empty() ? "unlowered opcodes" : lifted.error;
        return out;
    }
    out->lifted = true;
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
    CompareOperands(*lifted.function);
    if (const char* dump = std::getenv("NXAOT_DUMP_REGION"); dump && std::strtoull(dump, nullptr, 16) == br) {
        // Debugging: the optimised IR of every region lifted for this br (NXAOT_DUMP_REGION=<hex pc of the br>)
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
    out->reached = !markers.empty();
    for (llvm::CallInst* marker : markers) {
        Analysed a = AnalyseTarget(image, marker, lvi, module->getDataLayout(), max_table);
        const bool none = a.targets.empty();
        out->markers.push_back(std::move(a));
        if (none) break;
    }
    return out;
}

}  // namespace

TableResult ResolveJumpTable(const module::Image& image, const Function& fn, uint64_t br, const DiscoverOptions& options,
                             const std::function<bool(uint64_t)>& valid, const CallEffects& calls, bool optimistic, RegionMemo* memo) {
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
            trace(home, res.why);
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
        if (!image.InCode(pc)) return std::nullopt;
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
        uint64_t open_at = 0, open_from = 0;  // the first block entered from outside, and from where (~0: an entry of the function), for the trace
        for (const uint64_t n : slice) {
            if (n == e) continue;
            if (g.external.count(n) && closed) {
                closed = false;
                open_at = n;
                open_from = ~0ull;
            }
            for (const auto& [p, kind] : g.preds.at(n)) {
                if (!excluded.count(p) && (!slice.count(p) || !followable(p, kind)) && closed) {
                    closed = false;
                    open_at = n;
                    open_from = p;
                }
            }
        }
        if (!closed) {
            shape_why = "the region is entered from outside it";
            if (debug) {
                char buf[96];
                if (open_from == ~0ull) {
                    std::snprintf(buf, sizeof buf, " (%#llx, an entry of the function)", static_cast<unsigned long long>(open_at));
                } else {
                    std::snprintf(buf, sizeof buf, " (%#llx from %#llx)", static_cast<unsigned long long>(open_at), static_cast<unsigned long long>(open_from));
                }
                trace(e, shape_why + buf);
            }
            continue;
        }
        // The region's key (RegionMemo): what its lifting and analysis read. The lifter consults the call effects at the calls that end its
        // blocks and the known tables at its brs: those of every straight line from a block of the slice to the first instruction that ends
        // a block (a superset of what it consults: equal facts there give the same region)
        std::vector<uint64_t> key{br, e, fn.entry, fn.limit, calls ? 1u : 0u, options.max_table, slice.size()};
        key.insert(key.end(), slice.begin(), slice.end());
        for (const uint64_t s : slice) {
            for (uint64_t pc = s; pc < fn.limit || image.InAux(pc); pc += 4) {
                const auto w = read(pc);
                if (!w) break;
                const Insn i = ClassifyWord(*w, pc);
                if (!i.Ends()) continue;
                if ((i.kind == Kind::BL || i.kind == Kind::BLR) && calls) {
                    const auto c = calls(pc);
                    key.insert(key.end(), {pc, c ? 1u : 0u, c ? c->x : 0, c ? c->v : 0, c ? uint64_t{c->sp} : 0, c ? uint64_t{c->nzcv} : 0});
                } else if (i.kind == Kind::BR) {
                    const auto t = fn.tables.find(pc);
                    key.insert(key.end(), {pc, t == fn.tables.end() ? ~0ull : t->second.targets.size()});
                    if (t != fn.tables.end()) key.insert(key.end(), t->second.targets.begin(), t->second.targets.end());
                }
                break;
            }
        }
        // The memo keeps the key in 32-bit words (module offsets, effect bits and counts; no table: all ones), half the bytes (README.md,
        // "Memory at scale"); a key with a value that does not fit (no module of under 4 GiB has one) is not memoised: the region is lifted
        std::vector<uint32_t> key32;
        bool fits = memo != nullptr;
        if (fits) {
            key32.reserve(key.size());
            for (const uint64_t v : key) {
                if (v == ~0ull) {
                    key32.push_back(UINT32_MAX);
                } else if (v >= UINT32_MAX) {
                    fits = false;
                    break;
                } else {
                    key32.push_back(static_cast<uint32_t>(v));
                }
            }
        }
        std::shared_ptr<const RegionMemo::Outcome> outcome;
        if (fits) {
            if (const auto it = memo->regions.find(key32); it != memo->regions.end()) {
                outcome = it->second;
                ++res.reused;
            }
        }
        if (!outcome) {
            outcome = AnalyseRegion(image, fn, br, e, slice, calls, read, options.max_table);
            ++res.regions;
            if (fits) memo->regions.emplace(std::move(key32), outcome);
        }
        if (!outcome->lifted) {
            lifted_why = "the region could not be lifted (" + outcome->lift_error + ")";
            trace(e, lifted_why);
            continue;
        }
        if (!outcome->reached) {
            lifted_why = "the lifted region does not reach the br";
            trace(e, lifted_why);
            continue;
        }
        Analysed first;
        std::set<uint64_t> all;
        bool ok = true;
        for (size_t i = 0; i < outcome->markers.size() && ok; ++i) {
            Analysed a = outcome->markers[i];
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
