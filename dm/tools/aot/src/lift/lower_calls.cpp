// Function mode of the lifter (lifter.h, LiftOptions::plan): one guest function as one LLVM function of the convention of
// docs/design/aot-recompiler.md section 2.3, with the calls, returns and tail calls of section 3.3, the inline caches of section 4 and the host
// calls of section 2.4.
//
// Registers live in allocas, as in a region. What is passed comes from the arguments, the rest is loaded from the state block (loads nobody
// uses are removed by the optimiser). The block is the home of every register that is not in a host register: a forward data flow over the
// function's blocks (FnDataflow) gives, at every block end, the registers newer than their copy in the block ("dirty"), and the call, return and
// escape sites store exactly what the convention needs of them:
//   call to g        store dirty - PASS(g); after it take x0, x1 and RV(g) from the return value and reload WRITES(g) from the block
//   tail call to g   store dirty - PASS(g), and what g passes back unchanged that a caller of this function may read and reloads
//                    (LIVE_AFTER & WRITES - WRITES(g))
//   return           store dirty & LIVE_AFTER & WRITES - the returned registers, and the registers it restores (outside WRITES) whose block
//                    copy it changed before an inner call (the restored value again, design 2.3; tail calls too)
//   svc, traps, a non-local return    store every dirty register (the block then holds the whole register file)
// The correctness mode stores every register at every call, tail call and return, reloads every register after a call and takes nothing from
// arguments or return values: the reference the differential tests compare the summary-driven mode with.
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <llvm/ADT/StringExtras.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/terminal.h"
#include "lift/function_lifter.h"
#include "nxaot/aot_module.h"
#include "nxaot/guest_state.h"

