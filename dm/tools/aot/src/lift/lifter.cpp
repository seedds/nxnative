#include "lift/lifter.h"

#include <array>
#include <cstddef>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>

#include <llvm/ADT/StringExtras.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsX86.h>
#include <llvm/IR/Module.h>

#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/ir/acc_type.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/cond.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/opt/passes.h"
#include "dynarmic/ir/terminal.h"
#include "dynarmic/ir/type.h"
#include "lift/opcodes.h"
#include "nxaot/guest_state.h"
#include "nxaot_lock.h"

namespace nxaot::lift {

namespace {

// The LLVM the lifter is compiled against must be the one nxaot.lock pins (CMakeLists.txt checks the package at configure time)
constexpr bool SameString(const char* a, const char* b) {
    for (; *a && *a == *b; ++a, ++b) {
    }
    return *a == *b;
}
static_assert(SameString(LLVM_VERSION_STRING, NXAOT_LOCK_LLVM), "LLVM_VERSION_STRING differs from the llvm row of nxaot.lock");

using namespace Dynarmic;
using llvm::BasicBlock;
using llvm::ConstantInt;
using llvm::Value;

// The four flags as i1 values
struct Flags {
    Value* n = nullptr;
    Value* z = nullptr;
    Value* c = nullptr;
    Value* v = nullptr;
};

// What set the flags, when the block shows it: conditions then compare the operands directly (section 9.2) instead of combining flag bits
enum class OriginKind { None, Sub, Logic };
struct Origin {
    OriginKind kind = OriginKind::None;
    Value* a = nullptr;    // Sub: the operands of a - b (carry in 1)
    Value* b = nullptr;
    Value* res = nullptr;  // Sub and Logic: the result
};

class FunctionLifter {
public:
    FunctionLifter(llvm::Module& module, const Region& region, const decode::CodeReader& read)
            : M(module), C(module.getContext()), B(module.getContext()), region(region), read(read) {}

    Lifted Run(const std::string& name);
    std::string Dump();

private:
    // Phase 1: translate the region's blocks and check their opcodes
    void Discover(Lifted& out);
    static void Successors(const IR::Terminal& t, const IR::Block& block, std::vector<uint64_t>& out);
    bool InRegion(uint64_t pc) const { return pc >= region.lo && pc < region.hi; }

    // Phase 2: lowering
    void Prologue();
    void Epilogue();
    void LowerBlock(IR::Block& block);
    void LowerInst(IR::Inst& inst);
    void LowerTerminal(const IR::Terminal& t);
    void Goto(Value* target);
    void Exit(Value* pc, uint32_t reason, Value* arg);
    bool LowerContext(IR::Inst& inst);
    bool LowerFlags(IR::Inst& inst);
    bool LowerInteger(IR::Inst& inst);
    bool LowerMemory(IR::Inst& inst);

    // Values
    Value* Get(const IR::Value& v);
    Value* Arg(const IR::Inst& inst, size_t i) { return Get(inst.GetArg(i)); }
    const Flags& FlagsArg(const IR::Inst& inst, size_t i);
    void Def(const IR::Inst& inst, Value* v) { values[&inst] = v; }
    llvm::Type* TypeOf(IR::Type t);
    Value* Field(size_t offset) { return B.CreateConstInBoundsGEP1_64(B.getInt8Ty(), st, offset); }
    Value* LoadField(llvm::Type* ty, size_t offset) { return B.CreateAlignedLoad(ty, Field(offset), llvm::Align(ty->getPrimitiveSizeInBits() / 8)); }
    void StoreField(Value* v, size_t offset) { B.CreateAlignedStore(v, Field(offset), llvm::Align(v->getType()->getPrimitiveSizeInBits() / 8)); }
    Value* I(unsigned bits, uint64_t v) { return ConstantInt::get(B.getIntNTy(bits), v); }
    Value* SignBit(Value* v) { return B.CreateICmpSLT(v, ConstantInt::get(v->getType(), 0)); }
    static unsigned Bits(Value* v) { return v->getType()->getIntegerBitWidth(); }

    // Flags and conditions
    Flags CurrentFlags();
    void SetFlags(const Flags& f, const Origin& origin);
    Value* Cond(IR::Cond cond);
    Flags FlagsFromOp(const IR::Inst& producer, Origin* origin);
    Value* CarryOf(const IR::Inst& producer);
    Value* OverflowOf(const IR::Inst& producer);
    Value* PackNzcv(const Flags& f);
    Flags UnpackNzcv(Value* packed);

    // Integer helpers
    Value* Shift(IR::Opcode op, Value* v, Value* amount);
    Value* Div(bool is_signed, Value* a, Value* b);
    Value* Crc32Iso(Value* crc, Value* data, unsigned bytes);
    Value* Crc32c(Value* crc, Value* data, unsigned bits);

    // Memory helpers
    Value* GuestPtr(Value* vaddr) { return B.CreateIntToPtr(vaddr, B.getPtrTy()); }
    llvm::Instruction* Mark(llvm::Instruction* i);
    static bool Ordered(IR::AccType acc) { return acc == IR::AccType::ORDERED || acc == IR::AccType::ORDEREDRW || acc == IR::AccType::LIMITEDORDERED; }

    llvm::Module& M;
    llvm::LLVMContext& C;
    llvm::IRBuilder<> B;
    const Region& region;
    const decode::CodeReader& read;

    std::map<uint64_t, IR::Block> blocks;
    std::map<uint64_t, BasicBlock*> bbs;
    llvm::Function* F = nullptr;
    Value* st = nullptr;
    BasicBlock* exit_bb = nullptr;
    std::array<llvm::AllocaInst*, 31> x{};
    llvm::AllocaInst* sp = nullptr;
    std::array<llvm::AllocaInst*, 4> flag{};  // N, Z, C, V
    llvm::AllocaInst* check_bit = nullptr;
    std::array<bool, 31> wrote_x{};
    bool wrote_sp = false, wrote_nzcv = false;

