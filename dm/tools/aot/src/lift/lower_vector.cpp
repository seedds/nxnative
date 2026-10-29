// The integer vector operations of dynarmic's IR (Vector*, the 128-bit element accesses and permutes) and the saturating arithmetic, scalar and
// vector, with FPSR.QC (aot-recompiler.md section 7.2 rows 18 and 20). Generic LLVM vector IR, exact by definition, which the x86 backend selects
// to SSE4.1 / AVX2 instructions; an operation whose amount or index is an immediate of the IR folds it into the shuffle or shift. QC is ORed into
// the function's QC flag (FunctionLifter::OrQC) when any lane saturates, as dynarmic's x86-64 backend ORs fpsr_qc.
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsX86.h>

#include "lift/function_lifter.h"
#include "lift/opcodes.h"
#include "lift/runtime.h"

namespace nxaot::lift::detail {

using IR::Opcode;
using llvm::Intrinsic::ID;

namespace {

// The element size an opcode name ends with (VectorAdd16 -> 16); 0 when it has none
unsigned SizeSuffix(Opcode op) {
    const std::string n = OpcodeName(op);
    size_t i = n.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(n[i - 1]))) {
        --i;
    }
    return i == n.size() ? 0 : static_cast<unsigned>(std::stoul(n.substr(i)));
}

std::string BaseName(Opcode op) {
    const std::string n = OpcodeName(op);
    size_t i = n.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(n[i - 1]))) {
        --i;
    }
    return n.substr(0, i);
}

std::vector<int> Seq(unsigned n, int start, int step) {
    std::vector<int> m(n);
    for (unsigned i = 0; i < n; ++i) {
        m[i] = start + static_cast<int>(i) * step;
    }
    return m;
}

}  // namespace

Value* FunctionLifter::AnyLane(Value* v) {
    if (!v->getType()->isVectorTy()) {
        return v;
    }
    return Call(llvm::Intrinsic::vector_reduce_or, {v->getType()}, {v});
}

Value* FunctionLifter::PerLane(Value* vec_bits, unsigned esize, const std::function<Value*(Value* lane)>& fn) {
    Value* v = As(vec_bits, VecI(esize));
    Value* r = llvm::PoisonValue::get(VecI(esize));
    for (unsigned i = 0; i < 128 / esize; ++i) {
        r = B.CreateInsertElement(r, fn(B.CreateExtractElement(v, uint64_t{i})), uint64_t{i});
    }
    return r;
}