namespace nxaot::lift {

using llvm::BasicBlock;
using llvm::Value;

const char* CallConvName(CallConv cc) {
    return cc == CallConv::Ghc ? "ghc" : "swifttail";
}

Regs Convention::Passable() const {
    return cc == CallConv::Ghc ? Regs{0x70u, 0x3Fu, false, false} : Regs{0xF0u, 0xFFu, false, false};
}

unsigned Convention::LlvmCC() const {
    return cc == CallConv::Ghc ? llvm::CallingConv::GHC : llvm::CallingConv::SwiftTail;
}

llvm::StructType* Convention::RetType(llvm::LLVMContext& c) const {
    auto* i64 = llvm::Type::getInt64Ty(c);
    auto* v = llvm::FixedVectorType::get(i64, 2);
    return llvm::StructType::get(c, {i64, i64, v, v, v, v});
}

namespace {

// The registers a signature carries besides st, sp and lr
Regs Signature(const Convention& conv, const Regs& pass) {
    const Regs always = Regs::X(0xFu);
    return conv.cc == CallConv::Ghc ? always | conv.Passable() : always | (pass & conv.Passable());
}

}  // namespace

llvm::FunctionType* Convention::Type(llvm::LLVMContext& c, const Regs& pass) const {
    const Regs s = Signature(*this, pass);
    auto* i64 = llvm::Type::getInt64Ty(c);
    std::vector<llvm::Type*> params{llvm::PointerType::get(c, 0), i64, i64};
    for (unsigned i = 0; i < 31; ++i) {
        if (s.x >> i & 1) {
            params.push_back(i64);
        }
    }
    for (unsigned i = 0; i < 32; ++i) {
        if (s.v >> i & 1) {
            params.push_back(llvm::FixedVectorType::get(i64, 2));
        }
    }
    return llvm::FunctionType::get(RetType(c), params, false);
}

int Convention::ArgX(const Regs& pass, unsigned i) const {
    const Regs s = Signature(*this, pass);
    if (!(s.x >> i & 1)) {
        return -1;
    }
    return 3 + std::popcount(s.x & ((1u << i) - 1));
}

int Convention::ArgV(const Regs& pass, unsigned i) const {
    const Regs s = Signature(*this, pass);
    if (!(s.v >> i & 1)) {
        return -1;
    }
    return 3 + std::popcount(s.x) + std::popcount(i ? s.v & ((1u << i) - 1) : 0u);
}

void Convention::Apply(llvm::Function* f) const {
    f->setCallingConv(LlvmCC());
    if (cc == CallConv::SwiftTail) {
        f->addParamAttr(0, llvm::Attribute::SwiftSelf);
    }
}

void Convention::Apply(llvm::CallInst* call) const {
    call->setCallingConv(LlvmCC());
    if (cc == CallConv::SwiftTail) {
        call->addParamAttr(0, llvm::Attribute::SwiftSelf);
    }
}

namespace detail {

using namespace Dynarmic;

namespace {

constexpr uint64_t kProbeDelta = 1ull << 44;  // the second translation's displacement (FnRelative): a multiple of the page size, below 2^55
constexpr uint32_t kRetX = 0x3u;              // x0, x1: always in the return value
const Regs kSpLr{1u << 30, 0, true, false};   // sp and x30 travel as the sp and lr arguments of every call

int64_t SignExtend(uint64_t v, unsigned bits) {
    return static_cast<int64_t>(v << (64 - bits)) >> (64 - bits);
}

// What a call site stores to the block first (section 2.3): what the callee does not take in host registers, and through the dispatcher also
// what it takes that the union of the possible callees may change (CalleeView::exact), since the caller reloads that afterwards
Regs CallStores(const Regs& dirty, const CalleeView& c) {
    return c.exact ? dirty - c.pass - kSpLr : dirty - ((c.pass | kSpLr) - c.writes);
}

}  // namespace

// ---- Discover ----

void FunctionLifter::FnClassify(uint64_t pc, const IR::Block& block, std::vector<uint64_t>& next) {
    const uint64_t end = A64::LocationDescriptor{block.EndLocation()}.PC();
    End e;
    if (end >= pc + 4) {
        if (const auto w = read(end - 4)) {
            const uint32_t word = *w;
            if ((word & 0xFC000000u) == 0x94000000u) {
                e.kind = EndKind::Bl;
                e.target = end - 4 + static_cast<uint64_t>(SignExtend(word & 0x3FFFFFFu, 26) * 4);
            } else if ((word & 0xFFFFFC1Fu) == 0xD63F0000u) {
                e.kind = EndKind::Blr;
            } else if ((word & 0xFFFFFC1Fu) == 0xD65F0000u) {
                e.kind = EndKind::Ret;
            } else if ((word & 0xFFFFFC1Fu) == 0xD61F0000u) {
                e.kind = ((word >> 5) & 31) == 30 ? EndKind::Ret : EndKind::Br;  // br x30 returns, as ret does (section 3.3)
            }
        }
    }
    ends[pc] = e;
    for (const auto& inst : block) {
        if (inst.GetOpcode() == IR::Opcode::A64CallSupervisor) {
            svc_blocks.insert(pc);
        }
    }
    switch (e.kind) {
    case EndKind::Bl: {
        const CalleeView* c = plan->callee(e.target);
        if (!c || c->returns) {
            next.push_back(end);
        }
        break;
    }
    case EndKind::Blr:
        next.push_back(end);
        break;
    case EndKind::Ret:
        break;
    case EndKind::Br:
        if (plan->tables) {
            if (const std::vector<uint64_t>* targets = plan->tables(end - 4)) {
                table_targets[pc] = *targets;
                next.insert(next.end(), targets->begin(), targets->end());
            }
        }
        break;
    case EndKind::Other:
        Successors(block.GetTerminal(), block, next);
        break;
    }
    if ((e.kind == EndKind::Bl || e.kind == EndKind::Blr) && plan->resume) {
        // The resume points of the call site (landing pads, setjmp continuations) are blocks of the function: the pending check after the call
        // branches there (section 3.3)
        const std::vector<uint64_t> r = plan->resume(end - 4);
        next.insert(next.end(), r.begin(), r.end());
    }
    succs[pc] = next;
}

// Which immediates of a block are addresses built from the pc (adr, adrp, literal loads, return addresses, the pcs of exceptions and of svc):
// the block is translated a second time kProbeDelta higher, and every 64-bit immediate that moved by exactly that much is one. Those are module
// offsets in the first translation, lowered as base + offset (Arg), because a module runs wherever the host maps it (section 3.3)
void FunctionLifter::FnRelative(uint64_t pc, IR::Block& block, const std::function<void(IR::Block&)>& passes) {
    const decode::CodeReader shifted = [this](uint64_t a) -> std::optional<uint32_t> {
        if (a < kProbeDelta) {
            return std::nullopt;
        }
        return read(a - kProbeDelta);
    };
    IR::Block probe = decode::TranslateBlock(pc + kProbeDelta, shifted, fpcr, nullptr, GateStops(kProbeDelta));
    passes(probe);
    auto a = block.begin();
    auto b = probe.begin();
    for (; a != block.end() && b != probe.end(); ++a, ++b) {
        if (a->GetOpcode() != b->GetOpcode() || a->NumArgs() != b->NumArgs()) {
            error = "the block at " + llvm::utohexstr(pc) + " translates differently at another address";
            return;
        }
        for (size_t i = 0; i < a->NumArgs(); ++i) {
            const IR::Value x = a->GetArg(i);
            const IR::Value y = b->GetArg(i);
            if (x.IsImmediate() && y.IsImmediate() && x.GetType() == IR::Type::U64 && y.GetType() == IR::Type::U64 &&
                y.GetU64() - x.GetU64() == kProbeDelta) {
                relative.insert({&*a, i});
            }
        }
    }
    if (a != block.end() || b != probe.end()) {
        error = "the block at " + llvm::utohexstr(pc) + " has another length at another address";
    }
}

// ---- The dirty registers at every block end ----

// A loop whose every iteration makes a call (or an svc) stores at that call, each time round, every register newer than the block that the
// callee is not passed: the dirty set at the loop's head is the union over its predecessors, and the edge that enters the loop brings what the
// code before it changed and never stored (a loop of switch-gcc's main stored x4..x27 at its call each time round, two thirds of them
// values set before the loop: README.md, "Against native"). Here, for every head h of a natural loop (h dominates the source of a back edge
// into it) that is entered only through branches of the lifted code (not the function's entry, a landing pad, a part's entry, a resume
// point or a jump table's case, which carry no stores), and in which a call or svc block runs on every iteration (h dominates it and it
// dominates every back edge's source): entry_stores[h] is what that call stores of a full dirty set, without sp, x30 and NZCV (passed to every
// call, or kept apart). An edge from outside the loop stores the part of it that is dirty there and not at the head, once per entry instead
// of once per iteration (FnGoto); the store is a value's current copy in the block, which the convention allows at any point (design 2.3).
// Returns, per head, its predecessors inside its loop (the back edges' sources)
std::map<uint64_t, std::set<uint64_t>> FunctionLifter::FnLoopHeads() {
    std::map<uint64_t, std::set<uint64_t>> heads;
    if (plan->correctness) {
        return heads;
    }
    // Dominators over the guest blocks (Cooper, Harvey and Kennedy), from a root before every way in: the entry, the landing pads, the parts'
    // entries; the resume points are successors of their call blocks
    std::vector<uint64_t> order;  // reverse postorder
    std::map<uint64_t, int> index;
    std::map<uint64_t, std::vector<uint64_t>> preds;
    for (const auto& [pc, next] : succs) {
        for (const uint64_t s : next) {
            if (blocks.count(s)) preds[s].push_back(pc);
        }
    }
    std::vector<uint64_t> starts;
    if (blocks.count(plan->entry)) starts.push_back(plan->entry);
    for (const uint64_t r : plan->roots) {
        if (blocks.count(r)) starts.push_back(r);
    }
    for (const uint64_t e : plan->part_entries) {
        if (blocks.count(e)) starts.push_back(e);
    }
    {
        std::set<uint64_t> seen;
        std::vector<std::pair<uint64_t, size_t>> stack;
        std::vector<uint64_t> post;
        for (const uint64_t s : starts) {
            if (!seen.insert(s).second) continue;
            stack.push_back({s, 0});
            while (!stack.empty()) {
                auto& [pc, i] = stack.back();
                const auto& next = succs[pc];
                if (i < next.size()) {
                    const uint64_t n = next[i++];
                    if (blocks.count(n) && seen.insert(n).second) stack.push_back({n, 0});
                } else {
                    post.push_back(pc);
                    stack.pop_back();
                }
            }
        }
        order.assign(post.rbegin(), post.rend());
    }
    for (size_t i = 0; i < order.size(); ++i) index[order[i]] = static_cast<int>(i);
    // idom by reverse-postorder index; kRoot is the virtual root, kUnknown a block not reached yet. A dominator has a smaller index
    constexpr int kRoot = -1, kUnknown = -2;
    std::vector<int> idom(order.size(), kUnknown);
    const std::set<uint64_t> start_set(starts.begin(), starts.end());
    for (const uint64_t s : starts) idom[static_cast<size_t>(index.at(s))] = kRoot;
    const auto intersect = [&](int a, int b) {
        while (a != b) {
            while (a > b) a = idom[static_cast<size_t>(a)];  // never kUnknown: a and b are processed blocks, and so are their dominators
            while (b > a) b = idom[static_cast<size_t>(b)];
        }
        return a;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < order.size(); ++i) {
            const uint64_t pc = order[i];
            if (start_set.count(pc)) continue;  // the virtual root's successors
            int d = kUnknown;
            for (const uint64_t p : preds[pc]) {
                const auto it = index.find(p);
                if (it == index.end() || idom[static_cast<size_t>(it->second)] == kUnknown) continue;
                d = d == kUnknown ? it->second : intersect(d, it->second);
            }
            if (d != kUnknown && idom[i] != d) {
                idom[i] = d;
                changed = true;
            }
        }
    }
    const auto dominates = [&](uint64_t a, uint64_t b) {  // a dominates b
        const auto ia = index.find(a), ib = index.find(b);
        if (ia == index.end() || ib == index.end()) return false;
        for (int x = ib->second; x >= 0; x = idom[static_cast<size_t>(x)]) {
            if (x == ia->second) return true;
        }
        return false;
    };
    // The heads no lifted branch alone enters: they cannot carry an edge's stores
    std::set<uint64_t> excluded(starts.begin(), starts.end());
    for (const auto& [pc, targets] : table_targets) {
        excluded.insert(targets.begin(), targets.end());
    }
    if (plan->resume) {
        for (const auto& [pc, e] : ends) {
            if (e.kind == EndKind::Bl || e.kind == EndKind::Blr) {
                for (const uint64_t r : plan->resume(A64::LocationDescriptor{blocks.at(pc).EndLocation()}.PC() - 4)) excluded.insert(r);
            }
        }
    }
    // What a call block stores of a full dirty set (FnCall's CallStores; a guarded call's two paths both store it)
    const Regs all = Regs::All();
    const auto stores_of = [&](uint64_t pc) -> std::optional<Regs> {
        if (svc_blocks.count(pc)) return all;
        const End& e = ends[pc];
        const uint64_t site = A64::LocationDescriptor{blocks.at(pc).EndLocation()}.PC() - 4;
        if (e.kind == EndKind::Bl) {
            const CalleeView* c = plan->callee(e.target);
            return CallStores(all, c ? *c : plan->indirect);
        }
        if (e.kind == EndKind::Blr) {
            Regs r = CallStores(all, plan->indirect);
            if (const CalleeView* g = Guarded(site, true)) r = r & CallStores(all, *g);
            return r;
        }
        return std::nullopt;
    };
    for (const uint64_t h : order) {
        if (excluded.count(h)) continue;
        std::set<uint64_t> back;
        for (const uint64_t p : preds[h]) {
            if (dominates(h, p)) back.insert(p);
        }
        if (back.empty()) continue;
        // The loop's body: h and what reaches a back edge's source without passing h
        std::set<uint64_t> body{h};
        std::vector<uint64_t> work(back.begin(), back.end());
        while (!work.empty()) {
            const uint64_t b = work.back();
            work.pop_back();
            if (!body.insert(b).second) continue;
            for (const uint64_t p : preds[b]) {
                if (!body.count(p)) work.push_back(p);
            }
        }
        Regs m;
        for (const uint64_t b : body) {
            const auto s = stores_of(b);
            if (!s || !dominates(h, b)) continue;
            bool every = true;
            for (const uint64_t p : back) every = every && dominates(b, p);
            if (every) m = m | *s;
        }
        m = m - kSpLr;
        m.nzcv = false;
        if (m.Empty()) continue;
        entry_stores[h] = m;
        heads[h] = back;
    }
    return heads;
}

void FunctionLifter::FnDataflow() {
    for (auto& [pc, block] : blocks) {
        Regs g;
        for (const auto& inst : block) {
            switch (inst.GetOpcode()) {
            case IR::Opcode::A64SetW:
            case IR::Opcode::A64SetX:
                g.x |= 1u << static_cast<unsigned>(inst.GetArg(0).GetA64RegRef());
                break;
            case IR::Opcode::A64SetSP:
                g.sp = true;
                break;
            case IR::Opcode::A64SetS:
            case IR::Opcode::A64SetD:
            case IR::Opcode::A64SetQ:
                g.v |= 1u << static_cast<unsigned>(inst.GetArg(0).GetA64VecRef());
                break;
            case IR::Opcode::A64SetNZCV:
            case IR::Opcode::A64SetNZCVRaw:
                g.nzcv = true;
                break;
            default:
                break;
            }
        }
        gen[pc] = g;
    }
    // At the entry the passed registers, sp and lr are newer than the block (they came in host registers). A part of a split function is
    // entered with the whole register file in the block: nothing is newer at its entries
    if (plan->part_entries.empty()) {
        dirty_in[plan->entry] = plan->self.pass | kSpLr;
    }
    const auto after_call = [&](const Regs& d, const CalleeView& c) {
        // What stays newer than the block after a call: what was passed and not changed, and what the return value brought
        return ((d & (c.pass | kSpLr)) - c.writes) | Regs::X(kRetX) | Regs::V(c.rv);
    };
    const auto out_of = [&](uint64_t pc) {
        const Regs at_end = dirty_in[pc] | gen[pc];
        const End& e = ends[pc];
        if (svc_blocks.count(pc)) {
            return Regs{};  // the svc stores every dirty register and reloads x0..x8
        } else if (e.kind == EndKind::Bl) {
            const CalleeView* c = plan->callee(e.target);
            return c ? after_call(at_end, *c) : Regs{};
        } else if (e.kind == EndKind::Blr) {
            return after_call(at_end, plan->indirect);
        }
        return at_end;
    };
    // An edge into a loop whose iterations call stores what the call would store each time round (FnLoopHeads, FnGoto): the head's dirty
    // set is then what its back edges bring, and what the edges from outside keep after their stores
    const std::map<uint64_t, std::set<uint64_t>> heads = FnLoopHeads();
    const auto reaching = [&](uint64_t pc, uint64_t s, const Regs& out) {
        const auto h = heads.find(s);
        return h == heads.end() || h->second.count(pc) ? out : out - entry_stores.at(s);
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [pc, block] : blocks) {
            const Regs out = out_of(pc);
            for (const uint64_t s : succs[pc]) {
                if (!blocks.count(s)) {
                    continue;
                }
                const Regs next = dirty_in[s] | reaching(pc, s, out);
                if (!(next == dirty_in[s])) {
                    dirty_in[s] = next;
                    changed = true;
                }
            }
        }
    }
    // What the function may leave in the block with another value than its callers keep in host registers: a register it writes and restores
    // (outside WRITES, so its callers keep their own copy) whose value of the moment it stored there before an inner call or an svc. Section
    // 2.3: such a function stores the restored value again before it returns, so that a caller that kept the register in SSA and a later
    // callee that reads it from the block agree.
    // SP counts as stored at every call: it travels in a host register, and the callee may store the SP it is passed (its own svc, its own
    // return's store of a restored SP, a tail call through the dispatcher). A function that keeps SP but moves it, and calls, may thus leave
    // its own moved SP in the block, and a caller that reloads SP from the block after calling it (a view of it with SP in WRITES: an
    // indirect call, a call of a PLT stub, whose WRITES are the union over the standard entries) went on with that SP: Mario vs. Donkey Kong's
    // nnosInitialize returned through a frame record of the callee's (dm/tools/aot/README.md, "A callee's SP left in the block")
    Regs stored, written;
    for (auto& [pc, block] : blocks) {
        const Regs at_end = dirty_in[pc] | gen[pc];
        written = written | gen[pc];
        const End& e = ends[pc];
        if (e.kind == EndKind::Bl || e.kind == EndKind::Blr) {
            stored.sp = stored.sp || at_end.sp;
        }
        if (svc_blocks.count(pc)) {
            stored = stored | at_end;
        } else if (e.kind == EndKind::Bl) {
            if (const CalleeView* c = plan->callee(e.target)) {
                stored = stored | CallStores(at_end, *c);
            }
        } else if (e.kind == EndKind::Blr) {
            stored = stored | CallStores(at_end, plan->indirect);
            if (const CalleeView* g = Guarded(A64::LocationDescriptor{block.EndLocation()}.PC() - 4, true)) {
                // A guarded call (FnCallGuarded): its direct path stores what bl to the target stores, then what it keeps newer than the block beyond what the
                // indirect call keeps (the successors were computed with the indirect call's)
                stored = stored | CallStores(at_end, *g) | (after_call(at_end, *g) - after_call(at_end, plan->indirect));
            }
        }
        for (const uint64_t s : succs[pc]) {
            const auto h = entry_stores.find(s);
            if (h != entry_stores.end() && blocks.count(s)) {
                stored = stored | ((out_of(pc) & h->second) - dirty_in[s]);  // an edge into a loop head (FnGoto)
            }
        }
        // A safepoint poll's or a gate's cold path stores the registers newer than the block too (FnPoll, FnGate), with the values of that
        // moment: a backward branch's poll and a spin wait's gate (FnGoto) and a jump table's poll (a case at or before its block) store the
        // block's end state, an observation gate the state before the block. A register the function writes and restores, stored there with
        // its own value, must be stored again before the function returns, as one an inner call stored: a caller that keeps the register in SSA
        // across the call and later reloads it from the block (after an indirect call, whose WRITES are the union) read the function's value of
        // the moment. The entry's poll stores the entry values, the callers' own. Counted whether or not FnElidePolls removes the poll later
        // (README.md, "Polls and gates in restore_block")
        if (plan->obs_gates.count(pc)) {
            stored = stored | dirty_in[pc];
        }
        if (const uint64_t end = A64::LocationDescriptor{block.EndLocation()}.PC(); end >= 4 && plan->spin_gates.count(end - 4)) {
            stored = stored | at_end;
        }
        if (plan->safepoints && plan->rt_safepoint) {
            bool back = false;
            for (const uint64_t s : succs[pc]) back = back || (s <= pc && blocks.count(s));
            if (back) stored = stored | at_end;
        }
    }
    restore_block = (stored & written) - plan->self.writes;
    if (plan->split) {
        restore_block = plan->restore;  // the whole function's, which no part sees (FunctionPlan::restore)
    }

