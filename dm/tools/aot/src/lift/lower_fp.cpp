// Floating point, scalar and vector (aot-recompiler.md section 7). The arithmetic is the native x86 operation under the MXCSR of the
// specialised FPCR (7.1); Arm's NaN rules (7.2 rows 1 to 5) come from integer operations on the bits, never from LLVM's NaN-agnostic intrinsics.
//
// NaN chains (7.3). The P operations (add, subtract, multiply, divide, square root, fused multiply-add with FMA3 and FZ = 0, negate, absolute
// value, conversions between single and double precision, pairwise add) give bit-identical results on both ISAs unless the result is a NaN. Each
// is a ChainNode: its native result (`fast`) and how to rebuild the Arm result from the exact operands (`exact`). A use outside a chain (a
// register write, a store, an integer or another floating-point operation that keeps NaN bits) takes the node's escaped value (Escape): one NaN
// check of the fast value; when it fires, a cold block recomputes the chain from its inputs with Arm's rules. Compares and conversions to integer
// read fast values directly: every NaN compares and converts the same way. With FPCR.DN, an arithmetic node escapes branch-free (the default NaN
// where the fast value is a NaN). NaNMode::PerOp checks every operation instead (the reference the chains are measured against).
//
// The other operations (FMAX / FMIN and their numeric forms, FMULX, the reciprocal steps, rounding to integral, conversions to and from
// integers) have their own exact lowering; the estimates, the half-precision arithmetic and the conversions x86 has no instruction for call the
// helpers of lift/runtime.h. Exception flags (7.4): the native operations raise them in MXCSR; operations lowered in integers raise none and
// add Arm's bits explicitly (OrFpsrBits) where Arm raises some; helpers OR theirs into st->fpsr.
#include <llvm/IR/Constants.h>
#include <llvm/IR/FPEnv.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsX86.h>
#include <llvm/IR/MDBuilder.h>

#include "dynarmic/common/fp/rounding_mode.h"
#include "lift/function_lifter.h"
#include "lift/opcodes.h"
#include "lift/runtime.h"

namespace nxaot::lift::detail {

using IR::Opcode;
namespace ID = llvm::Intrinsic;

namespace {

constexpr uint32_t kIOC = 0x01, kIXC = 0x10;
[[maybe_unused]] constexpr uint32_t kIDC = 0x80;

struct FInfo {
    unsigned bits, mant;
    uint64_t sign, exp, quiet, mant_mask;
};

FInfo Info(unsigned bits) {
    const unsigned mant = bits == 16 ? 10 : bits == 32 ? 23 : 52;
    const uint64_t sign = 1ull << (bits - 1);
    const uint64_t mant_mask = (1ull << mant) - 1;
    const uint64_t exp = (sign - 1) & ~mant_mask;
    return {bits, mant, sign, exp, 1ull << (mant - 1), mant_mask};
}

// The bits of a power of two at a precision
uint64_t Pow2Bits(unsigned bits, int e) {
    const FInfo f = Info(bits);
    const int bias = bits == 16 ? 15 : bits == 32 ? 127 : 1023;
    return static_cast<uint64_t>(e + bias) << f.mant;
}

unsigned ScalarBits(Value* v) {
    return v->getType()->getScalarSizeInBits();
}

}  // namespace

// ---- Classification on the bits ----

Value* FunctionLifter::AsInt(Value* fp) {
    llvm::Type* t = fp->getType();
    if (t->isIntOrIntVectorTy()) {
        return fp;
    }
    if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(t)) {
        return B.CreateBitCast(fp, llvm::FixedVectorType::get(B.getIntNTy(vt->getScalarSizeInBits()), vt->getNumElements()));
    }
    return B.CreateBitCast(fp, B.getIntNTy(t->getPrimitiveSizeInBits()));
}

Value* FunctionLifter::AsFloat(Value* bits) {
    llvm::Type* t = bits->getType();
    if (t->isFPOrFPVectorTy()) {
        return bits;
    }
    if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(t)) {
        return B.CreateBitCast(bits, llvm::FixedVectorType::get(FloatTy(vt->getScalarSizeInBits()), vt->getNumElements()));
    }
    return B.CreateBitCast(bits, FloatTy(t->getPrimitiveSizeInBits()));
}

Value* FunctionLifter::IsNaN(Value* bits) {
    const FInfo f = Info(ScalarBits(bits));
    return B.CreateICmpUGT(B.CreateAnd(bits, SplatOf(bits, ~f.sign)), SplatOf(bits, f.exp));
}

Value* FunctionLifter::IsQNaN(Value* bits) {
    const FInfo f = Info(ScalarBits(bits));
    return B.CreateICmpUGE(B.CreateAnd(bits, SplatOf(bits, ~f.sign)), SplatOf(bits, f.exp | f.quiet));
}

Value* FunctionLifter::IsSNaN(Value* bits) {
    return B.CreateAnd(IsNaN(bits), B.CreateNot(IsQNaN(bits)));
}

Value* FunctionLifter::IsInfF(Value* bits) {
    const FInfo f = Info(ScalarBits(bits));
    return B.CreateICmpEQ(B.CreateAnd(bits, SplatOf(bits, ~f.sign)), SplatOf(bits, f.exp));
}

Value* FunctionLifter::IsZeroF(Value* bits) {
    const FInfo f = Info(ScalarBits(bits));
    return B.CreateICmpEQ(B.CreateAnd(bits, SplatOf(bits, ~f.sign)), SplatOf(bits, 0));
}

Value* FunctionLifter::IsDenormal(Value* bits) {
    const FInfo f = Info(ScalarBits(bits));
    Value* zero_exp = B.CreateICmpEQ(B.CreateAnd(bits, SplatOf(bits, f.exp)), SplatOf(bits, 0));
    Value* some_mant = B.CreateICmpNE(B.CreateAnd(bits, SplatOf(bits, f.mant_mask)), SplatOf(bits, 0));
    return B.CreateAnd(zero_exp, some_mant);
}

Value* FunctionLifter::Quiet(Value* bits) {
    return B.CreateOr(bits, SplatOf(bits, Info(ScalarBits(bits)).quiet));
}

Value* FunctionLifter::DefaultNaN(llvm::Type* int_type) {
    const unsigned bits = int_type->getScalarSizeInBits();
    const FInfo f = Info(bits);
    Value* c = ConstantInt::get(B.getIntNTy(bits), f.exp | f.quiet);
    if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(int_type)) {
        return llvm::ConstantVector::getSplat(vt->getElementCount(), llvm::cast<llvm::Constant>(c));
    }
    return c;
}

Value* FunctionLifter::FlushDenormal(Value* bits) {
    const FInfo f = Info(ScalarBits(bits));
    return B.CreateSelect(IsDenormal(bits), B.CreateAnd(bits, SplatOf(bits, f.sign)), bits);
}

Value* FunctionLifter::NaNResult(Value* r) {
    return B.CreateSelect(IsNaN(r), DefaultNaN(r->getType()), r);
}

Value* FunctionLifter::NaN1(Value* a, Value* r) {
    if (DN()) {
        return NaNResult(r);
    }
    return B.CreateSelect(IsNaN(a), Quiet(a), NaNResult(r));
}

Value* FunctionLifter::NaN2(Value* a, Value* b, Value* r) {
    if (DN()) {
        return B.CreateSelect(B.CreateOr(B.CreateOr(IsNaN(a), IsNaN(b)), IsNaN(r)), DefaultNaN(r->getType()), r);
    }
    // FPProcessNaNs: the first signalling NaN, else the first quiet NaN, quietened; else r (the default NaN for an invalid operation)
    Value* res = NaNResult(r);
    res = B.CreateSelect(IsNaN(b), Quiet(b), res);
    res = B.CreateSelect(IsNaN(a), Quiet(a), res);
    return B.CreateSelect(B.CreateAnd(IsSNaN(b), B.CreateNot(IsSNaN(a))), Quiet(b), res);
}

Value* FunctionLifter::NaN3(Value* a, Value* b, Value* c, Value* r) {
    if (DN()) {
        return B.CreateSelect(B.CreateOr(B.CreateOr(IsNaN(a), IsNaN(b)), B.CreateOr(IsNaN(c), IsNaN(r))), DefaultNaN(r->getType()), r);
    }
    // FPProcessNaNs3: signalling NaNs first, in operand order, then quiet NaNs in operand order
    Value* res = NaNResult(r);
    res = B.CreateSelect(IsNaN(c), Quiet(c), res);
    res = B.CreateSelect(IsNaN(b), Quiet(b), res);
    res = B.CreateSelect(IsNaN(a), Quiet(a), res);
    Value* sa = IsSNaN(a);
    Value* sb = IsSNaN(b);
    Value* sc = IsSNaN(c);
    Value* first_s = B.CreateSelect(sa, Quiet(a), B.CreateSelect(sb, Quiet(b), Quiet(c)));
    return B.CreateSelect(B.CreateOr(sa, B.CreateOr(sb, sc)), first_s, res);
}

