using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitAluHelper;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitHelper;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    delegate void AmpereAluRegistrar(int op9, InstName name, InstEmitter emitter, bool uniform = false, bool writesRd = true);
    delegate void AmpereRegistrar(int opcode, InstName name, InstEmitter emitter, InstProps props, bool uniform = false, bool writesRd = true);

    // Ampere ALU, conversion, move and predicate instructions. Field positions: Mesa NAK sm70_encode.rs. The uniform-datapath forms (UIADD3, ULOP3, ...)
    // share the vector forms' code with uniform registers and predicates.
    static partial class InstEmit
    {
        public static void RegisterAmpereAlu(AmpereAluRegistrar alu, AmpereRegistrar x)
        {
            const InstProps N = InstProps.None;

            alu(0x021, InstName.Fadd, AFadd);
            alu(0x020, InstName.Fmul, AFmul);
            alu(0x023, InstName.Ffma, AFfma);
            alu(0x009, InstName.Fmnmx, AFmnmx);
            alu(0x00a, InstName.Fset, AFset);
            alu(0x00b, InstName.Fsetp, AFsetp, writesRd: false);
            alu(0x008, InstName.Sel, AFsel);
            alu(0x108, InstName.Mufu, AMufu);
            alu(0x105, InstName.F2i, AF2i);
            alu(0x106, InstName.I2f, AI2f);
            alu(0x045, InstName.I2f, AI2fp);
            alu(0x107, InstName.F2f, AFrnd);
            alu(0x104, InstName.F2f, AF2f);
            alu(0x03e, InstName.F2f, AF2fp);
            alu(0x030, InstName.Hadd2, AHadd2);
            alu(0x032, InstName.Hmul2, AHmul2);
            alu(0x031, InstName.Hfma2, AHfma2);
            alu(0x002, InstName.Mov, AMov);
            alu(0x007, InstName.Sel, ASel);
            alu(0x087, InstName.Sel, ASel, uniform: true);
            alu(0x00c, InstName.Isetp, AIsetp, writesRd: false);
            alu(0x08c, InstName.Isetp, AIsetp, uniform: true, writesRd: false);
            alu(0x010, InstName.Iadd3, AIadd3);
            alu(0x090, InstName.Iadd3, AIadd3, uniform: true);
            alu(0x024, InstName.Imad, AImad);
            alu(0x0a4, InstName.Imad, AImad, uniform: true);
            alu(0x025, InstName.Imad, AImadWide);
            alu(0x027, InstName.Imad, AImadHi);
            alu(0x0a5, InstName.Imad, AImadWide, uniform: true);
            alu(0x012, InstName.Lop3, ALop3);
            alu(0x092, InstName.Lop3, ALop3, uniform: true);
            alu(0x019, InstName.Shf, AShf);
            alu(0x099, InstName.Shf, AShf, uniform: true);
            alu(0x011, InstName.Lea, ALea);
            alu(0x091, InstName.Lea, ALea, uniform: true);
            alu(0x017, InstName.Imnmx, AImnmx);
            alu(0x085, InstName.Imnmx, AImnmx, uniform: true);
            alu(0x013, InstName.I2i, AIabs);
            alu(0x016, InstName.Prmt, APrmt);
            alu(0x096, InstName.Prmt, APrmt, uniform: true);
            alu(0x109, InstName.Popc, APopc);
            alu(0x0bf, InstName.Popc, APopc, uniform: true);
            alu(0x100, InstName.Flo, AFlo);
            alu(0x0bd, InstName.Flo, AFlo, uniform: true);
            alu(0x01a, InstName.Bfe, ASgxt);
            alu(0x09a, InstName.Bfe, ASgxt, uniform: true);

            x(0x882, InstName.Mov, AUMov, N, uniform: true);
            x(0xc82, InstName.Mov, AUMov, N, uniform: true);
            x(0x805, InstName.Cs2r, ACs2r, N);
            x(0x919, InstName.S2r, AS2r, N);
            x(0x9c3, InstName.S2r, AS2r, N, uniform: true);
            x(0x3c2, InstName.Mov, AR2ur, N);
            x(0xab9, InstName.Ldc, AUldc, N, uniform: true);
            x(0xabb, InstName.Ldc, AUldc, N, uniform: true);
            x(AmpereOpcodes.Ldc, InstName.Ldc, ALdc, N);
            x(0x822, InstName.Fswzadd, AFswzadd, N);
            x(0x804, InstName.R2p, AR2p, N, writesRd: false);
            x(0x204, InstName.R2p, AR2p, N, writesRd: false);
            x(0x803, InstName.P2r, AP2r, N);
            x(0x203, InstName.P2r, AP2r, N);
            x(0x81c, InstName.Psetp, APlop3, N, writesRd: false);
            x(0x89c, InstName.Psetp, APlop3, N, uniform: true, writesRd: false);
            x(0x806, InstName.Vote, AVote, N);
            x(0x886, InstName.Vote, AVote, N);
            x(0x389, InstName.Shfl, AShfl, N);
            x(0x589, InstName.Shfl, AShfl, N);
            x(0x989, InstName.Shfl, AShfl, N);
            x(0xf89, InstName.Shfl, AShfl, N);
        }

        private static bool AUniform(EmitterContext context) => AmpereTable.HasUniformGuard(new AmpereOp(context.CurrOp).Opcode);

        // ---- float ----

        public static void AFadd(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (ASrc a, ASrc b) = ASecond(context, x);

            Operand res = context.FPAdd(AFloat(context, a), AFloat(context, b));

            ASet(context, x, context.FPSaturate(res, x.Bit(77)));
        }

        public static void AFmul(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (ASrc a, ASrc b) = ASecond(context, x);

            Operand res = context.FPMultiply(AFloat(context, a), AFloat(context, b));

            // Bits 84..86: the product times 2^(v - 4) (4: no scale)
            int scale = x.Bits(84, 87);

            if (scale == 0)
            {
                throw AFail(context, "FMUL scale 0");
            }

            if (scale != 4)
            {
                res = context.FPMultiply(res, ConstF(scale > 4 ? (float)(1 << (scale - 4)) : 1f / (1 << (4 - scale))));
            }

            ASet(context, x, context.FPSaturate(res, x.Bit(77)));
        }

        public static void AFfma(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (ASrc a, ASrc b, ASrc c) = AAluSrcs(context, x);

            Operand res = context.FPFusedMultiplyAdd(AFloat(context, a), AFloat(context, b), AFloat(context, c));

            ASet(context, x, context.FPSaturate(res, x.Bit(77)));
        }

        public static void AFmnmx(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (ASrc a, ASrc b) = ASecond(context, x);

            Operand srcA = AFloat(context, a);
            Operand srcB = AFloat(context, b);

            // The predicate (87..90) picks the minimum where it holds
            Operand res = context.ConditionalSelect(APred(context, x, 87, 90), context.FPMinimum(srcA, srcB), context.FPMaximum(srcA, srcB));

            ASet(context, x, res);
        }

        // FSET Rd, a, b: 1.0 where (a cmp b) bop P (87..90) holds, else 0.0
        public static void AFset(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (ASrc a, ASrc b) = ASecond(context, x);

            Operand res = GetFPComparison(context, (FComp)x.Bits(76, 80), AFloat(context, a), AFloat(context, b));
            res = GetPredLogicalOp(context, (BoolOp)x.Bits(74, 76), res, APred(context, x, 87, 90));

            ASet(context, x, context.ConditionalSelect(res, ConstF(1), Const(0)));
        }

        // FSETP Pd0, Pd1, a, b, P: Pd0 = (a cmp b) bop P, Pd1 = !(a cmp b) bop P
        public static void AFsetp(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (ASrc a, ASrc b) = ASecond(context, x);

            Operand cmp = GetFPComparison(context, (FComp)x.Bits(76, 80), AFloat(context, a), AFloat(context, b));
            Operand p = APred(context, x, 87, 90);
            BoolOp bop = (BoolOp)x.Bits(74, 76);

            ASetPred(context, x.Bits(81, 84), GetPredLogicalOp(context, bop, cmp, p));
            ASetPred(context, x.Bits(84, 87), GetPredLogicalOp(context, bop, context.BitwiseNot(cmp), p));
        }

        public static void AFsel(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (ASrc a, ASrc b) = ASecond(context, x);

            ASet(context, x, context.ConditionalSelect(APred(context, x, 87, 90), AFloat(context, a), AFloat(context, b)));
        }

        public static void AMufu(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x.Bit(73))
            {
                throw AFail(context, "MUFU.F16");
            }

            Operand res = AFloat(context, AOne(context, x));

            res = x.Bits(74, 80) switch
            {
                0 => context.FPCosine(res),
                1 => context.FPSine(res),
                2 => context.FPExponentB2(res),
                3 => context.FPLogarithmB2(res),
                4 => context.FPReciprocal(res),
                5 => context.FPReciprocalSquareRoot(res),
                6 => context.UnpackDouble2x32High(context.FPReciprocal(context.PackDouble2x32(Const(0), res), Instruction.FP64)),
                7 => context.UnpackDouble2x32High(context.FPReciprocalSquareRoot(context.PackDouble2x32(Const(0), res), Instruction.FP64)),
                8 => context.FPSquareRoot(res),
                int op => throw AFail(context, $"MUFU op {op}"),
            };

            ASet(context, x, res);
        }

        // F2I: destination signed (72) and size (75..77, log2 bytes), rounding (78..80), source size (84..86)
        public static void AF2i(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            ASrc s = AOne(context, x);

            if (x.Bits(84, 86) != 2)
            {
                throw AFail(context, "F2I from a float that is not 32-bit");
            }

            int size = x.Bits(75, 77);
            bool signed = x.Bit(72);

            if (size > 2)
            {
                throw AFail(context, $"F2I to a {8 << size}-bit integer");
            }

            IDstFmt dst = (IDstFmt)((signed ? 4 : 0) | System.Math.Max(size, 1));

            EmitF2I(context, DstFmt.F32, dst, (RoundMode2)x.Bits(78, 80), AGet(context, s), x.Rd, s.Abs, s.Neg);

            if (size == 0 && x.Rd != RegisterConsts.RegisterZeroIndex)
            {
                // 8 bits: the 16-bit conversion's result saturated to the byte's range
                Operand r = Register(x.Rd, RegisterType.Gpr);
                context.Copy(r, signed ? context.IClampS32(r, Const(-128), Const(127)) : context.IClampU32(r, Const(0), Const(255)));
            }
        }

        // I2F: source signed (74) and size (84..86), destination size (75..77), byte / half select of a narrow source (60..62)
        public static void AI2f(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int srcSize = x.Bits(84, 86);
            int dstSize = x.Bits(75, 77);

            if (srcSize > 2 || dstSize is not 1 and not 2)
            {
                throw AFail(context, "I2F size");
            }

            int select = x.Bits(60, 62);

            if (select != 0 && srcSize == 2)
            {
                throw AFail(context, "I2F byte select of a 32-bit source");
            }

            ISrcFmt src = (ISrcFmt)((x.Bit(74) ? 4 : 0) | srcSize);

            EmitI2F(context, src, (DstFmt)dstSize, AGet(context, AOne(context, x, neg: false, abs: false)), (ByteSel)(select * (srcSize == 1 ? 2 : 1)), x.Rd, false, false);
        }

        // I2FP.F32.S32 / .U32: signed at 74
        public static void AI2fp(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand src = AGet(context, AOne(context, x, neg: false, abs: false));

            ASet(context, x, x.Bit(74) ? context.IConvertS32ToFP32(src) : context.IConvertU32ToFP32(src));
        }

        // FRND: a float rounded to an integer value (78..80: nearest even, floor, ceiling, truncation)
        public static void AFrnd(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x.Bits(84, 86) != 2 || x.Bits(75, 77) != 2)
            {
                throw AFail(context, "FRND of a float that is not 32-bit");
            }

            Operand src = AFloat(context, AOne(context, x));

            Operand res = x.Bits(78, 80) switch
            {
                0 => context.FPRound(src),
                1 => context.FPFloor(src),
                2 => context.FPCeiling(src),
                _ => context.FPTruncate(src),
            };

            ASet(context, x, res);
        }

        // F2F: between half, single and double precision (sizes as F2I); a half lands in the low 16 bits
        public static void AF2f(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            ASrc s = AOne(context, x);
            int srcSize = x.Bits(84, 86);
            int dstSize = x.Bits(75, 77);

            if (srcSize is < 1 or > 3 || dstSize is < 1 or > 3)
            {
                throw AFail(context, "F2F size");
            }

            Operand src = AGet(context, s);

            if (srcSize == 1)
            {
                src = x.Bits(60, 62) is 1 or 3 ? context.UnpackHalf2x16High(src) : context.UnpackHalf2x16Low(src);
                srcSize = 2;
            }
            else if (srcSize == 3)
            {
                src = context.PackDouble2x32(src, AGetHigh(context, s));
            }

            EmitF2F(context, (DstFmt)srcSize, (DstFmt)dstSize, IntegerRound.Pass, src, x.Rd, s.Abs, s.Neg, false);
        }

        // F2FP.F16.F32.PACK_AB Rd, a, b: a to the high half, b to the low one
        public static void AF2fp(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x.Bit(78) || x.Bit(72))
            {
                throw AFail(context, "F2FP.MERGE_C");
            }

            (ASrc a, ASrc b) = ASecond(context, x, abs: false);

            ASet(context, x, context.PackHalf2x16(AFloat(context, b), AFloat(context, a)), AUniform(context));
        }

        // ---- packed halves ----

        // A half-pair operand's two lanes after its swizzle (0 as is, 2 low twice, 3 high twice; 1 the high half of a scalar), abs / neg applied.
        private static Operand[] AHalfLanes(EmitterContext context, ASrc s, int swizzle)
        {
            Operand v = AGet(context, s);
            Operand lo, hi;

            if (s.Kind == AKind.Imm || s.Kind == AKind.Cbuf || swizzle == 0)
            {
                lo = context.UnpackHalf2x16Low(v);
                hi = context.UnpackHalf2x16High(v);
            }
            else if (swizzle is 1 or 3)
            {
                lo = hi = context.UnpackHalf2x16High(v);
            }
            else
            {
                lo = hi = context.UnpackHalf2x16Low(v);
            }

            return [context.FPAbsNeg(lo, s.Abs, s.Neg), context.FPAbsNeg(hi, s.Abs, s.Neg)];
        }

        // The swizzle fields: A 74..76, B (in the 32 slot) 60..62, C (in the 64 slot) 81..83. Half-pair ops keep A's abs / neg at 73 / 72, C's at 83 / 84.
        private static (Operand[] A, Operand[] B, Operand[] C) AHalfSrcs(EmitterContext context, AmpereOp x)
        {
            (ASrc a, ASrc b, ASrc c) = AAluSrcs(context, x, abs: false, neg: false);

            a = a.WithMods(x.Bit(72), x.Bit(73));
            bool bIn32 = x.Form is 1 or 4 or 5 or 6;
            ASrc slot32 = bIn32 ? b : c;
            ASrc slot64 = bIn32 ? c : b;
            slot32 = slot32.WithMods(x.Bit(63), x.Bit(62));
            slot64 = slot64.WithMods(x.Bit(84), x.Bit(83));

            Operand[] ha = AHalfLanes(context, a, x.Bits(74, 76));
            Operand[] h32 = AHalfLanes(context, slot32, x.Bits(60, 62));
            Operand[] h64 = AHalfLanes(context, slot64, x.Bits(81, 83));

            return bIn32 ? (ha, h32, h64) : (ha, h64, h32);
        }

        // The result: two halves packed, or with .F32 (78) the low lane as one float. HFMA2 / HMUL2 (sm_86): .RELU (79) clamps at zero; .BF16_V2 (85)
        // is not handled.
        private static void AHalfResult(EmitterContext context, AmpereOp x, Operand lo, Operand hi)
        {
            bool sat = x.Bit(77);

            if (x.Bit(85))
            {
                throw AFail(context, "BF16 half pair");
            }

            if (x.Bit(79) && (x.Opcode & 0x1ff) is 0x031 or 0x032)
            {
                lo = context.FPMaximum(lo, ConstF(0));
                hi = context.FPMaximum(hi, ConstF(0));
            }

            if (x.Bit(78))
            {
                ASet(context, x, context.FPSaturate(lo, sat));
                return;
            }

            ASet(context, x, context.PackHalf2x16(context.FPSaturate(lo, sat), context.FPSaturate(hi, sat)));
        }

        public static void AHadd2(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (Operand[] a, Operand[] b, _) = AHalfSrcs(context, x);

            AHalfResult(context, x, context.FPAdd(a[0], b[0]), context.FPAdd(a[1], b[1]));
        }

        public static void AHmul2(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (Operand[] a, Operand[] b, _) = AHalfSrcs(context, x);

            AHalfResult(context, x, context.FPMultiply(a[0], b[0]), context.FPMultiply(a[1], b[1]));
        }

        public static void AHfma2(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (Operand[] a, Operand[] b, Operand[] c) = AHalfSrcs(context, x);

            AHalfResult(context, x, context.FPFusedMultiplyAdd(a[0], b[0], c[0]), context.FPFusedMultiplyAdd(a[1], b[1], c[1]));
        }

        // ---- moves ----

        public static void AMov(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            ASet(context, x, AGet(context, AOne(context, x, neg: false, abs: false)));
        }

        // UMOV URd, imm (form 4) or URb (form 6)
        public static void AUMov(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand src = x.Form == 4 ? Const((int)x.Imm32) : Register(x.Bits(32, 38), RegisterType.Uniform);

            ASet(context, x, src, uniform: true);
        }

        // R2UR URd, Ra
        public static void AR2ur(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            ASet(context, x, Register(x.Ra, RegisterType.Gpr), uniform: true);
        }

        public static void ASel(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool u = AUniform(context);
            (ASrc a, ASrc b) = ASecond(context, x, u, neg: false, abs: false);

            ASet(context, x, context.ConditionalSelect(APred(context, x, 87, 90, u), AGet(context, a), AGet(context, b)), u);
        }

        // CS2R Rd, SR (72..80; 255 = zero), .64 (80) two registers
        public static void ACs2r(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int sr = x.Bits(72, 80);

            for (int k = 0; k < (x.Bit(80) ? 2 : 1); k++)
            {
                if (x.Rd + k >= RegisterConsts.RegisterZeroIndex)
                {
                    break;
                }

                context.Copy(Register(x.Rd + k, RegisterType.Gpr), sr == 255 ? Const(0) : EmitSReg(context, (SReg)(sr + k)));
            }
        }

        // S2R Rd, SR / S2UR URd, SR (72..80)
        public static void AS2r(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            ASet(context, x, EmitSReg(context, (SReg)x.Bits(72, 80)), AUniform(context));
        }

        // ULDC URd, c[bank][offset] (0xab9) or c[bank][URa + offset] (0xabb, URa at 24..30): a byte, short, word or word pair (size 73..76)
        public static void AUldc(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x.Opcode == 0xab9 && x.Bit(91))
            {
                throw AFail(context, "ULDC with its bank in a uniform register");
            }

            Operand index = x.Opcode == 0xabb ? Register(x.Bits(24, 30), RegisterType.Uniform) : Const(0);

            ALoadConstant(context, x, x.Bits(54, 59), index, (int)x.SignedField(38, 54), uniform: true);
        }

        // LDC Rd, c[bank][Ra + offset]
        public static void ALdc(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x.Bit(91) || x.Bits(78, 80) != 0)
            {
                throw AFail(context, "LDC with a bindless bank or an address mode");
            }

            ALoadConstant(context, x, x.Bits(54, 59), Register(x.Ra, RegisterType.Gpr), (int)x.SignedField(38, 54), uniform: false);
        }

        private static void ALoadConstant(EmitterContext context, AmpereOp x, int bank, Operand index, int offset, bool uniform)
        {
            int size = x.Bits(73, 76);

            if (size > 5)
            {
                throw AFail(context, $"constant load size {size}");
            }

            Operand addr = context.IAdd(index, Const(offset));
            Operand wordOffset = context.ShiftRightU32(addr, Const(2));
            int rd = ARd(x, uniform);
            int zero = uniform ? RegisterConsts.UniformZeroIndex : RegisterConsts.RegisterZeroIndex;

            for (int k = 0; k < (size == 5 ? 2 : 1) && rd + k < zero; k++)
            {
                Operand value = EmitLoadConstant(context, Const(bank), context.IAdd(wordOffset, Const(k)));

                if (size < 4)
                {
                    value = ExtractSmallInt(context, (LsSize)size, GetBitOffset(context, addr), value);
                }

                context.Copy(ADestReg(rd + k, uniform), value);
            }
        }

        public static void AFswzadd(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            ASet(context, x, context.FPSwizzleAdd(Register(x.Ra, RegisterType.Gpr), Register(x.Rc, RegisterType.Gpr), x.Bits(32, 40)));
        }

        // ---- integer ----

        // ISETP / UISETP Pd0, Pd1, a, b, P: as FSETP with an integer comparison (76..79, signed 73). .EX (72): the high words of a 64-bit comparison, whose
        // low words' result is the predicate at 68..71.
        public static void AIsetp(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool u = AUniform(context);
            (ASrc a, ASrc b) = ASecond(context, x, u, neg: false, abs: false);

            IComp cmpOp = (IComp)x.Bits(76, 79);
            bool signed = x.Bit(73);
            Operand srcA = AGet(context, a);
            Operand srcB = AGet(context, b);
            Operand cmp;

            if (x.Bit(72))
            {
                Operand low = APred(context, x, 68, 71, u);
                Operand eq = context.ICompareEqual(srcA, srcB);

                cmp = cmpOp switch
                {
                    IComp.F => Const(IrConsts.False),
                    IComp.T => Const(IrConsts.True),
                    IComp.Eq => context.BitwiseAnd(eq, low),
                    IComp.Ne => context.BitwiseOr(context.ICompareNotEqual(srcA, srcB), low),
                    IComp.Lt or IComp.Le => context.BitwiseOr(GetIntComparison(context, IComp.Lt, srcA, srcB, signed), context.BitwiseAnd(eq, low)),
                    _ => context.BitwiseOr(GetIntComparison(context, IComp.Gt, srcA, srcB, signed), context.BitwiseAnd(eq, low)),
                };
            }
            else
            {
                cmp = GetIntComparison(context, cmpOp, srcA, srcB, signed);
            }

            Operand p = APred(context, x, 87, 90, u);
            BoolOp bop = (BoolOp)x.Bits(74, 76);

            ASetPred(context, x.Bits(81, 84), GetPredLogicalOp(context, bop, cmp, p), u);
            ASetPred(context, x.Bits(84, 87), GetPredLogicalOp(context, bop, context.BitwiseNot(cmp), p), u);
        }

        // IADD3 Rd, Pc0, Pc1, a, b, c: a + b + c. A negated source is its two's complement; with .X (74) its bitwise not, and the carry predicates (87..90,
        // 77..80) are added. The carries out of the 32-bit sum go to the predicates at 81..84 (the first) and 84..87 (the second).
        public static void AIadd3(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool u = AUniform(context);
            (ASrc a, ASrc b, ASrc c) = AAluSrcs(context, x, u, abs: false);
            bool ex = x.Bit(74);

            Operand[] terms = new Operand[5];
            int n = 0;
            int plusOne = 0;

            foreach (ASrc s in new[] { a, b, c })
            {
                Operand v = AGet(context, s);

                if (s.Neg)
                {
                    v = context.BitwiseNot(v);

                    if (!ex)
                    {
                        plusOne++;
                    }
                }

                terms[n++] = v;
            }

            if (ex)
            {
                foreach ((int lo, int notBit) in new[] { (87, 90), (77, 80) })
                {
                    if (x.Bits(lo, lo + 3) == RegisterConsts.PredicateTrueIndex && x.Bit(notBit))
                    {
                        continue;
                    }

                    terms[n++] = context.ConditionalSelect(APred(context, x, lo, notBit, u), Const(1), Const(0));
                }
            }

            if (plusOne != 0)
            {
                terms[n++] = Const(plusOne);
            }

            int o0 = x.Bits(81, 84);
            int o1 = x.Bits(84, 87);
            bool carries = o0 != RegisterConsts.PredicateTrueIndex || o1 != RegisterConsts.PredicateTrueIndex;

            Operand sum = terms[0];
            Operand carryCount = Const(0);

            for (int i = 1; i < n; i++)
            {
                Operand next = context.IAdd(sum, terms[i]);

                if (carries)
                {
                    carryCount = context.IAdd(carryCount, context.ConditionalSelect(context.ICompareLessUnsigned(next, sum), Const(1), Const(0)));
                }

                sum = next;
            }

            ASet(context, x, sum, u);

            if (carries)
            {
                ASetPred(context, o0, context.ICompareNotEqual(context.BitwiseAnd(carryCount, Const(1)), Const(0)), u);
                ASetPred(context, o1, context.ICompareNotEqual(context.BitwiseAnd(carryCount, Const(2)), Const(0)), u);
            }
        }

        // IMAD Rd, a, b, c: the low word of a * b + c (B and C may be negated)
        public static void AImad(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool u = AUniform(context);
            (ASrc a, ASrc b, ASrc c) = AAluSrcs(context, x, u, abs: false);

            if (x.Bits(81, 84) != RegisterConsts.PredicateTrueIndex)
            {
                throw AFail(context, "IMAD predicate output");
            }

            Operand res = context.IMultiply(AGet(context, a), AInt(context, b));

            if (!c.IsZeroReg)
            {
                res = context.IAdd(res, AInt(context, c));
            }

            ASet(context, x, res, u);
        }

        // IMAD.WIDE Rd, a, b, c: the 64-bit a * b (signed 73) + the pair {c + 1:c}, to the pair {Rd + 1:Rd}
        public static void AImadWide(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool u = AUniform(context);
            (ASrc a, ASrc b, ASrc c) = AAluSrcs(context, x, u, abs: false);

            if (x.Bits(81, 84) != RegisterConsts.PredicateTrueIndex || b.Neg || c.Neg)
            {
                throw AFail(context, "IMAD.WIDE predicate output or negation");
            }

            Operand srcA = AGet(context, a);
            Operand srcB = AGet(context, b);
            bool signed = x.Bit(73);

            Operand lo = context.IMultiply(srcA, srcB);
            Operand hi = signed ? context.MultiplyHighS32(srcA, srcB) : context.MultiplyHighU32(srcA, srcB);

            if (!c.IsZeroReg)
            {
                Operand cLo = AGet(context, c);
                Operand sum = context.IAdd(lo, cLo);
                Operand carry = context.ConditionalSelect(context.ICompareLessUnsigned(sum, lo), Const(1), Const(0));

                hi = context.IAdd(context.IAdd(hi, AGetHigh(context, c)), carry);
                lo = sum;
            }

            int rd = ARd(x, u);
            int zero = u ? RegisterConsts.UniformZeroIndex : RegisterConsts.RegisterZeroIndex;

            if (rd < zero)
            {
                context.Copy(ADestReg(rd, u), lo);

                if (rd + 1 < zero)
                {
                    context.Copy(ADestReg(rd + 1, u), hi);
                }
            }
        }

        // IMAD.HI Rd, a, b, c: the high word of a * b (signed 73), plus c
        public static void AImadHi(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (ASrc a, ASrc b, ASrc c) = AAluSrcs(context, x, abs: false);

            if (x.Bits(81, 84) != RegisterConsts.PredicateTrueIndex)
            {
                throw AFail(context, "IMAD.HI predicate output");
            }

            Operand srcA = AGet(context, a);
            Operand srcB = AInt(context, b);
            Operand res = x.Bit(73) ? context.MultiplyHighS32(srcA, srcB) : context.MultiplyHighU32(srcA, srcB);

            if (!c.IsZeroReg)
            {
                res = context.IAdd(res, AInt(context, c));
            }

            ASet(context, x, res);
        }

        // LOP3 Rd, Pd, a, b, c, lut (72..80): Pd = (result != 0) OR P (87..90), AND with .PAND (80)
        public static void ALop3(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool u = AUniform(context);
            (ASrc a, ASrc b, ASrc c) = AAluSrcs(context, x, u, neg: false, abs: false);

            Operand res = Lop3Expression.GetFromTruthTable(context, AGet(context, a), AGet(context, b), AGet(context, c), x.Bits(72, 80));

            ASet(context, x, res, u);

            int pd = x.Bits(81, 84);

            if (pd != RegisterConsts.PredicateTrueIndex)
            {
                Operand nz = context.ICompareNotEqual(res, Const(0));
                Operand q = APred(context, x, 87, 90, u);

                ASetPred(context, pd, x.Bit(80) ? context.BitwiseAnd(nz, q) : context.BitwiseOr(nz, q), u);
            }
        }

        // SHF Rd, lo, shift, hi: a funnel shift of {hi:lo}. Type (73..75: I64, U64, I32, U32), .W (75) wraps the shift instead of clamping it, .R (76),
        // .HI (80) the high word of the result.
        public static void AShf(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool u = AUniform(context);
            (ASrc a, ASrc b, ASrc c) = AAluSrcs(context, x, u, neg: false, abs: false);

            int type = x.Bits(73, 75);
            bool wide = type <= 1;
            bool signed = type is 0 or 2;
            bool wrap = x.Bit(75);
            bool right = x.Bit(76);
            bool high = x.Bit(80);
            int max = wide ? 64 : 32;

            Operand lo = AGet(context, a);
            Operand sh = AGet(context, b);
            Operand hi = AGet(context, c);

            if (wrap)
            {
                sh = context.BitwiseAnd(sh, Const(max - 1));
            }
            else
            {
                sh = context.IMinimumU32(sh, Const(max));
            }

            Operand res;

            if (!right && !high)
            {
                // the low word of {hi:lo} << sh
                res = context.ConditionalSelect(context.ICompareLessUnsigned(sh, Const(32)), context.ShiftLeft(lo, sh), Const(0));
            }
            else if (right && high)
            {
                // the high word of {hi:lo} >> sh
                Operand shr = signed ? context.ShiftRightS32(hi, context.IMinimumU32(sh, Const(31))) : context.ShiftRightU32(hi, sh);
                res = signed ? shr : context.ConditionalSelect(context.ICompareLessUnsigned(sh, Const(32)), shr, Const(0));
            }
            else
            {
                // the other word of the funnel: left .HI, or right low
                Operand inner = context.ICompareEqual(sh, Const(0));
                Operand under32 = context.ICompareLessUnsigned(sh, Const(32));
                Operand back = context.ISubtract(Const(32), sh);
                Operand over = context.ISubtract(sh, Const(32));

                if (!right)
                {
                    Operand v = context.BitwiseOr(context.ShiftLeft(hi, sh), context.ShiftRightU32(lo, back));
                    v = context.ConditionalSelect(inner, hi, v);
                    Operand big = context.ConditionalSelect(context.ICompareLessUnsigned(over, Const(32)), context.ShiftLeft(lo, over), Const(0));
                    res = context.ConditionalSelect(under32, v, big);
                }
                else
                {
                    Operand v = context.BitwiseOr(context.ShiftRightU32(lo, sh), context.ShiftLeft(hi, back));
                    v = context.ConditionalSelect(inner, lo, v);
                    Operand shiftedHi = signed ? context.ShiftRightS32(hi, context.IMinimumU32(over, Const(31))) : context.ShiftRightU32(hi, over);
                    Operand big = signed ? shiftedHi : context.ConditionalSelect(context.ICompareLessUnsigned(over, Const(32)), shiftedHi, Const(0));
                    res = context.ConditionalSelect(under32, v, big);
                }
            }

            ASet(context, x, res, u);
        }

        // LEA Rd, Pd, a, b, a_hi, shift (75..80): (a << shift) + b, or with .HI (80) the high word of ({a_hi:a} << shift) + b. The intermediate is negated
        // (72; with .X bitwise not). .X (74) adds the carry predicate (87..90); Pd (81..84) is the carry out.
        public static void ALea(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool u = AUniform(context);
            (ASrc a, ASrc b, ASrc c) = AAluSrcs(context, x, u, neg: false, abs: false);

            int shift = x.Bits(75, 80);
            bool high = x.Bit(80);
            bool ex = x.Bit(74);
            bool negInter = x.Bit(72);
            // B's negation (63) exists where B is a register, uniform register or constant in the 32 slot (forms 1, 5, 6)
            Operand srcB = AGet(context, b);

            if (x.Form is 1 or 5 or 6 && x.Bit(63))
            {
                srcB = context.INegate(srcB);
            }

            Operand lo = AGet(context, a);
            Operand t;

            if (!high)
            {
                t = context.ShiftLeft(lo, Const(shift));
            }
            else if (shift == 0)
            {
                t = AGet(context, c);
            }
            else
            {
                t = context.BitwiseOr(context.ShiftLeft(AGet(context, c), Const(shift)), context.ShiftRightU32(lo, Const(32 - shift)));
            }

            int plusOne = 0;

            if (negInter)
            {
                t = context.BitwiseNot(t);

                if (!ex)
                {
                    plusOne = 1;
                }
            }

            Operand sum = context.IAdd(t, srcB);
            Operand carry = context.ICompareLessUnsigned(sum, t);

            if (plusOne != 0)
            {
                Operand next = context.IAdd(sum, Const(1));
                carry = context.BitwiseOr(carry, context.ICompareLessUnsigned(next, sum));
                sum = next;
            }

            if (ex && !(x.Bits(87, 90) == RegisterConsts.PredicateTrueIndex && x.Bit(90)))
            {
                Operand next = context.IAdd(sum, context.ConditionalSelect(APred(context, x, 87, 90, u), Const(1), Const(0)));
                carry = context.BitwiseOr(carry, context.ICompareLessUnsigned(next, sum));
                sum = next;
            }

            ASet(context, x, sum, u);
            ASetPred(context, x.Bits(81, 84), carry, u);
        }

        public static void AImnmx(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool u = AUniform(context);
            (ASrc a, ASrc b) = ASecond(context, x, u, neg: false, abs: false);

            Operand srcA = AGet(context, a);
            Operand srcB = AGet(context, b);
            bool signed = x.Bit(73);

            Operand min = signed ? context.IMinimumS32(srcA, srcB) : context.IMinimumU32(srcA, srcB);
            Operand max = signed ? context.IMaximumS32(srcA, srcB) : context.IMaximumU32(srcA, srcB);

            ASet(context, x, context.ConditionalSelect(APred(context, x, 87, 90, u), min, max), u);
        }

        public static void AIabs(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            ASet(context, x, context.IAbsolute(AGet(context, AOne(context, x, neg: false, abs: false))));
        }

        // PRMT Rd, a, sel, c: byte i of the result is byte (s & 7) of {c:a}, or that byte's sign repeated when s & 8, s = sel's nibble i (mode 72..75: index)
        public static void APrmt(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool u = AUniform(context);
            (ASrc a, ASrc b, ASrc c) = AAluSrcs(context, x, u, neg: false, abs: false);

            if (x.Bits(72, 75) != 0)
            {
                throw AFail(context, $"PRMT mode {x.Bits(72, 75)}");
            }

            Operand srcA = AGet(context, a);
            Operand sel = AGet(context, b);
            Operand srcC = AGet(context, c);
            Operand res = Const(0);

            for (int i = 0; i < 4; i++)
            {
                Operand byteValue;

                if (sel.Type == OperandType.Constant)
                {
                    int s = (sel.Value >> (i * 4)) & 0xf;
                    Operand src = (s & 4) != 0 ? srcC : srcA;
                    byteValue = context.BitfieldExtractU32(src, Const((s & 3) * 8), Const(8));

                    if ((s & 8) != 0)
                    {
                        byteValue = context.ShiftRightS32(context.ShiftLeft(byteValue, Const(24)), Const(31));
                    }
                }
                else
                {
                    Operand s = context.BitfieldExtractU32(sel, Const(i * 4), Const(4));
                    Operand src = context.ConditionalSelect(context.ICompareNotEqual(context.BitwiseAnd(s, Const(4)), Const(0)), srcC, srcA);
                    byteValue = context.BitfieldExtractU32(src, context.ShiftLeft(context.BitwiseAnd(s, Const(3)), Const(3)), Const(8));
                    Operand sign = context.ShiftRightS32(context.ShiftLeft(byteValue, Const(24)), Const(31));
                    byteValue = context.ConditionalSelect(context.ICompareNotEqual(context.BitwiseAnd(s, Const(8)), Const(0)), sign, byteValue);
                }

                res = context.BitwiseOr(res, context.ShiftLeft(context.BitwiseAnd(byteValue, Const(0xff)), Const(i * 8)));
            }

            ASet(context, x, res, u);
        }

        // POPC Rd, b: bit count (63: of the bitwise not)
        public static void APopc(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool u = AUniform(context);
            Operand src = AGet(context, AOne(context, x, u, neg: false, abs: false));

            if (x.Bit(63))
            {
                src = context.BitwiseNot(src);
            }

            ASet(context, x, context.BitCount(src), u);
        }

        // FLO Rd, b: the index of the most significant set bit (signed 73: of the first bit unlike the sign), .SH (74) as a shift amount; 63 negates the input
        public static void AFlo(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool u = AUniform(context);

            if (x.Bits(81, 84) != RegisterConsts.PredicateTrueIndex)
            {
                throw AFail(context, "FLO predicate output");
            }

            Operand src = AGet(context, AOne(context, x, u, neg: false, abs: false));

            if (x.Bit(63))
            {
                src = context.BitwiseNot(src);
            }

            Operand res = x.Bit(73) ? context.FindMSBS32(src) : context.FindMSBU32(src);

            if (x.Bit(74))
            {
                Operand isValid = context.ICompareNotEqual(res, Const(-1));
                res = context.ConditionalSelect(isValid, context.BitwiseExclusiveOr(res, Const(31)), res);
            }

            ASet(context, x, res, u);
        }

        // SGXT Rd, a, n: the low n bits of a, sign-extended (73) or zero-extended; .W (75) wraps n, else it is clamped to 32
        public static void ASgxt(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool u = AUniform(context);
            (ASrc a, ASrc b) = ASecond(context, x, u, neg: false, abs: false);

            Operand n = AGet(context, b);
            n = x.Bit(75) ? context.BitwiseAnd(n, Const(31)) : context.IMinimumU32(n, Const(32));

            Operand src = AGet(context, a);
            Operand res = x.Bit(73) ? context.BitfieldExtractS32(src, Const(0), n) : context.BitfieldExtractU32(src, Const(0), n);
            res = context.ConditionalSelect(context.ICompareEqual(n, Const(0)), Const(0), res);

            ASet(context, x, res, u);
        }

        // ---- predicates ----

        // R2P PR, Ra, mask: predicates from one byte of Ra (76..78), the mask an immediate (0x804) or a register (0x204)
        public static void AR2p(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand mask = x.Opcode == 0x804 ? Const((int)x.Imm32) : Register(x.Rb, RegisterType.Gpr);

            EmitR2p(context, Register(x.Ra, RegisterType.Gpr), mask, (ByteSel)x.Bits(76, 78), false);
        }

        // P2R Rd, PR, Ra, mask: the predicates into one byte of Ra's value
        public static void AP2r(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            Operand mask = x.Opcode == 0x803 ? Const((int)x.Imm32) : Register(x.Rb, RegisterType.Gpr);

            EmitP2r(context, Register(x.Ra, RegisterType.Gpr), Register(x.Rd, RegisterType.Gpr), mask, (ByteSel)x.Bits(76, 78), false);
        }

        // PLOP3 Pd0, Pd1, a (87..90), b (77..80), c (68..71; uniform with bit 67), lut0 (64..67 | 72..77 << 3), lut1 (16..24)
        public static void APlop3(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool u = AUniform(context);

            Operand a = APred(context, x, 87, 90, u);
            Operand b = APred(context, x, 77, 80, u);
            Operand c = APred(context, x, 68, 71, u || x.Bit(67));

            int lut0 = x.Bits(64, 67) | (x.Bits(72, 77) << 3);
            int lut1 = x.Bits(16, 24);

            ASetPred(context, x.Bits(81, 84), APredLut(context, a, b, c, lut0), u);
            ASetPred(context, x.Bits(84, 87), APredLut(context, a, b, c, lut1), u);
        }

        // A three-input boolean function by its truth table (bit (a << 2 | b << 1 | c) ... as LOP3: a = 0xf0, b = 0xcc, c = 0xaa)
        private static Operand APredLut(EmitterContext context, Operand a, Operand b, Operand c, int lut)
        {
            lut &= 0xff;

            if (lut == 0)
            {
                return Const(IrConsts.False);
            }

            if (lut == 0xff)
            {
                return Const(IrConsts.True);
            }

            Operand res = null;

            for (int m = 0; m < 8; m++)
            {
                int bit = ((m & 4) != 0 ? 0xf0 : 0x0f) & ((m & 2) != 0 ? 0xcc : 0x33) & ((m & 1) != 0 ? 0xaa : 0x55);

                if ((lut & bit) == 0)
                {
                    continue;
                }

                Operand term = context.BitwiseAnd(context.BitwiseAnd((m & 4) != 0 ? a : context.BitwiseNot(a), (m & 2) != 0 ? b : context.BitwiseNot(b)),
                    (m & 1) != 0 ? c : context.BitwiseNot(c));
                res = res == null ? term : context.BitwiseOr(res, term);
            }

            return res;
        }

        // VOTE / VOTEU Rd, Pd, P: Pd = the vote (72..74: all, any, equal) of P (87..90); Rd (a uniform register for VOTEU) the ballot
        public static void AVote(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool toUniform = x.Opcode == 0x886;
            Operand pred = APred(context, x, 87, 90);

            Operand vote = EmitVote(context, (VoteMode)x.Bits(72, 74), pred);

            if (vote != null)
            {
                ASetPred(context, x.Bits(81, 84), vote);
            }

            int rd = ARd(x, toUniform);

            if (rd != (toUniform ? RegisterConsts.UniformZeroIndex : RegisterConsts.RegisterZeroIndex))
            {
                context.Copy(ADestReg(rd, toUniform), EmitBallot(context, pred));
            }
        }

        // SHFL Rd, Pd, a, lane, c: lane a register (32..40) or an immediate (53..58; 0x989, 0xf89); c a register (64..72) or an immediate (40..53; 0x589,
        // 0xf89); mode 58..60
        public static void AShfl(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool laneImm = x.Opcode is 0x989 or 0xf89;
            bool cImm = x.Opcode is 0x589 or 0xf89;

            Operand srcA = Register(x.Ra, RegisterType.Gpr);
            Operand lane = laneImm ? Const(x.Bits(53, 58)) : Register(x.Rb, RegisterType.Gpr);
            Operand c = cImm ? Const(x.Bits(40, 53)) : Register(x.Rc, RegisterType.Gpr);

            (Operand res, Operand valid) = (ShflMode)x.Bits(58, 60) switch
            {
                ShflMode.Idx => context.Shuffle(srcA, lane, c),
                ShflMode.Up => context.ShuffleUp(srcA, lane, c),
                ShflMode.Down => context.ShuffleDown(srcA, lane, c),
                _ => context.ShuffleXor(srcA, lane, c),
            };

            ASet(context, x, res);
            ASetPred(context, x.Bits(81, 84), valid);
        }
    }
}