    // Where NZCV is live (backward, over the function's blocks): a block reads it when an instruction or its terminal evaluates a condition, or
    // reads the flags, before the block sets them; a call, a return, a tail call or an svc ends the function's knowledge, so NZCV counts as live
    // after them. A safepoint poll stores NZCV only where it is live: storing a dead NZCV would keep the flags of every compare materialised in
    // the loop that polls (design 6.2's cost)
    const auto reads_cond = [](const IR::Terminal& t, const auto& self) -> bool {
        if (const auto* i = boost::get<IR::Term::If>(&t)) {
            return (i->if_ != IR::Cond::AL && i->if_ != IR::Cond::NV) || self(i->then_, self) || self(i->else_, self);
        }
        if (const auto* cb = boost::get<IR::Term::CheckBit>(&t)) {
            return self(cb->then_, self) || self(cb->else_, self);
        }
        if (const auto* ch = boost::get<IR::Term::CheckHalt>(&t)) {
            return self(ch->else_, self);
        }
        return false;
    };
    std::map<uint64_t, bool> use, def, out_unknown;
    for (auto& [pc, block] : blocks) {
        bool u = false, d = false;
        for (const auto& inst : block) {
            const IR::Opcode op = inst.GetOpcode();
            if (!d) {
                bool reads = op == IR::Opcode::A64GetNZCVRaw || op == IR::Opcode::A64GetCFlag;
                for (size_t i = 0; i < inst.NumArgs() && !reads; ++i) {
                    const IR::Value a = inst.GetArg(i);
                    reads = a.GetType() == IR::Type::Cond && a.GetCond() != IR::Cond::AL && a.GetCond() != IR::Cond::NV;
                }
                u = u || reads;
            }
            d = d || op == IR::Opcode::A64SetNZCV || op == IR::Opcode::A64SetNZCVRaw;
        }
        if (!d && reads_cond(block.GetTerminal(), reads_cond)) {
            u = true;
        }
        use[pc] = u;
        def[pc] = d;
        const End& e = ends[pc];
        bool unknown = svc_blocks.count(pc) || e.kind != EndKind::Other;
        if (!plan->correctness && !svc_blocks.count(pc)) {
            // A call reads NZCV when the callee may read it before writing it (its summary; the indirect view always), and after it the caller's
            // successor may: a dirty NZCV dead there is not stored for the call (FnCall), as a poll leaves a dead one alone (design 6.2). A return
            // reads it when a caller may after the call (LIVE_AFTER: FnReturn stores no other)
            if (e.kind == EndKind::Bl) {
                const CalleeView* c = plan->callee(e.target);
                unknown = !c || c->reads_nzcv;
            } else if (e.kind == EndKind::Blr) {
                unknown = plan->indirect.reads_nzcv;
            } else if (e.kind == EndKind::Ret) {
                unknown = plan->live_after.nzcv;
            }
        }
        for (const uint64_t n : succs[pc]) {
            unknown = unknown || !blocks.count(n);
        }
        out_unknown[pc] = unknown;
    }
    std::map<uint64_t, bool> live_in;
    for (bool again = true; again;) {
        again = false;
        for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
            const uint64_t pc = it->first;
            bool out = out_unknown[pc];
            for (const uint64_t n : succs[pc]) {
                out = out || (blocks.count(n) && live_in[n]);
            }
            const bool in = use[pc] || (out && !def[pc]);
            if (in != live_in[pc]) {
                live_in[pc] = in;
                again = true;
            }
        }
    }
    for (const auto& [pc, in] : live_in) {
        if (in) {
            nzcv_live.insert(pc);
        }
    }
    for (auto& [pc, block] : blocks) {
        bool out = out_unknown[pc];
        for (const uint64_t n : succs[pc]) {
            out = out || (blocks.count(n) && live_in[n]);
        }
        if (out) {
            nzcv_live_out.insert(pc);
        }
    }
}

// ---- Values ----

Value* FunctionLifter::Rel(uint64_t offset) {
    Value* v = B.CreateAdd(base, B.getInt64(offset), "rel_" + llvm::utohexstr(offset));
    rel_values[v] = offset;
    return v;
}

std::optional<uint64_t> FunctionLifter::OffsetOf(Value* v) {
    if (auto* k = llvm::dyn_cast<llvm::ConstantInt>(v)) {
        return k->getZExtValue();
    }
    const auto it = rel_values.find(v);
    if (it != rel_values.end()) {
        return it->second;
    }
    return std::nullopt;
}

uint64_t FunctionLifter::CurOffset() const {
    if (cur_inst) {
        const auto it = inst_pcs.find(cur_inst);
        if (it != inst_pcs.end()) {
            return it->second;
        }
    }
    return cur_end >= 4 ? cur_end - 4 : cur_start;
}

void FunctionLifter::LineEntry(llvm::Instruction* i, uint64_t offset) {
    if (plan->line_section.empty()) {
        return;
    }
    llvm::Metadata* aux = llvm::ConstantAsMetadata::get(B.getInt32(static_cast<uint32_t>(offset)));
    i->setMetadata(llvm::LLVMContext::MD_pcsections,
                   llvm::MDNode::get(C, {llvm::MDString::get(C, plan->line_section), llvm::MDNode::get(C, {aux})}));
    ++lines_;
}

void FunctionLifter::StoreRegs(const Regs& r) {
    for (unsigned i = 0; i < 31; ++i) {
        if (r.x >> i & 1) {
            StoreField(B.CreateLoad(B.getInt64Ty(), x[i]), offsetof(dm_guest_state, x) + 8 * i);
        }
    }
    if (r.sp) {
        StoreField(B.CreateLoad(B.getInt64Ty(), sp), offsetof(dm_guest_state, sp));
    }
    for (unsigned i = 0; i < 32; ++i) {
        if (r.v >> i & 1) {
            StoreField(B.CreateLoad(V128(), vreg[i]), offsetof(dm_guest_state, v) + 16 * i);
        }
    }
    if (r.nzcv) {
        StoreField(PackNzcv(CurrentFlags()), offsetof(dm_guest_state, nzcv));
    }
}

void FunctionLifter::ReloadRegs(const Regs& r) {
    for (unsigned i = 0; i < 31; ++i) {
        if (r.x >> i & 1) {
            B.CreateStore(LoadField(B.getInt64Ty(), offsetof(dm_guest_state, x) + 8 * i), x[i]);
        }
    }
    if (r.sp) {
        B.CreateStore(LoadField(B.getInt64Ty(), offsetof(dm_guest_state, sp)), sp);
    }
    for (unsigned i = 0; i < 32; ++i) {
        if (r.v >> i & 1) {
            B.CreateStore(LoadField(V128(), offsetof(dm_guest_state, v) + 16 * i), vreg[i]);
        }
    }
    if (r.nzcv) {
        if (deferred_nzcv && in_terminator) {
            error = "internal: NZCV reloaded on one path of a terminator while its flags are deferred";  // never so: a deferred NZCV
                                                                                                     // reaches the terminator only
                                                                                                     // where nothing after it reads it
        }
        deferred_nzcv = nullptr;  // the block's copy replaces them
        const Flags f = UnpackNzcv(LoadField(B.getInt32Ty(), offsetof(dm_guest_state, nzcv)));
        B.CreateStore(f.n, flag[0]);
        B.CreateStore(f.z, flag[1]);
        B.CreateStore(f.c, flag[2]);
        B.CreateStore(f.v, flag[3]);
        cur_origin = {};
    }
}

Value* FunctionLifter::RetValue() {
    llvm::StructType* rt = plan->conv.RetType(C);
    Value* r = llvm::PoisonValue::get(rt);
    r = B.CreateInsertValue(r, B.CreateLoad(B.getInt64Ty(), x[0]), 0);
    r = B.CreateInsertValue(r, B.CreateLoad(B.getInt64Ty(), x[1]), 1);
    for (unsigned i = 0; i < 4; ++i) {
        if (plan->self.rv >> i & 1) {
            r = B.CreateInsertValue(r, B.CreateLoad(V128(), vreg[i]), 2 + i);
        }
    }
    return r;
}

// ---- The prologue ----