    // Per block
    std::unordered_map<const IR::Inst*, Value*> values;
    std::unordered_map<const IR::Inst*, Flags> flag_values;
    std::unordered_map<const IR::Inst*, Origin> origins;
    Origin cur_origin;
    Value* last_pc = nullptr;
    bool terminated = false;
    std::string error;
};

// ---- Phase 1 ----

void FunctionLifter::Successors(const IR::Terminal& t, const IR::Block& block, std::vector<uint64_t>& out) {
    if (const auto* l = boost::get<IR::Term::LinkBlock>(&t)) {
        out.push_back(A64::LocationDescriptor{l->next}.PC());
    } else if (const auto* lf = boost::get<IR::Term::LinkBlockFast>(&t)) {
        out.push_back(A64::LocationDescriptor{lf->next}.PC());
    } else if (const auto* i = boost::get<IR::Term::If>(&t)) {
        Successors(i->then_, block, out);
        Successors(i->else_, block, out);
    } else if (const auto* cb = boost::get<IR::Term::CheckBit>(&t)) {
        Successors(cb->then_, block, out);
        Successors(cb->else_, block, out);
    } else if (const auto* ch = boost::get<IR::Term::CheckHalt>(&t)) {
        Successors(ch->else_, block, out);
    } else if (boost::get<IR::Term::ReturnToDispatch>(&t) || boost::get<IR::Term::PopRSBHint>(&t) || boost::get<IR::Term::FastDispatchHint>(&t)) {
        // The block's last A64SetPC, when it is a constant (isb, msr fpcr, a hint's exception)
        const IR::Inst* last = nullptr;
        for (const auto& inst : block) {
            if (inst.GetOpcode() == IR::Opcode::A64SetPC) {
                last = &inst;
            }
        }
        if (last && last->GetArg(0).IsImmediate()) {
            out.push_back(last->GetArg(0).GetU64());
        }
    }
}

void FunctionLifter::Discover(Lifted& out) {
    std::deque<uint64_t> work{region.entry};
    std::set<std::string> unsupported;
    std::set<IR::Opcode> seen;
    while (!work.empty()) {
        const uint64_t pc = work.front();
        work.pop_front();
        if (blocks.count(pc) || blocks.size() >= region.max_blocks) {
            continue;
        }
        IR::Block block = decode::TranslateBlock(pc, read);
        Optimization::IdentityRemovalPass(block);
        Optimization::DeadCodeElimination(block);
        for (const auto& inst : block) {
            if (!IsLowered(inst.GetOpcode())) {
                unsupported.insert(OpcodeName(inst.GetOpcode()));
            }
            if (inst.GetOpcode() != IR::Opcode::Void) {
                seen.insert(inst.GetOpcode());
            }
        }
        std::vector<uint64_t> next;
        Successors(block.GetTerminal(), block, next);
        blocks.emplace(pc, std::move(block));
        for (const uint64_t n : next) {
            if (InRegion(n) && !blocks.count(n)) {
                work.push_back(n);
            }
        }
    }
    out.unsupported.assign(unsupported.begin(), unsupported.end());
    out.opcodes.assign(seen.begin(), seen.end());
    out.blocks = blocks.size();
}

std::string FunctionLifter::Dump() {
    Lifted ignored;
    Discover(ignored);
    std::string text;
    for (const auto& [pc, block] : blocks) {
        text += IR::DumpBlock(block);
        text += "\n";
    }
    return text;
}

// ---- Phase 2 ----

Lifted FunctionLifter::Run(const std::string& name) {
    Lifted out;
    Discover(out);
    if (!out.unsupported.empty()) {
        return out;
    }

    auto* fn_type = llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false);
    F = llvm::Function::Create(fn_type, llvm::GlobalValue::ExternalLinkage, name, M);
    F->addFnAttr(llvm::Attribute::NoUnwind);
    F->addFnAttr(llvm::Attribute::getWithUWTableKind(C, llvm::UWTableKind::Async));
    F->addParamAttr(0, llvm::Attribute::NoAlias);
    F->addParamAttr(0, llvm::Attribute::NoCapture);
    F->addParamAttr(0, llvm::Attribute::NonNull);
    F->addParamAttr(0, llvm::Attribute::getWithAlignment(C, llvm::Align(64)));
    F->addParamAttr(0, llvm::Attribute::getWithDereferenceableBytes(C, sizeof(dm_guest_state)));
    st = F->getArg(0);
    st->setName("st");