Value* FunctionLifter::MulAddInvalid(Value* a, Value* m1, Value* m2) {
    auto zero = [this](Value* bits) { return FZ() ? B.CreateOr(IsZeroF(bits), IsDenormal(bits)) : IsZeroF(bits); };
    Value* infzero = B.CreateOr(B.CreateAnd(IsInfF(m1), zero(m2)), B.CreateAnd(zero(m1), IsInfF(m2)));
    Value* invalid = B.CreateAnd(IsQNaN(a), infzero);
    if (opts.fpsr_flags) {
        OrFpsrBits(B.CreateSelect(AnyLane(invalid), B.getInt32(0x01), B.getInt32(0)));
    }
    return invalid;
}

llvm::RoundingMode FunctionLifter::RoundingOf(unsigned arm_rounding) {
    switch (arm_rounding) {
    case 1:
        return llvm::RoundingMode::TowardPositive;
    case 2:
        return llvm::RoundingMode::TowardNegative;
    case 3:
        return llvm::RoundingMode::TowardZero;
    case 4:
        return llvm::RoundingMode::NearestTiesToAway;
    default:
        return llvm::RoundingMode::NearestTiesToEven;
    }
}

namespace {

// An FP intrinsic in its constrained form when the builder is constrained (the function is strictfp), else the plain one
Value* FPIntrinsic(llvm::IRBuilder<>& B, llvm::Module& M, ID::ID plain, ID::ID constrained, llvm::ArrayRef<Value*> args) {
    llvm::Type* t = args[0]->getType();
    if (B.getIsFPConstrained()) {
        return B.CreateConstrainedFPCall(ID::getDeclaration(&M, constrained, {t}), args);
    }
    return B.CreateCall(ID::getDeclaration(&M, plain, {t}), args);
}

}  // namespace

Value* FunctionLifter::RoundIntegral(Value* x, unsigned arm_rounding) {
    // To integral in a fixed mode without raising Inexact (Arm's FPRoundInt with exact = FALSE); x86's roundss with the precision exception
    // suppressed, through LLVM's floor / ceil / trunc / roundeven. Ties away from zero has no x86 mode: trunc, then the exact difference
    // decides (x - trunc(x) is exact; magnitudes of 2^mantissa and more, infinities and NaNs are integral already and skip the subtraction)
    switch (arm_rounding) {
    case 0:
        return FPIntrinsic(B, M, ID::roundeven, ID::experimental_constrained_roundeven, {x});
    case 1:
        return FPIntrinsic(B, M, ID::ceil, ID::experimental_constrained_ceil, {x});
    case 2:
        return FPIntrinsic(B, M, ID::floor, ID::experimental_constrained_floor, {x});
    case 3:
        return FPIntrinsic(B, M, ID::trunc, ID::experimental_constrained_trunc, {x});
    default: {
        const FInfo f = Info(ScalarBits(x));
        Value* bits = AsInt(x);
        Value* mag = B.CreateAnd(bits, SplatOf(bits, ~f.sign));
        Value* big = B.CreateICmpUGE(mag, SplatOf(bits, Pow2Bits(f.bits, static_cast<int>(f.mant))));
        Value* safe = AsFloat(B.CreateSelect(big, SplatOf(bits, 0), bits));
        Value* t = FPIntrinsic(B, M, ID::trunc, ID::experimental_constrained_trunc, {safe});
        Value* d = AsInt(B.CreateFSub(safe, t));
        Value* half = SplatOf(bits, Pow2Bits(f.bits, -1));
        Value* ge_half = B.CreateICmpUGE(B.CreateAnd(d, SplatOf(bits, ~f.sign)), half);
        Value* one = B.CreateOr(B.CreateAnd(bits, SplatOf(bits, f.sign)), SplatOf(bits, Pow2Bits(f.bits, 0)));
        Value* away = B.CreateFAdd(t, AsFloat(one));
        Value* small_r = B.CreateSelect(ge_half, away, t);
        Value* big_r = FPIntrinsic(B, M, ID::trunc, ID::experimental_constrained_trunc, {x});
        return B.CreateSelect(big, big_r, small_r);
    }
    }
}

void FunctionLifter::InputDenormal(std::initializer_list<Value*> operand_bits) {
    if (!FZ() || !opts.fpsr_flags) {
        return;
    }
    Value* any = nullptr;
    for (Value* v : operand_bits) {
        Value* d = AnyLane(IsDenormal(AsInt(v)));
        any = any ? B.CreateOr(any, d) : d;
    }
    OrFpsrBits(B.CreateSelect(any, B.getInt32(0x80), B.getInt32(0)));
    Note(kIssueNoInputDenormal);
}

bool FunctionLifter::LowHalfOnly(const IR::Inst& inst) const {
    const auto it = users.find(&inst);
    if (it == users.end() || it->second.empty()) {
        return false;
    }
    for (const IR::Inst* u : it->second) {
        if (u->GetOpcode() != Opcode::VectorZeroUpper && u->GetOpcode() != Opcode::A64SetD) {
            return false;
        }
    }
    return true;
}

// ---- Operands and chains ----

Value* FunctionLifter::FArg(const IR::Inst& inst, size_t i, unsigned fsize) {
    return As(Get(inst.GetArg(i)), FloatTy(fsize));
}

Value* FunctionLifter::FVecArg(const IR::Inst& inst, size_t i, unsigned fsize) {
    return As(Get(inst.GetArg(i)), VecF(fsize));
}

Value* FunctionLifter::ChainArg(const IR::Inst& inst, size_t i, llvm::Type* t) {
    const IR::Value a = inst.GetArg(i);
    if (IsChain(a)) {
        ChainNode& node = chain.at(a.GetInst());
        if (opts.nan_mode != NaNMode::Chains) {
            return As(Escape(node), t);
        }
        return As(node.fast, t);
    }
    return As(Get(a), t);
}

void FunctionLifter::DefChain(const IR::Inst& inst, ChainNode node) {
    for (size_t i = 0; i < node.args.size(); ++i) {
        if (node.fp_arg[i] && IsChain(node.args[i])) {
            node.exact_flags |= chain.at(node.args[i].GetInst()).exact_flags;
        }
    }
    auto [it, inserted] = chain.insert_or_assign(&inst, std::move(node));
    values[&inst] = it->second.fast;
    if (opts.nan_mode == NaNMode::PerOp) {
        Escape(it->second);
    }
}

Value* FunctionLifter::Escape(ChainNode& node) {
    if (node.escaped) {
        return node.escaped;
    }
    if (opts.nan_mode == NaNMode::Unchecked) {
        node.escaped = node.fast;
        return node.escaped;
    }
    Value* fast_bits = AsInt(node.fast);
    if (DN() && node.dn_select && !node.exact_flags) {
        node.escaped = AsFloat(NaNResult(fast_bits));
        return node.escaped;
    }
    // One check; the cold block recomputes the chain with Arm's rules from the operands' exact values. An arithmetic result is never a
    // signalling NaN, so the quiet compare (ucomiss / vcmpunordps) raises nothing; fabs / fneg can pass one through, and are tested on the bits
    Value* nan = node.dn_select ? AnyLane(B.CreateFCmp(llvm::CmpInst::FCMP_UNO, node.fast, node.fast)) : AnyLane(IsNaN(fast_bits));
    BasicBlock* from = B.GetInsertBlock();
    auto* slow = BasicBlock::Create(C, "nan_chain", F);
    auto* cont = BasicBlock::Create(C, "chain_ok", F);
    llvm::MDBuilder md{C};
    B.CreateCondBr(nan, slow, cont, md.createBranchWeights(1, 1 << 20));
    B.SetInsertPoint(slow);
    std::unordered_map<const IR::Inst*, Value*> memo;
    const IR::Inst* self = nullptr;
    for (const auto& [inst, n] : chain) {
        if (&n == &node) {
            self = inst;
            break;
        }
    }
    Value* exact = As(Exact(self, memo), node.fast->getType());
    BasicBlock* slow_end = B.GetInsertBlock();
    B.CreateBr(cont);
    B.SetInsertPoint(cont);
    auto* phi = B.CreatePHI(node.fast->getType(), 2);
    phi->addIncoming(node.fast, from);
    phi->addIncoming(exact, slow_end);
    node.escaped = phi;
    return phi;
}

Value* FunctionLifter::Exact(const IR::Inst* inst, std::unordered_map<const IR::Inst*, Value*>& memo) {
    const auto m = memo.find(inst);
    if (m != memo.end()) {
        return m->second;
    }
    ChainNode& node = chain.at(inst);
    std::vector<Value*> args;
    for (size_t i = 0; i < node.args.size(); ++i) {
        const IR::Value a = node.args[i];
        if (node.fp_arg[i] && IsChain(a)) {
            args.push_back(Exact(a.GetInst(), memo));
        } else if (node.fp_arg[i]) {
            const auto it = values.find(a.GetInst());
            args.push_back(a.IsImmediate() || it == values.end() ? Get(a) : it->second);
        } else {
            args.push_back(nullptr);
        }
    }
    Value* r = node.exact(args);
    memo[inst] = r;
    return r;
}

// ---- Scalar floating point ----