bool FunctionLifter::LowerVector(IR::Inst& inst) {
    const Opcode op = inst.GetOpcode();
    const std::string base = BaseName(op);
    const unsigned e = SizeSuffix(op);  // element size, when the opcode has one
    const unsigned n = e ? 128 / e : 0;  // lanes
    auto A = [&](size_t i) { return ArgVec(inst, i, e); };
    auto A8 = [&](size_t i) { return ArgVec(inst, i, 8); };
    auto imm = [&](size_t i) { return static_cast<unsigned>(inst.GetArg(i).GetU8()); };
    auto zero = [&](unsigned bits) { return llvm::Constant::getNullValue(VecI(bits)); };
    auto sext_mask = [&](Value* cond) { return B.CreateSExt(cond, VecI(e)); };
    // The lanes of the low 64 bits widened to the double width (sign or zero)
    auto widen_lower = [&](Value* v, unsigned esize, bool is_signed) {
        Value* lo = Shuffle(As(v, VecI(esize)), As(v, VecI(esize)), Seq(64 / esize, 0, 1));
        return is_signed ? B.CreateSExt(lo, llvm::FixedVectorType::get(B.getIntNTy(2 * esize), 64 / esize))
                         : B.CreateZExt(lo, llvm::FixedVectorType::get(B.getIntNTy(2 * esize), 64 / esize));
    };
    // n/2 lanes of `half` placed in the low 64 bits, the high 64 zero
    auto low_half = [&](Value* half, unsigned esize) {
        Value* z = llvm::Constant::getNullValue(half->getType());
        const unsigned h = 64 / esize;
        return Shuffle(half, z, Seq(2 * h, 0, 1));
    };

    switch (op) {
    // ---- Element access ----
    case Opcode::VectorGetElement8:
    case Opcode::VectorGetElement16:
    case Opcode::VectorGetElement32:
    case Opcode::VectorGetElement64: {
        // An element only floating-point operations read is extracted as a float, which keeps it in its xmm register (an integer extract
        // goes through a general register: vmovd there and back)
        bool fp_users = e >= 32;
        const auto u = users.find(&inst);
        if (u == users.end() || u->second.empty()) {
            fp_users = false;
        } else {
            for (const IR::Inst* user : u->second) {
                fp_users = fp_users && std::string(OpcodeName(user->GetOpcode())).rfind("FP", 0) == 0;
            }
        }
        if (fp_users) {
            Def(inst, B.CreateExtractElement(ArgAs(inst, 0, VecF(e)), uint64_t{imm(1) % n}));
            return true;
        }
        Def(inst, B.CreateExtractElement(A(0), uint64_t{imm(1) % n}));
        return true;
    }
    case Opcode::VectorSetElement8:
    case Opcode::VectorSetElement16:
    case Opcode::VectorSetElement32:
    case Opcode::VectorSetElement64:
        Def(inst, B.CreateInsertElement(A(0), Arg(inst, 2), uint64_t{imm(1) % n}));
        return true;
    case Opcode::ZeroVector:
        Def(inst, llvm::Constant::getNullValue(V128()));
        return true;
    case Opcode::VectorZeroUpper:
        Def(inst, ZeroUpper(Arg(inst, 0)));
        return true;
    case Opcode::ZeroExtendLongToQuad: {
        // A scalar written to a vector register (V_scalar: ZeroExtendLongToQuad(ZeroExtendWordToLong(s))): the narrow value inserted into a
        // zero vector directly, so that a single-precision result stays in its xmm register (vinsertps / vblendps) instead of a round trip
        // through a general register for the integer zero extension
        const IR::Value a = inst.GetArg(0);
        if (!a.IsImmediate()) {
            const IR::Inst* widen = a.GetInstRecursive();
            const IR::Opcode w = widen->GetOpcode();
            const unsigned bits = w == Opcode::ZeroExtendWordToLong ? 32 : w == Opcode::ZeroExtendHalfToLong ? 16 : w == Opcode::ZeroExtendByteToLong ? 8 : 0;
            if (bits) {
                // a floating-point result is inserted as a float (vblendps with a zero register), which keeps it in the FP domain
                Value* raw = Get(widen->GetArg(0));
                if (raw->getType()->isFloatingPointTy() && bits >= 16) {
                    Def(inst, B.CreateInsertElement(llvm::Constant::getNullValue(VecF(bits)), raw, uint64_t{0}));
                } else {
                    Def(inst, B.CreateInsertElement(llvm::Constant::getNullValue(VecI(bits)), As(raw, B.getIntNTy(bits)), uint64_t{0}));
                }
                return true;
            }
        }
        Def(inst, B.CreateInsertElement(llvm::Constant::getNullValue(V128()), Arg(inst, 0), uint64_t{0}));
        return true;
    }

    // ---- The scalar max / min the across-lanes translators (smaxv, uminv, ...) emit ----
    case Opcode::MaxSigned32:
        Def(inst, Call(llvm::Intrinsic::smax, {B.getInt32Ty()}, {Arg(inst, 0), Arg(inst, 1)}));
        return true;
    case Opcode::MaxUnsigned32:
        Def(inst, Call(llvm::Intrinsic::umax, {B.getInt32Ty()}, {Arg(inst, 0), Arg(inst, 1)}));
        return true;
    case Opcode::MinSigned32:
        Def(inst, Call(llvm::Intrinsic::smin, {B.getInt32Ty()}, {Arg(inst, 0), Arg(inst, 1)}));
        return true;
    case Opcode::MinUnsigned32:
        Def(inst, Call(llvm::Intrinsic::umin, {B.getInt32Ty()}, {Arg(inst, 0), Arg(inst, 1)}));
        return true;

    // ---- Bitwise ----
    case Opcode::VectorAnd:
        Def(inst, B.CreateAnd(Arg(inst, 0), Arg(inst, 1)));
        return true;
    case Opcode::VectorAndNot:
        Def(inst, B.CreateAnd(Arg(inst, 0), B.CreateNot(Arg(inst, 1))));
        return true;
    case Opcode::VectorEor:
        Def(inst, B.CreateXor(Arg(inst, 0), Arg(inst, 1)));
        return true;
    case Opcode::VectorOr:
        Def(inst, B.CreateOr(Arg(inst, 0), Arg(inst, 1)));
        return true;
    case Opcode::VectorNot:
        Def(inst, B.CreateNot(Arg(inst, 0)));
        return true;

    // ---- Lane arithmetic ----
    case Opcode::VectorAbs8:
    case Opcode::VectorAbs16:
    case Opcode::VectorAbs32:
    case Opcode::VectorAbs64:
        Def(inst, Call(llvm::Intrinsic::abs, {VecI(e)}, {A(0), B.getFalse()}));  // abs(INT_MIN) = INT_MIN, as Arm's ABS
        return true;
    case Opcode::VectorAdd8:
    case Opcode::VectorAdd16:
    case Opcode::VectorAdd32:
    case Opcode::VectorAdd64:
        Def(inst, B.CreateAdd(A(0), A(1)));
        return true;
    case Opcode::VectorSub8:
    case Opcode::VectorSub16:
    case Opcode::VectorSub32:
    case Opcode::VectorSub64:
        Def(inst, B.CreateSub(A(0), A(1)));
        return true;
    case Opcode::VectorMultiply8:
    case Opcode::VectorMultiply16:
    case Opcode::VectorMultiply32:
    case Opcode::VectorMultiply64:
        Def(inst, B.CreateMul(A(0), A(1)));
        return true;
    case Opcode::VectorMultiplySignedWiden8:
    case Opcode::VectorMultiplySignedWiden16:
    case Opcode::VectorMultiplySignedWiden32:
    case Opcode::VectorMultiplyUnsignedWiden8:
    case Opcode::VectorMultiplyUnsignedWiden16:
    case Opcode::VectorMultiplyUnsignedWiden32: {
        // dynarmic's polyfill (ir/opt/polyfill_pass.cpp): the low halves widened, multiplied at the double width
        const bool s = base == "VectorMultiplySignedWiden";
        Def(inst, B.CreateMul(widen_lower(Arg(inst, 0), e, s), widen_lower(Arg(inst, 1), e, s)));
        return true;
    }
    case Opcode::VectorMaxS8:
    case Opcode::VectorMaxS16:
    case Opcode::VectorMaxS32:
    case Opcode::VectorMaxS64:
        Def(inst, Call(llvm::Intrinsic::smax, {VecI(e)}, {A(0), A(1)}));
        return true;
    case Opcode::VectorMaxU8:
    case Opcode::VectorMaxU16:
    case Opcode::VectorMaxU32:
    case Opcode::VectorMaxU64:
        Def(inst, Call(llvm::Intrinsic::umax, {VecI(e)}, {A(0), A(1)}));
        return true;
    case Opcode::VectorMinS8:
    case Opcode::VectorMinS16:
    case Opcode::VectorMinS32:
    case Opcode::VectorMinS64:
        Def(inst, Call(llvm::Intrinsic::smin, {VecI(e)}, {A(0), A(1)}));
        return true;
    case Opcode::VectorMinU8:
    case Opcode::VectorMinU16:
    case Opcode::VectorMinU32:
    case Opcode::VectorMinU64:
        Def(inst, Call(llvm::Intrinsic::umin, {VecI(e)}, {A(0), A(1)}));
        return true;
    case Opcode::VectorSignedAbsoluteDifference8:
    case Opcode::VectorSignedAbsoluteDifference16:
    case Opcode::VectorSignedAbsoluteDifference32:
        Def(inst, B.CreateSub(Call(llvm::Intrinsic::smax, {VecI(e)}, {A(0), A(1)}), Call(llvm::Intrinsic::smin, {VecI(e)}, {A(0), A(1)})));
        return true;
    case Opcode::VectorUnsignedAbsoluteDifference8:
    case Opcode::VectorUnsignedAbsoluteDifference16:
    case Opcode::VectorUnsignedAbsoluteDifference32:
        Def(inst, B.CreateSub(Call(llvm::Intrinsic::umax, {VecI(e)}, {A(0), A(1)}), Call(llvm::Intrinsic::umin, {VecI(e)}, {A(0), A(1)})));
        return true;
    case Opcode::VectorHalvingAddS8:
    case Opcode::VectorHalvingAddS16:
    case Opcode::VectorHalvingAddS32:
    case Opcode::VectorHalvingAddU8:
    case Opcode::VectorHalvingAddU16:
    case Opcode::VectorHalvingAddU32:
    case Opcode::VectorHalvingSubS8:
    case Opcode::VectorHalvingSubS16:
    case Opcode::VectorHalvingSubS32:
    case Opcode::VectorHalvingSubU8:
    case Opcode::VectorHalvingSubU16:
    case Opcode::VectorHalvingSubU32:
    case Opcode::VectorRoundingHalvingAddS8:
    case Opcode::VectorRoundingHalvingAddS16:
    case Opcode::VectorRoundingHalvingAddS32:
    case Opcode::VectorRoundingHalvingAddU8:
    case Opcode::VectorRoundingHalvingAddU16:
    case Opcode::VectorRoundingHalvingAddU32: {
        // (a + b) >> 1, (a - b) >> 1, (a + b + 1) >> 1 at one bit more than the element, as the Arm ARM computes them
        const bool s = base.back() == 'S';
        auto* wide = llvm::FixedVectorType::get(B.getIntNTy(e + 1), n);
        Value* a = s ? B.CreateSExt(A(0), wide) : B.CreateZExt(A(0), wide);
        Value* b = s ? B.CreateSExt(A(1), wide) : B.CreateZExt(A(1), wide);
        Value* r = base.find("Sub") != std::string::npos ? B.CreateSub(a, b) : B.CreateAdd(a, b);
        if (base.rfind("VectorRounding", 0) == 0) {
            r = B.CreateAdd(r, llvm::ConstantVector::getSplat(llvm::ElementCount::getFixed(n), ConstantInt::get(B.getIntNTy(e + 1), 1)));
        }
        r = s ? B.CreateAShr(r, 1) : B.CreateLShr(r, 1);
        Def(inst, B.CreateTrunc(r, VecI(e)));
        return true;
    }
    case Opcode::VectorCountLeadingZeros8:
    case Opcode::VectorCountLeadingZeros16:
    case Opcode::VectorCountLeadingZeros32:
        Def(inst, Call(llvm::Intrinsic::ctlz, {VecI(e)}, {A(0), B.getFalse()}));
        return true;
    case Opcode::VectorPopulationCount:
        Def(inst, Call(llvm::Intrinsic::ctpop, {VecI(8)}, {A8(0)}));
        return true;
    case Opcode::VectorReverseBits:
        Def(inst, Call(llvm::Intrinsic::bitreverse, {VecI(8)}, {A8(0)}));
        return true;

    // ---- Comparisons ----
    case Opcode::VectorEqual8:
    case Opcode::VectorEqual16:
    case Opcode::VectorEqual32:
    case Opcode::VectorEqual64:
        Def(inst, sext_mask(B.CreateICmpEQ(A(0), A(1))));
        return true;
    case Opcode::VectorEqual128: {
        Value* eq = B.CreateICmpEQ(ArgAs(inst, 0, B.getInt128Ty()), ArgAs(inst, 1, B.getInt128Ty()));
        Def(inst, B.CreateBitCast(B.CreateSExt(eq, B.getInt128Ty()), V128()));
        return true;
    }
    case Opcode::VectorGreaterS8:
    case Opcode::VectorGreaterS16:
    case Opcode::VectorGreaterS32:
    case Opcode::VectorGreaterS64:
        Def(inst, sext_mask(B.CreateICmpSGT(A(0), A(1))));
        return true;

    // ---- Shifts ----
    case Opcode::VectorLogicalShiftLeft8:
    case Opcode::VectorLogicalShiftLeft16:
    case Opcode::VectorLogicalShiftLeft32:
    case Opcode::VectorLogicalShiftLeft64:
        Def(inst, imm(1) >= e ? zero(e) : B.CreateShl(A(0), Splat(e, imm(1))));
        return true;
    case Opcode::VectorLogicalShiftRight8:
    case Opcode::VectorLogicalShiftRight16:
    case Opcode::VectorLogicalShiftRight32:
    case Opcode::VectorLogicalShiftRight64:
        Def(inst, imm(1) >= e ? zero(e) : B.CreateLShr(A(0), Splat(e, imm(1))));
        return true;
    case Opcode::VectorArithmeticShiftRight8:
    case Opcode::VectorArithmeticShiftRight16:
    case Opcode::VectorArithmeticShiftRight32:
    case Opcode::VectorArithmeticShiftRight64:
        Def(inst, B.CreateAShr(A(0), Splat(e, std::min(imm(1), e - 1))));
        return true;
    case Opcode::VectorLogicalVShift8:
    case Opcode::VectorLogicalVShift16:
    case Opcode::VectorLogicalVShift32:
    case Opcode::VectorLogicalVShift64:
    case Opcode::VectorArithmeticVShift8:
    case Opcode::VectorArithmeticVShift16:
    case Opcode::VectorArithmeticVShift32:
    case Opcode::VectorArithmeticVShift64: {
        // ushl / sshl: the shift is the signed low byte of each lane of the second operand; negative shifts right. An amount of the width or
        // more gives 0 (or the sign, shifting right arithmetically), where LLVM's shifts would be poison
        const bool arith = base == "VectorArithmeticVShift";
        Value* a = A(0);
        Value* s = B.CreateSExt(B.CreateTrunc(A(1), llvm::FixedVectorType::get(B.getInt8Ty(), n)), VecI(e));
        if (e == 8) {
            s = B.CreateTrunc(A(1), VecI(8));
        }
        Value* zv = zero(e);
        Value* width = Splat(e, e);
        Value* neg = B.CreateICmpSLT(s, zv);
        Value* amount = B.CreateSelect(neg, B.CreateNeg(s), s);  // |s|, as an unsigned count (128 for s = -128 at 8 bits: compared unsigned below)
        Value* big = B.CreateICmpUGE(amount, width);  // unsigned: |-128| at 8 bits is 0x80, which compares as >= 8
        Value* safe = B.CreateSelect(big, zv, amount);
        Value* left = B.CreateSelect(big, zv, B.CreateShl(a, safe));
        Value* right = arith ? B.CreateAShr(a, B.CreateSelect(big, Splat(e, e - 1), amount)) : B.CreateSelect(big, zv, B.CreateLShr(a, safe));
        Def(inst, B.CreateSelect(neg, right, left));
        return true;
    }
    case Opcode::VectorRoundingShiftLeftS8:
    case Opcode::VectorRoundingShiftLeftS16:
    case Opcode::VectorRoundingShiftLeftS32:
    case Opcode::VectorRoundingShiftLeftS64:
    case Opcode::VectorRoundingShiftLeftU8:
    case Opcode::VectorRoundingShiftLeftU16:
    case Opcode::VectorRoundingShiftLeftU32:
    case Opcode::VectorRoundingShiftLeftU64: {
        // srshl / urshl: a left shift, or for a negative amount n the rounding right shift (a + 2^(n-1)) >> n, computed as
        // (a >> n) + ((a >> (n - 1)) & 1) so that it cannot overflow; dynarmic's RoundingShiftLeft (emit_x64_vector.cpp)
        const bool sgn = base.back() == 'S';
        Value* a = A(0);
        Value* s = e == 8 ? B.CreateTrunc(A(1), VecI(8)) : B.CreateSExt(B.CreateTrunc(A(1), llvm::FixedVectorType::get(B.getInt8Ty(), n)), VecI(e));
        Value* zv = zero(e);
        Value* neg = B.CreateICmpSLT(s, zv);
        Value* amount = B.CreateSelect(neg, B.CreateNeg(s), s);
        Value* width = Splat(e, e);
        Value* big = B.CreateICmpUGE(amount, width);
        Value* left = B.CreateSelect(big, zv, B.CreateShl(a, B.CreateSelect(big, zv, amount)));
        Value* nm1 = B.CreateSub(amount, Splat(e, 1));
        Value* right = nullptr;
        if (sgn) {
            // shifts of the width or more saturate at width - 1: the arithmetic shift fills with the sign, and the rounding bit is the sign bit
            Value* max = Splat(e, e - 1);
            Value* n_c = B.CreateSelect(B.CreateICmpUGT(amount, max), max, amount);
            Value* nm1_c = B.CreateSelect(B.CreateICmpUGT(nm1, max), max, nm1);
            right = B.CreateAdd(B.CreateAShr(a, n_c), B.CreateAnd(B.CreateLShr(a, nm1_c), Splat(e, 1)));
        } else {
            Value* shr = B.CreateSelect(big, zv, B.CreateLShr(a, B.CreateSelect(big, zv, amount)));
            Value* bit_big = B.CreateICmpUGE(nm1, width);
            Value* bit = B.CreateSelect(bit_big, zv, B.CreateAnd(B.CreateLShr(a, B.CreateSelect(bit_big, zv, nm1)), Splat(e, 1)));
            right = B.CreateAdd(shr, bit);
        }
        Def(inst, B.CreateSelect(neg, right, left));
        return true;
    }

    // ---- Broadcasts ----
    case Opcode::VectorBroadcast8:
    case Opcode::VectorBroadcast16:
    case Opcode::VectorBroadcast32:
    case Opcode::VectorBroadcast64:
        Def(inst, B.CreateVectorSplat(n, Arg(inst, 0)));
        return true;
    case Opcode::VectorBroadcastLower8:
    case Opcode::VectorBroadcastLower16:
    case Opcode::VectorBroadcastLower32:
        Def(inst, ZeroUpper(B.CreateVectorSplat(n, Arg(inst, 0))));
        return true;
    case Opcode::VectorBroadcastElement8:
    case Opcode::VectorBroadcastElement16:
    case Opcode::VectorBroadcastElement32:
    case Opcode::VectorBroadcastElement64:
        Def(inst, Shuffle(A(0), A(0), std::vector<int>(n, static_cast<int>(imm(1) % n))));
        return true;
    case Opcode::VectorBroadcastElementLower8:
    case Opcode::VectorBroadcastElementLower16:
    case Opcode::VectorBroadcastElementLower32:
        Def(inst, ZeroUpper(Shuffle(A(0), A(0), std::vector<int>(n, static_cast<int>(imm(1) % n)))));
        return true;

    // ---- Permutes ----
    case Opcode::VectorDeinterleaveEven8:
    case Opcode::VectorDeinterleaveEven16:
    case Opcode::VectorDeinterleaveEven32:
    case Opcode::VectorDeinterleaveEven64:
        Def(inst, Shuffle(A(0), A(1), Seq(n, 0, 2)));
        return true;
    case Opcode::VectorDeinterleaveOdd8:
    case Opcode::VectorDeinterleaveOdd16:
    case Opcode::VectorDeinterleaveOdd32:
    case Opcode::VectorDeinterleaveOdd64:
        Def(inst, Shuffle(A(0), A(1), Seq(n, 1, 2)));
        return true;
    case Opcode::VectorDeinterleaveEvenLower8:
    case Opcode::VectorDeinterleaveEvenLower16:
    case Opcode::VectorDeinterleaveEvenLower32:
    case Opcode::VectorDeinterleaveOddLower8:
    case Opcode::VectorDeinterleaveOddLower16:
    case Opcode::VectorDeinterleaveOddLower32: {
        // uzp1 / uzp2 of 64-bit vectors: the even (odd) lanes of a's low half, then of b's, in the low 64 bits
        const int first = base == "VectorDeinterleaveOddLower" ? 1 : 0;
        std::vector<int> m;
        for (unsigned i = 0; i < n / 4; ++i) {
            m.push_back(first + 2 * static_cast<int>(i));
        }
        for (unsigned i = 0; i < n / 4; ++i) {
            m.push_back(static_cast<int>(n) + first + 2 * static_cast<int>(i));
        }
        for (unsigned i = 0; i < n / 2; ++i) {
            m.push_back(static_cast<int>(n));  // lane 0 of b: replaced by zero below
        }
        Value* r = Shuffle(A(0), A(1), m);
        Def(inst, ZeroUpper(r));
        return true;
    }
    case Opcode::VectorInterleaveLower8:
    case Opcode::VectorInterleaveLower16:
    case Opcode::VectorInterleaveLower32:
    case Opcode::VectorInterleaveLower64:
    case Opcode::VectorInterleaveUpper8:
    case Opcode::VectorInterleaveUpper16:
    case Opcode::VectorInterleaveUpper32:
    case Opcode::VectorInterleaveUpper64: {
        const int start = base == "VectorInterleaveUpper" ? static_cast<int>(n / 2) : 0;
        std::vector<int> m;
        for (unsigned i = 0; i < n / 2; ++i) {
            m.push_back(start + static_cast<int>(i));
            m.push_back(static_cast<int>(n) + start + static_cast<int>(i));
        }
        Def(inst, Shuffle(A(0), A(1), m));
        return true;
    }
    case Opcode::VectorTranspose8:
    case Opcode::VectorTranspose16:
    case Opcode::VectorTranspose32:
    case Opcode::VectorTranspose64: {
        // trn1 (part 0): a0 b0 a2 b2 ...; trn2 (part 1): a1 b1 a3 b3 ...
        const int part = inst.GetArg(2).GetU1() ? 1 : 0;
        std::vector<int> m;
        for (unsigned i = 0; i < n / 2; ++i) {
            m.push_back(static_cast<int>(2 * i) + part);
            m.push_back(static_cast<int>(n + 2 * i) + part);
        }
        Def(inst, Shuffle(A(0), A(1), m));
        return true;
    }
    case Opcode::VectorExtract: {
        // ext: (b:a) >> position, position a multiple of 8
        const unsigned p = imm(2) / 8;
        Def(inst, Shuffle(A8(0), A8(1), Seq(16, static_cast<int>(p), 1)));
        return true;
    }
    case Opcode::VectorExtractLower: {
        // ext of 64-bit vectors: (b[63:0]:a[63:0]) >> position, in the low 64 bits
        const unsigned p = imm(2) / 8;
        std::vector<int> m;
        for (unsigned i = 0; i < 16; ++i) {
            const unsigned k = i < 8 ? i + p : 0;
            m.push_back(k < 8 ? static_cast<int>(k) : static_cast<int>(16 + k - 8));
        }
        Def(inst, ZeroUpper(Shuffle(A8(0), A8(1), m)));
        return true;
    }
    case Opcode::VectorRotateWholeVectorRight: {
        const unsigned lanes = imm(1) / 32;
        Def(inst, Shuffle(ArgVec(inst, 0, 32), ArgVec(inst, 0, 32), {static_cast<int>(lanes % 4), static_cast<int>((lanes + 1) % 4),
                                                                      static_cast<int>((lanes + 2) % 4), static_cast<int>((lanes + 3) % 4)}));
        return true;
    }
    case Opcode::VectorReverseElementsInHalfGroups8:
    case Opcode::VectorReverseElementsInWordGroups8:
    case Opcode::VectorReverseElementsInWordGroups16:
    case Opcode::VectorReverseElementsInLongGroups8:
    case Opcode::VectorReverseElementsInLongGroups16:
    case Opcode::VectorReverseElementsInLongGroups32: {
        const unsigned group = base == "VectorReverseElementsInHalfGroups" ? 16 : base == "VectorReverseElementsInWordGroups" ? 32 : 64;
        const unsigned per = group / e;
        std::vector<int> m;
        for (unsigned i = 0; i < n; ++i) {
            m.push_back(static_cast<int>((i / per) * per + (per - 1 - i % per)));
        }
        Def(inst, Shuffle(A(0), A(0), m));
        return true;
    }
    case Opcode::VectorNarrow16:
    case Opcode::VectorNarrow32:
    case Opcode::VectorNarrow64: {
        // xtn: every lane truncated to half its width, into the low 64 bits
        Value* t = B.CreateTrunc(A(0), llvm::FixedVectorType::get(B.getIntNTy(e / 2), n));
        Def(inst, low_half(t, e / 2));
        return true;
    }
    case Opcode::VectorSignExtend8:
    case Opcode::VectorSignExtend16:
    case Opcode::VectorSignExtend32:
    case Opcode::VectorSignExtend64:
    case Opcode::VectorZeroExtend8:
    case Opcode::VectorZeroExtend16:
    case Opcode::VectorZeroExtend32:
    case Opcode::VectorZeroExtend64: {
        // sxtl / uxtl: the lanes of the low 64 bits at the double width
        const bool s = base == "VectorSignExtend";
        if (e == 64) {
            Value* lo = B.CreateExtractElement(ArgVec(inst, 0, 64), uint64_t{0});
            Value* hi = s ? B.CreateAShr(lo, 63) : B.getInt64(0);
            Def(inst, B.CreateInsertElement(B.CreateInsertElement(llvm::PoisonValue::get(V128()), lo, uint64_t{0}), hi, uint64_t{1}));
            return true;
        }
        Def(inst, widen_lower(Arg(inst, 0), e, s));
        return true;
    }

    // ---- Pairwise and across lanes ----
    case Opcode::VectorPairedAdd8:
    case Opcode::VectorPairedAdd16:
    case Opcode::VectorPairedAdd32:
    case Opcode::VectorPairedAdd64:
        Def(inst, B.CreateAdd(Shuffle(A(0), A(1), Seq(n, 0, 2)), Shuffle(A(0), A(1), Seq(n, 1, 2))));
        return true;
    case Opcode::VectorPairedAddLower8:
    case Opcode::VectorPairedAddLower16:
    case Opcode::VectorPairedAddLower32: {
        // the pairs of a's low half, then of b's, in the low 64 bits
        std::vector<int> ev, od;
        for (unsigned i = 0; i < n / 4; ++i) {
            ev.push_back(static_cast<int>(2 * i));
            od.push_back(static_cast<int>(2 * i + 1));
        }
        for (unsigned i = 0; i < n / 4; ++i) {
            ev.push_back(static_cast<int>(n + 2 * i));
            od.push_back(static_cast<int>(n + 2 * i + 1));
        }
        for (unsigned i = 0; i < n / 2; ++i) {
            ev.push_back(0);
            od.push_back(0);
        }
        Def(inst, ZeroUpper(B.CreateAdd(Shuffle(A(0), A(1), ev), Shuffle(A(0), A(1), od))));
        return true;
    }
    case Opcode::VectorPairedAddSignedWiden8:
    case Opcode::VectorPairedAddSignedWiden16:
    case Opcode::VectorPairedAddSignedWiden32:
    case Opcode::VectorPairedAddUnsignedWiden8:
    case Opcode::VectorPairedAddUnsignedWiden16:
    case Opcode::VectorPairedAddUnsignedWiden32: {
        // saddlp / uaddlp: adjacent pairs of the whole vector, widened
        const bool s = base == "VectorPairedAddSignedWiden";
        auto* wide = VecI(2 * e);
        Value* ev = Shuffle(A(0), A(0), Seq(n / 2, 0, 2));
        Value* od = Shuffle(A(0), A(0), Seq(n / 2, 1, 2));
        Value* w0 = s ? B.CreateSExt(ev, wide) : B.CreateZExt(ev, wide);
        Value* w1 = s ? B.CreateSExt(od, wide) : B.CreateZExt(od, wide);
        Def(inst, B.CreateAdd(w0, w1));
        return true;
    }
    case Opcode::VectorPairedMaxS8:
    case Opcode::VectorPairedMaxS16:
    case Opcode::VectorPairedMaxS32:
    case Opcode::VectorPairedMaxU8:
    case Opcode::VectorPairedMaxU16:
    case Opcode::VectorPairedMaxU32:
    case Opcode::VectorPairedMinS8:
    case Opcode::VectorPairedMinS16:
    case Opcode::VectorPairedMinS32:
    case Opcode::VectorPairedMinU8:
    case Opcode::VectorPairedMinU16:
    case Opcode::VectorPairedMinU32:
    case Opcode::VectorPairedMaxLowerS8:
    case Opcode::VectorPairedMaxLowerS16:
    case Opcode::VectorPairedMaxLowerS32:
    case Opcode::VectorPairedMaxLowerU8:
    case Opcode::VectorPairedMaxLowerU16:
    case Opcode::VectorPairedMaxLowerU32:
    case Opcode::VectorPairedMinLowerS8:
    case Opcode::VectorPairedMinLowerS16:
    case Opcode::VectorPairedMinLowerS32:
    case Opcode::VectorPairedMinLowerU8:
    case Opcode::VectorPairedMinLowerU16:
    case Opcode::VectorPairedMinLowerU32: {
        const bool lower = base.find("Lower") != std::string::npos;
        const bool is_max = base.find("Max") != std::string::npos;
        const bool s = base.back() == 'S';
        const ID id = is_max ? (s ? llvm::Intrinsic::smax : llvm::Intrinsic::umax) : (s ? llvm::Intrinsic::smin : llvm::Intrinsic::umin);
        std::vector<int> ev, od;
        if (lower) {
            for (unsigned i = 0; i < n / 4; ++i) {
                ev.push_back(static_cast<int>(2 * i));
                od.push_back(static_cast<int>(2 * i + 1));
            }
            for (unsigned i = 0; i < n / 4; ++i) {
                ev.push_back(static_cast<int>(n + 2 * i));
                od.push_back(static_cast<int>(n + 2 * i + 1));
            }
            for (unsigned i = 0; i < n / 2; ++i) {
                ev.push_back(0);
                od.push_back(0);
            }
        } else {
            ev = Seq(n, 0, 2);
            od = Seq(n, 1, 2);
        }
        Value* r = Call(id, {VecI(e)}, {Shuffle(A(0), A(1), ev), Shuffle(A(0), A(1), od)});
        Def(inst, lower ? ZeroUpper(r) : r);
        return true;
    }
    case Opcode::VectorReduceAdd8:
    case Opcode::VectorReduceAdd16:
    case Opcode::VectorReduceAdd32:
    case Opcode::VectorReduceAdd64: {
        // addv: the sum of every lane in lane 0, the others zero
        Value* sum = Call(llvm::Intrinsic::vector_reduce_add, {VecI(e)}, {A(0)});
        Def(inst, B.CreateInsertElement(zero(e), sum, uint64_t{0}));
        return true;
    }

    // ---- Polynomial multiplication ----
    case Opcode::VectorPolynomialMultiply8: {
        // pmul: the low 8 bits of each byte's carry-less product
        Value* a = A8(0);
        Value* b = A8(1);
        Value* r = zero(8);
        for (unsigned bit = 0; bit < 8; ++bit) {
            Value* take = B.CreateICmpNE(B.CreateAnd(b, Splat(8, 1u << bit)), zero(8));
            r = B.CreateXor(r, B.CreateSelect(take, B.CreateShl(a, Splat(8, bit)), zero(8)));
        }
        Def(inst, r);
        return true;
    }
    case Opcode::VectorPolynomialMultiplyLong8: {
        // pmull (8B): the low eight bytes, 16-bit carry-less products
        Value* a = widen_lower(Arg(inst, 0), 8, false);
        Value* b = widen_lower(Arg(inst, 1), 8, false);
        Value* r = zero(16);
        for (unsigned bit = 0; bit < 8; ++bit) {
            Value* take = B.CreateICmpNE(B.CreateAnd(b, Splat(16, 1u << bit)), zero(16));
            r = B.CreateXor(r, B.CreateSelect(take, B.CreateShl(a, Splat(16, bit)), zero(16)));
        }
        Def(inst, r);
        return true;
    }
    case Opcode::VectorPolynomialMultiplyLong64: {
        // pmull (1Q): the 128-bit carry-less product of the low 64-bit lanes
        if (opts.target.pclmul) {
            Def(inst, Call(llvm::Intrinsic::x86_pclmulqdq, {}, {ArgVec(inst, 0, 64), ArgVec(inst, 1, 64), B.getInt8(0)}));
            return true;
        }
        Value* a = B.CreateZExt(B.CreateExtractElement(ArgVec(inst, 0, 64), uint64_t{0}), B.getInt128Ty());
        Value* b = B.CreateExtractElement(ArgVec(inst, 1, 64), uint64_t{0});
        Value* r = llvm::Constant::getNullValue(B.getInt128Ty());
        for (unsigned bit = 0; bit < 64; ++bit) {
            Value* take = B.CreateTrunc(B.CreateLShr(b, bit), B.getInt1Ty());
            r = B.CreateXor(r, B.CreateSelect(take, B.CreateShl(a, bit), llvm::Constant::getNullValue(B.getInt128Ty())));
        }
        Def(inst, B.CreateBitCast(r, V128()));
        return true;
    }

    // ---- Table lookup ----
    case Opcode::VectorTable:
        // Nothing to compute: VectorTableLookup reads the registers of its table operand
        Def(inst, llvm::UndefValue::get(B.getInt8Ty()));
        return true;
    case Opcode::VectorTableLookup128: {
        // tbl / tbx: each index byte selects a byte of the table registers (16 per register); an index past the table keeps the default byte
        // (0 for tbl, the destination's byte for tbx). pshufb per table register, a select on the index's high bits
        const IR::Inst* table = inst.GetArg(1).GetInst();
        Value* idx = A8(2);
        Value* result = A8(0);
        Value* high = B.CreateAnd(idx, Splat(8, 0xF0));
        for (size_t t = 0; t < table->NumArgs(); ++t) {
            const IR::Value tv = table->GetArg(t);
            if (tv.IsEmpty()) {
                break;
            }
            Value* reg = As(Get(tv), VecI(8));
            Value* picked = Call(llvm::Intrinsic::x86_ssse3_pshuf_b_128, {}, {reg, B.CreateAnd(idx, Splat(8, 0x0F))});
            Value* in = B.CreateICmpEQ(high, Splat(8, 16 * t));
            result = B.CreateSelect(in, picked, result);
        }
        Def(inst, result);
        return true;
    }

    // ---- Unsigned estimates ----
    case Opcode::VectorUnsignedRecipEstimate:
    case Opcode::VectorUnsignedRecipSqrtEstimate: {
        // urecpe / ursqrte (32-bit lanes): the Arm ARM's 9-bit-input estimate tables (dynarmic's Common::RecipEstimate, RecipSqrtEstimate)
        const bool sqrt = op == Opcode::VectorUnsignedRecipSqrtEstimate;
        const std::string tname = sqrt ? "nxaot.ursqrte.table" : "nxaot.urecpe.table";
        llvm::GlobalVariable* gt = M.getGlobalVariable(tname, true);
        if (!gt) {
            std::vector<uint32_t> entries(512);
            for (uint32_t i = 0; i < 512; ++i) {
                const bool valid = sqrt ? i >= 128 : i >= 256;
                entries[i] = valid ? ((0x100u | (sqrt ? UnsignedRecipSqrtEstimate(i) : UnsignedRecipEstimate(i))) << 23) : 0;
            }
            auto* init = llvm::ConstantDataArray::get(C, entries);
            gt = new llvm::GlobalVariable(M, init->getType(), true, llvm::GlobalValue::PrivateLinkage, init, tname);
        }
        Def(inst, PerLane(Arg(inst, 0), 32, [&](Value* lane) {
            Value* index = B.CreateZExt(B.CreateLShr(lane, 23), B.getInt64Ty());
            Value* entry = B.CreateAlignedLoad(B.getInt32Ty(), B.CreateInBoundsGEP(gt->getValueType(), gt, {B.getInt64(0), index}), llvm::Align(4));
            Value* small = sqrt ? B.CreateICmpULT(lane, B.getInt32(0x40000000)) : B.CreateICmpULT(lane, B.getInt32(0x80000000u));
            return B.CreateSelect(small, B.getInt32(0xFFFFFFFFu), entry);
        }));
        return true;
    }
    default:
        return false;
    }
}