void FunctionLifter::FnPrologue() {
    auto* entry = BasicBlock::Create(C, "entry", F, &F->getEntryBlock());
    B.SetInsertPoint(entry);
    if (plan->debug) {
        B.SetCurrentDebugLocation(llvm::DILocation::get(C, static_cast<unsigned>(plan->entry), 0, plan->debug));
    }
    for (size_t i = 0; i < 31; ++i) {
        x[i] = B.CreateAlloca(B.getInt64Ty(), nullptr, "x" + std::to_string(i));
    }
    for (size_t i = 0; i < 32; ++i) {
        vreg[i] = B.CreateAlloca(V128(), nullptr, "v" + std::to_string(i));
    }
    sp = B.CreateAlloca(B.getInt64Ty(), nullptr, "sp");
    static const char* const flag_names[] = {"n", "z", "c", "v"};
    for (size_t i = 0; i < 4; ++i) {
        flag[i] = B.CreateAlloca(B.getInt1Ty(), nullptr, flag_names[i]);
    }
    check_bit = B.CreateAlloca(B.getInt1Ty(), nullptr, "check_bit");
    qc = B.CreateAlloca(B.getInt1Ty(), nullptr, "qc");
    fpsr_bits = B.CreateAlloca(B.getInt32Ty(), nullptr, "fpsr_bits");
    auto* base_load = B.CreateAlignedLoad(B.getInt64Ty(), plan->base, llvm::Align(8), "base");
    base_load->setMetadata(llvm::LLVMContext::MD_invariant_load, llvm::MDNode::get(C, {}));
    base = base_load;
    init_after = base_load;  // the registers' first values go here once the body is lowered (FnInitRegisters)
    entry_lr = F->getArg(2);
    entry_lr->setName("lr");
    F->getArg(1)->setName("sp_in");
    const Convention& conv = plan->conv;
    if (!plan->part_entries.empty()) {
        // A part of a split function (FunctionPlan::part_entries): every register from the block, and the block to start at from x0's argument.
        // No poll here: the stub that is the function's body polls at its entry, and a transfer between parts that goes backwards polls
        // before it (FnSplit)
        llvm::Argument* at = F->getArg(static_cast<unsigned>(conv.ArgX(Regs{}, 0)));
        at->setName("part_entry");
        if (plan->part_entries.size() == 1) {
            B.CreateBr(bbs.at(plan->part_entries.front()));
            return;
        }
        auto* none = BasicBlock::Create(C, "part_none", F);
        auto* sw = B.CreateSwitch(at, none, static_cast<unsigned>(plan->part_entries.size()));
        for (const uint64_t e : plan->part_entries) {
            sw->addCase(B.getInt64(e), bbs.at(e));
        }
        B.SetInsertPoint(none);
        B.CreateUnreachable();  // the parts pass only their entries' offsets (src/build/pack.cpp)
        return;
    }
    if (!plan->correctness) {
        for (unsigned i = 0; i < 31; ++i) {
            const int a = conv.ArgX(plan->self.pass, i);
            if (i != 30 && a >= 0 && (plan->self.pass.x >> i & 1)) F->getArg(static_cast<unsigned>(a))->setName("x" + std::to_string(i) + "_in");
        }
        for (unsigned i = 0; i < 32; ++i) {
            const int a = conv.ArgV(plan->self.pass, i);
            if (a >= 0 && (plan->self.pass.v >> i & 1)) F->getArg(static_cast<unsigned>(a))->setName("v" + std::to_string(i) + "_in");
        }
    }
    // Section 6.2: a poll at the entry of every function that calls or tail-calls (every cycle of the call graph, recursion and chains of tail
    // calls, passes through one), and at every backward branch (every loop: FnGoto). A function that does neither runs a bounded time, so the
    // polls of the code around it bound its own: no poll at its entry
    bool calls = false;
    for (const auto& [pc, e] : ends) {
        calls = calls || e.kind == EndKind::Bl || e.kind == EndKind::Blr || (e.kind == EndKind::Br && !table_targets.count(pc));
    }
    for (const auto& [pc, next] : succs) {
        for (const uint64_t n : next) {
            calls = calls || !InRegion(n);
        }
    }
    if (calls) {
        FnPoll(Rel(plan->entry), plan->self.pass | kSpLr, PollKind::Entry);
    }
    B.CreateBr(bbs.at(plan->entry));
}

// The registers' first values, at the entry after the base: an argument, or the copy in the block. Stored once the body is lowered, and only
// for the registers some part of the function reads (a load of its alloca): the prologue stored all 63 and the flags in every function, and
// SROA and the dead-code passes removed the unread ones again, a fixed cost of some 250 IR instructions per function, more than most functions'
// bodies (README.md, "Pack throughput")
void FunctionLifter::FnInitRegisters() {
    const auto read = [](llvm::AllocaInst* a) {
        for (const llvm::User* u : a->users()) {
            const auto* s = llvm::dyn_cast<llvm::StoreInst>(u);
            if (!s || s->getValueOperand() == a) return true;  // a load, or any use but a store into it
        }
        return false;
    };
    B.SetInsertPoint(init_after->getNextNode());
    B.SetCurrentDebugLocation(plan->debug ? llvm::DebugLoc(llvm::DILocation::get(C, static_cast<unsigned>(plan->entry), 0, plan->debug))
                                          : llvm::DebugLoc());
    const Convention& conv = plan->conv;
    // The correctness mode takes everything from the block, which the caller filled; so does a part of a split function (its arguments are
    // the function's entry SP and lr and the block it starts at, FunctionPlan::part_entries)
    const bool args = !plan->correctness && plan->part_entries.empty();
    for (unsigned i = 0; i < 31; ++i) {
        if (!read(x[i])) continue;
        const int a = conv.ArgX(plan->self.pass, i);
        Value* v = nullptr;
        if (i == 30 && args) {
            v = entry_lr;
        } else if (args && a >= 0 && (plan->self.pass.x >> i & 1)) {
            v = F->getArg(static_cast<unsigned>(a));
        } else {
            v = LoadField(B.getInt64Ty(), offsetof(dm_guest_state, x) + 8 * i);
        }
        B.CreateStore(v, x[i]);
    }
    for (unsigned i = 0; i < 32; ++i) {
        if (!read(vreg[i])) continue;
        const int a = conv.ArgV(plan->self.pass, i);
        Value* v = nullptr;
        if (args && a >= 0 && (plan->self.pass.v >> i & 1)) {
            v = F->getArg(static_cast<unsigned>(a));
        } else {
            v = LoadField(V128(), offsetof(dm_guest_state, v) + 16 * i);
        }
        B.CreateStore(v, vreg[i]);
    }
    if (read(sp)) {
        B.CreateStore(args ? static_cast<Value*>(F->getArg(1)) : LoadField(B.getInt64Ty(), offsetof(dm_guest_state, sp)), sp);
    }
    if (read(flag[0]) || read(flag[1]) || read(flag[2]) || read(flag[3])) {
        const Flags f = UnpackNzcv(LoadField(B.getInt32Ty(), offsetof(dm_guest_state, nzcv)));
        const Value* values[4] = {f.n, f.z, f.c, f.v};
        for (size_t i = 0; i < 4; ++i) {
            if (read(flag[i])) B.CreateStore(const_cast<Value*>(values[i]), flag[i]);
        }
    }
    if (read(check_bit)) B.CreateStore(B.getFalse(), check_bit);
    if (read(qc)) B.CreateStore(B.getFalse(), qc);
    if (read(fpsr_bits)) B.CreateStore(B.getInt32(0), fpsr_bits);
}

// ---- Safepoints (section 6.2) ----

void FunctionLifter::FnPoll(Value* pc, const Regs& dirty, PollKind kind, uint64_t site, uint64_t target) {
    if (!plan->safepoints || !plan->rt_safepoint) {
        return;
    }
    switch (kind) {
    case PollKind::Entry: ++stats->polls_entry; break;
    case PollKind::Back: ++stats->polls_back; break;
    case PollKind::Table: ++stats->polls_table; break;
    case PollKind::Split: ++stats->polls_split; break;
    }
    // One compare and branch: the flag is read as a monotonic atomic, so that LLVM neither hoists it out of a loop nor merges two reads (the
    // requester writes it from another thread); on x86 it is a plain cmpb
    auto* flag = B.CreateAlignedLoad(B.getInt8Ty(), Field(offsetof(dm_guest_state, poll)), llvm::Align(1), "poll");
    flag->setAtomic(llvm::AtomicOrdering::Monotonic);
    auto* cold = BasicBlock::Create(C, "safepoint", F);
    auto* cont = BasicBlock::Create(C, "polled", F);
    llvm::MDBuilder md{C};
    auto* br = B.CreateCondBr(B.CreateICmpNE(flag, B.getInt8(0)), cold, cont, md.createBranchWeights(1, 1u << 20));
    polls.push_back({kind, site, target, flag, br, cold, cont});
    B.SetInsertPoint(cold);
    // The registers newer than their block copy, then pc: the block holds the whole register file at a safepoint (a paused thread's context)
    StoreRegs(plan->correctness ? Regs::All() : dirty);
    FlushFpsr();
    StoreField(pc, offsetof(dm_guest_state, pc));
    auto* call = B.CreateCall(plan->rt_safepoint, {st});
    call->setCallingConv(plan->rt_safepoint->getCallingConv());
    B.CreateBr(cont);
    B.SetInsertPoint(cont);
}

// ---- The polls a loop bounded by a constant does not need (section 6.2) ----
//
// What 6.2 needs is a bound on how long a thread runs between two safepoints, not a poll on every backward branch. The polls bound it so: every
// cycle of a function's blocks holds a branch to a block at or before its own start (FnGoto; addresses cannot only grow around a cycle) and
// every such branch polls, every cycle of the call graph passes a function entry that polls (FnPrologue), and a stretch without a poll is
// therefore a path through each frame's code that repeats no block. This pass leaves out the poll of a backward branch that goes back to the
// head h of a loop L (LLVM's natural loops) that has no inner loop, no call but intrinsics and the run time's floating-point and AES helpers
// (no guest code, no host call, no gate), and that ScalarEvolution bounds by a constant: the header runs at most N = (constant maximum
// backedge-taken count + 1) times per entry into L, and N x P <= kPollBudget, P the longest path from h over L's blocks in operations (cold
// paths left out: they poll; a helper call counts kHelperCost; the sum of L's blocks when a cycle avoids h). Every pass through h but the first
// comes over a back edge, and between two passes through h no block repeats without a poll (a cycle inside L that avoids h is no natural
// loop's, so it holds a backward branch that does not go to h, which keeps its poll), so one entry into L runs at most kPollBudget operations
// without a poll. A cycle through the branch that leaves L would have to enter L again at h, which it passes once: so every cycle through the
// branch is inside L, and every cycle that leaves L holds a backward branch outside this rule (an innermost loop that contains h is L itself),
// which polls: a stretch without a poll enters each such loop at most once, and grows by at most kPollBudget operations at each, as
// straight-line code of that length would. The decision is made on a copy of the function after mem2reg (registers in SSA), InstCombine (a
// 32-bit counter kept in a 64-bit register becomes a narrow induction variable) and LoopSimplify (a preheader for every loop, and its back edges
// joined in one empty block, so that ScalarEvolution counts the passes through h and the values L is entered with are its own): transformations
// that keep the function's meaning and only split or join edges with empty blocks, so a run of the copy is a run of the function with the same
// polls and a bound on the copy's runs between polls is one on the function's. The function itself then loses the poll (its compare, branch and
// cold path), before the tier's passes, which then see the loop without it.
// The budget: 4,096 operations of the lifted IR (about as many x86 instructions; 1 to 3 per guest instruction: a few thousand guest
// instructions, about a microsecond at one instruction per nanosecond, a few hundred microseconds if every one were a cache miss). The
// requester of a pause (hle_thread.c SetThreadActivity) waits on a condition variable re-checked every 100 us and warns after a second; its own
// wake-up of the paused thread costs microseconds, and a stretch of straight-line code or a chain of leaf calls is already that long.
// Jump-table polls, transfers between parts and the function's entry poll stay. At every poll left, its cold path is as it was: the block holds
// the whole register file there. The spin gates and observation gates are calls (rt_gate): their loops keep their polls. A loop whose bound is
// a register, whose counter changes on some paths only, or that has a call keeps its poll (no constant bound, or the call). src/build asks for
// it at tiers H and H+ only (FunctionPlan::elide_polls; README.md, "Threads and safepoints": at W, measured, it made programs slower as well
// as faster)
namespace {

constexpr uint64_t kPollBudget = 4096;
constexpr uint64_t kHelperCost = 256;

// A call a loop without a poll may make: one that returns in bounded time and runs no guest code (or a trap, which does not return)
std::optional<uint64_t> BoundedCallCost(const llvm::CallBase& call, const FunctionPlan& plan) {
    const llvm::Function* f = call.getCalledFunction();
    if (!f) {
        return std::nullopt;
    }
    if (f->isIntrinsic()) {
        return 1;
    }
    if (f == plan.rt_trap) {
        return 1;
    }
    const llvm::StringRef n = f->getName();
    if (n.starts_with("nxaot_rt_fp_") || n == "nxaot_rt_aes" || n.starts_with("nxaot.nan")) {
        return kHelperCost;
    }
    return std::nullopt;
}

}  // namespace