bool FunctionLifter::LowerFP(IR::Inst& inst) {
    const Opcode op = inst.GetOpcode();
    const std::string name = OpcodeName(op);
    auto fsize_of = [&]() -> unsigned { return name.back() == '6' ? 16 : name.back() == '2' ? 32 : 64; };  // ...16, ...32, ...64
    Value* fpcr_v = B.getInt32(fpcr);

    // A chain node of a binary P operation (scalar or vector): fast native result now, Arm NaN rules on the exact operands later
    auto binary_p = [&](llvm::Type* t, std::function<Value*(Value*, Value*)> fn) {
        ChainNode node;
        node.args = {inst.GetArg(0), inst.GetArg(1)};
        node.fp_arg = {true, true};
        node.type = t;
        node.dn_select = true;
        Value* a = ChainArg(inst, 0, t);
        Value* b = ChainArg(inst, 1, t);
        InputDenormal({a, b});
        node.fast = fn(a, b);
        node.exact = [this, t, fn](const std::vector<Value*>& a) {
            Value* x = As(a[0], t);
            Value* y = As(a[1], t);
            Value* r = fn(x, y);
            return AsFloat(NaN2(AsInt(x), AsInt(y), AsInt(r)));
        };
        DefChain(inst, std::move(node));
    };
    auto unary_p = [&](llvm::Type* t, std::function<Value*(Value*)> fn, bool nan_rule, bool dn_select) {
        ChainNode node;
        node.args = {inst.GetArg(0)};
        node.fp_arg = {true};
        node.type = t;
        node.dn_select = dn_select;
        Value* a = ChainArg(inst, 0, t);
        if (nan_rule) {
            InputDenormal({a});  // fsqrt unpacks its operand; fabs / fneg do not
        }
        node.fast = fn(a);
        node.exact = [this, t, fn, nan_rule](const std::vector<Value*>& a) {
            Value* x = As(a[0], t);
            Value* r = fn(x);
            if (!nan_rule) {
                return r;  // fabs / fneg: bit operations, exact for NaNs too
            }
            return AsFloat(NaN1(AsInt(x), AsInt(r)));
        };
        DefChain(inst, std::move(node));
    };
    auto fpsr_ptr = [&]() { return FpsrPtr(); };

    switch (op) {
    case Opcode::FPAdd32:
    case Opcode::FPAdd64:
        binary_p(FloatTy(fsize_of()), [this](Value* a, Value* b) { return B.CreateFAdd(a, b); });
        return true;
    case Opcode::FPSub32:
    case Opcode::FPSub64:
        binary_p(FloatTy(fsize_of()), [this](Value* a, Value* b) { return B.CreateFSub(a, b); });
        return true;
    case Opcode::FPMul32:
    case Opcode::FPMul64:
        binary_p(FloatTy(fsize_of()), [this](Value* a, Value* b) { return B.CreateFMul(a, b); });
        return true;
    case Opcode::FPDiv32:
    case Opcode::FPDiv64:
        binary_p(FloatTy(fsize_of()), [this](Value* a, Value* b) { return B.CreateFDiv(a, b); });
        return true;
    case Opcode::FPSqrt32:
    case Opcode::FPSqrt64:
        unary_p(
            FloatTy(fsize_of()), [this](Value* a) { return FPIntrinsic(B, M, ID::sqrt, ID::experimental_constrained_sqrt, {a}); }, true, true);
        return true;
    case Opcode::FPAbs32:
    case Opcode::FPAbs64:
    case Opcode::FPNeg32:
    case Opcode::FPNeg64: {
        const bool neg = name.rfind("FPNeg", 0) == 0;
        const unsigned bits = fsize_of();
        unary_p(
            FloatTy(bits),
            [this, neg, bits](Value* a) {
                const FInfo f = Info(bits);
                Value* i = AsInt(a);
                return AsFloat(neg ? B.CreateXor(i, SplatOf(i, f.sign)) : B.CreateAnd(i, SplatOf(i, ~f.sign)));
            },
            false, false);
        return true;
    }
    case Opcode::FPAbs16:
        Def(inst, B.CreateAnd(Arg(inst, 0), B.getInt16(0x7FFF)));
        return true;
    case Opcode::FPNeg16:
        Def(inst, B.CreateXor(Arg(inst, 0), B.getInt16(0x8000)));
        return true;

    case Opcode::FPMulAdd32:
    case Opcode::FPMulAdd64:
    case Opcode::FPMulSub32:
    case Opcode::FPMulSub64: {
        // FMADD / FMSUB (addend + op1 * op2, minuend - op1 * op2: dynarmic negates op1 first, so a NaN op1 comes back negated), rounded once
        const unsigned bits = fsize_of();
        const bool sub = name.rfind("FPMulSub", 0) == 0;
        llvm::Type* t = FloatTy(bits);
        const FInfo f = Info(bits);
        auto native = [this, sub, f](Value* addend, Value* op1, Value* op2) {
            Value* m1 = sub ? AsFloat(B.CreateXor(AsInt(op1), SplatOf(AsInt(op1), f.sign))) : op1;
            return FPIntrinsic(B, M, ID::fma, ID::experimental_constrained_fma, {m1, op2, addend});
        };
        auto exact = [this, sub, f](Value* addend, Value* op1, Value* op2, Value* r) {
            Value* a = AsInt(addend);
            Value* m1 = AsInt(op1);
            if (sub) {
                m1 = B.CreateXor(m1, SplatOf(m1, f.sign));
            }
            Value* m2 = AsInt(op2);
            Value* res = NaN3(a, m1, m2, AsInt(r));
            // Arm: a quiet NaN addend with infinity times zero is the default NaN, with Invalid Operation
            return AsFloat(B.CreateSelect(MulAddInvalid(a, m1, m2), DefaultNaN(a->getType()), res));
        };
        if (DN()) {
            Note(kIssueMulAddInfZero);
        }
        if (!opts.target.fma) {
            Value* r = HelperCall("nxaot_rt_fp_muladd", B.getIntNTy(64),
                                  {B.getInt32(bits), B.getInt32(sub ? 1 : 0), B.CreateZExt(ArgAs(inst, 0, B.getIntNTy(bits)), B.getInt64Ty()),
                                   B.CreateZExt(ArgAs(inst, 1, B.getIntNTy(bits)), B.getInt64Ty()),
                                   B.CreateZExt(ArgAs(inst, 2, B.getIntNTy(bits)), B.getInt64Ty()), fpcr_v, fpsr_ptr()});
            Def(inst, B.CreateTrunc(r, B.getIntNTy(bits)));
            return true;
        }
        if (!DN()) {
            Note(kIssueMulAddNaNProduct);
        }
        if (!FZ()) {
            ChainNode node;
            node.args = {inst.GetArg(0), inst.GetArg(1), inst.GetArg(2)};
            node.fp_arg = {true, true, true};
            node.type = t;
            node.dn_select = true;
            node.exact_flags = opts.fpsr_flags;
            node.fast = native(ChainArg(inst, 0, t), ChainArg(inst, 1, t), ChainArg(inst, 2, t));
            node.exact = [native, exact, t, this](const std::vector<Value*>& a) {
                Value* x = As(a[0], t);
                Value* y = As(a[1], t);
                Value* z = As(a[2], t);
                return exact(x, y, z, native(x, y, z));
            };
            DefChain(inst, std::move(node));
            return true;
        }
        // FPCR.FZ: x86 flushes after rounding, Arm before (7.2 row 13). dynarmic's check: a result of +-(smallest normal), or a NaN, is
        // computed again by the software FPMulAdd (its flags added to the native operation's)
        Value* addend = FArg(inst, 0, bits);
        Value* op1 = FArg(inst, 1, bits);
        Value* op2 = FArg(inst, 2, bits);
        InputDenormal({addend, op1, op2});
        Value* r = native(addend, op1, op2);
        Value* ri = AsInt(r);
        Value* tiny = B.CreateICmpEQ(B.CreateAnd(ri, SplatOf(ri, ~f.sign)), SplatOf(ri, Pow2Bits(bits, bits == 32 ? -126 : -1022)));
        // with DN and the flags modelled, a NaN too: the software FPMulAdd raises the invalid case's IOC
        Value* bad = DN() && !opts.fpsr_flags ? tiny : B.CreateOr(tiny, IsNaN(ri));
        Value* fast_r = DN() ? NaNResult(ri) : ri;
        BasicBlock* from = B.GetInsertBlock();
        auto* slow = BasicBlock::Create(C, "fma_fz", F);
        auto* cont = BasicBlock::Create(C, "fma_done", F);
        B.CreateCondBr(bad, slow, cont);
        B.SetInsertPoint(slow);
        Value* h = HelperCall("nxaot_rt_fp_muladd", B.getInt64Ty(),
                              {B.getInt32(bits), B.getInt32(sub ? 1 : 0), B.CreateZExt(AsInt(addend), B.getInt64Ty()), B.CreateZExt(AsInt(op1), B.getInt64Ty()),
                               B.CreateZExt(AsInt(op2), B.getInt64Ty()), fpcr_v, fpsr_ptr()});
        Value* hb = B.CreateTrunc(h, ri->getType());
        B.CreateBr(cont);
        B.SetInsertPoint(cont);
        auto* phi = B.CreatePHI(ri->getType(), 2);
        phi->addIncoming(fast_r, from);
        phi->addIncoming(hb, slow);
        Def(inst, phi);
        return true;
    }
    case Opcode::FPMulAdd16:
    case Opcode::FPMulSub16: {
        const bool sub = name.rfind("FPMulSub", 0) == 0;
        Value* r = HelperCall("nxaot_rt_fp_muladd", B.getInt64Ty(),
                              {B.getInt32(16), B.getInt32(sub ? 1 : 0), B.CreateZExt(Arg(inst, 0), B.getInt64Ty()), B.CreateZExt(Arg(inst, 1), B.getInt64Ty()),
                               B.CreateZExt(Arg(inst, 2), B.getInt64Ty()), fpcr_v, fpsr_ptr()});
        Def(inst, B.CreateTrunc(r, B.getInt16Ty()));
        return true;
    }

    case Opcode::FPMulX32:
    case Opcode::FPMulX64: {
        // FMULX: a product of infinity and zero is +-2.0 (the operands' signs XORed); NaN operands follow FPProcessNaNs
        const unsigned bits = fsize_of();
        const FInfo f = Info(bits);
        Value* a = FArg(inst, 0, bits);
        Value* b = FArg(inst, 1, bits);
        InputDenormal({a, b});
        Value* r = AsInt(B.CreateFMul(a, b));
        Value* ai = AsInt(a);
        Value* bi = AsInt(b);
        Value* two = B.CreateOr(B.CreateAnd(B.CreateXor(ai, bi), SplatOf(ai, f.sign)), SplatOf(ai, Pow2Bits(bits, 1)));
        Value* operands_nan = B.CreateOr(IsNaN(ai), IsNaN(bi));
        Value* res = B.CreateSelect(B.CreateAnd(IsNaN(r), B.CreateNot(operands_nan)), two, NaN2(ai, bi, r));
        Def(inst, res);
        return true;
    }

    case Opcode::FPMax32:
    case Opcode::FPMax64:
    case Opcode::FPMin32:
    case Opcode::FPMin64:
    case Opcode::FPMaxNumeric32:
    case Opcode::FPMaxNumeric64:
    case Opcode::FPMinNumeric32:
    case Opcode::FPMinNumeric64: {
        const unsigned bits = fsize_of();
        Def(inst, MinMax(name.rfind("FPMax", 0) == 0, name.find("Numeric") != std::string::npos, ArgAs(inst, 0, B.getIntNTy(bits)), ArgAs(inst, 1, B.getIntNTy(bits))));
        return true;
    }

    case Opcode::FPCompare32:
    case Opcode::FPCompare64: {
        // FCMP (quiet: Invalid Operation for a signalling NaN) or FCMPE (signalling: for any NaN), as ucomiss / comiss. NZCV: less 1000,
        // equal 0110, greater 0010, unordered 0011
        const unsigned bits = fsize_of();
        llvm::Type* t = FloatTy(bits);
        Value* a = ChainArg(inst, 0, t);
        Value* b = ChainArg(inst, 1, t);
        const bool signalling = inst.GetArg(2).GetU1();
        InputDenormal({a, b});
        auto cmp = [&](llvm::CmpInst::Predicate p) { return signalling ? B.CreateFCmpS(p, a, b) : B.CreateFCmp(p, a, b); };
        Flags fl;
        fl.n = cmp(llvm::CmpInst::FCMP_OLT);
        fl.z = cmp(llvm::CmpInst::FCMP_OEQ);
        fl.c = cmp(llvm::CmpInst::FCMP_UGE);
        fl.v = cmp(llvm::CmpInst::FCMP_UNO);
        flag_values[&inst] = fl;
        origins[&inst] = {};
        Def(inst, PackNzcv(fl));
        return true;
    }

    case Opcode::FPRecipEstimate16:
    case Opcode::FPRecipEstimate32:
    case Opcode::FPRecipEstimate64:
    case Opcode::FPRSqrtEstimate16:
    case Opcode::FPRSqrtEstimate32:
    case Opcode::FPRSqrtEstimate64:
    case Opcode::FPRecipExponent16:
    case Opcode::FPRecipExponent32:
    case Opcode::FPRecipExponent64: {
        // Arm's estimate algorithms (7.2 row 9): dynarmic's FPRecipEstimate / FPRSqrtEstimate / FPRecipExponent
        const unsigned bits = fsize_of();
        const uint32_t which = name.rfind("FPRecipEstimate", 0) == 0 ? NXAOT_RT_RECIP_ESTIMATE
                               : name.rfind("FPRSqrtEstimate", 0) == 0 ? NXAOT_RT_RSQRT_ESTIMATE
                                                                       : NXAOT_RT_RECIP_EXPONENT;
        if (which == NXAOT_RT_RSQRT_ESTIMATE && bits != 16) {
            Note(kIssueRSqrtEstimateNative);
        }
        Value* r = HelperCall("nxaot_rt_fp_unary", B.getInt64Ty(),
                              {B.getInt32(which), B.getInt32(bits), B.CreateZExt(ArgAs(inst, 0, B.getIntNTy(bits)), B.getInt64Ty()), fpcr_v, fpsr_ptr()});
        Def(inst, B.CreateTrunc(r, B.getIntNTy(bits)));
        return true;
    }
    case Opcode::FPRecipStepFused16:
    case Opcode::FPRecipStepFused32:
    case Opcode::FPRecipStepFused64:
    case Opcode::FPRSqrtStepFused16:
    case Opcode::FPRSqrtStepFused32:
    case Opcode::FPRSqrtStepFused64: {
        const unsigned bits = fsize_of();
        const bool rsqrt = name.rfind("FPRSqrt", 0) == 0;
        Def(inst, StepFused(rsqrt, bits, ArgAs(inst, 0, B.getIntNTy(bits)), ArgAs(inst, 1, B.getIntNTy(bits))));
        return true;
    }

    case Opcode::FPRoundInt16:
    case Opcode::FPRoundInt32:
    case Opcode::FPRoundInt64: {
        const unsigned bits = fsize_of();
        const unsigned rounding = inst.GetArg(1).GetU8();
        const bool exact = inst.GetArg(2).GetU1();
        Def(inst, RoundInt(bits, ArgAs(inst, 0, B.getIntNTy(bits)), rounding, exact));
        return true;
    }

    case Opcode::FPHalfToSingle:
    case Opcode::FPHalfToDouble:
    case Opcode::FPSingleToHalf:
    case Opcode::FPDoubleToHalf:
    case Opcode::FPSingleToDouble:
    case Opcode::FPDoubleToSingle: {
        const unsigned from = name.rfind("FPHalf", 0) == 0 ? 16 : name.rfind("FPSingle", 0) == 0 ? 32 : 64;
        const unsigned to = name.find("ToHalf") != std::string::npos ? 16 : name.find("ToSingle") != std::string::npos ? 32 : 64;
        const unsigned rounding = inst.GetArg(1).GetU8();
        const bool native_rounding = rounding == RMode();
        const bool f16_native = opts.target.f16c && !AHP() && !FZ16();
        if ((from == 32 && to == 64) || (from == 64 && to == 32)) {
            if (native_rounding) {
                // cvtss2sd / cvtsd2ss: a P operation (x86 converts NaNs as Arm's FPConvertNaN does; DN forces the default NaN)
                llvm::Type* tf = FloatTy(from);
                llvm::Type* tt = FloatTy(to);
                ChainNode node;
                node.args = {inst.GetArg(0)};
                node.fp_arg = {true};
                node.type = tt;
                node.dn_select = true;
                auto conv = [this, from, to, tt](Value* x) { return from < to ? B.CreateFPExt(x, tt) : B.CreateFPTrunc(x, tt); };
                Value* x = ChainArg(inst, 0, tf);
                InputDenormal({x});
                node.fast = conv(x);
                node.exact = [this, conv, tf](const std::vector<Value*>& a) {
                    Value* r = conv(As(a[0], tf));
                    return DN() ? AsFloat(NaNResult(AsInt(r))) : r;
                };
                DefChain(inst, std::move(node));
                return true;
            }
        } else if (f16_native && from == 16) {
            // vcvtph2ps (and cvtss2sd): exact
            Value* h = ArgAs(inst, 0, B.getHalfTy());
            Value* r = B.CreateFPExt(h, FloatTy(to));
            Def(inst, DN() ? NaNResult(AsInt(r)) : AsInt(r));
            return true;
        } else if (f16_native && from == 32 && native_rounding) {
            // vcvtps2ph in the current mode
            Value* x = FArg(inst, 0, 32);
            InputDenormal({x});
            Value* r = AsInt(B.CreateFPTrunc(x, B.getHalfTy()));
            Def(inst, DN() ? NaNResult(r) : r);
            return true;
        }
        // Everything else (double to half, the AHP or FZ16 forms, round to odd, another rounding mode): dynarmic's FPConvert
        Value* r = HelperCall("nxaot_rt_fp_convert", B.getInt64Ty(),
                              {B.getInt32(to), B.getInt32(from), B.CreateZExt(ArgAs(inst, 0, B.getIntNTy(from)), B.getInt64Ty()), B.getInt32(rounding),
                               fpcr_v, fpsr_ptr()});
        Def(inst, B.CreateTrunc(r, B.getIntNTy(to)));
        return true;
    }

    default:
        break;
    }

    // Conversions to and from fixed point / integers
    if (name.rfind("FPDoubleToFixed", 0) == 0 || name.rfind("FPSingleToFixed", 0) == 0 || name.rfind("FPHalfToFixed", 0) == 0) {
        const unsigned fsize = name.rfind("FPDouble", 0) == 0 ? 64 : name.rfind("FPSingle", 0) == 0 ? 32 : 16;
        const bool is_unsigned = name.find("FixedU") != std::string::npos;
        const unsigned isize = static_cast<unsigned>(std::stoul(name.substr(name.size() - 2)));
        const unsigned fbits = inst.GetArg(1).GetU8();
        const unsigned rounding = inst.GetArg(2).GetU8();
        Value* x = ArgAs(inst, 0, B.getIntNTy(fsize));
        if (fsize == 16) {
            Value* r = HelperCall("nxaot_rt_fp_to_fixed", B.getInt64Ty(),
                                  {B.getInt32(fsize), B.getInt32(isize), B.getInt32(is_unsigned), B.getInt32(fbits), B.getInt32(rounding),
                                   B.CreateZExt(x, B.getInt64Ty()), fpcr_v, fpsr_ptr()});
            Def(inst, B.CreateTrunc(r, B.getIntNTy(isize)));
            return true;
        }
        Def(inst, ToFixed(x, fsize, isize, is_unsigned, fbits, rounding));
        return true;
    }
    if (name.rfind("FPFixed", 0) == 0) {
        // FPFixedS32ToSingle, FPFixedU64ToDouble, ...
        const bool is_unsigned = name[7] == 'U';
        const unsigned isize = static_cast<unsigned>(std::stoul(name.substr(8, 2)));
        const unsigned to = name.find("ToSingle") != std::string::npos ? 32 : 64;
        const unsigned fbits = inst.GetArg(1).GetU8();
        const unsigned rounding = inst.GetArg(2).GetU8();
        if (rounding != RMode()) {
            error = "a conversion from fixed point in a rounding mode other than FPCR's";
            return true;
        }
        Def(inst, AsInt(FromFixed(ArgAs(inst, 0, B.getIntNTy(isize)), isize, is_unsigned, to, fbits)));
        return true;
    }
    return false;
}