    for (auto& [pc, block] : blocks) {
        bbs[pc] = BasicBlock::Create(C, "pc_" + llvm::utohexstr(pc), F);
    }
    exit_bb = BasicBlock::Create(C, "exit", F);
    Prologue();
    for (auto& [pc, block] : blocks) {
        B.SetInsertPoint(bbs[pc]);
        LowerBlock(block);
        if (!error.empty()) {
            break;
        }
    }
    if (error.empty()) {
        Epilogue();
    }
    if (!error.empty()) {
        F->eraseFromParent();
        out.error = error;
        return out;
    }
    out.function = F;
    return out;
}

void FunctionLifter::Prologue() {
    auto* entry = BasicBlock::Create(C, "entry", F, &F->getEntryBlock());
    B.SetInsertPoint(entry);
    for (size_t i = 0; i < 31; ++i) {
        x[i] = B.CreateAlloca(B.getInt64Ty(), nullptr, "x" + std::to_string(i));
    }
    sp = B.CreateAlloca(B.getInt64Ty(), nullptr, "sp");
    static const char* const flag_names[] = {"n", "z", "c", "v"};
    for (size_t i = 0; i < 4; ++i) {
        flag[i] = B.CreateAlloca(B.getInt1Ty(), nullptr, flag_names[i]);
    }
    check_bit = B.CreateAlloca(B.getInt1Ty(), nullptr, "check_bit");
    for (size_t i = 0; i < 31; ++i) {
        B.CreateStore(LoadField(B.getInt64Ty(), offsetof(dm_guest_state, x) + 8 * i), x[i]);
    }
    B.CreateStore(LoadField(B.getInt64Ty(), offsetof(dm_guest_state, sp)), sp);
    const Flags f = UnpackNzcv(LoadField(B.getInt32Ty(), offsetof(dm_guest_state, nzcv)));
    B.CreateStore(f.n, flag[0]);
    B.CreateStore(f.z, flag[1]);
    B.CreateStore(f.c, flag[2]);
    B.CreateStore(f.v, flag[3]);
    B.CreateStore(B.getFalse(), check_bit);
    B.CreateBr(bbs.at(region.entry));
}

void FunctionLifter::Epilogue() {
    B.SetInsertPoint(exit_bb);
    for (size_t i = 0; i < 31; ++i) {
        if (wrote_x[i]) {
            StoreField(B.CreateLoad(B.getInt64Ty(), x[i]), offsetof(dm_guest_state, x) + 8 * i);
        }
    }
    if (wrote_sp) {
        StoreField(B.CreateLoad(B.getInt64Ty(), sp), offsetof(dm_guest_state, sp));
    }
    if (wrote_nzcv) {
        StoreField(PackNzcv(CurrentFlags()), offsetof(dm_guest_state, nzcv));
    }
    B.CreateRetVoid();
}

void FunctionLifter::LowerBlock(IR::Block& block) {
    values.clear();
    flag_values.clear();
    origins.clear();
    cur_origin = {};
    last_pc = nullptr;
    terminated = false;
    for (auto& inst : block) {
        if (inst.GetOpcode() == IR::Opcode::Void) {
            continue;
        }
        if (terminated) {
            error = "an instruction follows an escape in the block at " + llvm::utohexstr(A64::LocationDescriptor{block.Location()}.PC()) + ": " +
                    OpcodeName(inst.GetOpcode());
            return;
        }
        LowerInst(inst);
        if (!error.empty()) {
            return;
        }
    }
    if (!terminated) {
        LowerTerminal(block.GetTerminal());
    }
}

void FunctionLifter::LowerInst(IR::Inst& inst) {
    if (LowerContext(inst) || LowerFlags(inst) || LowerInteger(inst) || LowerMemory(inst)) {
        return;
    }
    // GroupOf said Lowered, but no lowering took it: a bug of this file, not of the guest code
    error = std::string("opcode marked lowered in opcodes.cpp has no lowering: ") + OpcodeName(inst.GetOpcode());
}

void FunctionLifter::LowerTerminal(const IR::Terminal& t) {
    if (const auto* l = boost::get<IR::Term::LinkBlock>(&t)) {
        Goto(I(64, A64::LocationDescriptor{l->next}.PC()));
    } else if (const auto* lf = boost::get<IR::Term::LinkBlockFast>(&t)) {
        Goto(I(64, A64::LocationDescriptor{lf->next}.PC()));
    } else if (const auto* in = boost::get<IR::Term::Interpret>(&t)) {
        Exit(I(64, A64::LocationDescriptor{in->next}.PC()), DM_EXIT_UNTRANSLATED, I(32, 0));
    } else if (boost::get<IR::Term::ReturnToDispatch>(&t) || boost::get<IR::Term::PopRSBHint>(&t) || boost::get<IR::Term::FastDispatchHint>(&t)) {
        if (!last_pc) {
            error = "a dispatch terminal without an A64SetPC";
            return;
        }
        Goto(last_pc);
    } else if (const auto* i = boost::get<IR::Term::If>(&t)) {
        const IR::Term::If& term = *i;
        if (term.if_ == IR::Cond::AL || term.if_ == IR::Cond::NV) {
            LowerTerminal(term.then_);
            return;
        }
        auto* then_bb = BasicBlock::Create(C, "if_then", F);
        auto* else_bb = BasicBlock::Create(C, "if_else", F);
        B.CreateCondBr(Cond(term.if_), then_bb, else_bb);
        B.SetInsertPoint(then_bb);
        LowerTerminal(term.then_);
        B.SetInsertPoint(else_bb);
        LowerTerminal(term.else_);
    } else if (const auto* cb = boost::get<IR::Term::CheckBit>(&t)) {
        auto* then_bb = BasicBlock::Create(C, "bit_set", F);
        auto* else_bb = BasicBlock::Create(C, "bit_clear", F);
        B.CreateCondBr(B.CreateLoad(B.getInt1Ty(), check_bit), then_bb, else_bb);
        B.SetInsertPoint(then_bb);
        LowerTerminal(cb->then_);
        B.SetInsertPoint(else_bb);
        LowerTerminal(cb->else_);
    } else if (const auto* ch = boost::get<IR::Term::CheckHalt>(&t)) {
        // Lifted code has no halt request yet (safepoints, section 6.2, are a later task)
        LowerTerminal(ch->else_);
    } else {
        error = "a block without a valid terminal";
    }
}

void FunctionLifter::Goto(Value* target) {
    if (auto* k = llvm::dyn_cast<ConstantInt>(target)) {
        const auto it = bbs.find(k->getZExtValue());
        if (it != bbs.end()) {
            B.CreateBr(it->second);
            return;
        }
    }
    Exit(target, DM_EXIT_NONE, I(32, 0));
}

void FunctionLifter::Exit(Value* pc, uint32_t reason, Value* arg) {
    StoreField(pc, offsetof(dm_guest_state, pc));
    StoreField(I(32, reason), offsetof(dm_guest_state, exit_reason));
    StoreField(arg, offsetof(dm_guest_state, exit_arg));
    B.CreateBr(exit_bb);
}

// ---- Values ----

llvm::Type* FunctionLifter::TypeOf(IR::Type t) {
    switch (t) {
    case IR::Type::U1:
        return B.getInt1Ty();
    case IR::Type::U8:
        return B.getInt8Ty();
    case IR::Type::U16:
        return B.getInt16Ty();
    case IR::Type::U32:
        return B.getInt32Ty();
    case IR::Type::U64:
        return B.getInt64Ty();
    case IR::Type::U128:
        return B.getInt128Ty();
    default:
        return nullptr;
    }
}

Value* FunctionLifter::Get(const IR::Value& v) {
    if (v.IsImmediate()) {
        switch (v.GetType()) {
        case IR::Type::U1:
            return B.getInt1(v.GetU1());
        case IR::Type::U8:
            return B.getInt8(v.GetU8());
        case IR::Type::U16:
            return B.getInt16(v.GetU16());
        case IR::Type::U32:
            return B.getInt32(v.GetU32());
        case IR::Type::U64:
            return B.getInt64(v.GetU64());
        default:
            error = "an immediate of a type the lowering does not take";
            return llvm::UndefValue::get(B.getInt64Ty());
        }
    }
    const auto it = values.find(v.GetInst());
    if (it == values.end()) {
        error = std::string("a value used before its definition: ") + OpcodeName(v.GetInst()->GetOpcode());
        return llvm::UndefValue::get(B.getInt64Ty());
    }
    return it->second;
}

const Flags& FunctionLifter::FlagsArg(const IR::Inst& inst, size_t i) {
    static const Flags none{};
    const IR::Value v = inst.GetArg(i);
    const auto it = v.IsImmediate() ? flag_values.end() : flag_values.find(v.GetInst());
    if (it == flag_values.end()) {
        error = "an NZCV operand the lowering did not define";
        return none;
    }
    return it->second;
}

// ---- Flags ----

Flags FunctionLifter::CurrentFlags() {
    return {B.CreateLoad(B.getInt1Ty(), flag[0]), B.CreateLoad(B.getInt1Ty(), flag[1]), B.CreateLoad(B.getInt1Ty(), flag[2]),
            B.CreateLoad(B.getInt1Ty(), flag[3])};
}

void FunctionLifter::SetFlags(const Flags& f, const Origin& origin) {
    B.CreateStore(f.n, flag[0]);
    B.CreateStore(f.z, flag[1]);
    B.CreateStore(f.c, flag[2]);
    B.CreateStore(f.v, flag[3]);
    cur_origin = origin;
    wrote_nzcv = true;
}

Value* FunctionLifter::PackNzcv(const Flags& f) {
    Value* r = B.CreateShl(B.CreateZExt(f.n, B.getInt32Ty()), 31);
    r = B.CreateOr(r, B.CreateShl(B.CreateZExt(f.z, B.getInt32Ty()), 30));
    r = B.CreateOr(r, B.CreateShl(B.CreateZExt(f.c, B.getInt32Ty()), 29));
    return B.CreateOr(r, B.CreateShl(B.CreateZExt(f.v, B.getInt32Ty()), 28));
}

Flags FunctionLifter::UnpackNzcv(Value* packed) {
    auto bit = [&](unsigned b) { return B.CreateTrunc(B.CreateLShr(packed, b), B.getInt1Ty()); };
    return {bit(31), bit(30), bit(29), bit(28)};
}

Value* FunctionLifter::Cond(IR::Cond cond) {
    using IR::Cond;
    if (cond == Cond::AL || cond == Cond::NV) {
        return B.getTrue();
    }
    if (cur_origin.kind == OriginKind::Sub) {
        Value* a = cur_origin.a;
        Value* b = cur_origin.b;
        switch (cond) {
        case Cond::EQ:
            return B.CreateICmpEQ(a, b);
        case Cond::NE:
            return B.CreateICmpNE(a, b);
        case Cond::CS:
            return B.CreateICmpUGE(a, b);
        case Cond::CC:
            return B.CreateICmpULT(a, b);
        case Cond::HI:
            return B.CreateICmpUGT(a, b);
        case Cond::LS:
            return B.CreateICmpULE(a, b);
        case Cond::GE:
            return B.CreateICmpSGE(a, b);
        case Cond::LT:
            return B.CreateICmpSLT(a, b);
        case Cond::GT:
            return B.CreateICmpSGT(a, b);
        case Cond::LE:
            return B.CreateICmpSLE(a, b);
        default:
            break;  // MI, PL, VS, VC: from the flag bits below
        }
    } else if (cur_origin.kind == OriginKind::Logic) {
        Value* r = cur_origin.res;
        Value* zero = ConstantInt::get(r->getType(), 0);
        switch (cond) {
        case Cond::EQ:
            return B.CreateICmpEQ(r, zero);
        case Cond::NE:
            return B.CreateICmpNE(r, zero);
        case Cond::MI:
        case Cond::LT:
            return B.CreateICmpSLT(r, zero);
        case Cond::PL:
        case Cond::GE:
            return B.CreateICmpSGE(r, zero);
        case Cond::GT:
            return B.CreateICmpSGT(r, zero);
        case Cond::LE:
            return B.CreateICmpSLE(r, zero);
        case Cond::CS:
        case Cond::HI:
        case Cond::VS:
            return B.getFalse();
        case Cond::CC:
        case Cond::LS:
        case Cond::VC:
            return B.getTrue();
        default:
            break;
        }
    }
    const Flags f = CurrentFlags();
    switch (cond) {
    case Cond::EQ:
        return f.z;
    case Cond::NE:
        return B.CreateNot(f.z);
    case Cond::CS:
        return f.c;
    case Cond::CC:
        return B.CreateNot(f.c);
    case Cond::MI:
        return f.n;
    case Cond::PL:
        return B.CreateNot(f.n);
    case Cond::VS:
        return f.v;
    case Cond::VC:
        return B.CreateNot(f.v);
    case Cond::HI:
        return B.CreateAnd(f.c, B.CreateNot(f.z));
    case Cond::LS:
        return B.CreateOr(B.CreateNot(f.c), f.z);
    case Cond::GE:
        return B.CreateICmpEQ(f.n, f.v);
    case Cond::LT:
        return B.CreateICmpNE(f.n, f.v);
    case Cond::GT:
        return B.CreateAnd(B.CreateNot(f.z), B.CreateICmpEQ(f.n, f.v));
    case Cond::LE:
        return B.CreateOr(f.z, B.CreateICmpNE(f.n, f.v));
    default:
        error = "an invalid condition";
        return B.getFalse();
    }
}

// Carry out of Add (a + b + c) and Sub (a + ~b + c) at the operation's width
Value* FunctionLifter::CarryOf(const IR::Inst& producer) {
    const IR::Opcode op = producer.GetOpcode();
    if (op == IR::Opcode::Add32 || op == IR::Opcode::Add64 || op == IR::Opcode::Sub32 || op == IR::Opcode::Sub64) {
        const bool sub = op == IR::Opcode::Sub32 || op == IR::Opcode::Sub64;
        Value* a = Arg(producer, 0);
        Value* b = Arg(producer, 1);
        const IR::Value cin = producer.GetArg(2);
        if (sub && cin.IsImmediate() && cin.GetU1()) {
            return B.CreateICmpUGE(a, b);
        }
        if (!sub && cin.IsImmediate() && !cin.GetU1()) {
            return B.CreateICmpULT(values.at(&producer), a);
        }
        const unsigned n = Bits(a);
        llvm::Type* wide = B.getIntNTy(n + 1);
        Value* bb = sub ? B.CreateNot(b) : b;
        Value* sum = B.CreateAdd(B.CreateAdd(B.CreateZExt(a, wide), B.CreateZExt(bb, wide)), B.CreateZExt(Get(cin), wide));
        return B.CreateTrunc(B.CreateLShr(sum, n), B.getInt1Ty());
    }
    error = std::string("a carry of ") + OpcodeName(op);
    return B.getFalse();
}

Value* FunctionLifter::OverflowOf(const IR::Inst& producer) {
    const IR::Opcode op = producer.GetOpcode();
    if (op != IR::Opcode::Add32 && op != IR::Opcode::Add64 && op != IR::Opcode::Sub32 && op != IR::Opcode::Sub64) {
        error = std::string("an overflow of ") + OpcodeName(op);
        return B.getFalse();
    }
    const bool sub = op == IR::Opcode::Sub32 || op == IR::Opcode::Sub64;
    Value* a = Arg(producer, 0);
    Value* b = Arg(producer, 1);
    Value* res = values.at(&producer);
    // Add: a and b have one sign and the result the other; Sub (a + ~b + c): a and b differ in sign and the result differs from a
    Value* t = sub ? B.CreateAnd(B.CreateXor(a, b), B.CreateXor(a, res)) : B.CreateAnd(B.CreateXor(a, res), B.CreateXor(b, res));
    return SignBit(t);
}

Flags FunctionLifter::FlagsFromOp(const IR::Inst& producer, Origin* origin) {
    *origin = {};
    const auto it = values.find(&producer);
    if (it == values.end()) {
        error = "GetNZCVFromOp of a value the lowering did not define";
        return {B.getFalse(), B.getFalse(), B.getFalse(), B.getFalse()};
    }
    Value* res = it->second;
    Flags f;
    f.n = SignBit(res);
    f.z = B.CreateICmpEQ(res, ConstantInt::get(res->getType(), 0));
    switch (producer.GetOpcode()) {
    case IR::Opcode::Add32:
    case IR::Opcode::Add64:
        f.c = CarryOf(producer);
        f.v = OverflowOf(producer);
        break;
    case IR::Opcode::Sub32:
    case IR::Opcode::Sub64: {
        f.c = CarryOf(producer);
        f.v = OverflowOf(producer);
        const IR::Value cin = producer.GetArg(2);
        if (cin.IsImmediate() && cin.GetU1()) {
            *origin = {OriginKind::Sub, Arg(producer, 0), Arg(producer, 1), res};
        }
        break;
    }
    default:
        // The logical operations (Inst::MayGetNZCVFromOp lists them): N and Z from the result, C = V = 0, as AArch64's ANDS / BICS / TST
        // and as dynarmic's x64 backend computes them (test; lahf; mov al, 0)
        f.c = B.getFalse();
        f.v = B.getFalse();
        *origin = {OriginKind::Logic, nullptr, nullptr, res};
        break;
    }
    return f;
}

bool FunctionLifter::LowerFlags(IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::GetNZCVFromOp: {
        Origin origin;
        flag_values[&inst] = FlagsFromOp(*inst.GetArg(0).GetInst(), &origin);
        origins[&inst] = origin;
        return true;
    }
    case IR::Opcode::NZCVFromPackedFlags:
        flag_values[&inst] = UnpackNzcv(Arg(inst, 0));
        origins[&inst] = {};
        return true;
    case IR::Opcode::ConditionalSelectNZCV: {
        Value* cond = Cond(inst.GetArg(0).GetCond());
        const Flags a = FlagsArg(inst, 1);
        const Flags b = FlagsArg(inst, 2);
        flag_values[&inst] = {B.CreateSelect(cond, a.n, b.n), B.CreateSelect(cond, a.z, b.z), B.CreateSelect(cond, a.c, b.c),
                              B.CreateSelect(cond, a.v, b.v)};
        origins[&inst] = {};
        return true;
    }
    default:
        return false;
    }
}

