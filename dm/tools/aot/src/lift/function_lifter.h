// The lifter's lowering of one region (src/lift/lifter.h), shared by its source files: lifter.cpp (regions, terminals, registers, flags, integer
// and memory operations), lower_fp.cpp (floating point, scalar and vector, with the NaN chains of aot-recompiler.md section 7.3), lower_vector.cpp
// (integer vector and saturating operations), lower_crypto.cpp (AES, SHA-256, SM4) and lower_calls.cpp (function mode: calls, returns, tail
// calls, the inline caches, host calls; sections 2.3, 3.3, 4). Internal to src/lift.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

#include "dynarmic/ir/acc_type.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/cond.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"
#include "lift/lifter.h"
#include "nxaot/guest_state.h"

namespace nxaot::lift::detail {

using llvm::BasicBlock;
using llvm::ConstantInt;
using llvm::Value;
namespace IR = Dynarmic::IR;

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

// A value of a NaN chain (section 7.3): an operation whose result is bit-identical on Arm and x86 unless it is a NaN. `fast` is the native
// result; `exact` rebuilds the Arm result from the exact values of the operands (on the cold path, after a check on an escaping value found a NaN)
struct ChainNode {
    std::vector<IR::Value> args;      // the operands of the IR instruction (the floating-point ones are looked up as chain values)
    std::vector<bool> fp_arg;         // which operands are floating-point values of the chain's width
    std::function<Value*(const std::vector<Value*>& exact_args)> exact;
    Value* fast = nullptr;
    llvm::Type* type = nullptr;       // float, double, <4 x float>, <2 x double>
    bool dn_select = false;           // with FPCR.DN, the exact result is the default NaN exactly where the fast one is a NaN
    bool exact_flags = false;         // its exact path (or a predecessor's) raises FPSR bits the fast one does not: with FPCR.DN too, an escape
                                      // that sees a NaN takes the cold block instead of the default-NaN select
    Value* escaped = nullptr;         // the exact value, once a use outside the chain asked for it
};

class FunctionLifter {
public:
    FunctionLifter(llvm::Module& module, const Region& region, const decode::CodeReader& read, const LiftOptions& options);

    Lifted Run(const std::string& name);
    std::string Dump();

    // ---- Phase 1: translate the region's blocks and check their opcodes ----
    void Discover(Lifted& out);
    static void Successors(const IR::Terminal& t, const IR::Block& block, std::vector<uint64_t>& out);
    bool InRegion(uint64_t pc) const { return pc >= region.lo && pc < region.hi; }

    // ---- Phase 2: lowering ----
    void Prologue();
    void Epilogue();
    void LowerBlock(IR::Block& block);
    void LowerInst(IR::Inst& inst);
    void LowerTerminal(const IR::Terminal& t);
    void Goto(Value* target);
    void Exit(Value* pc, uint32_t reason, Value* arg);
    Value* Opaque(llvm::Type* t);  // an unknown value of type t (the registers a call may change, Region::calls)
    bool LowerContext(IR::Inst& inst);
    bool LowerFlags(IR::Inst& inst);
    bool LowerInteger(IR::Inst& inst);
    bool LowerMemory(IR::Inst& inst);
    bool LowerVectorRegisters(IR::Inst& inst);
    bool LowerFP(IR::Inst& inst);         // lower_fp.cpp: scalar floating point
    bool LowerFPVector(IR::Inst& inst);   // lower_fp.cpp: vector floating point
    bool LowerVector(IR::Inst& inst);     // lower_vector.cpp
    bool LowerSaturated(IR::Inst& inst);  // lower_vector.cpp: scalar and vector saturating arithmetic
    bool LowerCrypto(IR::Inst& inst);     // lower_crypto.cpp

    // ---- Values ----
    Value* Get(const IR::Value& v);                  // the raw value; a chain value escapes (its exact value)
    Value* Arg(const IR::Inst& inst, size_t i);      // converted to the natural type of the IR argument (integers, <2 x i64> for U128)
    Value* ArgAs(const IR::Inst& inst, size_t i, llvm::Type* t);  // bit-cast to t (same size)
    const Flags& FlagsArg(const IR::Inst& inst, size_t i);
    void Def(const IR::Inst& inst, Value* v) { values[&inst] = v; }
    llvm::Type* TypeOf(IR::Type t);
    Value* As(Value* v, llvm::Type* t);              // bit-cast (or i128 <-> vector) to a type of the same size
    Value* Field(size_t offset) { return B.CreateConstInBoundsGEP1_64(B.getInt8Ty(), st, offset); }
    Value* LoadField(llvm::Type* ty, size_t offset) { return B.CreateAlignedLoad(ty, Field(offset), llvm::Align(ty->getPrimitiveSizeInBits() / 8)); }
    void StoreField(Value* v, size_t offset) { B.CreateAlignedStore(v, Field(offset), llvm::Align(v->getType()->getPrimitiveSizeInBits() / 8)); }
    Value* I(unsigned bits, uint64_t v) { return ConstantInt::get(B.getIntNTy(bits), v); }
    Value* SignBit(Value* v) { return B.CreateICmpSLT(v, llvm::Constant::getNullValue(v->getType())); }
    static unsigned Bits(Value* v) { return v->getType()->getScalarSizeInBits(); }
    llvm::Function* Intrinsic(llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Type*> types = {});
    Value* Call(llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Type*> types, llvm::ArrayRef<Value*> args);