bool FunctionLifter::LowerSaturated(IR::Inst& inst) {
    const Opcode op = inst.GetOpcode();
    const std::string base = BaseName(op);
    const unsigned e = SizeSuffix(op);
    const bool vector = base.rfind("Vector", 0) == 0;
    const unsigned n = vector && e ? 128 / e : 1;
    llvm::Type* lane_t = e ? (vector ? static_cast<llvm::Type*>(VecI(e)) : static_cast<llvm::Type*>(B.getIntNTy(e))) : nullptr;
    auto A = [&](size_t i) { return ArgAs(inst, i, lane_t); };
    auto splat = [&](unsigned bits, uint64_t v) -> Value* {
        return vector ? Splat(bits, v) : static_cast<Value*>(ConstantInt::get(B.getIntNTy(bits), v));
    };
    auto wide_t = [&](unsigned bits) -> llvm::Type* {
        return vector ? static_cast<llvm::Type*>(llvm::FixedVectorType::get(B.getIntNTy(bits), n)) : static_cast<llvm::Type*>(B.getIntNTy(bits));
    };
    auto wconst = [&](unsigned bits, const llvm::APInt& v) -> Value* {
        Value* c = ConstantInt::get(B.getIntNTy(bits), v);
        return vector ? B.CreateVectorSplat(n, c) : c;
    };
    // value clamped to the signed or unsigned range of `to` bits, from a wider type; QC when it was outside
    auto clamp = [&](Value* wide, unsigned bits, unsigned to, bool to_signed) {
        const llvm::APInt lo = to_signed ? llvm::APInt::getSignedMinValue(to).sext(bits) : llvm::APInt::getZero(bits);
        const llvm::APInt hi = to_signed ? llvm::APInt::getSignedMaxValue(to).sext(bits) : llvm::APInt::getMaxValue(to).zext(bits);
        Value* below = B.CreateICmpSLT(wide, wconst(bits, lo));
        Value* above = B.CreateICmpSGT(wide, wconst(bits, hi));
        OrQC(AnyLane(B.CreateOr(below, above)));
        Value* r = B.CreateSelect(below, wconst(bits, lo), B.CreateSelect(above, wconst(bits, hi), wide));
        return B.CreateTrunc(r, wide_t(to));
    };

    switch (op) {
    case Opcode::SignedSaturatedAdd8:
    case Opcode::SignedSaturatedAdd16:
    case Opcode::SignedSaturatedAdd32:
    case Opcode::SignedSaturatedAdd64:
    case Opcode::VectorSignedSaturatedAdd8:
    case Opcode::VectorSignedSaturatedAdd16:
    case Opcode::VectorSignedSaturatedAdd32:
    case Opcode::VectorSignedSaturatedAdd64:
    case Opcode::SignedSaturatedSub8:
    case Opcode::SignedSaturatedSub16:
    case Opcode::SignedSaturatedSub32:
    case Opcode::SignedSaturatedSub64:
    case Opcode::VectorSignedSaturatedSub8:
    case Opcode::VectorSignedSaturatedSub16:
    case Opcode::VectorSignedSaturatedSub32:
    case Opcode::VectorSignedSaturatedSub64:
    case Opcode::UnsignedSaturatedAdd8:
    case Opcode::UnsignedSaturatedAdd16:
    case Opcode::UnsignedSaturatedAdd32:
    case Opcode::UnsignedSaturatedAdd64:
    case Opcode::VectorUnsignedSaturatedAdd8:
    case Opcode::VectorUnsignedSaturatedAdd16:
    case Opcode::VectorUnsignedSaturatedAdd32:
    case Opcode::VectorUnsignedSaturatedAdd64:
    case Opcode::UnsignedSaturatedSub8:
    case Opcode::UnsignedSaturatedSub16:
    case Opcode::UnsignedSaturatedSub32:
    case Opcode::UnsignedSaturatedSub64:
    case Opcode::VectorUnsignedSaturatedSub8:
    case Opcode::VectorUnsignedSaturatedSub16:
    case Opcode::VectorUnsignedSaturatedSub32:
    case Opcode::VectorUnsignedSaturatedSub64: {
        const bool s = base.find("Unsigned") == std::string::npos;
        const bool sub = base.find("Sub") != std::string::npos;
        const ID id = s ? (sub ? llvm::Intrinsic::ssub_sat : llvm::Intrinsic::sadd_sat) : (sub ? llvm::Intrinsic::usub_sat : llvm::Intrinsic::uadd_sat);
        Value* a = A(0);
        Value* b = A(1);
        Value* r = Call(id, {lane_t}, {a, b});
        Value* wrapped = sub ? B.CreateSub(a, b) : B.CreateAdd(a, b);
        OrQC(AnyLane(B.CreateICmpNE(r, wrapped)));
        Def(inst, r);
        return true;
    }
    case Opcode::SignedSaturatedDoublingMultiplyReturnHigh16:
    case Opcode::SignedSaturatedDoublingMultiplyReturnHigh32:
    case Opcode::VectorSignedSaturatedDoublingMultiplyHigh16:
    case Opcode::VectorSignedSaturatedDoublingMultiplyHigh32:
    case Opcode::VectorSignedSaturatedDoublingMultiplyHighRounding16:
    case Opcode::VectorSignedSaturatedDoublingMultiplyHighRounding32: {
        // sqdmulh / sqrdmulh: (2ab [+ 2^(e-1)]) >> e, saturated (only INT_MIN * INT_MIN overflows)
        const bool rounding = base.find("Rounding") != std::string::npos;
        const unsigned w = 2 * e + 2;
        Value* a = B.CreateSExt(A(0), wide_t(w));
        Value* b = B.CreateSExt(A(1), wide_t(w));
        Value* p = B.CreateShl(B.CreateMul(a, b), 1);
        if (rounding) {
            p = B.CreateAdd(p, wconst(w, llvm::APInt(w, 1).shl(e - 1)));
        }
        Def(inst, clamp(B.CreateAShr(p, e), w, e, true));
        return true;
    }
    case Opcode::VectorSignedSaturatedDoublingMultiplyLong16:
    case Opcode::VectorSignedSaturatedDoublingMultiplyLong32: {
        // sqdmull: the low halves, 2ab at the double width, saturated
        Value* a = Shuffle(A(0), A(0), Seq(64 / e, 0, 1));
        Value* b = Shuffle(A(1), A(1), Seq(64 / e, 0, 1));
        const unsigned w = 2 * e + 2;
        auto* wt = llvm::FixedVectorType::get(B.getIntNTy(w), 64 / e);
        Value* p = B.CreateShl(B.CreateMul(B.CreateSExt(a, wt), B.CreateSExt(b, wt)), 1);
        const llvm::APInt lo = llvm::APInt::getSignedMinValue(2 * e).sext(w), hi = llvm::APInt::getSignedMaxValue(2 * e).sext(w);
        Value* lo_v = B.CreateVectorSplat(64 / e, ConstantInt::get(B.getIntNTy(w), lo));
        Value* hi_v = B.CreateVectorSplat(64 / e, ConstantInt::get(B.getIntNTy(w), hi));
        Value* above = B.CreateICmpSGT(p, hi_v);
        Value* below = B.CreateICmpSLT(p, lo_v);
        OrQC(AnyLane(B.CreateOr(above, below)));
        Value* r = B.CreateSelect(above, hi_v, B.CreateSelect(below, lo_v, p));
        Def(inst, B.CreateTrunc(r, VecI(2 * e)));
        return true;
    }
    case Opcode::VectorSignedSaturatedAbs8:
    case Opcode::VectorSignedSaturatedAbs16:
    case Opcode::VectorSignedSaturatedAbs32:
    case Opcode::VectorSignedSaturatedAbs64:
    case Opcode::VectorSignedSaturatedNeg8:
    case Opcode::VectorSignedSaturatedNeg16:
    case Opcode::VectorSignedSaturatedNeg32:
    case Opcode::VectorSignedSaturatedNeg64: {
        // sqabs / sqneg: INT_MIN gives INT_MAX
        Value* a = A(0);
        Value* min = splat(e, llvm::APInt::getSignedMinValue(e).getZExtValue());
        Value* is_min = B.CreateICmpEQ(a, min);
        OrQC(AnyLane(is_min));
        Value* r = base == "VectorSignedSaturatedAbs" ? Call(llvm::Intrinsic::abs, {lane_t}, {a, B.getFalse()}) : B.CreateNeg(a);
        Def(inst, B.CreateSelect(is_min, splat(e, llvm::APInt::getSignedMaxValue(e).getZExtValue()), r));
        return true;
    }
    case Opcode::VectorSignedSaturatedAccumulateUnsigned8:
    case Opcode::VectorSignedSaturatedAccumulateUnsigned16:
    case Opcode::VectorSignedSaturatedAccumulateUnsigned32:
    case Opcode::VectorSignedSaturatedAccumulateUnsigned64: {
        // suqadd: the signed accumulator (operand 1, Vd) plus the unsigned operand 0 (Vn), saturated signed
        const unsigned w = e + 2;
        Value* sum = B.CreateAdd(B.CreateZExt(A(0), wide_t(w)), B.CreateSExt(A(1), wide_t(w)));
        Def(inst, clamp(sum, w, e, true));
        return true;
    }
    case Opcode::VectorUnsignedSaturatedAccumulateSigned8:
    case Opcode::VectorUnsignedSaturatedAccumulateSigned16:
    case Opcode::VectorUnsignedSaturatedAccumulateSigned32:
    case Opcode::VectorUnsignedSaturatedAccumulateSigned64: {
        // usqadd: the unsigned accumulator (operand 1, Vd) plus the signed operand 0 (Vn), saturated unsigned
        const unsigned w = e + 2;
        Value* sum = B.CreateAdd(B.CreateSExt(A(0), wide_t(w)), B.CreateZExt(A(1), wide_t(w)));
        Def(inst, clamp(sum, w, e, false));
        return true;
    }
    case Opcode::VectorSignedSaturatedNarrowToSigned16:
    case Opcode::VectorSignedSaturatedNarrowToSigned32:
    case Opcode::VectorSignedSaturatedNarrowToSigned64:
    case Opcode::VectorSignedSaturatedNarrowToUnsigned16:
    case Opcode::VectorSignedSaturatedNarrowToUnsigned32:
    case Opcode::VectorSignedSaturatedNarrowToUnsigned64:
    case Opcode::VectorUnsignedSaturatedNarrow16:
    case Opcode::VectorUnsignedSaturatedNarrow32:
    case Opcode::VectorUnsignedSaturatedNarrow64: {
        // sqxtn, sqxtun, uqxtn: each lane saturated to half its width, into the low 64 bits
        const bool from_signed = base.rfind("VectorSignedSaturated", 0) == 0;
        const bool to_signed = base == "VectorSignedSaturatedNarrowToSigned";
        const unsigned w = e + 1;
        Value* wide = from_signed ? B.CreateSExt(A(0), wide_t(w)) : B.CreateZExt(A(0), wide_t(w));
        Value* narrow = clamp(wide, w, e / 2, to_signed);
        Value* z = llvm::Constant::getNullValue(narrow->getType());
        Def(inst, Shuffle(narrow, z, Seq(2 * n, 0, 1)));
        return true;
    }
    case Opcode::VectorSignedSaturatedShiftLeft8:
    case Opcode::VectorSignedSaturatedShiftLeft16:
    case Opcode::VectorSignedSaturatedShiftLeft32:
    case Opcode::VectorSignedSaturatedShiftLeft64:
    case Opcode::VectorUnsignedSaturatedShiftLeft8:
    case Opcode::VectorUnsignedSaturatedShiftLeft16:
    case Opcode::VectorUnsignedSaturatedShiftLeft32:
    case Opcode::VectorUnsignedSaturatedShiftLeft64: {
        // sqshl / uqshl (register): the signed low byte of each lane of operand 1; a left shift saturates, a right shift truncates (for the
        // signed form by at most e - 1, filling with the sign; for the unsigned by e or more to 0). dynarmic's
        // VectorSignedSaturatedShiftLeft / VectorUnsignedSaturatedShiftLeft (emit_x64_vector.cpp)
        const bool s = base == "VectorSignedSaturatedShiftLeft";
        Value* a = A(0);
        Value* sh = e == 8 ? B.CreateTrunc(A(1), VecI(8)) : B.CreateSExt(B.CreateTrunc(A(1), llvm::FixedVectorType::get(B.getInt8Ty(), n)), VecI(e));
        Value* zv = llvm::Constant::getNullValue(VecI(e));
        Value* neg = B.CreateICmpSLT(sh, zv);
        Value* amount = B.CreateSelect(neg, B.CreateNeg(sh), sh);
        Value* width = Splat(e, e);
        Value* right = s ? B.CreateAShr(a, B.CreateSelect(B.CreateICmpUGT(amount, Splat(e, e - 1)), Splat(e, e - 1), amount))
                         : B.CreateSelect(B.CreateICmpUGE(amount, width), zv, B.CreateLShr(a, B.CreateSelect(B.CreateICmpUGE(amount, width), zv, amount)));
        // left: exact when shifting back gives the operand; else saturate (to the sign's extreme, or the unsigned maximum)
        Value* big = B.CreateICmpUGE(amount, width);
        Value* safe = B.CreateSelect(big, zv, amount);
        Value* shifted = B.CreateShl(a, safe);
        Value* back = s ? B.CreateAShr(shifted, safe) : B.CreateLShr(shifted, safe);
        Value* lossless = B.CreateAnd(B.CreateNot(big), B.CreateICmpEQ(back, a));
        Value* is_zero = B.CreateICmpEQ(a, zv);
        Value* sat_value = s ? B.CreateSelect(B.CreateICmpSLT(a, zv), Splat(e, llvm::APInt::getSignedMinValue(e).getZExtValue()),
                                              Splat(e, llvm::APInt::getSignedMaxValue(e).getZExtValue()))
                             : Splat(e, llvm::APInt::getMaxValue(e).getZExtValue());
        Value* left_sat = B.CreateAnd(B.CreateNot(neg), B.CreateAnd(B.CreateNot(is_zero), B.CreateNot(lossless)));
        OrQC(AnyLane(left_sat));
        Value* left = B.CreateSelect(left_sat, sat_value, B.CreateSelect(is_zero, zv, shifted));
        Def(inst, B.CreateSelect(neg, right, left));
        return true;
    }
    case Opcode::VectorSignedSaturatedShiftLeftUnsigned8:
    case Opcode::VectorSignedSaturatedShiftLeftUnsigned16:
    case Opcode::VectorSignedSaturatedShiftLeftUnsigned32:
    case Opcode::VectorSignedSaturatedShiftLeftUnsigned64: {
        // sqshlu (immediate): a signed operand shifted left, saturated to the unsigned range; a negative operand gives 0
        const unsigned amount = static_cast<unsigned>(inst.GetArg(1).GetU8());
        Value* a = A(0);
        Value* zv = llvm::Constant::getNullValue(VecI(e));
        Value* negative = B.CreateICmpSLT(a, zv);
        Value* shifted = amount >= e ? zv : B.CreateShl(a, Splat(e, amount));
        Value* back = amount >= e ? zv : B.CreateLShr(shifted, Splat(e, amount));
        Value* overflow = B.CreateAnd(B.CreateNot(negative), B.CreateICmpNE(back, a));
        OrQC(AnyLane(B.CreateOr(negative, overflow)));
        Def(inst, B.CreateSelect(negative, zv, B.CreateSelect(overflow, Splat(e, llvm::APInt::getMaxValue(e).getZExtValue()), shifted)));
        return true;
    }
    default:
        return false;
    }
}

Value* FunctionLifter::HelperCall(const char* name, llvm::Type* ret, llvm::ArrayRef<Value*> args) {
    std::vector<llvm::Type*> types;
    for (Value* a : args) {
        types.push_back(a->getType());
    }
    auto* fty = llvm::FunctionType::get(ret, types, false);
    llvm::FunctionCallee callee = M.getOrInsertFunction(name, fty);
    if (auto* f = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
        f->addFnAttr(llvm::Attribute::NoUnwind);
    }
    uses_fpsr = true;
    return B.CreateCall(callee, args);
}

}  // namespace nxaot::lift::detail
