using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitHelper;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    static class InstEmitAluHelper
    {
        // A float comparison with .FTZ (FSET and FSETP, Maxwell's FCMP): a denormal operand compares as zero, as on NVIDIA; Apple's GPUs keep fp32 denormals. The compilers set the flag on nearly
        // every comparison, so an immediate or RZ, the usual second operand, is decided here and adds no code
        public static Operand FlushDenormal(EmitterContext context, Operand v)
        {
            if (v.Type == OperandType.Constant)
            {
                return float.IsSubnormal(v.AsFloat()) ? ConstF(0) : v;
            }

            return context.ConditionalSelect(context.FPCompareLess(context.FPAbsolute(v), ConstF(1.17549435e-38f)), ConstF(0), v);
        }

        // Directed rounding of FADD, FMUL and FFMA (.RM, .RP, .RZ; RoundMode 1, 2, 3). Metal only rounds to nearest, so the instruction is computed to nearest (r), the exact error of r is
        // computed with the operations below, and r is moved one ULP (an integer step on its bits) where the error lies on the side the mode excludes. Only the instructions with a directed
        // mode get the extra code (except the sin / cos argument scale, see FPMultiplyRounded); a plain FADD, FMUL or FFMA is unchanged. The error's
        // operations are marked precise: spirv-cross writes them as spvFAdd / spvFSub / spvFMul, [[clang::optnone]] functions, so fast math cannot fold the TwoSum (b - ((a + b) - a) is 0 to
        // it; unmarked, FADD and FFMA kept their nearest result on the Orin test's lanes). The error is exact while nothing in it is a denormal, and Metal flushes those anyway
        private static Operand FPPrecise(Operand v)
        {
            if (v.AsgOp is Operation op)
            {
                op.ForcePrecise = true;
            }

            return v;
        }

        // a + b - s, exactly, for s = a + b rounded to nearest (Knuth's TwoSum)
        private static Operand FPAddError(EmitterContext context, Operand a, Operand b, Operand s)
        {
            Operand bv = FPPrecise(context.FPSubtract(s, a));
            Operand av = FPPrecise(context.FPSubtract(s, bv));

            return FPPrecise(context.FPAdd(FPPrecise(context.FPSubtract(a, av)), FPPrecise(context.FPSubtract(b, bv))));
        }

        // FADD with a directed mode: a + b, rounded per mode. Two operands below 2^-63 are scaled by 2^100 first (exactly), or the error could be a denormal, which Metal flushes
        public static Operand FPAddRounded(EmitterContext context, RoundMode mode, Operand a, Operand b)
        {
            Operand r = context.FPAdd(a, b);

            if (mode == RoundMode.Rn)
            {
                return r;
            }

            Operand small = context.ICompareLessUnsigned(context.BitwiseAnd(context.BitwiseOr(a, b), Const(0x7fffffff)), Const(0x20000000));
            Operand k = context.ConditionalSelect(small, ConstF(BitConverter.Int32BitsToSingle(0x71800000)), ConstF(1f));
            Operand a2 = context.FPMultiply(a, k), b2 = context.FPMultiply(b, k);
            Operand s2 = FPPrecise(context.FPAdd(a2, b2));

            // An exact zero sum of addends that are not both +0 is -0 when rounding down
            Operand negZero = mode == RoundMode.Rm ? context.BitwiseAnd(FPIsZero(context, s2), context.ICompareNotEqual(context.BitwiseOr(a, b), Const(0))) : null;

            return FPRoundDirected(context, mode, r, FPAddError(context, a2, b2, s2), FPAllFinite(context, a, b), negZero);
        }

        // FMUL with a directed mode: a * b times scale (a power of two, null for none), rounded per mode. Scaling is exact, and so is giving both factors the exponent 0: the error of the
        // product of the two significands has the sign of the product's error, and it is never a denormal
        public static Operand FPMultiplyRounded(EmitterContext context, RoundMode mode, Operand a, Operand b, Operand scale)
        {
            Operand r = context.FPMultiply(a, b);

            if (scale != null)
            {
                r = context.FPMultiply(r, scale);
            }

            // NVIDIA's compiler scales every sin and cos argument by 1 / 2pi (0x3e22f983) with FMUL.RZ before MUFU.SIN / MUFU.COS (about 430,000 in Ocarina of Time). Metal's sin and cos
            // are approximations whose error dwarfs one ULP of the argument, so that multiply stays the plain one
            if (mode == RoundMode.Rn || (b.Type == OperandType.Constant && b.Value == 0x3e22f983))
            {
                return r;
            }

            Operand a1 = FPSignificand(context, a), b1 = FPSignificand(context, b);
            Operand err = context.FPFusedMultiplyAdd(a1, b1, context.FPNegate(context.FPMultiply(a1, b1)));

            return FPRoundDirected(context, mode, r, err, FPAllFinite(context, a, b), null);
        }

        // FFMA with a directed mode: a * b + c, rounded per mode. The error is Boldo and Muller's ErrFma (2011): the product split into its rounded value and its error, then two TwoSums;
        // its result has the sign of the exact error. It is computed on a copy scaled by a power of two that brings the larger of a * b and c to [1, 4) (a with the exponent 0, b and c
        // with their exponents shifted), so that no step is a denormal; a term below 2^-70 there is far below an ULP of the other, and it keeps only its sign and significand at 2^-70
        public static Operand FPFusedMultiplyAddRounded(EmitterContext context, RoundMode mode, Operand a, Operand b, Operand c)
        {
            Operand r = context.FPFusedMultiplyAdd(a, b, c);

            if (mode == RoundMode.Rn)
            {
                return r;
            }

            Operand ea = FPExponent(context, a), eb = FPExponent(context, b), ec = FPExponent(context, c);
            Operand pZero = context.BitwiseOr(context.ICompareEqual(ea, Const(0)), context.ICompareEqual(eb, Const(0)));
            Operand cZero = context.ICompareEqual(ec, Const(0));

            // Biased exponents: the product's (ea + eb - 127) and the larger term's (M)
            Operand pe = context.ISubtract(context.IAdd(ea, eb), Const(127));
            Operand m = context.IMaximumS32(context.ConditionalSelect(pZero, Const(-1000), pe), context.ConditionalSelect(cZero, Const(-1000), ec));

            Operand a1 = FPSignificand(context, a);
            Operand b1 = context.ConditionalSelect(pZero, Const(0), FPWithExponent(context, b, context.IMaximumS32(context.ISubtract(context.IAdd(ea, eb), m), Const(57))));
            Operand c1 = context.ConditionalSelect(cZero, Const(0), FPWithExponent(context, c, context.IMaximumS32(context.IAdd(context.ISubtract(ec, m), Const(127)), Const(57))));

            Operand r1 = context.FPFusedMultiplyAdd(a1, b1, c1);
            Operand u1 = FPPrecise(context.FPMultiply(a1, b1));
            Operand u2 = context.FPFusedMultiplyAdd(a1, b1, context.FPNegate(u1));
            Operand alpha1 = FPPrecise(context.FPAdd(c1, u2));
            Operand z = FPAddError(context, c1, u2, alpha1);
            Operand beta1 = FPPrecise(context.FPAdd(u1, alpha1));
            Operand beta2 = FPAddError(context, u1, alpha1, beta1);
            Operand gamma = FPPrecise(context.FPAdd(FPPrecise(context.FPSubtract(beta1, r1)), beta2));
            Operand err = FPPrecise(context.FPAdd(gamma, z));

            Operand negZero = null;

            if (mode == RoundMode.Rm)
            {
                // An exact zero sum is -0 unless the product and c are both +0; the product is +0 when a factor is zero and the signs agree
                Operand pNotPosZero = context.BitwiseOr(
                    context.BitwiseAnd(context.ICompareNotEqual(ea, Const(0)), context.ICompareNotEqual(eb, Const(0))),
                    context.ICompareLess(context.BitwiseExclusiveOr(a, b), Const(0)));
                negZero = context.BitwiseAnd(FPIsZero(context, r1), context.BitwiseOr(pNotPosZero, context.ICompareNotEqual(c, Const(0))));
            }

            return FPRoundDirected(context, mode, r, err, FPAllFinite(context, a, b, c), negZero);
        }

        private static Operand FPExponent(EmitterContext context, Operand v)
        {
            return context.BitwiseAnd(context.ShiftRightU32(v, Const(23)), Const(0xff));
        }

        // v with the biased exponent e (sign and significand kept)
        private static Operand FPWithExponent(EmitterContext context, Operand v, Operand e)
        {
            return context.BitwiseOr(context.BitwiseAnd(v, Const(unchecked((int)0x807fffff))), context.ShiftLeft(e, Const(23)));
        }

        // v's sign and significand, in [1, 2)
        private static Operand FPSignificand(EmitterContext context, Operand v)
        {
            return context.BitwiseOr(context.BitwiseAnd(v, Const(unchecked((int)0x807fffff))), Const(0x3f800000));
        }

        private static Operand FPIsZero(EmitterContext context, Operand v)
        {
            return context.ICompareEqual(context.BitwiseAnd(v, Const(0x7fffffff)), Const(0));
        }

        private static Operand FPAllFinite(EmitterContext context, params Operand[] values)
        {
            Operand res = null;

            foreach (Operand v in values)
            {
                Operand finite = context.ICompareNotEqual(context.BitwiseAnd(v, Const(0x7f800000)), Const(0x7f800000));
                res = res == null ? finite : context.BitwiseAnd(res, finite);
            }

            return res;
        }

        // r rounded to nearest, err the exact value minus r (only its sign counts; nothing is corrected when it is zero, Inf or NaN). A finite r moves one ULP toward zero (bits - 1)
        // or away from it (bits + 1). An Inf r from finite operands is an overflow, whose exact value lies toward zero: RZ gives the largest float of its sign, RM gives +max and -Inf,
        // RP +Inf and -max. negZero (RM only): the result is an exact zero that is -0
        private static Operand FPRoundDirected(EmitterContext context, RoundMode mode, Operand r, Operand err, Operand allFinite, Operand negZero)
        {
            Operand rAbs = context.BitwiseAnd(r, Const(0x7fffffff));
            Operand errAbs = context.BitwiseAnd(err, Const(0x7fffffff));

            // r and err finite and not zero
            Operand ok = context.BitwiseAnd(
                context.ICompareLessUnsigned(context.ISubtract(rAbs, Const(1)), Const(0x7f7fffff)),
                context.ICompareLessUnsigned(context.ISubtract(errAbs, Const(1)), Const(0x7f7fffff)));

            Operand differ = context.ICompareLess(context.BitwiseExclusiveOr(r, err), Const(0));
            Operand overflow = context.BitwiseAnd(context.ICompareEqual(rAbs, Const(0x7f800000)), allFinite);

            // The exact value lies toward zero from r, or away from zero
            Operand toZero = context.BitwiseOr(context.BitwiseAnd(ok, differ), overflow);
            Operand away = context.BitwiseAnd(ok, context.ICompareGreaterOrEqual(context.BitwiseExclusiveOr(r, err), Const(0)));

            Operand neg = context.ICompareLess(r, Const(0));
            Operand pos = context.ICompareGreaterOrEqual(r, Const(0));

            Operand down = context.ISubtract(r, Const(1));
            Operand up = context.IAdd(r, Const(1));

            Operand res = mode switch
            {
                RoundMode.Rz => context.ConditionalSelect(toZero, down, r),
                RoundMode.Rm => context.ConditionalSelect(context.BitwiseAnd(neg, away), up, context.ConditionalSelect(context.BitwiseAnd(pos, toZero), down, r)),
                _ => context.ConditionalSelect(context.BitwiseAnd(pos, away), up, context.ConditionalSelect(context.BitwiseAnd(neg, toZero), down, r)),
            };

            if (negZero != null)
            {
                res = context.ConditionalSelect(context.BitwiseAnd(context.ICompareEqual(r, Const(0)), negZero), Const(int.MinValue), res);
            }

            return res;
        }

        public static long GetIntMin(IDstFmt type)
        {
            return type switch
            {
                IDstFmt.U16 => ushort.MinValue,
                IDstFmt.S16 => short.MinValue,
                IDstFmt.U32 => uint.MinValue,
                IDstFmt.S32 => int.MinValue,
                _ => throw new ArgumentException($"The type \"{type}\" is not a supported integer type."),
            };
        }

        public static long GetIntMax(IDstFmt type)
        {
            return type switch
            {
                IDstFmt.U16 => ushort.MaxValue,
                IDstFmt.S16 => short.MaxValue,
                IDstFmt.U32 => uint.MaxValue,
                IDstFmt.S32 => int.MaxValue,
                _ => throw new ArgumentException($"The type \"{type}\" is not a supported integer type."),
            };
        }

        public static long GetIntMin(ISrcDstFmt type)
        {
            return type switch
            {
                ISrcDstFmt.U8 => byte.MinValue,
                ISrcDstFmt.S8 => sbyte.MinValue,
                ISrcDstFmt.U16 => ushort.MinValue,
                ISrcDstFmt.S16 => short.MinValue,
                ISrcDstFmt.U32 => uint.MinValue,
                ISrcDstFmt.S32 => int.MinValue,
                _ => throw new ArgumentException($"The type \"{type}\" is not a supported integer type."),
            };
        }

        public static long GetIntMax(ISrcDstFmt type)
        {
            return type switch
            {
                ISrcDstFmt.U8 => byte.MaxValue,
                ISrcDstFmt.S8 => sbyte.MaxValue,
                ISrcDstFmt.U16 => ushort.MaxValue,
                ISrcDstFmt.S16 => short.MaxValue,
                ISrcDstFmt.U32 => uint.MaxValue,
                ISrcDstFmt.S32 => int.MaxValue,
                _ => throw new ArgumentException($"The type \"{type}\" is not a supported integer type."),
            };
        }

        public static Operand GetPredLogicalOp(EmitterContext context, BoolOp logicOp, Operand input, Operand pred)
        {
            return logicOp switch
            {
                BoolOp.And => context.BitwiseAnd(input, pred),
                BoolOp.Or => context.BitwiseOr(input, pred),
                BoolOp.Xor => context.BitwiseExclusiveOr(input, pred),
                _ => input,
            };
        }

        public static Operand Extend(EmitterContext context, Operand src, VectorSelect type)
        {
            return type switch
            {
                VectorSelect.U8B0 => ZeroExtendTo32(context, context.ShiftRightU32(src, Const(0)), 8),
                VectorSelect.U8B1 => ZeroExtendTo32(context, context.ShiftRightU32(src, Const(8)), 8),
                VectorSelect.U8B2 => ZeroExtendTo32(context, context.ShiftRightU32(src, Const(16)), 8),
                VectorSelect.U8B3 => ZeroExtendTo32(context, context.ShiftRightU32(src, Const(24)), 8),
                VectorSelect.U16H0 => ZeroExtendTo32(context, context.ShiftRightU32(src, Const(0)), 16),
                VectorSelect.U16H1 => ZeroExtendTo32(context, context.ShiftRightU32(src, Const(16)), 16),
                VectorSelect.S8B0 => SignExtendTo32(context, context.ShiftRightU32(src, Const(0)), 8),
                VectorSelect.S8B1 => SignExtendTo32(context, context.ShiftRightU32(src, Const(8)), 8),
                VectorSelect.S8B2 => SignExtendTo32(context, context.ShiftRightU32(src, Const(16)), 8),
                VectorSelect.S8B3 => SignExtendTo32(context, context.ShiftRightU32(src, Const(24)), 8),
                VectorSelect.S16H0 => SignExtendTo32(context, context.ShiftRightU32(src, Const(0)), 16),
                VectorSelect.S16H1 => SignExtendTo32(context, context.ShiftRightU32(src, Const(16)), 16),
                _ => src,
            };
        }

        public static void SetZnFlags(EmitterContext context, Operand dest, bool setCC, bool extended = false)
        {
            if (!setCC)
            {
                return;
            }

            if (extended)
            {
                // When the operation is extended, it means we are doing
                // the operation on a long word with any number of bits,
                // so we need to AND the zero flag from result with the
                // previous result when extended is specified, to ensure
                // we have ZF set only if all words are zero, and not just
                // the last one.
                Operand oldZF = GetZF();

                Operand res = context.BitwiseAnd(context.ICompareEqual(dest, Const(0)), oldZF);

                context.Copy(GetZF(), res);
            }
            else
            {
                context.Copy(GetZF(), context.ICompareEqual(dest, Const(0)));
            }

            context.Copy(GetNF(), context.ICompareLess(dest, Const(0)));
        }

        public static void SetFPZnFlags(EmitterContext context, Operand dest, bool setCC, Instruction fpType = Instruction.FP32)
        {
            if (setCC)
            {
                Operand zero = ConstF(0);

                if (fpType == Instruction.FP64)
                {
                    zero = context.FP32ConvertToFP64(zero);
                }

                context.Copy(GetZF(), context.FPCompareEqual(dest, zero, fpType));
                context.Copy(GetNF(), context.FPCompareLess(dest, zero, fpType));
            }
        }

        public static (Operand, Operand) NegateLong(EmitterContext context, Operand low, Operand high)
        {
            low = context.BitwiseNot(low);
            high = context.BitwiseNot(high);
            low = AddWithCarry(context, low, Const(1), out Operand carryOut);
            high = context.IAdd(high, carryOut);
            return (low, high);
        }

        public static Operand AddWithCarry(EmitterContext context, Operand lhs, Operand rhs, out Operand carryOut)
        {
            Operand result = context.IAdd(lhs, rhs);

            // C = Rd < Rn
            carryOut = context.INegate(context.ICompareLessUnsigned(result, lhs));

            return result;
        }
    }
}