    // Vector types: n lanes of an integer of bits, of a float of bits (16, 32, 64)
    llvm::FixedVectorType* VecI(unsigned bits) { return llvm::FixedVectorType::get(B.getIntNTy(bits), 128 / bits); }
    llvm::FixedVectorType* VecF(unsigned bits) { return llvm::FixedVectorType::get(FloatTy(bits), 128 / bits); }
    llvm::Type* FloatTy(unsigned bits);
    llvm::Type* V128() { return VecI(64); }
    Value* ArgVec(const IR::Inst& inst, size_t i, unsigned esize) { return ArgAs(inst, i, VecI(esize)); }
    Value* Splat(unsigned esize, uint64_t v) { return llvm::ConstantVector::getSplat(llvm::ElementCount::getFixed(128 / esize), ConstantInt::get(B.getIntNTy(esize), v)); }
    Value* SplatOf(Value* lanes_like, uint64_t v);  // a constant of v in every lane of the integer vector type of lanes_like (or the scalar)
    Value* Shuffle(Value* a, Value* b, llvm::ArrayRef<int> mask) { return B.CreateShuffleVector(a, b, mask); }
    Value* ZeroUpper(Value* v128);  // the low 64 bits kept, the high 64 zero

    // ---- Flags and conditions ----
    Flags CurrentFlags();
    void SetFlags(const Flags& f, const Origin& origin);
    Value* Cond(IR::Cond cond);
    Flags FlagsFromOp(const IR::Inst& producer, Origin* origin);
    Value* CarryOf(const IR::Inst& producer);
    Value* OverflowOf(const IR::Inst& producer);
    Value* PackNzcv(const Flags& f);
    Flags UnpackNzcv(Value* packed);

    // ---- Integer helpers ----
    Value* Shift(IR::Opcode op, Value* v, Value* amount);
    Value* Div(bool is_signed, Value* a, Value* b);
    Value* Crc32Iso(Value* crc, Value* data, unsigned bytes);
    Value* Crc32c(Value* crc, Value* data, unsigned bits);

    // ---- Memory helpers ----
    Value* GuestPtr(Value* vaddr) { return B.CreateIntToPtr(vaddr, B.getPtrTy()); }
    llvm::Instruction* Mark(llvm::Instruction* i);
    static bool Ordered(IR::AccType acc) { return acc == IR::AccType::ORDERED || acc == IR::AccType::ORDEREDRW || acc == IR::AccType::LIMITEDORDERED; }

    // ---- FPSR: QC and the cumulative exception bits the lowering raises itself (section 7.4) ----
    void OrQC(Value* saturated);                 // i1
    void OrFpsrBits(Value* bits);                // i32 of FPSR cumulative bits (IOC 1, DZC 2, OFC 4, UFC 8, IXC 0x10, IDC 0x80)
    Value* FpsrPtr() { return Field(offsetof(dm_guest_state, fpsr)); }
    void FlushFpsr();                            // the accumulated QC and bits into st->fpsr (before a helper call, an FPSR read, an exit)
    Value* MxcsrFlags();                         // the native operations' flags as FPSR bits (stmxcsr)
    void SetMxcsr(Value* fpcr, bool clear_flags);