// ---- Helpers shared by scalar and vector forms ----

Value* FunctionLifter::MinMax(bool is_max, bool numeric, Value* a, Value* b) {
    // FMAX / FMIN (7.2 row 4): NaNs per FPProcessNaNs; equal values (+0 and -0 included) give a AND b (max) or a OR b (min); else the larger
    // (smaller). The numeric forms (row 5) first replace a single quiet NaN by the infinity that loses. FZ flushes the operands first. Quiet
    // compares only: Invalid Operation for signalling NaNs, as Arm raises it
    const FInfo f = Info(ScalarBits(a));
    InputDenormal({a, b});
    if (numeric) {
        Value* qa = IsQNaN(a);
        Value* qb = IsQNaN(b);
        Value* inf = SplatOf(a, f.exp | (is_max ? f.sign : 0));  // max: -inf loses; min: +inf loses
        Value* na = B.CreateSelect(B.CreateAnd(qa, B.CreateNot(qb)), inf, a);
        Value* nb = B.CreateSelect(B.CreateAnd(qb, B.CreateNot(qa)), inf, b);
        a = na;
        b = nb;
    }
    Value* af = FZ() ? FlushDenormal(a) : a;
    Value* bf = FZ() ? FlushDenormal(b) : b;
    Value* fa = AsFloat(af);
    Value* fb = AsFloat(bf);
    Value* larger = B.CreateFCmp(is_max ? llvm::CmpInst::FCMP_OGT : llvm::CmpInst::FCMP_OLT, fa, fb);
    Value* equal = B.CreateFCmp(llvm::CmpInst::FCMP_OEQ, fa, fb);
    Value* pick = B.CreateSelect(larger, af, bf);
    Value* r = B.CreateSelect(equal, is_max ? B.CreateAnd(af, bf) : B.CreateOr(af, bf), pick);
    Value* any_nan = B.CreateOr(IsNaN(a), IsNaN(b));
    return B.CreateSelect(any_nan, NaN2(a, b, r), r);
}