// ---- Context: registers, flags, system registers, escapes ----

bool FunctionLifter::LowerContext(IR::Inst& inst) {
    using IR::Opcode;
    auto reg = [&]() { return static_cast<size_t>(inst.GetArg(0).GetA64RegRef()); };
    switch (inst.GetOpcode()) {
    case Opcode::Identity:
        Def(inst, Arg(inst, 0));
        return true;
    case Opcode::PushRSB:
        return true;
    case Opcode::A64SetCheckBit:
        B.CreateStore(Arg(inst, 0), check_bit);
        return true;
    case Opcode::A64GetCFlag:
        Def(inst, B.CreateLoad(B.getInt1Ty(), flag[2]));
        return true;
    case Opcode::A64GetNZCVRaw:
        Def(inst, PackNzcv(CurrentFlags()));
        return true;
    case Opcode::A64SetNZCVRaw:
        SetFlags(UnpackNzcv(Arg(inst, 0)), {});
        return true;
    case Opcode::A64SetNZCV: {
        const Flags f = FlagsArg(inst, 0);
        const IR::Value v = inst.GetArg(0);
        const auto o = v.IsImmediate() ? origins.end() : origins.find(v.GetInst());
        SetFlags(f, o == origins.end() ? Origin{} : o->second);
        return true;
    }
    case Opcode::A64GetW:
        Def(inst, B.CreateTrunc(B.CreateLoad(B.getInt64Ty(), x[reg()]), B.getInt32Ty()));
        return true;
    case Opcode::A64GetX:
        Def(inst, B.CreateLoad(B.getInt64Ty(), x[reg()]));
        return true;
    case Opcode::A64SetW:
        B.CreateStore(B.CreateZExt(Arg(inst, 1), B.getInt64Ty()), x[reg()]);
        wrote_x[reg()] = true;
        return true;
    case Opcode::A64SetX:
        B.CreateStore(Arg(inst, 1), x[reg()]);
        wrote_x[reg()] = true;
        return true;
    case Opcode::A64GetSP:
        Def(inst, B.CreateLoad(B.getInt64Ty(), sp));
        return true;
    case Opcode::A64SetSP:
        B.CreateStore(Arg(inst, 0), sp);
        wrote_sp = true;
        return true;
    case Opcode::A64SetPC:
        last_pc = Arg(inst, 0);
        return true;
    case Opcode::A64GetFPCR:
        Def(inst, LoadField(B.getInt32Ty(), offsetof(dm_guest_state, fpcr)));
        return true;
    case Opcode::A64SetFPCR:
        // Integer code does not depend on FPCR. Once floating point is lowered, a change away from the FPCR the block was specialised for
        // must leave the function here (section 7.1)
        StoreField(B.CreateAnd(Arg(inst, 0), kFpcrMask), offsetof(dm_guest_state, fpcr));
        return true;
    case Opcode::A64GetFPSR:
        Def(inst, LoadField(B.getInt32Ty(), offsetof(dm_guest_state, fpsr)));
        return true;
    case Opcode::A64SetFPSR:
        StoreField(B.CreateAnd(Arg(inst, 0), kFpsrMask), offsetof(dm_guest_state, fpsr));
        return true;
    case Opcode::A64GetCNTFRQ:
        Def(inst, B.getInt32(kCntfrq));
        return true;
    case Opcode::A64GetCNTPCT:
        Def(inst, LoadField(B.getInt64Ty(), offsetof(dm_guest_state, cnt)));
        return true;
    case Opcode::A64GetCTR:
        Def(inst, B.getInt32(kCtr));
        return true;
    case Opcode::A64GetDCZID:
        Def(inst, B.getInt32(kDczid));
        return true;
    case Opcode::A64GetTPIDR:
        Def(inst, LoadField(B.getInt64Ty(), offsetof(dm_guest_state, tpidr_el0)));
        return true;
    case Opcode::A64GetTPIDRRO:
        Def(inst, LoadField(B.getInt64Ty(), offsetof(dm_guest_state, tpidrro_el0)));
        return true;
    case Opcode::A64SetTPIDR:
        StoreField(Arg(inst, 0), offsetof(dm_guest_state, tpidr_el0));
        return true;
    case Opcode::A64DataSynchronizationBarrier:
    case Opcode::A64DataMemoryBarrier:
        // dmb and dsb of every kind: mfence (section 6.3). dynarmic's IR does not keep the option, so dmb ishld / ishst get the full fence too
        B.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
        return true;
    case Opcode::A64InstructionSynchronizationBarrier:
    case Opcode::A64InstructionCacheOperationRaised:
        // x86 instruction fetch is coherent, and recompiled code is not modified (section 6.3)
        return true;
    case Opcode::A64DataCacheOperationRaised: {
        const auto op = static_cast<A64::DataCacheOperation>(inst.GetArg(1).GetU64());
        if (op == A64::DataCacheOperation::ZeroByVA) {
            // dc zva zeroes the naturally aligned 64-byte block that holds the address (dczid_el0 = 4)
            Value* base = B.CreateAnd(Arg(inst, 2), ~(kZvaBytes - 1));
            for (uint64_t off = 0; off < kZvaBytes; off += 8) {
                Mark(B.CreateAlignedStore(B.getInt64(0), GuestPtr(B.CreateAdd(base, B.getInt64(off))), llvm::Align(8), true));
            }
        }
        // The other data cache operations do nothing: x86 caches are coherent (section 9.2)
        return true;
    }
    case Opcode::A64CallSupervisor:
        // An escape to the host's SVC path (section 2.4). The svc returns through an exception return, which clears the local monitor, as
        // dynarmic's backend clears its exclusive state after CallSVC
        if (!last_pc) {
            error = "svc without the A64SetPC before it";
            return true;
        }
        StoreField(B.getInt32(0), offsetof(dm_guest_state, excl_valid));
        Exit(last_pc, DM_EXIT_SVC, Arg(inst, 0));
        terminated = true;
        return true;
    case Opcode::A64ExceptionRaised: {
        const uint64_t pc = inst.GetArg(0).GetU64();
        const auto e = static_cast<A64::Exception>(inst.GetArg(1).GetU64());
        switch (e) {
        case A64::Exception::Yield:
        case A64::Exception::WaitForEvent:
        case A64::Exception::WaitForInterrupt:
            // A hint: pause (section 6.3). Spin regions get the host's spin gate here later
            B.CreateCall(llvm::Intrinsic::getDeclaration(&M, llvm::Intrinsic::x86_sse2_pause));
            return true;
        case A64::Exception::SendEvent:
        case A64::Exception::SendEventLocal:
            return true;
        case A64::Exception::Breakpoint:
            Exit(I(64, pc), DM_EXIT_BRK, I(32, 0));
            break;
        case A64::Exception::UnallocatedEncoding:
        case A64::Exception::ReservedValue:
            Exit(I(64, pc), DM_EXIT_UNDEFINED, I(32, 0));
            break;
        case A64::Exception::UnpredictableInstruction:
            Exit(I(64, pc), DM_EXIT_UNPREDICTABLE, I(32, 0));
            break;
        case A64::Exception::NoExecuteFault:
            Exit(I(64, pc), DM_EXIT_NO_CODE, I(32, 0));
            break;
        default:
            error = "an exception the lowering does not know";
            return true;
        }
        terminated = true;
        return true;
    }
    default:
        return false;
    }
}

