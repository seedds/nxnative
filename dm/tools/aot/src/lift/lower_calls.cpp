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

#include <llvm/ADT/StringExtras.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>

#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"
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
    IR::Block probe = decode::TranslateBlock(pc + kProbeDelta, shifted, fpcr);
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
    // At the entry the passed registers, sp and lr are newer than the block (they came in host registers)
    dirty_in[plan->entry] = plan->self.pass | kSpLr;
    const auto after_call = [&](const Regs& d, const CalleeView& c) {
        // What stays newer than the block after a call: what was passed and not changed, and what the return value brought
        return ((d & (c.pass | kSpLr)) - c.writes) | Regs::X(kRetX) | Regs::V(c.rv);
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [pc, block] : blocks) {
            const Regs at_end = dirty_in[pc] | gen[pc];
            Regs out = at_end;
            const End& e = ends[pc];
            if (svc_blocks.count(pc)) {
                out = {};  // the svc stores every dirty register and reloads x0..x8
            } else if (e.kind == EndKind::Bl) {
                const CalleeView* c = plan->callee(e.target);
                out = c ? after_call(at_end, *c) : Regs{};
            } else if (e.kind == EndKind::Blr) {
                out = after_call(at_end, plan->indirect);
            }
            for (const uint64_t s : succs[pc]) {
                if (!blocks.count(s)) {
                    continue;
                }
                const Regs next = dirty_in[s] | out;
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
    // callee that reads it from the block agree
    Regs stored, written;
    for (auto& [pc, block] : blocks) {
        const Regs at_end = dirty_in[pc] | gen[pc];
        written = written | gen[pc];
        const End& e = ends[pc];
        if (svc_blocks.count(pc)) {
            stored = stored | at_end;
        } else if (e.kind == EndKind::Bl) {
            if (const CalleeView* c = plan->callee(e.target)) {
                stored = stored | CallStores(at_end, *c);
            }
        } else if (e.kind == EndKind::Blr) {
            stored = stored | CallStores(at_end, plan->indirect);
        }
    }
    restore_block = (stored & written) - plan->self.writes;
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
    entry_lr = F->getArg(2);
    entry_lr->setName("lr");
    F->getArg(1)->setName("sp_in");
    const Convention& conv = plan->conv;
    const bool args = !plan->correctness;  // the correctness mode takes everything from the block, which the caller filled
    for (unsigned i = 0; i < 31; ++i) {
        const int a = conv.ArgX(plan->self.pass, i);
        Value* v = nullptr;
        if (i == 30 && args) {
            v = entry_lr;
        } else if (args && a >= 0 && (plan->self.pass.x >> i & 1)) {
            v = F->getArg(static_cast<unsigned>(a));
            v->setName("x" + std::to_string(i) + "_in");
        } else {
            v = LoadField(B.getInt64Ty(), offsetof(dm_guest_state, x) + 8 * i);
        }
        B.CreateStore(v, x[i]);
    }
    for (unsigned i = 0; i < 32; ++i) {
        const int a = conv.ArgV(plan->self.pass, i);
        Value* v = nullptr;
        if (args && a >= 0 && (plan->self.pass.v >> i & 1)) {
            v = F->getArg(static_cast<unsigned>(a));
            v->setName("v" + std::to_string(i) + "_in");
        } else {
            v = LoadField(V128(), offsetof(dm_guest_state, v) + 16 * i);
        }
        B.CreateStore(v, vreg[i]);
    }
    B.CreateStore(args ? static_cast<Value*>(F->getArg(1)) : LoadField(B.getInt64Ty(), offsetof(dm_guest_state, sp)), sp);
    const Flags f = UnpackNzcv(LoadField(B.getInt32Ty(), offsetof(dm_guest_state, nzcv)));
    B.CreateStore(f.n, flag[0]);
    B.CreateStore(f.z, flag[1]);
    B.CreateStore(f.c, flag[2]);
    B.CreateStore(f.v, flag[3]);
    B.CreateStore(B.getFalse(), check_bit);
    B.CreateStore(B.getFalse(), qc);
    B.CreateStore(B.getInt32(0), fpsr_bits);
    B.CreateBr(bbs.at(plan->entry));
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
    // what it takes but the union of the possible callees may change (it is reloaded afterwards; CalleeView::exact)
    StoreRegs(plan->correctness ? Regs::All() : CallStores(cur_dirty, c));
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
        for (const uint64_t r : plan->resume(call_pc)) {
            const auto it = bbs.find(r);
            if (it == bbs.end()) {
                continue;
            }
            // A resume point of this call site, entered with this frame's SP: the frame takes the transfer, with every register from the block
            auto* take = BasicBlock::Create(C, "resume_" + llvm::utohexstr(r), F);
            auto* other = BasicBlock::Create(C, "pending_next", F);
            Value* here = B.CreateAnd(B.CreateICmpEQ(pending, Rel(r)),
                                      B.CreateICmpEQ(LoadField(B.getInt64Ty(), offsetof(dm_guest_state, sp)), frame_sp));
            B.CreateCondBr(here, take, other);
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
}

void FunctionLifter::FnCallDirect(uint64_t target, uint64_t call_pc) {
    const CalleeView* c = plan->callee(target);
    if (!c) {
        FnTrap(Rel(target), NXAOT_TRAP_JIT_TARGET);  // discovery makes every bl target an entry; this is a bl into something else
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
    Value* fn = FnDispatch(target, call_pc);
    FnCall(plan->indirect, plan->conv.Type(C, plan->indirect.pass), fn, call_pc);
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
        FnTrap(Rel(target), NXAOT_TRAP_JIT_TARGET);
        return;
    }
    ++stats->tail_calls;
    FnTail(*c, c->fn->getFunctionType(), c->fn);
}

void FunctionLifter::FnTailIndirect(Value* target, uint64_t site) {
    ++stats->indirect_tails;
    Value* fn = FnDispatch(target, site);
    FnTail(plan->indirect, plan->conv.Type(C, plan->indirect.pass), fn);
}

// The inline cache of section 4: a slot per site that points at an immutable {guest, native} descriptor. A hit is two loads and a compare (and
// the load of the native entry beside the compare); a miss calls the dispatcher through a preserve_all wrapper, which looks the target up in the
// module's table and replaces the slot's descriptor
Value* FunctionLifter::FnDispatch(Value* target, uint64_t site) {
    auto* slot = new llvm::GlobalVariable(M, B.getPtrTy(), false, llvm::GlobalValue::InternalLinkage, plan->ic_empty,
                                          plan->ic_prefix + llvm::utohexstr(site));
    slot->setAlignment(llvm::Align(8));
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
    if (reason == DM_EXIT_NO_CODE && off && plan->callee(*off)) {
        FnTailDirect(*off);  // the code runs into the next function: a tail call to it
        return;
    }
    FnTrap(pc, reason == DM_EXIT_NONE ? NXAOT_TRAP_JIT_FPCR : reason);
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
        const auto it = bbs.find(*off);
        if (it != bbs.end()) {
            B.CreateBr(it->second);
        } else if (plan->callee(*off)) {
            FnTailDirect(*off);  // b, b.cond, cbz, tbz to another function (section 2.3)
        } else {
            FnTrap(target, NXAOT_TRAP_JIT_TARGET);  // into the middle of another function: the JIT's (section 11)
        }
        return;
    }
    const uint64_t site = cur_end - 4;
    if (cur_table) {
        // A recovered jump table: an LLVM switch over the module offsets of its targets (section 4); another value goes to the dispatcher
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