Value* FunctionLifter::StepFused(bool rsqrt, unsigned bits, Value* a, Value* b) {
    // FRECPS: 2 - a * b fused; FRSQRTS: (3 - a * b) / 2 fused. As dynarmic's x86-64 backend: the native fused operation, and the software
    // FPRecipStepFused / FPRSqrtStepFused (0 * inf gives 2.0 / 1.5, NaN rules) when a lane of it is a NaN, or for FRSQRTS near infinity
    // (x86 cannot fuse the halving); then for every lane, as dynarmic's fallback is
    const bool vector = a->getType()->isVectorTy();
    const unsigned lanes = vector ? 128 / bits : 1;
    if (bits == 16 || !opts.target.fma) {
        auto one = [&](Value* x, Value* y) {
            Value* r = HelperCall("nxaot_rt_fp_binary", B.getInt64Ty(),
                                  {B.getInt32(rsqrt ? NXAOT_RT_RSQRT_STEP : NXAOT_RT_RECIP_STEP), B.getInt32(bits), B.CreateZExt(x, B.getInt64Ty()),
                                   B.CreateZExt(y, B.getInt64Ty()), B.getInt32(fpcr), FpsrPtr()});
            return B.CreateTrunc(r, B.getIntNTy(bits));
        };
        if (!vector) {
            return one(a, b);
        }
        Value* out = llvm::PoisonValue::get(a->getType());
        for (unsigned i = 0; i < lanes; ++i) {
            out = B.CreateInsertElement(out, one(B.CreateExtractElement(a, uint64_t{i}), B.CreateExtractElement(b, uint64_t{i})), uint64_t{i});
        }
        return out;
    }
    const FInfo f = Info(bits);
    InputDenormal({a, b});
    Value* fb = AsFloat(b);
    Value* na = AsFloat(B.CreateXor(a, SplatOf(a, f.sign)));
    Value* c = AsFloat(SplatOf(a, Pow2Bits(bits, 1) | (rsqrt ? f.quiet : 0)));  // 2.0, or 3.0 (1.5 * 2)
    Value* t = AsInt(FPIntrinsic(B, M, ID::fma, ID::experimental_constrained_fma, {na, fb, c}));
    Value* bad = nullptr;
    if (rsqrt) {
        // the intermediate's exponent near the top (dynarmic: (bits & 0x7f80...) >= 0x7f00... for single, 0x7fe0... for double)
        const uint64_t top = bits == 32 ? 0x7F000000u : 0x7FE0000000000000ull;
        bad = B.CreateICmpEQ(B.CreateAnd(t, SplatOf(t, top)), SplatOf(t, top));
    } else {
        bad = IsNaN(t);
    }
    Value* any_bad = AnyLane(bad);
    auto* slow = BasicBlock::Create(C, "step_fallback", F);
    auto* fast_bb = BasicBlock::Create(C, "step_fast", F);
    auto* cont = BasicBlock::Create(C, "step_done", F);
    B.CreateCondBr(any_bad, slow, fast_bb);
    B.SetInsertPoint(fast_bb);
    Value* fast_r = rsqrt ? AsInt(B.CreateFMul(AsFloat(t), AsFloat(SplatOf(t, Pow2Bits(bits, -1))))) : t;
    B.CreateBr(cont);
    B.SetInsertPoint(slow);
    Value* slow_r = nullptr;
    {
        auto one = [&](Value* x, Value* y) {
            Value* r = HelperCall("nxaot_rt_fp_binary", B.getInt64Ty(),
                                  {B.getInt32(rsqrt ? NXAOT_RT_RSQRT_STEP : NXAOT_RT_RECIP_STEP), B.getInt32(bits), B.CreateZExt(x, B.getInt64Ty()),
                                   B.CreateZExt(y, B.getInt64Ty()), B.getInt32(fpcr), FpsrPtr()});
            return B.CreateTrunc(r, B.getIntNTy(bits));
        };
        if (!vector) {
            slow_r = one(a, b);
        } else {
            slow_r = llvm::PoisonValue::get(a->getType());
            for (unsigned i = 0; i < lanes; ++i) {
                slow_r = B.CreateInsertElement(slow_r, one(B.CreateExtractElement(a, uint64_t{i}), B.CreateExtractElement(b, uint64_t{i})), uint64_t{i});
            }
        }
    }
    BasicBlock* slow_end = B.GetInsertBlock();
    B.CreateBr(cont);
    B.SetInsertPoint(cont);
    auto* phi = B.CreatePHI(a->getType(), 2);
    phi->addIncoming(fast_r, fast_bb);
    phi->addIncoming(slow_r, slow_end);
    return phi;
}