// ---- Integer calculations ----

Value* FunctionLifter::Shift(IR::Opcode op, Value* v, Value* amount) {
    using IR::Opcode;
    const unsigned n = Bits(v);
    Value* s = B.CreateZExt(amount, v->getType());
    Value* n_const = ConstantInt::get(v->getType(), n);
    Value* in_range = B.CreateICmpULT(s, n_const);
    Value* safe = B.CreateSelect(in_range, s, ConstantInt::get(v->getType(), 0));
    switch (op) {
    case Opcode::LogicalShiftLeft32:
    case Opcode::LogicalShiftLeft64:
        // An amount of the width or more gives 0 (Arm semantics; x86 and LLVM differ: emit_x64_data_processing.cpp)
        return B.CreateSelect(in_range, B.CreateShl(v, safe), ConstantInt::get(v->getType(), 0));
    case Opcode::LogicalShiftRight32:
    case Opcode::LogicalShiftRight64:
        return B.CreateSelect(in_range, B.CreateLShr(v, safe), ConstantInt::get(v->getType(), 0));
    case Opcode::ArithmeticShiftRight32:
    case Opcode::ArithmeticShiftRight64:
        // The width or more: every bit is the sign
        return B.CreateAShr(v, B.CreateSelect(in_range, s, ConstantInt::get(v->getType(), n - 1)));
    case Opcode::RotateRight32:
    case Opcode::RotateRight64: {
        llvm::Function* fshr = llvm::Intrinsic::getDeclaration(&M, llvm::Intrinsic::fshr, {v->getType()});
        return B.CreateCall(fshr, {v, v, s});  // fshr takes the amount modulo the width
    }
    default:
        error = std::string("Shift of ") + OpcodeName(op);
        return v;
    }
}

Value* FunctionLifter::Div(bool is_signed, Value* a, Value* b) {
    // Arm: a divisor of 0 gives 0, INT_MIN / -1 gives INT_MIN; LLVM's udiv / sdiv are undefined there, so the divisor is made safe first
    llvm::Type* t = a->getType();
    Value* b_zero = B.CreateICmpEQ(b, ConstantInt::get(t, 0));
    Value* bad = b_zero;
    if (is_signed) {
        Value* min = ConstantInt::get(t, llvm::APInt::getSignedMinValue(Bits(a)));
        bad = B.CreateOr(bad, B.CreateAnd(B.CreateICmpEQ(a, min), B.CreateICmpEQ(b, ConstantInt::getAllOnesValue(t))));
    }
    Value* d = B.CreateSelect(bad, ConstantInt::get(t, 1), b);
    Value* q = is_signed ? B.CreateSDiv(a, d) : B.CreateUDiv(a, d);  // INT_MIN / 1 = INT_MIN, the answer for INT_MIN / -1
    return B.CreateSelect(b_zero, ConstantInt::get(t, 0), q);
}