void FunctionLifter::FnElidePolls() {
    static const bool log = std::getenv("NXAOT_POLL_LOG") != nullptr;  // a line per backward branch's poll: left out or kept, and why
    std::vector<size_t> back;
    for (size_t i = 0; i < polls.size(); ++i) {
        if (polls[i].kind == PollKind::Back) back.push_back(i);
    }
    if (back.empty() || !plan->elide_polls) {
        return;
    }
    std::vector<std::string> verdict(polls.size());
    std::vector<bool> elide(polls.size(), false);
    // First on the function itself (its control flow is the copy's): the loops that cannot qualify (no natural loop, inner loops, a call:
    // LoopSimplify only adds nesting and keeps the calls), so that a function with no other candidate is not copied
    const auto loop_call = [this](const llvm::Loop* loop, const std::set<const BasicBlock*>& colds) -> std::string {
        for (const BasicBlock* b : loop->blocks()) {
            if (colds.count(b)) continue;
            for (const llvm::Instruction& inst : *b) {
                const auto* cb = llvm::dyn_cast<llvm::CallBase>(&inst);
                if (cb && !BoundedCallCost(*cb, *plan)) {
                    const llvm::Function* f = cb->getCalledFunction();
                    return f ? f->getName().str() : "an indirect target";
                }
            }
        }
        return {};
    };
    std::vector<size_t> pending;
    {
        llvm::DominatorTree dt{*F};
        llvm::LoopInfo li{dt};
        std::set<const BasicBlock*> colds;
        for (const PollSite& p : polls) colds.insert(p.cold);
        for (const size_t i : back) {
            const llvm::Loop* loop = li.getLoopFor(polls[i].br->getParent());
            if (!loop) {
                verdict[i] = "kept: in no natural loop";
            } else if (!loop->getSubLoops().empty()) {
                verdict[i] = "kept: its loop has inner loops";
            } else if (const std::string call = loop_call(loop, colds); !call.empty()) {
                verdict[i] = "kept: its loop calls " + call;
            } else {
                pending.push_back(i);
            }
        }
    }
    // The copy, for the loops left
    if (!pending.empty()) {
        llvm::ValueToValueMapTy vmap;
        llvm::Function* copy = llvm::CloneFunction(F, vmap);
        llvm::stripDebugInfo(*copy);  // which would name F's subprogram twice
        const auto in_copy = [&](BasicBlock* b) { return llvm::dyn_cast_or_null<BasicBlock>(static_cast<Value*>(vmap.lookup(b))); };
        llvm::PassBuilder pb;
        llvm::LoopAnalysisManager lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager cgam;
        llvm::ModuleAnalysisManager mam;
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        llvm::FunctionPassManager fpm;
        fpm.addPass(llvm::PromotePass());
        fpm.addPass(llvm::InstCombinePass());
        fpm.addPass(llvm::LoopSimplifyPass());
        fpm.run(*copy, fam);
        llvm::LoopInfo& li = fam.getResult<llvm::LoopAnalysis>(*copy);
        llvm::ScalarEvolution& se = fam.getResult<llvm::ScalarEvolutionAnalysis>(*copy);
        std::set<const BasicBlock*> colds;  // every poll's cold path, in the copy
        for (const PollSite& p : polls) {
            if (BasicBlock* c = in_copy(p.cold)) colds.insert(c);
        }
        for (const size_t i : pending) {
            const PollSite& p = polls[i];
            BasicBlock* at = in_copy(p.br->getParent());
            BasicBlock* cont = in_copy(p.cont);
            if (!at || !cont) {
                verdict[i] = "kept: its blocks were folded in the copy";
                continue;
            }
            const llvm::Loop* loop = li.getLoopFor(at);
            if (!loop) {
                verdict[i] = "kept: in no natural loop";
                continue;
            }
            if (!loop->getSubLoops().empty()) {
                verdict[i] = "kept: its loop has inner loops";
                continue;
            }
            // The branch goes back to the loop's head: straight there, or through the block LoopSimplify joined the back edges in
            const BasicBlock* head = loop->getHeader();
            const BasicBlock* latch = loop->getLoopLatch();
            const BasicBlock* to = cont->getSingleSuccessor();
            const bool joined = to && to == latch && latch->getSingleSuccessor() == head && latch->getFirstNonPHI() == latch->getTerminator();
            if (!latch || !loop->contains(cont) || (to != head && !joined)) {
                verdict[i] = "kept: not a branch back to its loop's head";
                continue;
            }
            // What one pass through the loop costs at most: the longest path from the head over the loop's blocks (cold paths left out: they
            // poll), with the operations of each block (a helper call kHelperCost); a call of anything else keeps the poll
            std::map<const BasicBlock*, uint64_t> weight;
            std::string call;
            for (const BasicBlock* b : loop->blocks()) {
                if (colds.count(b)) continue;
                uint64_t w = 0;
                for (const llvm::Instruction& inst : *b) {
                    if (llvm::isa<llvm::PHINode>(inst)) continue;
                    uint64_t cost = 1;
                    if (const auto* cb = llvm::dyn_cast<llvm::CallBase>(&inst)) {
                        const auto c = BoundedCallCost(*cb, *plan);
                        if (!c) {
                            const llvm::Function* f = cb->getCalledFunction();
                            call = f ? f->getName().str() : "an indirect target";
                            break;
                        }
                        cost = *c;
                    }
                    w += cost;
                }
                if (!call.empty()) break;
                weight[b] = w;
            }
            if (!call.empty()) {
                verdict[i] = "kept: its loop calls " + call;
                continue;
            }
            uint64_t path = 0;
            {
                // Depth first from the head over the loop's edges but those back to it: postorder, and whether some edge closes a cycle (one
                // that avoids the head polls on it: then each block counts once per pass, the sum of all)
                std::map<const BasicBlock*, int> state;  // 1 on the stack, 2 done
                std::vector<const BasicBlock*> post;
                std::vector<std::pair<const BasicBlock*, llvm::const_succ_iterator>> stack;
                bool acyclic = true;
                state[head] = 1;
                stack.push_back({head, llvm::succ_begin(head)});
                while (!stack.empty()) {
                    auto& [b, it] = stack.back();
                    if (it == llvm::succ_end(b)) {
                        state[b] = 2;
                        post.push_back(b);
                        stack.pop_back();
                        continue;
                    }
                    const BasicBlock* s = *it++;
                    if (s == head || !weight.count(s)) continue;
                    if (state[s] == 1) acyclic = false;
                    if (state[s] == 0) {
                        state[s] = 1;
                        stack.push_back({s, llvm::succ_begin(s)});
                    }
                }
                if (acyclic) {
                    std::map<const BasicBlock*, uint64_t> longest;
                    for (auto b = post.rbegin(); b != post.rend(); ++b) {
                        uint64_t in = 0;
                        for (const BasicBlock* q : llvm::predecessors(*b)) {
                            if (*b != head && longest.count(q)) in = std::max(in, longest[q]);
                        }
                        longest[*b] = in + weight[*b];
                        path = std::max(path, longest[*b]);
                    }
                } else {
                    for (const auto& [b, w] : weight) path += w;
                }
            }
            const auto* k = llvm::dyn_cast<llvm::SCEVConstant>(se.getConstantMaxBackedgeTakenCount(loop));
            if (!k) {
                verdict[i] = "kept: no constant bound on its trip count";
                continue;
            }
            const llvm::APInt& n = k->getAPInt();
            if (n.getActiveBits() > 32) {
                verdict[i] = "kept: at most 2^" + std::to_string(n.getActiveBits()) + " trips";
                continue;
            }
            const uint64_t trips = n.getZExtValue() + 1;
            const std::string bound = std::to_string(trips) + " trips x " + std::to_string(path) + " operations";
            if (trips * path > kPollBudget) {
                verdict[i] = "kept: " + bound + " over the budget of " + std::to_string(kPollBudget);
                continue;
            }
            elide[i] = true;
            verdict[i] = "left out: " + bound;
        }
        fam.clear();
        copy->eraseFromParent();
    }
    for (const size_t i : back) {
        if (log) {
            std::fprintf(stderr, "aot: poll of the branch at %#llx back to %#llx (function %#llx): %s\n", static_cast<unsigned long long>(polls[i].site),
                         static_cast<unsigned long long>(polls[i].target), static_cast<unsigned long long>(plan->entry), verdict[i].c_str());
        }
        if (!elide[i]) continue;
        // The branch to the continuation alone, the compare and the flag's load gone, the cold path deleted (no predecessor is left)
        PollSite& p = polls[i];
        llvm::BranchInst::Create(p.cont, p.br);
        auto* cmp = llvm::dyn_cast<llvm::Instruction>(p.br->getCondition());
        p.br->eraseFromParent();
        p.br = nullptr;
        if (cmp && cmp->use_empty()) {
            cmp->eraseFromParent();
        }
        if (p.flag->use_empty()) {
            auto* addr = llvm::dyn_cast<llvm::Instruction>(p.flag->getPointerOperand());
            p.flag->eraseFromParent();
            p.flag = nullptr;
            if (addr) llvm::RecursivelyDeleteTriviallyDeadInstructions(addr);
        }
        llvm::DeleteDeadBlock(p.cold);
        p.cold = nullptr;
        ++stats->polls_bounded;
    }
}