    // ---- Floating point (lower_fp.cpp) ----
    uint32_t Fpcr() const { return fpcr; }
    bool DN() const { return (fpcr >> 25) & 1; }
    bool FZ() const { return (fpcr >> 24) & 1; }
    bool AHP() const { return (fpcr >> 26) & 1; }
    bool FZ16() const { return (fpcr >> 19) & 1; }
    unsigned RMode() const { return (fpcr >> 22) & 3; }
    Value* FArg(const IR::Inst& inst, size_t i, unsigned fsize);        // an exact scalar float operand
    Value* FVecArg(const IR::Inst& inst, size_t i, unsigned fsize);     // an exact float vector operand
    Value* ChainArg(const IR::Inst& inst, size_t i, llvm::Type* t);     // the fast value of a chain operand (a P operation or a compare)
    void DefChain(const IR::Inst& inst, ChainNode node);
    Value* Escape(ChainNode& node);
    Value* Exact(const IR::Inst* inst, std::unordered_map<const IR::Inst*, Value*>& memo);
    bool IsChain(const IR::Value& v) const { return !v.IsImmediate() && chain.count(v.GetInst()); }
    // Integer-domain classification of float bits (no FP flags): t is an integer or integer vector type of the float's width
    Value* IsNaN(Value* bits);
    Value* IsSNaN(Value* bits);
    Value* IsQNaN(Value* bits);
    Value* IsDenormal(Value* bits);
    Value* IsZeroF(Value* bits);
    Value* IsInfF(Value* bits);
    Value* Quiet(Value* bits);
    Value* DefaultNaN(llvm::Type* int_type);
    Value* FlushDenormal(Value* bits);           // FPCR.FZ: a denormal becomes a zero of its sign
    Value* NaN2(Value* a, Value* b, Value* r);   // Arm's FPProcessNaNs for two operands, else r (with FPCR.DN the default NaN), on integer bits
    Value* NaN3(Value* a, Value* b, Value* c, Value* r);
    // FPMulAdd's invalid case: a quiet NaN addend with infinity times zero (zero as FPCR.FZ reads it), per lane; raises IOC when the flags are
    // modelled (x86's FMA returns the quiet NaN and raises nothing)
    Value* MulAddInvalid(Value* addend_bits, Value* op1_bits, Value* op2_bits);
    Value* NaN1(Value* a, Value* r);
    Value* NaNResult(Value* r);                  // r, or the default NaN where r is a NaN (an invalid operation)
    Value* AnyLane(Value* i1_or_vector);
    Value* AsInt(Value* fp);                     // float bits as the integer (vector) type of the same width
    Value* AsFloat(Value* bits);
    llvm::RoundingMode RoundingOf(unsigned arm_rounding);
    Value* RoundIntegral(Value* x, unsigned arm_rounding);   // to integral, no Inexact raised (floor, ceil, trunc, roundeven, ties away)
    Value* HelperCall(const char* name, llvm::Type* ret, llvm::ArrayRef<Value*> args);
    Value* PerLane(Value* vec_bits, unsigned esize, const std::function<Value*(Value* lane)>& fn);
    // The exact lowerings shared by the scalar and vector forms; operands and results are integer bits (scalars or vectors)
    Value* MinMax(bool is_max, bool numeric, Value* a, Value* b);
    Value* StepFused(bool rsqrt, unsigned bits, Value* a, Value* b);
    Value* RoundInt(unsigned bits, Value* x, unsigned rounding, bool exact);
    Value* ToFixed(Value* x, unsigned fsize, unsigned isize, bool is_unsigned, unsigned fbits, unsigned rounding);
    Value* FromFixed(Value* x, unsigned isize, bool is_unsigned, unsigned to, unsigned fbits);  // a float value

    llvm::Module& M;
    llvm::LLVMContext& C;
    llvm::IRBuilder<> B;
    const Region& region;
    const decode::CodeReader& read;
    const LiftOptions opts;
    const uint32_t fpcr;  // the FPCR the region is specialised for, as dynarmic's location descriptor keeps it (mode bits only)

    std::map<uint64_t, IR::Block> blocks;
    std::map<uint64_t, BasicBlock*> bbs;
    std::map<uint64_t, CallEffect> call_effects;  // blocks ending in a call the region continues past (Region::calls), by block start
    std::map<uint64_t, std::vector<uint64_t>> table_targets;  // blocks ending in a br with a known jump table (Region::tables), by block start
    llvm::Function* F = nullptr;
    Value* st = nullptr;
    BasicBlock* exit_bb = nullptr;
    std::array<llvm::AllocaInst*, 31> x{};
    std::array<llvm::AllocaInst*, 32> vreg{};
    llvm::AllocaInst* sp = nullptr;
    std::array<llvm::AllocaInst*, 4> flag{};  // N, Z, C, V
    llvm::AllocaInst* check_bit = nullptr;
    llvm::AllocaInst* qc = nullptr;           // i1: saturation since the last flush
    llvm::AllocaInst* fpsr_bits = nullptr;    // i32: cumulative bits the lowering raised since the last flush
    std::array<bool, 31> wrote_x{};
    std::array<bool, 32> wrote_v{};
    bool wrote_sp = false, wrote_nzcv = false, uses_fpsr = false;