Value* FunctionLifter::Crc32c(Value* crc, Value* data, unsigned bits) {
    using llvm::Intrinsic::ID;
    switch (bits) {
    case 8:
        return B.CreateCall(llvm::Intrinsic::getDeclaration(&M, llvm::Intrinsic::x86_sse42_crc32_32_8), {crc, B.CreateTrunc(data, B.getInt8Ty())});
    case 16:
        return B.CreateCall(llvm::Intrinsic::getDeclaration(&M, llvm::Intrinsic::x86_sse42_crc32_32_16), {crc, B.CreateTrunc(data, B.getInt16Ty())});
    case 32:
        return B.CreateCall(llvm::Intrinsic::getDeclaration(&M, llvm::Intrinsic::x86_sse42_crc32_32_32), {crc, data});
    default: {
        Value* r = B.CreateCall(llvm::Intrinsic::getDeclaration(&M, llvm::Intrinsic::x86_sse42_crc32_64_64), {B.CreateZExt(crc, B.getInt64Ty()), data});
        return B.CreateTrunc(r, B.getInt32Ty());
    }
    }
}

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320) without the initial and final inversion, as Arm's CRC32B/H/W/X: a byte at a time, lowest first,
// through a 256-entry table. x86 has no instruction for this polynomial; a PCLMULQDQ form is a later optimisation (section 7.2, row 16)
Value* FunctionLifter::Crc32Iso(Value* crc, Value* data, unsigned bytes) {
    llvm::GlobalVariable* table = M.getGlobalVariable("nxaot.crc32.table", true);
    if (!table) {
        std::vector<uint32_t> entries(256);
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            }
            entries[i] = c;
        }
        auto* init = llvm::ConstantDataArray::get(C, entries);
        table = new llvm::GlobalVariable(M, init->getType(), true, llvm::GlobalValue::PrivateLinkage, init, "nxaot.crc32.table");
    }
    Value* d64 = B.CreateZExt(data, B.getInt64Ty());
    for (unsigned i = 0; i < bytes; ++i) {
        Value* byte = B.CreateTrunc(B.CreateLShr(d64, 8 * i), B.getInt32Ty());
        Value* index = B.CreateZExt(B.CreateAnd(B.CreateXor(crc, byte), 0xFF), B.getInt64Ty());
        Value* entry = B.CreateAlignedLoad(B.getInt32Ty(), B.CreateInBoundsGEP(table->getValueType(), table, {B.getInt64(0), index}), llvm::Align(4));
        crc = B.CreateXor(entry, B.CreateLShr(crc, 8));
    }
    return crc;
}