// ---- The gates of design 1.1: observation gates and spin waits' back-edges ----

decode::StopBefore FunctionLifter::GateStops(uint64_t delta) const {
    if (!plan || plan->obs_gates.empty()) {
        return {};
    }
    const std::set<uint64_t>* obs = &plan->obs_gates;
    return [obs, delta](uint64_t a) { return a >= delta && obs->count(a - delta) > 0; };
}

void FunctionLifter::FnGate(uint64_t site, Value* pc, const Regs& dirty, bool reload) {
    if (!plan->rt_gate) {
        error = "a gate at " + llvm::utohexstr(site) + " without the run time's gate entry";
        return;
    }
    // The block holds the whole register file at a gate, as on Apple the gate frame does (dm_gate saves every register): the observer reads it,
    // and a thread the spin wait's back-off pauses has its context there (GetThreadContext3)
    StoreRegs(plan->correctness ? Regs::All() : dirty);
    FlushFpsr();
    StoreField(pc, offsetof(dm_guest_state, pc));
    auto* call = B.CreateCall(plan->rt_gate, {st, B.getInt64(site)});
    call->setCallingConv(plan->rt_gate->getCallingConv());
    if (reload) {
        ReloadRegs(plan->correctness ? Regs::All() : Regs::X(0x3FFFFFFFu));  // x0..x29: an observer may change them (OBS_STDALLOC_INIT writes x2)
    }
    gates_emitted.insert(site);
}

// ---- The JIT fallback (section 11) ----

void FunctionLifter::FnJitCall(uint64_t target, uint64_t call_pc) {
    // A call to an address that is no compiled entry: the JIT runs it, with this call's return as its stop (jit_entry; st->pc = the target)
    ++stats->traps;
    StoreField(Rel(target), offsetof(dm_guest_state, pc));
    StoreField(B.getInt32(NXAOT_TRAP_JIT_TARGET), offsetof(dm_guest_state, exit_reason));
    FnCall(plan->indirect, plan->conv.Type(C, plan->indirect.pass), plan->jit_entry, call_pc);
    Goto(B.getInt64(cur_end));
}

void FunctionLifter::FnJitTail(Value* target) {
    // A branch to an address that is no compiled entry (into the middle of another function): the JIT from there, whose stop is this function's
    // return: its entry's lr, whatever x30 holds (FnTailLr), the guest's x30 in the block. It was a tail call of jit_entry with x30 as the lr, which
    // took a return to the entry's lr for a non-local transfer when the function had changed x30 (FnTailLr), and returned jit_entry's v0..v3 (rv 0)
    // where this function's callers take its RV from the return value
    FnJitContinue(target, NXAOT_TRAP_JIT_TARGET);
}

void FunctionLifter::FnJitContinue(Value* pc, uint32_t reason) {
    ++stats->traps;
    JitContinue(pc, reason);
}

void FunctionLifter::FnTailLr(Value* target, uint32_t reason) {
    // A callee returns natively when the guest returns to the lr it was given (FnReturn), and a tail call gives it x30; the native frame that
    // receives that return is this function's caller, which waits for this function's entry lr. They are the same in a real tail call (the
    // epilogue restored x30, or nothing changed it: LLVM folds the compare then). With another x30 the b or br is no tail call: a br through a jump
    // table discovery could not bound, after a call of this function changed x30; a branch into a function discovery split off. A native tail
    // call would take the guest's return to this function's entry lr, through its epilogue, for a non-local transfer that no frame takes (each
    // frame resumes only at its own resume points), and the thread's outermost level would run the rest of the thread in the JIT; and a return to
    // that x30 for this function's return. So the JIT runs from the target, with x30 in the block and this function's return as its stop: it calls
    // a compiled entry there natively, which goes on until the guest comes back here (dm/host/aot_run.c jit_loop)
    Value* lr_now = B.CreateLoad(B.getInt64Ty(), x[30], "x30_at_tail");
    auto* tail = BasicBlock::Create(C, "tail_lr", F);
    auto* other = BasicBlock::Create(C, "tail_other_lr", F);
    llvm::MDBuilder md{C};
    B.CreateCondBr(B.CreateICmpEQ(lr_now, entry_lr), tail, other, md.createBranchWeights(1u << 16, 1));
    B.SetInsertPoint(other);
    const Regs dirty = cur_dirty;
    JitContinue(target, reason);
    cur_dirty = dirty;
    B.SetInsertPoint(tail);
}

void FunctionLifter::JitContinue(Value* pc, uint32_t reason) {
    // The rest of this function cannot run as compiled code: every dirty register and pc to the block, the JIT from there until the guest
    // returns where this function returns (its entry's lr and SP), then a native return of what the JIT left in the block (the whole register
    // file; with st->pending set when the guest left non-locally, which the callers propagate)
    StoreRegs(plan->correctness ? Regs::All() : cur_dirty);
    FlushFpsr();
    const auto off = OffsetOf(pc);
    StoreField(off ? Rel(*off) : pc, offsetof(dm_guest_state, pc));
    StoreField(B.getInt32(reason), offsetof(dm_guest_state, exit_reason));
    B.CreateCall(plan->rt_jit, {st, entry_lr, F->getArg(1)});
    ReloadRegs(Regs::X(kRetX) | Regs::V(plan->self.rv));
    B.CreateRet(RetValue());
}

// ---- Calls ----

std::vector<Value*> FunctionLifter::CallArgs(const CalleeView& c, llvm::FunctionType* type, Value* lr) {
    std::vector<Value*> args(type->getNumParams());
    args[0] = st;
    args[1] = B.CreateLoad(B.getInt64Ty(), sp);
    args[2] = lr;
    const Convention& conv = plan->conv;
    for (unsigned i = 0; i < 31; ++i) {
        const int a = conv.ArgX(c.pass, i);
        if (a >= 0) {
            args[static_cast<size_t>(a)] = (c.pass.x >> i & 1) || plan->correctness ? static_cast<Value*>(B.CreateLoad(B.getInt64Ty(), x[i]))
                                                                                     : llvm::PoisonValue::get(B.getInt64Ty());
        }
    }
    for (unsigned i = 0; i < 32; ++i) {
        const int a = conv.ArgV(c.pass, i);
        if (a >= 0) {
            args[static_cast<size_t>(a)] = (c.pass.v >> i & 1) || plan->correctness ? static_cast<Value*>(B.CreateLoad(V128(), vreg[i]))
                                                                                     : llvm::PoisonValue::get(V128());
        }
    }
    return args;
}

llvm::CallInst* FunctionLifter::EmitCall(const CalleeView& c, llvm::FunctionType* type, Value* callee, Value* lr, bool tail) {
    auto* call = B.CreateCall(type, callee, CallArgs(c, type, lr));
    plan->conv.Apply(call);
    if (tail) {
        call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    }
    return call;
}

void FunctionLifter::FnCall(const CalleeView& c, llvm::FunctionType* type, Value* callee, uint64_t call_pc) {
    // Before the call: what the callee does not take in host registers must be current in the block (section 2.3); through the dispatcher also
    // what it takes but the union of the possible callees may change (it is reloaded afterwards; CalleeView::exact). NZCV only where it is live
    // after the call: the callee may read it, or the caller after it (FnDataflow); a dead one is left as a poll leaves it (design 6.2). In a loop
    // of calls the flags of every compare were packed and stored at each call (int_ops-gcc's main: README.md, "Against native")
    Regs stores = plan->correctness ? Regs::All() : CallStores(cur_dirty, c);
    stores.nzcv = stores.nzcv && (plan->correctness || nzcv_live_out.count(cur_start));
    StoreRegs(stores);
    FlushFpsr();  // the QC and FPSR bits the lowering raised, to st->fpsr (a callee or the host may read them)
    Value* frame_sp = B.CreateLoad(B.getInt64Ty(), sp, "frame_sp");
    llvm::CallInst* ret = EmitCall(c, type, callee, B.CreateLoad(B.getInt64Ty(), x[30]), false);
    // After it: one compare and branch on st->pending (section 3.3)
    Value* pending = LoadField(B.getInt64Ty(), offsetof(dm_guest_state, pending));
    auto* cont = BasicBlock::Create(C, "after_" + llvm::utohexstr(call_pc), F);
    auto* pend = BasicBlock::Create(C, "pending_" + llvm::utohexstr(call_pc), F);
    llvm::MDBuilder md{C};
    B.CreateCondBr(B.CreateICmpNE(pending, B.getInt64(0)), pend, cont, md.createBranchWeights(1, 1u << 16));
    B.SetInsertPoint(pend);
    if (plan->resume) {
        // A resume point takes the transfer only with this frame's SP and under the FPCR the code is specialised for (section 7.1): the code
        // that threw or longjmp'd may have left another rounding mode or FZ in st->fpcr, and this frame's code would run on under the wrong one.
        // Under another FPCR the transfer goes on past this frame, as one for no frame of compiled code does: the host runs it in the JIT,
        // as it runs a return under another FPCR (aot_run.c jit_loop; README.md, "Resume points and the FPCR")
        Value* same_fpcr = nullptr;
        const auto here_of = [&](uint64_t r) {
            if (!same_fpcr) {
                same_fpcr = B.CreateICmpEQ(B.CreateAnd(LoadField(B.getInt32Ty(), offsetof(dm_guest_state, fpcr)), B.getInt32(kFpcrModeMask)),
                                           B.getInt32(fpcr), "same_fpcr");
            }
            return B.CreateAnd(B.CreateAnd(B.CreateICmpEQ(pending, Rel(r)),
                                           B.CreateICmpEQ(LoadField(B.getInt64Ty(), offsetof(dm_guest_state, sp)), frame_sp)),
                               same_fpcr);
        };
        for (const uint64_t r : plan->resume(call_pc)) {
            const auto it = bbs.find(r);
            llvm::Function* other_part = it == bbs.end() && plan->part_of ? plan->part_of(r) : nullptr;
            if (it == bbs.end() && !other_part) {
                continue;
            }
            if (other_part) {
                // A resume point in another part of a split function: the block holds the whole register file, the part takes it from there
                auto* take = BasicBlock::Create(C, "resume_" + llvm::utohexstr(r), F);
                auto* other = BasicBlock::Create(C, "pending_next", F);
                B.CreateCondBr(here_of(r), take, other);
                B.SetInsertPoint(take);
                StoreField(B.getInt64(0), offsetof(dm_guest_state, pending));
                FnSplit(r, false);
                B.SetInsertPoint(other);
                continue;
            }
            // A resume point of this call site, entered with this frame's SP: the frame takes the transfer, with every register from the block
            auto* take = BasicBlock::Create(C, "resume_" + llvm::utohexstr(r), F);
            auto* other = BasicBlock::Create(C, "pending_next", F);
            B.CreateCondBr(here_of(r), take, other);
            B.SetInsertPoint(take);
            StoreField(B.getInt64(0), offsetof(dm_guest_state, pending));
            ReloadRegs(Regs::All());
            B.CreateBr(it->second);
            B.SetInsertPoint(other);
        }
    }
    // Not for this frame: return at once, writing nothing; the callers propagate it to the frame it belongs to, or to the host (section 3.3)
    B.CreateRet(llvm::PoisonValue::get(plan->conv.RetType(C)));
    B.SetInsertPoint(cont);
    if (plan->correctness) {
        ReloadRegs(Regs::All());
        cur_dirty = {};
        return;
    }
    B.CreateStore(B.CreateExtractValue(ret, 0), x[0]);
    B.CreateStore(B.CreateExtractValue(ret, 1), x[1]);
    for (unsigned i = 0; i < 4; ++i) {
        if (c.rv >> i & 1) {
            B.CreateStore(B.CreateExtractValue(ret, 2 + i), vreg[i]);
        }
    }
    ReloadRegs(c.writes - Regs::X(kRetX) - Regs::V(c.rv));
    // What is newer than the block from here on (FnDataflow's after_call): what a transfer that follows the call in this block stores (FnSplit
    // into another part, a tail call into the next function). The block's end state was before the call, without x0, x1 and RV
    cur_dirty = ((cur_dirty & (c.pass | kSpLr)) - c.writes) | Regs::X(kRetX) | Regs::V(c.rv);
}