Value* FunctionLifter::RoundInt(unsigned bits, Value* x, unsigned rounding, bool exact) {
    // FRINT*: 16 bits in software; otherwise native without Inexact (N, P, M, Z, A), or rint in the current mode for FRINTX (exact), whose
    // Inexact is Arm's IXC. A NaN comes back quietened (x86's round does what FPProcessNaN does); DN forces the default NaN
    const bool vector = x->getType()->isVectorTy();
    if (bits == 16 || (exact && rounding != RMode()) || rounding > 4) {
        auto one = [&](Value* v) {
            Value* r = HelperCall("nxaot_rt_fp_round_int", B.getInt64Ty(),
                                  {B.getInt32(bits), B.CreateZExt(v, B.getInt64Ty()), B.getInt32(rounding), B.getInt32(exact ? 1 : 0), B.getInt32(fpcr),
                                   FpsrPtr()});
            return B.CreateTrunc(r, B.getIntNTy(bits));
        };
        if (!vector) {
            return one(x);
        }
        return PerLane(x, bits, one);
    }
    Value* fx = AsFloat(x);
    InputDenormal({x});
    Value* r = nullptr;
    if (exact) {
        r = FPIntrinsic(B, M, ID::rint, ID::experimental_constrained_rint, {fx});
    } else {
        r = RoundIntegral(fx, rounding);
        if (rounding <= 3) {
            Note(kIssueRoundIntInexact);  // dynarmic: roundss without the precision-suppress bit
        }
    }
    Value* ri = AsInt(r);
    return DN() ? NaNResult(ri) : ri;
}

Value* FunctionLifter::ToFixed(Value* x, unsigned fsize, unsigned isize, bool is_unsigned, unsigned fbits, unsigned rounding) {
    // FPToFixed (7.2 row 7): round(x * 2^fbits) saturated to the integer type; NaN gives 0. Arm raises Invalid Operation for a NaN or a
    // saturated result, Inexact for an inexact one: both are computed explicitly, and no x86 conversion sees an out-of-range value. The work
    // is in double precision (exact for single-precision inputs); magnitudes of 2^(isize+1) and more saturate before scaling, so the scaling
    // cannot overflow
    const bool vector = x->getType()->isVectorTy();
    const unsigned lanes = vector ? 128 / fsize : 1;
    auto vt = [&](llvm::Type* scalar) -> llvm::Type* { return vector ? llvm::FixedVectorType::get(scalar, lanes) : scalar; };
    llvm::Type* dbl = vt(B.getDoubleTy());
    llvm::Type* i64t = vt(B.getInt64Ty());
    InputDenormal({x});
    if (rounding != 4) {
        Note(kIssueToFixedNative);
    } else if (is_unsigned) {
        Note(kIssueToFixedSoftware);  // dynarmic converts ties-away in software
    }
    Value* fx = AsFloat(x);
    Value* xd = fsize == 32 ? B.CreateFPExt(fx, dbl) : fx;
    Value* bits = AsInt(xd);
    const FInfo f = Info(64);
    Value* nan = IsNaN(bits);
    Value* mag = B.CreateAnd(bits, SplatOf(bits, ~f.sign));
    Value* big = B.CreateICmpUGE(mag, SplatOf(bits, Pow2Bits(64, static_cast<int>(isize) + 1)));
    Value* negative = B.CreateICmpSLT(bits, SplatOf(bits, 0));
    Value* safe = AsFloat(B.CreateSelect(B.CreateOr(nan, big), SplatOf(bits, 0), bits));
    Value* scaled = fbits ? B.CreateFMul(safe, AsFloat(SplatOf(bits, Pow2Bits(64, static_cast<int>(fbits))))) : safe;
    Value* rounded = RoundIntegral(scaled, rounding);
    Value* inexact = B.CreateFCmp(llvm::CmpInst::FCMP_UNE, rounded, scaled);
    // limits as doubles: [min, max_excl) with max_excl = 2^(isize-1) signed, 2^isize unsigned
    const int top = is_unsigned ? static_cast<int>(isize) : static_cast<int>(isize) - 1;
    Value* max_excl = AsFloat(SplatOf(bits, Pow2Bits(64, top)));
    Value* too_high = B.CreateFCmp(llvm::CmpInst::FCMP_OGE, rounded, max_excl);
    Value* too_low = nullptr;
    if (is_unsigned) {
        too_low = B.CreateFCmp(llvm::CmpInst::FCMP_OLT, rounded, AsFloat(SplatOf(bits, 0)));
    } else {
        too_low = B.CreateFCmp(llvm::CmpInst::FCMP_OLT, rounded, AsFloat(SplatOf(bits, Pow2Bits(64, top) | f.sign)));
    }
    Value* overflow = B.CreateOr(big, B.CreateOr(too_high, too_low));
    Value* in_range = AsFloat(B.CreateSelect(overflow, SplatOf(bits, 0), AsInt(rounded)));
    Value* conv = nullptr;
    llvm::Type* it = vt(B.getIntNTy(isize));
    if (is_unsigned && isize == 64) {
        conv = B.CreateFPToUI(in_range, i64t);
    } else if (is_unsigned) {
        conv = B.CreateTrunc(B.CreateFPToSI(in_range, i64t), it);
    } else {
        conv = B.CreateFPToSI(in_range, it);
    }
    const uint64_t imax = is_unsigned ? (isize == 64 ? ~0ull : (1ull << isize) - 1) : (1ull << (isize - 1)) - 1;
    const uint64_t imin = is_unsigned ? 0 : (1ull << (isize - 1));
    Value* low_side = big ? B.CreateOr(B.CreateAnd(big, negative), too_low) : too_low;
    Value* sat = B.CreateSelect(low_side, SplatOf(conv, imin), SplatOf(conv, imax));
    Value* r = B.CreateSelect(overflow, sat, conv);
    r = B.CreateSelect(nan, SplatOf(conv, 0), r);
    if (opts.fpsr_flags) {
        Value* ioc = B.CreateOr(nan, overflow);
        Value* ixc = B.CreateAnd(inexact, B.CreateNot(ioc));
        Value* flags = B.CreateOr(B.CreateSelect(AnyLane(ioc), B.getInt32(kIOC), B.getInt32(0)), B.CreateSelect(AnyLane(ixc), B.getInt32(kIXC), B.getInt32(0)));
        OrFpsrBits(flags);
    }
    return r;
}

Value* FunctionLifter::FromFixed(Value* x, unsigned isize, bool is_unsigned, unsigned to, unsigned fbits) {
    // SCVTF / UCVTF: one conversion in the current mode (cvtsi2ss: Inexact as Arm's IXC), then the exact scaling by 2^-fbits. Unsigned 32-bit
    // values convert from 64 bits; unsigned 64-bit values of 2^63 and more are halved first, the lost bit kept as a sticky bit, and doubled
    // after (correctly rounded in every mode)
    const bool vector = x->getType()->isVectorTy();
    const unsigned lanes = vector ? llvm::cast<llvm::FixedVectorType>(x->getType())->getNumElements() : 1;
    auto vt = [&](llvm::Type* scalar) -> llvm::Type* { return vector ? llvm::FixedVectorType::get(scalar, lanes) : scalar; };
    llvm::Type* ft = vt(FloatTy(to));
    Value* r = nullptr;
    if (!is_unsigned) {
        r = B.CreateSIToFP(x, ft);
    } else if (isize < 64) {
        r = B.CreateSIToFP(B.CreateZExt(x, vt(B.getInt64Ty())), ft);
    } else {
        Value* hi = B.CreateICmpSLT(x, SplatOf(x, 0));
        Value* halved = B.CreateOr(B.CreateLShr(x, SplatOf(x, 1)), B.CreateAnd(x, SplatOf(x, 1)));
        Value* v = B.CreateSelect(hi, halved, x);
        Value* c = B.CreateSIToFP(v, ft);
        r = B.CreateSelect(hi, B.CreateFMul(c, AsFloat(SplatOf(AsInt(c), Pow2Bits(to, 1)))), c);
    }
    if (fbits) {
        r = B.CreateFMul(r, AsFloat(SplatOf(AsInt(r), Pow2Bits(to, -static_cast<int>(fbits)))));
    }
    return r;
}

// ---- Vector floating point ----