bool FunctionLifter::LowerInteger(IR::Inst& inst) {
    using IR::Opcode;
    const Opcode op = inst.GetOpcode();
    auto intrinsic = [&](llvm::Intrinsic::ID id, std::initializer_list<Value*> args) {
        return B.CreateCall(llvm::Intrinsic::getDeclaration(&M, id, {(*args.begin())->getType()}), args);
    };
    switch (op) {
    case Opcode::Pack2x32To1x64:
        Def(inst, B.CreateOr(B.CreateZExt(Arg(inst, 0), B.getInt64Ty()), B.CreateShl(B.CreateZExt(Arg(inst, 1), B.getInt64Ty()), 32)));
        return true;
    case Opcode::Pack2x64To1x128:
        Def(inst, B.CreateOr(B.CreateZExt(Arg(inst, 0), B.getInt128Ty()), B.CreateShl(B.CreateZExt(Arg(inst, 1), B.getInt128Ty()), 64)));
        return true;
    case Opcode::LeastSignificantWord:
        Def(inst, B.CreateTrunc(Arg(inst, 0), B.getInt32Ty()));
        return true;
    case Opcode::LeastSignificantHalf:
        Def(inst, B.CreateTrunc(Arg(inst, 0), B.getInt16Ty()));
        return true;
    case Opcode::LeastSignificantByte:
        Def(inst, B.CreateTrunc(Arg(inst, 0), B.getInt8Ty()));
        return true;
    case Opcode::MostSignificantWord:
        Def(inst, B.CreateTrunc(B.CreateLShr(Arg(inst, 0), 32), B.getInt32Ty()));
        return true;
    case Opcode::IsZero32:
    case Opcode::IsZero64: {
        Value* v = Arg(inst, 0);
        Def(inst, B.CreateICmpEQ(v, ConstantInt::get(v->getType(), 0)));
        return true;
    }
    case Opcode::TestBit: {
        Value* bit = B.CreateAnd(B.CreateZExt(Arg(inst, 1), B.getInt64Ty()), 63);  // bt takes the index modulo 64
        Def(inst, B.CreateTrunc(B.CreateLShr(Arg(inst, 0), bit), B.getInt1Ty()));
        return true;
    }
    case Opcode::ConditionalSelect32:
    case Opcode::ConditionalSelect64:
        Def(inst, B.CreateSelect(Cond(inst.GetArg(0).GetCond()), Arg(inst, 1), Arg(inst, 2)));
        return true;
    case Opcode::LogicalShiftLeft32:
    case Opcode::LogicalShiftRight32:
    case Opcode::ArithmeticShiftRight32:
    case Opcode::RotateRight32:
    case Opcode::LogicalShiftLeft64:
    case Opcode::LogicalShiftRight64:
    case Opcode::ArithmeticShiftRight64:
    case Opcode::RotateRight64:
        Def(inst, Shift(op, Arg(inst, 0), Arg(inst, 1)));
        return true;
    case Opcode::LogicalShiftLeftMasked32:
    case Opcode::LogicalShiftLeftMasked64:
    case Opcode::LogicalShiftRightMasked32:
    case Opcode::LogicalShiftRightMasked64:
    case Opcode::ArithmeticShiftRightMasked32:
    case Opcode::ArithmeticShiftRightMasked64:
    case Opcode::RotateRightMasked32:
    case Opcode::RotateRightMasked64: {
        // The register shifts of A64 (lslv, ...): the amount modulo the width (section 7.2, row 15)
        Value* v = Arg(inst, 0);
        Value* s = B.CreateAnd(Arg(inst, 1), ConstantInt::get(v->getType(), Bits(v) - 1));
        Value* r = nullptr;
        if (op == Opcode::LogicalShiftLeftMasked32 || op == Opcode::LogicalShiftLeftMasked64) {
            r = B.CreateShl(v, s);
        } else if (op == Opcode::LogicalShiftRightMasked32 || op == Opcode::LogicalShiftRightMasked64) {
            r = B.CreateLShr(v, s);
        } else if (op == Opcode::ArithmeticShiftRightMasked32 || op == Opcode::ArithmeticShiftRightMasked64) {
            r = B.CreateAShr(v, s);
        } else {
            r = B.CreateCall(llvm::Intrinsic::getDeclaration(&M, llvm::Intrinsic::fshr, {v->getType()}), {v, v, s});
        }
        Def(inst, r);
        return true;
    }
    case Opcode::Add32:
    case Opcode::Add64:
        Def(inst, B.CreateAdd(B.CreateAdd(Arg(inst, 0), Arg(inst, 1)), B.CreateZExt(Arg(inst, 2), Arg(inst, 0)->getType())));
        return true;
    case Opcode::Sub32:
    case Opcode::Sub64: {
        // a + ~b + carry_in: with a carry in of 1 the plain subtraction
        Value* a = Arg(inst, 0);
        Value* b = Arg(inst, 1);
        const IR::Value cin = inst.GetArg(2);
        if (cin.IsImmediate() && cin.GetU1()) {
            Def(inst, B.CreateSub(a, b));
        } else {
            Def(inst, B.CreateAdd(B.CreateAdd(a, B.CreateNot(b)), B.CreateZExt(Get(cin), a->getType())));
        }
        return true;
    }
    case Opcode::Mul32:
    case Opcode::Mul64:
        Def(inst, B.CreateMul(Arg(inst, 0), Arg(inst, 1)));
        return true;
    case Opcode::SignedMultiplyHigh64:
    case Opcode::UnsignedMultiplyHigh64: {
        const bool s = op == Opcode::SignedMultiplyHigh64;
        Value* a = s ? B.CreateSExt(Arg(inst, 0), B.getInt128Ty()) : B.CreateZExt(Arg(inst, 0), B.getInt128Ty());
        Value* b = s ? B.CreateSExt(Arg(inst, 1), B.getInt128Ty()) : B.CreateZExt(Arg(inst, 1), B.getInt128Ty());
        Def(inst, B.CreateTrunc(B.CreateLShr(B.CreateMul(a, b), 64), B.getInt64Ty()));
        return true;
    }
    case Opcode::UnsignedDiv32:
    case Opcode::UnsignedDiv64:
        Def(inst, Div(false, Arg(inst, 0), Arg(inst, 1)));
        return true;
    case Opcode::SignedDiv32:
    case Opcode::SignedDiv64:
        Def(inst, Div(true, Arg(inst, 0), Arg(inst, 1)));
        return true;
    case Opcode::And32:
    case Opcode::And64:
        Def(inst, B.CreateAnd(Arg(inst, 0), Arg(inst, 1)));
        return true;
    case Opcode::AndNot32:
    case Opcode::AndNot64:
        Def(inst, B.CreateAnd(Arg(inst, 0), B.CreateNot(Arg(inst, 1))));
        return true;
    case Opcode::Eor32:
    case Opcode::Eor64:
        Def(inst, B.CreateXor(Arg(inst, 0), Arg(inst, 1)));
        return true;
    case Opcode::Or32:
    case Opcode::Or64:
        Def(inst, B.CreateOr(Arg(inst, 0), Arg(inst, 1)));
        return true;
    case Opcode::Not32:
    case Opcode::Not64:
        Def(inst, B.CreateNot(Arg(inst, 0)));
        return true;
    case Opcode::SignExtendByteToWord:
    case Opcode::SignExtendHalfToWord:
        Def(inst, B.CreateSExt(Arg(inst, 0), B.getInt32Ty()));
        return true;
    case Opcode::SignExtendByteToLong:
    case Opcode::SignExtendHalfToLong:
    case Opcode::SignExtendWordToLong:
        Def(inst, B.CreateSExt(Arg(inst, 0), B.getInt64Ty()));
        return true;
    case Opcode::ZeroExtendByteToWord:
    case Opcode::ZeroExtendHalfToWord:
        Def(inst, B.CreateZExt(Arg(inst, 0), B.getInt32Ty()));
        return true;
    case Opcode::ZeroExtendByteToLong:
    case Opcode::ZeroExtendHalfToLong:
    case Opcode::ZeroExtendWordToLong:
        Def(inst, B.CreateZExt(Arg(inst, 0), B.getInt64Ty()));
        return true;
    case Opcode::ByteReverseWord:
    case Opcode::ByteReverseDual:
        Def(inst, intrinsic(llvm::Intrinsic::bswap, {Arg(inst, 0)}));
        return true;
    case Opcode::CountLeadingZeros32:
    case Opcode::CountLeadingZeros64: {
        Value* v = Arg(inst, 0);
        Def(inst, B.CreateCall(llvm::Intrinsic::getDeclaration(&M, llvm::Intrinsic::ctlz, {v->getType()}), {v, B.getFalse()}));
        return true;
    }
    case Opcode::ExtractRegister32:
    case Opcode::ExtractRegister64: {
        // (arg1:arg0) >> lsb, the low half (x86 shrd arg0, arg1, lsb in dynarmic's backend)
        Value* lo = Arg(inst, 0);
        Value* hi = Arg(inst, 1);
        Value* lsb = B.CreateZExt(Arg(inst, 2), lo->getType());
        Def(inst, B.CreateCall(llvm::Intrinsic::getDeclaration(&M, llvm::Intrinsic::fshr, {lo->getType()}), {hi, lo, lsb}));
        return true;
    }
    case Opcode::ReplicateBit32:
    case Opcode::ReplicateBit64: {
        Value* v = Arg(inst, 0);
        Value* bit = B.CreateTrunc(B.CreateLShr(v, B.CreateZExt(Arg(inst, 1), v->getType())), B.getInt1Ty());
        Def(inst, B.CreateSExt(bit, v->getType()));
        return true;
    }
    case Opcode::CRC32Castagnoli8:
        Def(inst, Crc32c(Arg(inst, 0), Arg(inst, 1), 8));
        return true;
    case Opcode::CRC32Castagnoli16:
        Def(inst, Crc32c(Arg(inst, 0), Arg(inst, 1), 16));
        return true;
    case Opcode::CRC32Castagnoli32:
        Def(inst, Crc32c(Arg(inst, 0), Arg(inst, 1), 32));
        return true;
    case Opcode::CRC32Castagnoli64:
        Def(inst, Crc32c(Arg(inst, 0), Arg(inst, 1), 64));
        return true;
    case Opcode::CRC32ISO8:
        Def(inst, Crc32Iso(Arg(inst, 0), Arg(inst, 1), 1));
        return true;
    case Opcode::CRC32ISO16:
        Def(inst, Crc32Iso(Arg(inst, 0), Arg(inst, 1), 2));
        return true;
    case Opcode::CRC32ISO32:
        Def(inst, Crc32Iso(Arg(inst, 0), Arg(inst, 1), 4));
        return true;
    case Opcode::CRC32ISO64:
        Def(inst, Crc32Iso(Arg(inst, 0), Arg(inst, 1), 8));
        return true;
    case Opcode::VectorGetElement64: {
        if (!inst.GetArg(1).IsImmediate()) {
            error = "VectorGetElement64 with a variable index";
            return true;
        }
        const unsigned index = inst.GetArg(1).GetU8() & 1;
        Def(inst, B.CreateTrunc(B.CreateLShr(Arg(inst, 0), 64 * index), B.getInt64Ty()));
        return true;
    }
    default:
        return false;
    }
}

// ---- Memory ----

llvm::Instruction* FunctionLifter::Mark(llvm::Instruction* i) {
    // Guest memory accesses carry !nxaot.guest (section 9.2), for the code-generation checks and later passes
    i->setMetadata("nxaot.guest", llvm::MDNode::get(C, {}));
    return i;
}