void FunctionLifter::FnCallDirect(uint64_t target, uint64_t call_pc) {
    const CalleeView* c = plan->callee(target);
    if (!c) {
        FnJitCall(target, call_pc);  // discovery makes every bl target an entry; this is a bl into something else: the JIT's (section 11)
        return;
    }
    ++stats->calls;
    FnCall(*c, c->fn->getFunctionType(), c->fn, call_pc);
    if (c->returns) {
        Goto(B.getInt64(cur_end));
    } else {
        FnTrap(B.getInt64(call_pc), NXAOT_TRAP_NORETURN);
    }
}

void FunctionLifter::FnCallIndirect(Value* target, uint64_t call_pc) {
    ++stats->indirect_calls;
    if (const CalleeView* g = Guarded(call_pc, true)) {
        FnCallGuarded(target, call_pc, plan->guarded.at(call_pc), *g);
        return;
    }
    Value* fn = FnDispatch(target, call_pc);
    FnCall(plan->indirect, plan->conv.Type(C, plan->indirect.pass), fn, call_pc);
    Goto(B.getInt64(cur_end));
}

// ---- Guarded indirect sites (design 12's hot indirect targets: FunctionPlan::guarded; README.md, "Hot indirect targets") ----

const CalleeView* FunctionLifter::Guarded(uint64_t site, bool call) const {
    if (!plan || plan->guarded.empty() || !plan->callee) {
        return nullptr;
    }
    const auto it = plan->guarded.find(site);
    if (it == plan->guarded.end()) {
        return nullptr;
    }
    const CalleeView* c = plan->callee(it->second);
    return c && (!call || c->returns) ? c : nullptr;
}

// A blr whose profile names a dominant target `to`: the computed address against the target's (base + to), on a match a direct call of the target's body as bl makes
// it (its own signature: what it takes in host registers, its exact writes reloaded after it, its own return registers; a real call that LLVM may inline when the
// target is in this unit), anything else the inline cache's call as before. Both paths join before the call's block goes on, with the dirty set the indirect call
// leaves (FnDataflow's after_call over plan->indirect: the successors' dirty sets were computed with it); the direct path first stores what it keeps newer than the
// block beyond that set (a register the target does not write but the union of indirect targets may: the indirect path reloads it, the direct path keeps it in SSA),
// which FnDataflow adds to what the function may leave in the block (restore_block). The pending checks, resume points and return address are the call site's on
// both paths, so the return table and the non-local transfers (section 3.3) are unchanged; the guard's branch carries the inline cache's own weights (2^16 : 1)
void FunctionLifter::FnCallGuarded(Value* target, uint64_t call_pc, uint64_t to, const CalleeView& c) {
    guarded_sites.insert(call_pc);
    const CalleeView& ind = plan->indirect;
    const Regs before = cur_dirty;
    const Regs after = ((before & (ind.pass | kSpLr)) - ind.writes) | Regs::X(kRetX) | Regs::V(ind.rv);
    const std::string at = llvm::utohexstr(call_pc);
    auto* hit = BasicBlock::Create(C, "guard_hit_" + at + "_", F);
    auto* miss = BasicBlock::Create(C, "guard_miss_" + at + "_", F);
    auto* join = BasicBlock::Create(C, "guard_join_" + at + "_", F);
    llvm::MDBuilder md{C};
    B.CreateCondBr(B.CreateICmpEQ(target, Rel(to)), hit, miss, md.createBranchWeights(1u << 16, 1));
    B.SetInsertPoint(hit);
    FnCall(c, c.fn->getFunctionType(), c.fn, call_pc);
    if (!plan->correctness) {
        StoreRegs(cur_dirty - after);
    }
    B.CreateBr(join);
    B.SetInsertPoint(miss);
    cur_dirty = before;
    Value* fn = FnDispatch(target, call_pc);
    FnCall(ind, plan->conv.Type(C, ind.pass), fn, call_pc);
    B.CreateBr(join);
    B.SetInsertPoint(join);
    Goto(B.getInt64(cur_end));
}

void FunctionLifter::FnTail(const CalleeView& c, llvm::FunctionType* type, Value* callee) {
    // A guest tail call is a real tail call (section 2.3). What the callee takes in host registers and leaves alone must still reach this
    // function's callers through the block when they reload it (what they may read and this function may change)
    Regs store = Regs::All();
    if (!plan->correctness) {
        // Through the dispatcher the callee's own writes are unknown (CalleeView::exact): it may store none of what it is passed
        store = (cur_dirty - c.pass - kSpLr) |
                (((cur_dirty & (c.pass | kSpLr) & plan->live_after & plan->self.writes) - (c.exact ? c.writes : Regs{})) - Regs::X(kRetX) -
                 Regs::V(c.rv)) |
                restore_block;
    }
    StoreRegs(store);
    FlushFpsr();  // the QC and FPSR bits the lowering raised, to st->fpsr (a callee or the host may read them)
    llvm::CallInst* call = EmitCall(c, type, callee, B.CreateLoad(B.getInt64Ty(), x[30]), true);
    B.CreateRet(call);
}

void FunctionLifter::FnTailDirect(uint64_t target) {
    const CalleeView* c = plan->callee(target);
    if (!c) {
        FnJitTail(Rel(target));
        return;
    }
    ++stats->tail_calls;
    // On another x30 the JIT enters the target by its address: a miss there (a function with no standard entry) asks for one, as the
    // dispatcher's does (indirect)
    FnTailLr(Rel(target), NXAOT_TRAP_JIT_MISS);
    FnTail(*c, c->fn->getFunctionType(), c->fn);
}

void FunctionLifter::FnTailIndirect(Value* target, uint64_t site) {
    ++stats->indirect_tails;
    FnTailLr(target, NXAOT_TRAP_JIT_MISS);  // before the inline cache: on another x30 the JIT looks the target up itself
    if (const CalleeView* g = Guarded(site, false)) {
        // A guarded tail call (FunctionPlan::guarded, design 12's hot indirect targets): the computed address against the dominant target's, on a match a tail call of
        // the target's body as b makes it (FnTailDirect: musttail between the convention's prototypes, which FnTailDirect already relies on), else the inline cache's
        // tail call below. Neither path comes back, so nothing joins
        guarded_sites.insert(site);
        const std::string at = llvm::utohexstr(site);
        auto* hit = BasicBlock::Create(C, "guard_hit_" + at + "_", F);
        auto* miss = BasicBlock::Create(C, "guard_miss_" + at + "_", F);
        llvm::MDBuilder md{C};
        B.CreateCondBr(B.CreateICmpEQ(target, Rel(plan->guarded.at(site))), hit, miss, md.createBranchWeights(1u << 16, 1));
        B.SetInsertPoint(hit);
        FnTail(*g, g->fn->getFunctionType(), g->fn);
        B.SetInsertPoint(miss);
    }
    Value* fn = FnDispatch(target, site);
    FnTail(plan->indirect, plan->conv.Type(C, plan->indirect.pass), fn);
}

// A transfer to a block of another part of a split function (design 9.3: "joined by tail calls through the state block";
// FunctionPlan::part_entries): the whole register file to the block (unless the block already holds it: store false, a resume point's path), a
// safepoint poll when it goes backwards (a loop through parts, section 6.2), then a tail call of that part with the function's entry SP and lr
// and the block's offset. The part returns what the function returns.
// The whole file, not the registers newer than the block: a register this part holds unchanged need not be current in the block. SP and lr
// travel in host registers at every call, so after a callee returns the block's copies are the last ones an inner frame stored
// (include/nxaot/aot_module.h, NXAOT_LOOKUP_SP_KEPT); and a callee's safepoint poll, gate or JIT continuation stores its own values of
// registers it restores before it returns, which only its inner calls and svcs make it store again (FnDataflow, restore_block). The function
// itself holds the right values in SSA; the next part reads the block. Found by CTest split: fnptr-gcc's main resumed in its last part with the
// SP of a callee's frame and returned to a stale lr
void FunctionLifter::FnSplit(uint64_t target, bool store) {
    llvm::Function* part = plan->part_of(target);
    ++stats->splits;
    if (store) {
        StoreRegs(Regs::All());
        FlushFpsr();
    }
    if (target <= cur_start) {
        FnPoll(Rel(target), Regs{}, PollKind::Split);
    }
    llvm::FunctionType* type = part->getFunctionType();
    std::vector<Value*> args(type->getNumParams());
    for (unsigned i = 0; i < type->getNumParams(); ++i) {
        args[i] = llvm::PoisonValue::get(type->getParamType(i));
    }
    args[0] = st;
    args[1] = F->getArg(1);
    args[2] = entry_lr;
    args[static_cast<size_t>(plan->conv.ArgX(Regs{}, 0))] = B.getInt64(target);
    auto* call = B.CreateCall(type, part, args);
    plan->conv.Apply(call);
    call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    B.CreateRet(call);
}