bool FunctionLifter::LowerFPVector(IR::Inst& inst) {
    const Opcode op = inst.GetOpcode();
    const std::string name = OpcodeName(op);
    if (name.rfind("FPVector", 0) != 0) {
        return false;
    }
    const unsigned e = name.back() == '6' ? 16 : name.back() == '2' ? 32 : 64;
    const std::string base = name.substr(8, name.size() - 10);  // FPVectorAdd32 -> Add
    llvm::Type* vt = e == 16 ? static_cast<llvm::Type*>(VecI(16)) : static_cast<llvm::Type*>(VecF(e));
    const unsigned lanes = 128 / e;
    Value* fpcr_v = B.getInt32(fpcr);

    auto binary_p = [&](std::function<Value*(Value*, Value*)> fn) {
        ChainNode node;
        node.args = {inst.GetArg(0), inst.GetArg(1)};
        node.fp_arg = {true, true};
        node.type = vt;
        node.dn_select = true;
        Value* a = ChainArg(inst, 0, vt);
        Value* b = ChainArg(inst, 1, vt);
        InputDenormal({a, b});
        node.fast = fn(a, b);
        node.exact = [this, fn, vt](const std::vector<Value*>& args) {
            Value* x = As(args[0], vt);
            Value* y = As(args[1], vt);
            Value* r = fn(x, y);
            return AsFloat(NaN2(AsInt(x), AsInt(y), AsInt(r)));
        };
        DefChain(inst, std::move(node));
    };

    if (e == 16) {
        // Half precision (FEAT_FP16 forms the A57 lacks but dynarmic decodes): bit operations, or the software helpers lane by lane
        if (base == "Abs") {
            Def(inst, B.CreateAnd(ArgVec(inst, 0, 16), Splat(16, 0x7FFF)));
            return true;
        }
        if (base == "Neg") {
            Def(inst, B.CreateXor(ArgVec(inst, 0, 16), Splat(16, 0x8000)));
            return true;
        }
        Value* a = ArgVec(inst, 0, 16);
        if (base == "Equal") {
            Value* b = ArgVec(inst, 1, 16);
            Value* out = llvm::PoisonValue::get(VecI(16));
            for (unsigned i = 0; i < 8; ++i) {
                Value* r = HelperCall("nxaot_rt_fp_binary", B.getInt64Ty(),
                                      {B.getInt32(NXAOT_RT_COMPARE_EQ), B.getInt32(16), B.CreateZExt(B.CreateExtractElement(a, uint64_t{i}), B.getInt64Ty()),
                                       B.CreateZExt(B.CreateExtractElement(b, uint64_t{i}), B.getInt64Ty()), fpcr_v, FpsrPtr()});
                out = B.CreateInsertElement(out, B.CreateSExt(B.CreateTrunc(r, B.getInt1Ty()), B.getInt16Ty()), uint64_t{i});
            }
            Def(inst, out);
            return true;
        }
        if (base == "MulAdd") {
            Value* b = ArgVec(inst, 1, 16);
            Value* c = ArgVec(inst, 2, 16);
            Value* out = llvm::PoisonValue::get(VecI(16));
            for (unsigned i = 0; i < 8; ++i) {
                Value* r = HelperCall("nxaot_rt_fp_muladd", B.getInt64Ty(),
                                      {B.getInt32(16), B.getInt32(0), B.CreateZExt(B.CreateExtractElement(a, uint64_t{i}), B.getInt64Ty()),
                                       B.CreateZExt(B.CreateExtractElement(b, uint64_t{i}), B.getInt64Ty()),
                                       B.CreateZExt(B.CreateExtractElement(c, uint64_t{i}), B.getInt64Ty()), fpcr_v, FpsrPtr()});
                out = B.CreateInsertElement(out, B.CreateTrunc(r, B.getInt16Ty()), uint64_t{i});
            }
            Def(inst, out);
            return true;
        }
        if (base == "RecipEstimate" || base == "RSqrtEstimate") {
            const uint32_t which = base == "RecipEstimate" ? NXAOT_RT_RECIP_ESTIMATE : NXAOT_RT_RSQRT_ESTIMATE;
            Def(inst, PerLane(a, 16, [&](Value* lane) {
                Value* r = HelperCall("nxaot_rt_fp_unary", B.getInt64Ty(), {B.getInt32(which), B.getInt32(16), B.CreateZExt(lane, B.getInt64Ty()), fpcr_v, FpsrPtr()});
                return B.CreateTrunc(r, B.getInt16Ty());
            }));
            return true;
        }
        if (base == "RecipStepFused" || base == "RSqrtStepFused") {
            Def(inst, StepFused(base == "RSqrtStepFused", 16, a, ArgVec(inst, 1, 16)));
            return true;
        }
        if (base == "RoundInt") {
            Def(inst, RoundInt(16, a, inst.GetArg(1).GetU8(), inst.GetArg(2).GetU1()));
            return true;
        }
        error = std::string("a half-precision vector operation the lowering does not know: ") + name;
        return true;
    }

    llvm::Type* it = VecI(e);
    if (base == "Add") {
        binary_p([this](Value* a, Value* b) { return B.CreateFAdd(a, b); });
    } else if (base == "Sub") {
        binary_p([this](Value* a, Value* b) { return B.CreateFSub(a, b); });
    } else if (base == "Mul") {
        binary_p([this](Value* a, Value* b) { return B.CreateFMul(a, b); });
    } else if (base == "Div") {
        if (e == 32 && LowHalfOnly(inst)) {
            // fdiv 2S: only lanes 0 and 1 divide; the upper lanes get 1.0 / 1.0 instead of the 0 / 0 of the zero-extended operands
            Note(kIssueVectorUpperLanes);
            binary_p([this](Value* a, Value* b) {
                Value* one = AsFloat(Splat(32, 0x3F800000));
                return B.CreateFDiv(Shuffle(a, one, {0, 1, 6, 7}), Shuffle(b, one, {0, 1, 6, 7}));
            });
        } else {
            binary_p([this](Value* a, Value* b) { return B.CreateFDiv(a, b); });
        }
    } else if (base == "PairedAdd" || base == "PairedAddLower") {
        // faddp: lane i of the result adds lanes 2i and 2i+1 of a:b (a's pairs, then b's); the low form (64-bit vectors) pairs the low halves,
        // as dynarmic's punpcklqdq + haddps, its upper lanes 0 + 0
        const bool lower = base == "PairedAddLower";
        auto pairs = [this, lower, lanes, e](Value* a, Value* b, bool odd) -> Value* {
            std::vector<int> m;
            if (!lower) {
                for (unsigned i = 0; i < lanes; ++i) {
                    m.push_back(static_cast<int>(2 * i) + (odd ? 1 : 0));
                }
                return Shuffle(a, b, m);
            }
            // [a0, a1, b0, b1] (32) or [a0, b0] (64) as the low concatenation, paired; upper lanes from a zero vector
            Value* z = llvm::Constant::getNullValue(a->getType());
            Value* lo = Shuffle(a, b, e == 32 ? std::vector<int>{0, 1, 4, 5} : std::vector<int>{0, 2});
            if (e == 32) {
                return Shuffle(lo, z, odd ? std::vector<int>{1, 3, 5, 7} : std::vector<int>{0, 2, 4, 6});
            }
            return Shuffle(lo, z, odd ? std::vector<int>{1, 3} : std::vector<int>{0, 2});
        };
        ChainNode node;
        node.args = {inst.GetArg(0), inst.GetArg(1)};
        node.fp_arg = {true, true};
        node.type = vt;
        node.dn_select = true;
        Value* a = ChainArg(inst, 0, vt);
        Value* b = ChainArg(inst, 1, vt);
        Value* p0 = pairs(a, b, false);
        Value* p1 = pairs(a, b, true);
        InputDenormal({p0, p1});
        node.fast = B.CreateFAdd(p0, p1);
        node.exact = [this, pairs, vt](const std::vector<Value*>& args) {
            Value* x = As(args[0], vt);
            Value* y = As(args[1], vt);
            Value* p = pairs(x, y, false);
            Value* q = pairs(x, y, true);
            Value* r = B.CreateFAdd(p, q);
            return AsFloat(NaN2(AsInt(p), AsInt(q), AsInt(r)));
        };
        DefChain(inst, std::move(node));
    } else if (base == "Sqrt") {
        ChainNode node;
        node.args = {inst.GetArg(0)};
        node.fp_arg = {true};
        node.type = vt;
        node.dn_select = true;
        auto fn = [this](Value* a) { return FPIntrinsic(B, M, ID::sqrt, ID::experimental_constrained_sqrt, {a}); };
        Value* x = ChainArg(inst, 0, vt);
        InputDenormal({x});
        node.fast = fn(x);
        node.exact = [this, fn, vt](const std::vector<Value*>& args) {
            Value* x = As(args[0], vt);
            return AsFloat(NaN1(AsInt(x), AsInt(fn(x))));
        };
        DefChain(inst, std::move(node));
    } else if (base == "Abs" || base == "Neg") {
        const bool neg = base == "Neg";
        const FInfo f = Info(e);
        ChainNode node;
        node.args = {inst.GetArg(0)};
        node.fp_arg = {true};
        node.type = vt;
        auto fn = [this, neg, f](Value* a) {
            Value* i = AsInt(a);
            return AsFloat(neg ? B.CreateXor(i, SplatOf(i, f.sign)) : B.CreateAnd(i, SplatOf(i, ~f.sign)));
        };
        node.fast = fn(ChainArg(inst, 0, vt));
        node.exact = [fn, vt, this](const std::vector<Value*>& args) { return fn(As(args[0], vt)); };
        DefChain(inst, std::move(node));
    } else if (base == "MulAdd") {
        // fmla / fmls (the frontend negates for fmls): addend + op1 * op2 lane-wise
        const FInfo f = Info(e);
        auto native = [this](Value* addend, Value* op1, Value* op2) { return FPIntrinsic(B, M, ID::fma, ID::experimental_constrained_fma, {op1, op2, addend}); };
        auto exact = [this](Value* addend, Value* op1, Value* op2, Value* r) {
            Value* a = AsInt(addend);
            Value* m1 = AsInt(op1);
            Value* m2 = AsInt(op2);
            Value* res = NaN3(a, m1, m2, AsInt(r));
            return AsFloat(B.CreateSelect(MulAddInvalid(a, m1, m2), DefaultNaN(a->getType()), res));
        };
        Note(kIssueMulAddInfZero);
        if (!opts.target.fma) {
            Value* a = ArgVec(inst, 0, e);
            Value* b = ArgVec(inst, 1, e);
            Value* c = ArgVec(inst, 2, e);
            Value* out = llvm::PoisonValue::get(it);
            for (unsigned i = 0; i < lanes; ++i) {
                Value* r = HelperCall("nxaot_rt_fp_muladd", B.getInt64Ty(),
                                      {B.getInt32(e), B.getInt32(0), B.CreateZExt(B.CreateExtractElement(a, uint64_t{i}), B.getInt64Ty()),
                                       B.CreateZExt(B.CreateExtractElement(b, uint64_t{i}), B.getInt64Ty()),
                                       B.CreateZExt(B.CreateExtractElement(c, uint64_t{i}), B.getInt64Ty()), fpcr_v, FpsrPtr()});
                out = B.CreateInsertElement(out, B.CreateTrunc(r, B.getIntNTy(e)), uint64_t{i});
            }
            Def(inst, out);
            return true;
        }
        if (!FZ()) {
            ChainNode node;
            node.args = {inst.GetArg(0), inst.GetArg(1), inst.GetArg(2)};
            node.fp_arg = {true, true, true};
            node.type = vt;
            node.dn_select = true;
            node.exact_flags = opts.fpsr_flags;
            Value* x = ChainArg(inst, 0, vt);
            Value* y = ChainArg(inst, 1, vt);
            Value* z = ChainArg(inst, 2, vt);
            InputDenormal({x, y, z});
            node.fast = native(x, y, z);
            node.exact = [native, exact, vt, this](const std::vector<Value*>& args) {
                Value* x = As(args[0], vt);
                Value* y = As(args[1], vt);
                Value* z = As(args[2], vt);
                return exact(x, y, z, native(x, y, z));
            };
            DefChain(inst, std::move(node));
            return true;
        }
        // FZ: dynarmic's EmitFPVectorMulAddFallback: lanes equal to +-(smallest normal) by the software FPMulAdd, NaN lanes by the NaN rules
        Value* addend = FVecArg(inst, 0, e);
        Value* op1 = FVecArg(inst, 1, e);
        Value* op2 = FVecArg(inst, 2, e);
        InputDenormal({addend, op1, op2});
        Value* r = AsInt(native(addend, op1, op2));
        Value* tiny = B.CreateICmpEQ(B.CreateAnd(r, SplatOf(r, ~f.sign)), SplatOf(r, Pow2Bits(e, e == 32 ? -126 : -1022)));
        Value* nan = IsNaN(r);
        Value* bad = DN() && !opts.fpsr_flags ? tiny : B.CreateOr(tiny, nan);
        BasicBlock* from = B.GetInsertBlock();
        auto* slow = BasicBlock::Create(C, "fmla_fz", F);
        auto* cont = BasicBlock::Create(C, "fmla_done", F);
        Value* fast_r = DN() ? NaNResult(r) : r;
        B.CreateCondBr(AnyLane(bad), slow, cont);
        B.SetInsertPoint(slow);
        Value* nan_fixed = r;
        if (DN()) {
            MulAddInvalid(AsInt(addend), AsInt(op1), AsInt(op2));
            nan_fixed = NaNResult(r);
        } else {
            nan_fixed = AsInt(exact(addend, op1, op2, AsFloat(r)));
        }
        Value* out = B.CreateSelect(nan, nan_fixed, r);
        for (unsigned i = 0; i < lanes; ++i) {
            Value* lane_tiny = B.CreateExtractElement(tiny, uint64_t{i});
            BasicBlock* here = B.GetInsertBlock();
            auto* call_bb = BasicBlock::Create(C, "fmla_lane", F);
            auto* next_bb = BasicBlock::Create(C, "fmla_next", F);
            B.CreateCondBr(lane_tiny, call_bb, next_bb);
            B.SetInsertPoint(call_bb);
            Value* h = HelperCall("nxaot_rt_fp_muladd", B.getInt64Ty(),
                                  {B.getInt32(e), B.getInt32(0), B.CreateZExt(B.CreateExtractElement(AsInt(addend), uint64_t{i}), B.getInt64Ty()),
                                   B.CreateZExt(B.CreateExtractElement(AsInt(op1), uint64_t{i}), B.getInt64Ty()),
                                   B.CreateZExt(B.CreateExtractElement(AsInt(op2), uint64_t{i}), B.getInt64Ty()), fpcr_v, FpsrPtr()});
            Value* with = B.CreateInsertElement(out, B.CreateTrunc(h, B.getIntNTy(e)), uint64_t{i});
            B.CreateBr(next_bb);
            B.SetInsertPoint(next_bb);
            auto* phi = B.CreatePHI(out->getType(), 2);
            phi->addIncoming(out, here);
            phi->addIncoming(with, call_bb);
            out = phi;
        }
        BasicBlock* slow_end = B.GetInsertBlock();
        B.CreateBr(cont);
        B.SetInsertPoint(cont);
        auto* phi = B.CreatePHI(r->getType(), 2);
        phi->addIncoming(fast_r, from);
        phi->addIncoming(out, slow_end);
        Def(inst, phi);
    } else if (base == "MulX") {
        const FInfo f = Info(e);
        Value* a = FVecArg(inst, 0, e);
        Value* b = FVecArg(inst, 1, e);
        InputDenormal({a, b});
        Value* r = AsInt(B.CreateFMul(a, b));
        Value* ai = AsInt(a);
        Value* bi = AsInt(b);
        Value* two = B.CreateOr(B.CreateAnd(B.CreateXor(ai, bi), SplatOf(ai, f.sign)), SplatOf(ai, Pow2Bits(e, 1)));
        Value* operands_nan = B.CreateOr(IsNaN(ai), IsNaN(bi));
        Def(inst, B.CreateSelect(B.CreateAnd(IsNaN(r), B.CreateNot(operands_nan)), two, NaN2(ai, bi, r)));
    } else if (base == "Max" || base == "Min" || base == "MaxNumeric" || base == "MinNumeric") {
        Note(kIssueVectorMinMaxQNaN);
        Def(inst, MinMax(base.rfind("Max", 0) == 0, base.find("Numeric") != std::string::npos, ArgVec(inst, 0, e), ArgVec(inst, 1, e)));
    } else if (base == "Equal" || base == "Greater" || base == "GreaterEqual") {
        // fcmeq (quiet), fcmgt / fcmge (signalling: Invalid Operation for any NaN), on flushed operands under FZ
        Value* a = ArgVec(inst, 0, e);
        Value* b = ArgVec(inst, 1, e);
        InputDenormal({a, b});
        if (FZ()) {
            a = FlushDenormal(a);
            b = FlushDenormal(b);
        }
        Value* fa = AsFloat(a);
        Value* fb = AsFloat(b);
        Value* c = base == "Equal" ? B.CreateFCmp(llvm::CmpInst::FCMP_OEQ, fa, fb)
                                   : B.CreateFCmpS(base == "Greater" ? llvm::CmpInst::FCMP_OGT : llvm::CmpInst::FCMP_OGE, fa, fb);
        Def(inst, B.CreateSExt(c, it));
    } else if (base == "RecipEstimate" || base == "RSqrtEstimate") {
        const uint32_t which = base == "RecipEstimate" ? NXAOT_RT_RECIP_ESTIMATE : NXAOT_RT_RSQRT_ESTIMATE;
        if (which == NXAOT_RT_RSQRT_ESTIMATE) {
            Note(kIssueRSqrtEstimateNative);
        }
        // the 64-bit forms estimate their two lanes only (an estimate of the zero upper lanes would raise Divide by Zero)
        const bool low = e == 32 && LowHalfOnly(inst);
        if (low) {
            Note(kIssueVectorUpperLanes);
        }
        Value* in = ArgVec(inst, 0, e);
        Value* out = llvm::Constant::getNullValue(VecI(e));
        for (unsigned i = 0; i < (low ? lanes / 2 : lanes); ++i) {
            Value* r = HelperCall("nxaot_rt_fp_unary", B.getInt64Ty(),
                                  {B.getInt32(which), B.getInt32(e), B.CreateZExt(B.CreateExtractElement(in, uint64_t{i}), B.getInt64Ty()), fpcr_v, FpsrPtr()});
            out = B.CreateInsertElement(out, B.CreateTrunc(r, B.getIntNTy(e)), uint64_t{i});
        }
        Def(inst, out);
    } else if (base == "RecipStepFused" || base == "RSqrtStepFused") {
        Def(inst, StepFused(base == "RSqrtStepFused", e, ArgVec(inst, 0, e), ArgVec(inst, 1, e)));
    } else if (base == "RoundInt") {
        Def(inst, RoundInt(e, ArgVec(inst, 0, e), inst.GetArg(1).GetU8(), inst.GetArg(2).GetU1()));
    } else if (base == "ToSignedFixed" || base == "ToUnsignedFixed") {
        Def(inst, ToFixed(ArgVec(inst, 0, e), e, e, base == "ToUnsignedFixed", inst.GetArg(1).GetU8(), inst.GetArg(2).GetU8()));
    } else if (base == "FromSignedFixed" || base == "FromUnsignedFixed") {
        if (inst.GetArg(2).GetU8() != RMode()) {
            error = "a vector conversion from fixed point in a rounding mode other than FPCR's";
            return true;
        }
        Def(inst, AsInt(FromFixed(ArgVec(inst, 0, e), e, base == "FromUnsignedFixed", e, inst.GetArg(1).GetU8())));
    } else {
        return false;
    }
    return true;
}

}  // namespace nxaot::lift::detail