bool FunctionLifter::LowerMemory(IR::Inst& inst) {
    using IR::Opcode;
    const Opcode op = inst.GetOpcode();
    auto bits_of = [](Opcode o) -> unsigned {
        switch (o) {
        case Opcode::A64ReadMemory8:
        case Opcode::A64WriteMemory8:
        case Opcode::A64ExclusiveReadMemory8:
        case Opcode::A64ExclusiveWriteMemory8:
            return 8;
        case Opcode::A64ReadMemory16:
        case Opcode::A64WriteMemory16:
        case Opcode::A64ExclusiveReadMemory16:
        case Opcode::A64ExclusiveWriteMemory16:
            return 16;
        case Opcode::A64ReadMemory32:
        case Opcode::A64WriteMemory32:
        case Opcode::A64ExclusiveReadMemory32:
        case Opcode::A64ExclusiveWriteMemory32:
            return 32;
        case Opcode::A64ReadMemory64:
        case Opcode::A64WriteMemory64:
        case Opcode::A64ExclusiveReadMemory64:
        case Opcode::A64ExclusiveWriteMemory64:
            return 64;
        default:
            return 128;
        }
    };
    switch (op) {
    case Opcode::A64ClearExclusive:
        StoreField(B.getInt32(0), offsetof(dm_guest_state, excl_valid));
        return true;
    case Opcode::A64ReadMemory8:
    case Opcode::A64ReadMemory16:
    case Opcode::A64ReadMemory32:
    case Opcode::A64ReadMemory64: {
        const unsigned bits = bits_of(op);
        const IR::AccType acc = inst.GetArg(2).GetAccType();
        llvm::Type* ty = B.getIntNTy(bits);
        if (Ordered(acc)) {
            // ldar: an acquire load at natural alignment (Arm faults on misaligned ordered accesses; section 6.4)
            auto* l = B.CreateAlignedLoad(ty, GuestPtr(Arg(inst, 1)), llvm::Align(bits / 8), true);
            l->setAtomic(llvm::AtomicOrdering::Acquire);
            Def(inst, Mark(l));
        } else {
            Def(inst, Mark(B.CreateAlignedLoad(ty, GuestPtr(Arg(inst, 1)), llvm::Align(1), true)));
        }
        return true;
    }
    case Opcode::A64WriteMemory8:
    case Opcode::A64WriteMemory16:
    case Opcode::A64WriteMemory32:
    case Opcode::A64WriteMemory64: {
        const unsigned bits = bits_of(op);
        const IR::AccType acc = inst.GetArg(3).GetAccType();
        if (Ordered(acc)) {
            // stlr: a sequentially consistent store (xchg on x86), which keeps it before a later ldar (section 6.3)
            auto* s = B.CreateAlignedStore(Arg(inst, 2), GuestPtr(Arg(inst, 1)), llvm::Align(bits / 8), true);
            s->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
            Mark(s);
        } else {
            Mark(B.CreateAlignedStore(Arg(inst, 2), GuestPtr(Arg(inst, 1)), llvm::Align(1), true));
        }
        return true;
    }
    case Opcode::A64ExclusiveReadMemory8:
    case Opcode::A64ExclusiveReadMemory16:
    case Opcode::A64ExclusiveReadMemory32:
    case Opcode::A64ExclusiveReadMemory64:
    case Opcode::A64ExclusiveReadMemory128: {
        // ldxr: an atomic load at natural alignment that arms the monitor with its address, size and value (section 6.5)
        const unsigned bits = bits_of(op);
        const IR::AccType acc = inst.GetArg(2).GetAccType();
        Value* vaddr = Arg(inst, 1);
        auto* l = B.CreateAlignedLoad(B.getIntNTy(bits), GuestPtr(vaddr), llvm::Align(bits / 8), true);
        l->setAtomic(Ordered(acc) ? llvm::AtomicOrdering::Acquire : llvm::AtomicOrdering::Monotonic);
        Mark(l);
        StoreField(vaddr, offsetof(dm_guest_state, excl_addr));
        if (bits == 128) {
            StoreField(B.CreateTrunc(l, B.getInt64Ty()), offsetof(dm_guest_state, excl_val));
            StoreField(B.CreateTrunc(B.CreateLShr(l, 64), B.getInt64Ty()), offsetof(dm_guest_state, excl_val) + 8);
        } else {
            StoreField(B.CreateZExt(l, B.getInt64Ty()), offsetof(dm_guest_state, excl_val));
        }
        StoreField(B.getInt32(bits / 8), offsetof(dm_guest_state, excl_size));
        StoreField(B.getInt32(1), offsetof(dm_guest_state, excl_valid));
        Def(inst, l);
        return true;
    }
    case Opcode::A64ExclusiveWriteMemory8:
    case Opcode::A64ExclusiveWriteMemory16:
    case Opcode::A64ExclusiveWriteMemory32:
    case Opcode::A64ExclusiveWriteMemory64:
    case Opcode::A64ExclusiveWriteMemory128: {
        // stxr: fails (status 1) unless the monitor holds this address and size; then a compare-and-swap against the value the ldxr read
        // (section 6.5). The monitor is cleared either way, as Arm's local monitor and dynarmic's exclusive state are
        const unsigned bits = bits_of(op);
        llvm::Type* ty = B.getIntNTy(bits);
        const IR::AccType acc = inst.GetArg(3).GetAccType();
        Value* vaddr = Arg(inst, 1);
        Value* value = Arg(inst, 2);
        Value* valid = B.CreateICmpNE(LoadField(B.getInt32Ty(), offsetof(dm_guest_state, excl_valid)), B.getInt32(0));
        Value* same_addr = B.CreateICmpEQ(LoadField(B.getInt64Ty(), offsetof(dm_guest_state, excl_addr)), vaddr);
        Value* same_size = B.CreateICmpEQ(LoadField(B.getInt32Ty(), offsetof(dm_guest_state, excl_size)), B.getInt32(bits / 8));
        Value* armed = B.CreateAnd(valid, B.CreateAnd(same_addr, same_size));
        Value* expected = nullptr;
        if (bits == 128) {
            Value* lo = B.CreateZExt(LoadField(B.getInt64Ty(), offsetof(dm_guest_state, excl_val)), B.getInt128Ty());
            Value* hi = B.CreateZExt(LoadField(B.getInt64Ty(), offsetof(dm_guest_state, excl_val) + 8), B.getInt128Ty());
            expected = B.CreateOr(lo, B.CreateShl(hi, 64));
        } else {
            expected = B.CreateTrunc(LoadField(B.getInt64Ty(), offsetof(dm_guest_state, excl_val)), ty);
        }
        StoreField(B.getInt32(0), offsetof(dm_guest_state, excl_valid));
        BasicBlock* from = B.GetInsertBlock();
        auto* cas_bb = BasicBlock::Create(C, "stxr_cas", F);
        auto* done_bb = BasicBlock::Create(C, "stxr_done", F);
        B.CreateCondBr(armed, cas_bb, done_bb);
        B.SetInsertPoint(cas_bb);
        const auto order = Ordered(acc) ? llvm::AtomicOrdering::SequentiallyConsistent : llvm::AtomicOrdering::Monotonic;
        auto* cas = B.CreateAtomicCmpXchg(GuestPtr(vaddr), expected, value, llvm::MaybeAlign(bits / 8), order, llvm::AtomicOrdering::Monotonic);
        cas->setVolatile(true);
        Mark(cas);
        Value* status_cas = B.CreateZExt(B.CreateNot(B.CreateExtractValue(cas, 1)), B.getInt32Ty());
        B.CreateBr(done_bb);
        B.SetInsertPoint(done_bb);
        auto* status = B.CreatePHI(B.getInt32Ty(), 2);
        status->addIncoming(B.getInt32(1), from);
        status->addIncoming(status_cas, cas_bb);
        Def(inst, status);
        return true;
    }
    default:
        return false;
    }
}

}  // namespace

Lifted LiftRegion(llvm::Module& module, const std::string& name, const Region& region, const decode::CodeReader& read) {
    FunctionLifter lifter{module, region, read};
    return lifter.Run(name);
}

std::string DumpRegion(const Region& region, const decode::CodeReader& read) {
    llvm::LLVMContext context;
    llvm::Module module{"dump", context};
    FunctionLifter lifter{module, region, read};
    return lifter.Dump();
}

}  // namespace nxaot::lift