// The inline cache of section 4: a slot per site that points at an immutable {guest, native} descriptor. A hit is two loads and a compare (and
// the load of the native entry beside the compare); a miss calls the dispatcher through a preserve_all wrapper, which looks the target up in the
// module's table and replaces the slot's descriptor
Value* FunctionLifter::FnDispatch(Value* target, uint64_t site) {
    // One slot per guest call site: a site that two lifted blocks hold (a block that runs into a loop's head holds the loop's first instructions too, and the head's own
    // block holds them again) shares its cache, so that what one copy learnt the other does not miss again
    const std::string name = plan->ic_prefix + llvm::utohexstr(site);
    llvm::GlobalVariable* slot = M.getNamedGlobal(name);
    if (!slot) {
        slot = new llvm::GlobalVariable(M, B.getPtrTy(), false, llvm::GlobalValue::InternalLinkage, plan->ic_empty, name);
        slot->setAlignment(llvm::Align(8));
    }
    auto* desc = B.CreateAlignedLoad(B.getPtrTy(), slot, llvm::Align(8), "ic");
    desc->setAtomic(llvm::AtomicOrdering::Unordered);
    Value* guest = B.CreateAlignedLoad(B.getInt64Ty(), desc, llvm::Align(8), "ic_guest");
    Value* hit = B.CreateICmpEQ(guest, target);
    auto* hit_bb = BasicBlock::Create(C, "ic_hit", F);
    auto* miss_bb = BasicBlock::Create(C, "ic_miss", F);
    auto* join = BasicBlock::Create(C, "ic_join", F);
    llvm::MDBuilder md{C};
    B.CreateCondBr(hit, hit_bb, miss_bb, md.createBranchWeights(1u << 16, 1));
    B.SetInsertPoint(hit_bb);
    Value* native_hit = B.CreateAlignedLoad(B.getPtrTy(), B.CreateConstInBoundsGEP1_64(B.getInt8Ty(), desc, 8), llvm::Align(8), "ic_native");
    B.CreateBr(join);
    B.SetInsertPoint(miss_bb);
    auto* miss = B.CreateCall(plan->rt_dispatch, {st, target, slot, B.getInt64(site)});
    miss->setCallingConv(plan->rt_dispatch->getCallingConv());
    B.CreateBr(join);
    B.SetInsertPoint(join);
    auto* native = B.CreatePHI(B.getPtrTy(), 2, "native");
    native->addIncoming(native_hit, hit_bb);
    native->addIncoming(miss, miss_bb);
    return native;
}

// ---- Returns ----

void FunctionLifter::FnReturn(Value* target) {
    ++stats->returns;
    // Section 3.3: a native return when the guest returns where the native frame expects (its entry's lr; folded away where x30 is never
    // written), else a non-local transfer: the whole register file to the block, the target in st->pending, and a native return that the
    // frames above propagate
    auto* ret_bb = BasicBlock::Create(C, "ret", F);
    auto* away = BasicBlock::Create(C, "ret_nonlocal", F);
    llvm::MDBuilder md{C};
    B.CreateCondBr(B.CreateICmpEQ(target, entry_lr), ret_bb, away, md.createBranchWeights(1u << 16, 1));
    B.SetInsertPoint(ret_bb);
    StoreRegs(plan->correctness ? Regs::All()
                                : ((cur_dirty & plan->live_after & plan->self.writes) - Regs::X(kRetX) - Regs::V(plan->self.rv)) | restore_block);
    FlushFpsr();  // the QC and FPSR bits the lowering raised, to st->fpsr (a callee or the host may read them)
    B.CreateRet(RetValue());
    B.SetInsertPoint(away);
    StoreRegs(plan->correctness ? Regs::All() : cur_dirty);
    FlushFpsr();  // the QC and FPSR bits the lowering raised, to st->fpsr (a callee or the host may read them)
    StoreField(target, offsetof(dm_guest_state, pending));
    StoreField(target, offsetof(dm_guest_state, pc));
    B.CreateRet(llvm::PoisonValue::get(plan->conv.RetType(C)));
}

// ---- Host calls and traps ----

void FunctionLifter::FnSvc(Value* number, Value* next_pc) {
    ++stats->svcs;
    // Section 2.4: the whole register file and pc to the block, the host's SVC dispatcher, then x0..x8 back (its outputs)
    StoreRegs(plan->correctness ? Regs::All() : cur_dirty);
    FlushFpsr();  // the QC and FPSR bits the lowering raised, to st->fpsr (a callee or the host may read them)
    StoreField(next_pc, offsetof(dm_guest_state, pc));
    B.CreateCall(plan->rt_svc, {st, number});
    ReloadRegs(plan->correctness ? Regs::All() : Regs::X(0x1FFu));
    cur_dirty = {};
}

void FunctionLifter::FnTrap(Value* pc, uint32_t reason) {
    ++stats->traps;
    StoreRegs(plan->correctness ? Regs::All() : cur_dirty);
    FlushFpsr();  // the QC and FPSR bits the lowering raised, to st->fpsr (a callee or the host may read them)
    const auto off = OffsetOf(pc);
    Value* offset = off ? static_cast<Value*>(B.getInt64(*off)) : B.CreateSub(pc, base);
    StoreField(off ? Rel(*off) : pc, offsetof(dm_guest_state, pc));
    auto* call = B.CreateCall(plan->rt_trap, {st, offset, B.getInt32(reason)});
    call->setDoesNotReturn();
    B.CreateUnreachable();
}

void FunctionLifter::FnExit(Value* pc, uint32_t reason) {
    const auto off = OffsetOf(pc);
    if (reason == DM_EXIT_NO_CODE && off && plan->part_of && plan->part_of(*off)) {
        FnSplit(*off, true);  // the code runs into the next part of a split function
        return;
    }
    if (reason == DM_EXIT_NO_CODE && off && plan->callee(*off)) {
        FnTailDirect(*off);  // the code runs into the next function: a tail call to it
        return;
    }
    switch (reason) {
    case DM_EXIT_NONE:         // msr fpcr left the specialisation (section 7.1): the JIT runs the rest under the new FPCR
    case DM_EXIT_NO_CODE:      // the code runs into what is no function's entry
    case DM_EXIT_UNTRANSLATED: // an encoding dynarmic's frontend leaves to an interpreter
        FnJitContinue(pc, reason == DM_EXIT_NONE ? NXAOT_TRAP_JIT_FPCR : reason);
        return;
    default:                   // brk, undefined and unpredictable encodings: traps
        FnTrap(pc, reason);
        return;
    }
}

// ---- Terminals ----

void FunctionLifter::FnLowerTerminal(const IR::Terminal& t) {
    const End& e = ends[cur_start];
    switch (e.kind) {
    case EndKind::Bl:
        FnCallDirect(e.target, cur_end - 4);
        return;
    case EndKind::Blr:
    case EndKind::Ret:
    case EndKind::Br:
        if (!last_pc) {
            error = "an indirect branch without its A64SetPC at " + llvm::utohexstr(cur_end - 4);
            return;
        }
        if (e.kind == EndKind::Blr) {
            FnCallIndirect(last_pc, cur_end - 4);
        } else if (e.kind == EndKind::Ret) {
            FnReturn(last_pc);
        } else if (const auto bound = plan->bound ? plan->bound(cur_end - 4) : std::nullopt) {
            FnGoto(Rel(*bound));  // a PLT stub the loader binds to this module's function: b to it (the pack's nxaot_bindings holds the binding)
        } else {
            FnGoto(last_pc);
        }
        return;
    case EndKind::Other:
        LowerTerminal(t);
        return;
    }
}

void FunctionLifter::FnGoto(Value* target) {
    if (const auto off = OffsetOf(target)) {
        if (cur_end >= 4) {
            // A spin wait's back-edge taken back to its head (design 1.1): the host's back-off, as Apple's copy of the loop passes its spin gate
            // on that edge. Then the edge's safepoint poll, as any backward branch's
            const auto spin = plan->spin_gates.find(cur_end - 4);
            if (spin != plan->spin_gates.end() && spin->second == *off) {
                Regs dirty = cur_dirty;
                dirty.nzcv = dirty.nzcv && nzcv_live.count(*off);
                FnGate(cur_end - 4, Rel(*off), dirty, false);
            }
        }
        const auto it = bbs.find(*off);
        if (it != bbs.end()) {
            if (*off <= cur_start) {
                // A branch to a block at or before this one: every cycle of the function holds at least one such edge (addresses cannot
                // only grow around a cycle), so a poll here bounds the time between polls in any loop (section 6.2)
                Regs dirty = cur_dirty;
                dirty.nzcv = dirty.nzcv && nzcv_live.count(*off);
                FnPoll(Rel(*off), dirty, PollKind::Back, cur_end - 4, *off);
            }
            const auto loop = entry_stores.find(*off);
            if (loop != entry_stores.end()) {
                // An edge into a loop whose iterations call (FnLoopHeads): what the call would store each time round, once here; nothing on
                // the loop's own back edges, whose dirty set is the head's
                StoreRegs((cur_dirty & loop->second) - dirty_in.at(*off));
            }
            B.CreateBr(it->second);
        } else if (plan->part_of && plan->part_of(*off)) {
            FnSplit(*off, true);  // to a block of another part of a split function
        } else if (plan->callee(*off)) {
            FnTailDirect(*off);  // b, b.cond, cbz, tbz to another function (section 2.3)
        } else {
            FnJitTail(target);  // into the middle of another function: the JIT's (section 11)
        }
        return;
    }
    const uint64_t site = cur_end - 4;
    if (cur_table) {
        // A recovered jump table: an LLVM switch over the module offsets of its targets (section 4); another value goes to the dispatcher.
        // A case at or before this block closes a loop through the table: the poll comes before the switch, at the br itself
        if (std::any_of(cur_table->begin(), cur_table->end(), [&](uint64_t t) { return t <= cur_start && bbs.count(t); })) {
            FnPoll(Rel(site), cur_dirty, PollKind::Table);
        }
        auto* other = BasicBlock::Create(C, "table_miss", F);
        auto* sw = B.CreateSwitch(B.CreateSub(target, base), other, static_cast<unsigned>(cur_table->size()));
        std::set<uint64_t> seen;
        for (const uint64_t t : *cur_table) {
            if (!seen.insert(t).second) {
                continue;
            }
            const auto it = bbs.find(t);
            if (it != bbs.end()) {
                sw->addCase(B.getInt64(t), it->second);
            } else if (plan->part_of && plan->part_of(t)) {
                // A case in another part of a split function
                auto* split = BasicBlock::Create(C, "table_part_" + llvm::utohexstr(t), F);
                sw->addCase(B.getInt64(t), split);
                B.SetInsertPoint(split);
                FnSplit(t, true);
            } else if (plan->callee(t)) {
                // A case that is another function's entry: a computed tail call
                auto* tail = BasicBlock::Create(C, "table_tail_" + llvm::utohexstr(t), F);
                sw->addCase(B.getInt64(t), tail);
                B.SetInsertPoint(tail);
                FnTailDirect(t);
            }
        }
        B.SetInsertPoint(other);
    }
    FnTailIndirect(target, site);  // br through a register, ending the function: an indirect tail call through the inline cache
}

}  // namespace detail

}  // namespace nxaot::lift