    // Per block
    std::unordered_map<const IR::Inst*, Value*> values;
    std::unordered_map<const IR::Inst*, Flags> flag_values;
    std::unordered_map<const IR::Inst*, Origin> origins;
    std::unordered_map<const IR::Inst*, ChainNode> chain;
    std::unordered_map<const IR::Inst*, std::vector<const IR::Inst*>> users;
    Origin cur_origin;
    uint64_t cur_end = 0;                   // the pc after the block's last instruction
    const CallEffect* cur_call = nullptr;   // the block's call effects, when it ends in a call the region continues past
    const std::vector<uint64_t>* cur_table = nullptr;  // the block's jump-table targets, when it ends in a br with a known table
    Value* last_pc = nullptr;
    Value* new_fpcr = nullptr;  // the value an msr fpcr of the block wrote (its terminal checks it)
    bool terminated = false;
    std::string error;
    std::set<int> issues;            // DynarmicIssue values met (Lifted::dynarmic_issues)
    void Note(int issue) { issues.insert(issue); }
    // Arm's Input Denormal (IDC) under FPCR.FZ for the operands of an operation that unpacks them (FPUnpack): x86's DAZ flushes silently
    void InputDenormal(std::initializer_list<Value*> operand_bits);
    // The 64-bit vector forms (2S, 4H): the result's only uses keep its low 64 bits (VectorZeroUpper, SetD), so the upper lanes, zeros from
    // GetD, are computed by nobody on Arm and must raise no exception here
    bool LowHalfOnly(const IR::Inst& inst) const;

    // ---- Function mode (lower_calls.cpp; LiftOptions::plan, lifter.h) ----
    const FunctionPlan* plan = nullptr;
    bool Fn() const { return plan != nullptr; }
    enum class EndKind : uint8_t { Other, Bl, Blr, Ret, Br };  // how a block's last guest instruction leaves it
    struct End {
        EndKind kind = EndKind::Other;
        uint64_t target = 0;  // Bl: the callee's offset
    };
    std::map<uint64_t, End> ends;                       // per block start
    std::map<uint64_t, std::vector<uint64_t>> succs;    // per block start: the successors inside the function
    std::map<uint64_t, Regs> gen;                       // per block start: the registers it writes
    std::map<uint64_t, Regs> dirty_in;                  // per block start: the registers newer than their copy in the block at its start
    std::set<uint64_t> svc_blocks;                      // blocks that end in svc (it stores the whole register file)
    std::set<std::pair<const IR::Inst*, size_t>> relative;  // immediate arguments that are module offsets (base-relative addresses)
    decode::InstPcs inst_pcs;                           // the guest offset each IR instruction came from
    std::unordered_map<Value*, uint64_t> rel_values;    // the base + offset values made for relative immediates, with their offsets
    Value* base = nullptr;                              // the module's base address, loaded once at the entry
    Value* entry_lr = nullptr;                          // the lr the function was entered with (section 3.3)
    Regs cur_dirty;                                     // the block's dirty set at its end (dirty_in | gen)
    Regs restore_block;                                 // restored registers whose block copy the function changed: stored again at returns
    const IR::Inst* cur_inst = nullptr;                 // the instruction being lowered (relative immediates, line table, debug locations)
    uint64_t cur_start = 0;                             // the block being lowered
    Lifted* stats = nullptr;
    unsigned lines_ = 0;

    void FnClassify(uint64_t pc, const IR::Block& block, std::vector<uint64_t>& next);  // Discover: the block's end and successors
    void FnRelative(uint64_t pc, IR::Block& block, const std::function<void(IR::Block&)>& passes);  // the base-relative immediates
    void FnDataflow();
    void FnPrologue();
    void FnLowerTerminal(const IR::Terminal& t);
    void FnGoto(Value* target);
    void FnExit(Value* pc, uint32_t reason);
    Value* Rel(uint64_t offset);                 // base + offset, remembered as an offset
    std::optional<uint64_t> OffsetOf(Value* v);  // the module offset of a constant or base + offset value
    uint64_t CurOffset() const;                  // the guest offset of the instruction being lowered
    void LineEntry(llvm::Instruction* i, uint64_t offset);  // !pcsections: a line-table entry for i (section 3.5)
    void StoreRegs(const Regs& r);
    void ReloadRegs(const Regs& r);
    std::vector<Value*> CallArgs(const CalleeView& c, llvm::FunctionType* type, Value* lr);
    llvm::CallInst* EmitCall(const CalleeView& c, llvm::FunctionType* type, Value* callee, Value* lr, bool tail);
    void FnCall(const CalleeView& c, llvm::FunctionType* type, Value* callee, uint64_t call_pc);
    void FnCallDirect(uint64_t target, uint64_t call_pc);
    void FnCallIndirect(Value* target, uint64_t call_pc);
    void FnTail(const CalleeView& c, llvm::FunctionType* type, Value* callee);
    void FnTailDirect(uint64_t target);
    void FnTailIndirect(Value* target, uint64_t site);
    void FnReturn(Value* target);
    void FnTrap(Value* pc, uint32_t reason);
    void FnSvc(Value* number, Value* next_pc);
    Value* FnDispatch(Value* target, uint64_t site);  // the inline cache: the native standard entry of a guest address
    llvm::Value* RetValue();                          // {x0, x1, v0..v3} of the current registers (poison outside self.rv)
};

}  // namespace nxaot::lift::detail
