using System.Linq;
using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitAluHelper;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitHelper;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    // Ampere warp-cooperative matrix instructions (HMMA, LDSM, MOVM) and packed-half compares, translated per thread. A matrix fragment is spread over the warp's 32
    // lanes in NVIDIA's documented layouts (PTX mma.m16n8k16 / ldmatrix / movmatrix): lane L holds rows L / 4 (and L / 4 + 8) at columns 2 (L % 4) and + 1. Each lane
    // gathers what it needs from the lanes that hold it with subgroup shuffles (Metal SIMD-groups are 32 wide, as the warp), and computes its own part. Field
    // positions: Mesa NAK sm70_encode.rs (OpHmma, OpLdsm, OpMovm, OpHSetP2, OpHSet2, OpHMnMx2).
    static partial class InstEmit
    {
        public static void RegisterAmpereTensor(AmpereRegistrar x)
        {
            const InstProps N = InstProps.None;

            x(0x23c, InstName.Hfma2, AHmma, N);
            x(0x83b, InstName.Lds, ALdsm, N);
            x(0x23a, InstName.Mov, AMovm, N);
            x(0x3a1, InstName.Vote, AMatch, N);
            x(0x3c4, InstName.Vote, ARedux, N, writesRd: false);
        }

        // REDUX URd, Ra (Mesa NAK OpRedux): Ra of the warp's active lanes reduced into a uniform register; 78..81: and, or, xor, sum, min, max; 73: signed min / max
        public static void ARedux(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int op = x.Bits(78, 81);
            bool signed = x.Bit(73);

            if (op > 5)
            {
                throw AFail(context, $"REDUX operation {op}");
            }

            Operand src = AGpr(x.Ra);
            Operand active = context.Ballot(Const(IrConsts.True), 0);
            Operand acc = null;

            for (int lane = 0; lane < 32; lane++)
            {
                Operand v = context.Shuffle(src, Const(lane));
                Operand r = acc == null ? v : op switch
                {
                    0 => context.BitwiseAnd(acc, v),
                    1 => context.BitwiseOr(acc, v),
                    2 => context.BitwiseExclusiveOr(acc, v),
                    3 => context.IAdd(acc, v),
                    4 => signed ? context.IMinimumS32(acc, v) : context.IMinimumU32(acc, v),
                    _ => signed ? context.IMaximumS32(acc, v) : context.IMaximumU32(acc, v),
                };
                Operand on = context.ICompareNotEqual(context.BitwiseAnd(context.ShiftRightU32(active, Const(lane)), Const(1)), Const(0));

                // The first active lane starts the reduction; lanes that are not active leave it as it is
                acc = acc == null ? context.ConditionalSelect(on, v, Const(op == 0 ? -1 : op == 4 ? (signed ? int.MaxValue : -1) : op == 5 ? (signed ? int.MinValue : 0) : 0))
                                  : context.ConditionalSelect(on, r, acc);
            }

            if (x.Rd != RegisterConsts.UniformZeroIndex)
            {
                context.Copy(Register(x.Rd, RegisterType.Uniform), acc);
            }
        }

        // MATCH.ANY / .ALL Rd, Ra (Mesa NAK OpMatch: .U64 73, .ANY 79, the predicate of .ALL 81..84): the active lanes whose value (Ra, or Ra and Ra + 1) equals this lane's,
        // as a lane mask; .ALL gives the active mask only when every active lane agrees, and 0 otherwise, and sets the predicate to whether they do
        public static void AMatch(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool u64 = x.Bit(73), any = x.Bit(79);
            Operand lo = AGpr(x.Ra), hi = u64 ? AGpr(x.Ra + 1) : null;
            Operand active = context.Ballot(Const(IrConsts.True), 0);
            Operand same = Const(0);

            for (int lane = 0; lane < 32; lane++)
            {
                Operand eq = context.ICompareEqual(context.Shuffle(lo, Const(lane)), lo);

                if (u64)
                {
                    eq = context.BitwiseAnd(eq, context.ICompareEqual(context.Shuffle(hi, Const(lane)), hi));
                }

                same = context.BitwiseOr(same, context.ConditionalSelect(eq, Const(1 << lane), Const(0)));
            }

            same = context.BitwiseAnd(same, active);
            Operand all = context.ICompareEqual(same, active);
            Operand res = any ? same : context.ConditionalSelect(all, active, Const(0));

            if (x.Rd < RegisterConsts.RegisterZeroIndex)
            {
                context.Copy(Register(x.Rd, RegisterType.Gpr), res);
            }

            if (!any)
            {
                ASetPred(context, x.Bits(81, 84), all);
            }
        }

        public static void RegisterAmpereHalfCompare(AmpereAluRegistrar alu)
        {
            alu(0x034, InstName.Hsetp2, AHsetp2, writesRd: false);
            alu(0x033, InstName.Hset2, AHset2);
            alu(0x040, InstName.Hadd2, AHmnmx2);
        }

        private static Operand ALaneId(EmitterContext context) => EmitLoadSubgroupLaneId(context);

        private static Operand AGpr(int r) => r >= RegisterConsts.RegisterZeroIndex ? Const(0) : Register(r, RegisterType.Gpr);

        // The low (0) or high (1) half of a packed pair, as a float
        private static Operand AHalf(EmitterContext context, Operand packed, int half)
        {
            return half == 0 ? context.UnpackHalf2x16Low(packed) : context.UnpackHalf2x16High(packed);
        }

        // nxnative: one element of a packed pair of HMMA sources as a float: f16 (type 0), or bf16 (1), the high half of a float, so its bits moved up 16
        private static Operand AMmaHalf(EmitterContext context, Operand packed, int half, int type)
        {
            if (type == 0)
            {
                return AHalf(context, packed, half);
            }

            return half == 0 ? context.ShiftLeft(packed, Const(16)) : context.BitwiseAnd(packed, Const(unchecked((int)0xffff0000)));
        }

        // HMMA.16816 / .1688 Rd, Ra, Rb, Rc: D (16 x 8) = A (16 x 16, or 16 x 8) * B (16 x 8, or 8 x 8) + C. Size 75 / 78 (0 m16n8k8, 1 m16n8k16), F32 accumulator and result
        // (76; else packed halves), source type 82..84 (0 f16, 1 bf16, 2 tf32). Lane L, g = L / 4, t = L % 4: A regs (row g / g + 8) x (columns 2t, 2t + 1; + 8 in the second half
        // of k), B regs (k 2t, 2t + 1; + 8) x (column g), C / D (row g / g + 8) x (columns 2t, 2t + 1). TF32 (one element a register): A regs (row g / g + 8) x (column t; + 4),
        // B regs (k t; + 4) x (column g).
        public static void AHmma(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int size = x.Bits(75, 76) | (x.Bits(78, 79) << 1);
            bool f32 = x.Bit(76);
            int type = x.Bits(82, 84);

            // nxnative: BF16 (m16n8k16) and TF32 (m16n8k8) sources, both with an f32 accumulator only (PTX mma.sync)
            if (size > 1 || type > 2 || (type != 0 && !f32) || (type == 2 && size != 0))
            {
                throw AFail(context, $"HMMA size {size} source type {type}");
            }

            // nxnative: HMMA.16816 with an f16 accumulator in a compute shader (agl's upscaler networks: every HMMA of Donkey Kong Bananza's agl_ndsr.pck) becomes
            // MatrixMultiplyAdd, then a call of dmsh_hmma16816_f16 (TensorPass), which the shader build turns into simdgroup_matrix code. Emulated per lane, it took 32
            // shuffles and 64 FMAs a lane per HMMA, 2.3-6.5 ms a frame for the upscaler's dispatch on an M4 Pro, five times the simdgroup_matrix version's cost
            ShaderDefinitions defs = context.TranslatorContext.Definitions;

            if (size == 1 && !f32 && type == 0 && defs.Stage == ShaderStage.Compute)
            {
                int threads = defs.ComputeLocalSizeX * defs.ComputeLocalSizeY * defs.ComputeLocalSizeZ;
                int memoryId = context.ResourceManager.GetHmmaScratchMemoryId(System.Math.Max(1, (threads + 31) / 32));

                Operand tidX = context.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(0));
                Operand tidY = context.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(1));
                Operand tidZ = context.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(2));
                Operand flat = context.IAdd(tidX, context.IAdd(context.IMultiply(tidY, Const(defs.ComputeLocalSizeX)),
                                                              context.IMultiply(tidZ, Const(defs.ComputeLocalSizeX * defs.ComputeLocalSizeY))));
                Operand block = context.ShiftLeft(context.ShiftRightU32(flat, Const(5)), Const(8));   // the subgroup's 256 words (Metal SIMD-groups are 32 wide, as the warp)

                (Operand d0, Operand d1) = context.Add(Instruction.MatrixMultiplyAdd, (Local(), Local()), Const(memoryId), ALaneId(context), block,
                    AGpr(x.Ra), AGpr(x.Ra + 1), AGpr(x.Ra + 2), AGpr(x.Ra + 3), AGpr(x.Rb), AGpr(x.Rb + 1), AGpr(x.Rc), AGpr(x.Rc + 1));

                if (x.Rd < RegisterConsts.RegisterZeroIndex)
                {
                    context.Copy(Register(x.Rd, RegisterType.Gpr), d0);
                }

                if (x.Rd + 1 < RegisterConsts.RegisterZeroIndex)
                {
                    context.Copy(Register(x.Rd + 1, RegisterType.Gpr), d1);
                }

                return;
            }

            int k = size == 1 ? 16 : 8;
            int aRegs = k / 4;   // 4 (k16) or 2 (k8): (row g, k lo), (row g + 8, k lo), (row g, k hi), (row g + 8, k hi)
            int bRegs = k / 8;   // 2 or 1

            Operand lane = ALaneId(context);
            Operand g = context.ShiftRightU32(lane, Const(2));
            Operand t = context.BitwiseAnd(lane, Const(3));

            Operand[,] a = new Operand[2, 16], aWord = new Operand[2, 16];   // aWord: the register holding a[row, kk] (half kk & 1 of it, or all of a tf32)
            Operand[,] b = new Operand[2, 16], bWord = new Operand[2, 16];

            if (type == 2)
            {
                // nxnative: the tensor core reads a tf32 as the float's top 19 bits (sign, exponent, 10 mantissa bits) and ignores the low 13 (a Jetson Orin's results:
                // with them kept, no output of 4,096 matches), so the FMA chain an Inf or NaN takes (AHmmaExact) clears them too
                Operand Tf32(Operand v) => context.BitwiseAnd(v, Const(unchecked((int)0xffffe000)));

                // A rows g and g + 8, k = 0..7: from lanes 4g + j (j = 0..3), which hold k = j (regs 0, 1) and j + 4 (regs 2, 3)
                for (int j = 0; j < 4; j++)
                {
                    Operand src = context.IAdd(context.ShiftLeft(g, Const(2)), Const(j));

                    for (int r = 0; r < 4; r++)
                    {
                        Operand v = context.Shuffle(AGpr(x.Ra + r), src);
                        int row = r & 1, kk = (r >> 1) * 4 + j;
                        a[row, kk] = Tf32(v);
                        aWord[row, kk] = v;
                    }
                }

                // B columns 2t and 2t + 1, k = 0..7: from lanes 4 col + j, which hold k = j (reg 0) and j + 4 (reg 1)
                for (int c = 0; c < 2; c++)
                {
                    Operand col = context.IAdd(context.ShiftLeft(t, Const(1)), Const(c));

                    for (int j = 0; j < 4; j++)
                    {
                        Operand src = context.IAdd(context.ShiftLeft(col, Const(2)), Const(j));

                        for (int r = 0; r < 2; r++)
                        {
                            Operand v = context.Shuffle(AGpr(x.Rb + r), src);
                            b[c, r * 4 + j] = Tf32(v);
                            bWord[c, r * 4 + j] = v;
                        }
                    }
                }
            }
            else
            {
                // A rows g and g + 8, every k: from lanes 4g + j (j = 0..3), which hold k = 2j, 2j + 1 (+ 8)
                for (int j = 0; j < 4; j++)
                {
                    Operand src = context.IAdd(context.ShiftLeft(g, Const(2)), Const(j));

                    for (int r = 0; r < aRegs; r++)
                    {
                        Operand v = context.Shuffle(AGpr(x.Ra + r), src);
                        int row = r & 1, kb = (r >> 1) * 8 + 2 * j;
                        a[row, kb] = AMmaHalf(context, v, 0, type);
                        a[row, kb + 1] = AMmaHalf(context, v, 1, type);
                        aWord[row, kb] = aWord[row, kb + 1] = v;
                    }
                }

                // B columns 2t and 2t + 1, every k: from lanes 4 col + j, which hold k = 2j, 2j + 1 (+ 8)
                for (int c = 0; c < 2; c++)
                {
                    Operand col = context.IAdd(context.ShiftLeft(t, Const(1)), Const(c));

                    for (int j = 0; j < 4; j++)
                    {
                        Operand src = context.IAdd(context.ShiftLeft(col, Const(2)), Const(j));

                        for (int r = 0; r < bRegs; r++)
                        {
                            Operand v = context.Shuffle(AGpr(x.Rb + r), src);
                            int kb = r * 8 + 2 * j;
                            b[c, kb] = AMmaHalf(context, v, 0, type);
                            b[c, kb + 1] = AMmaHalf(context, v, 1, type);
                            bWord[c, kb] = bWord[c, kb + 1] = v;
                        }
                    }
                }
            }

            // C: (row, column) of this lane's four outputs
            Operand[] acc = new Operand[4];

            for (int i = 0; i < 4; i++)
            {
                int row = i >> 1, c = i & 1;
                acc[i] = f32 ? AGpr(x.Rc + i) : AHalf(context, AGpr(x.Rc + row), c);

                for (int kk = 0; kk < k; kk++)
                {
                    acc[i] = context.FPFusedMultiplyAdd(a[row, kk], b[c, kk], acc[i]);
                }
            }

            Operand[] d = AHmmaExact(context, x, k, type, f32, aWord, bWord, acc);

            if (f32)
            {
                for (int i = 0; i < 4 && x.Rd + i < RegisterConsts.RegisterZeroIndex; i++)
                {
                    context.Copy(Register(x.Rd + i, RegisterType.Gpr), d[i]);
                }
            }
            else
            {
                for (int row = 0; row < 2 && x.Rd + row < RegisterConsts.RegisterZeroIndex; row++)
                {
                    context.Copy(Register(x.Rd + row, RegisterType.Gpr), context.BitwiseOr(d[row * 2], context.ShiftLeft(d[row * 2 + 1], Const(16))));
                }
            }
        }

        // nxnative: HMMA with an f32 accumulator adds as Ampere's tensor core does (the A100 model of Khattak and Mikaitis, "Accurate models of NVIDIA tensor cores",
        // 2025, and their MATLAB-tensor-core toolbox; it matches all 4,096 lanes a Jetson Orin computed): the products are exact; each block of 8 products and C (the
        // previous block's result for the second block of k16) are aligned to the largest exponent with 24 fraction bits (one more than f32's), every bit below truncated,
        // summed exactly, and the sum truncated to f32 (round toward zero). The FMA chain (round to nearest after every product) was 1 ULP off in two thirds of the lanes,
        // thousands where the alignment drops a small product. Integer operations on the bits, so Metal's fast math cannot change it; an Inf or NaN operand takes the chain.
        // nxnative: bf16 sources (type 1) add the same way in blocks of 8, tf32 (type 2, k8) in blocks of 4, each of their products with its 24 fraction bits too: the same
        // model matches all 4,096 of the Orin's outputs for each
        // nxnative: with an f16 accumulator (f16 sources only) the blocks are the same and each block's sum is rounded to f16 (to nearest, even), which is the next block's
        // C. On this Mac's GPU it matches all 4,718,592 outputs of 3 x 4,096 warps of single and chained HMMA.16816 a Jetson Orin computed; the FMA chain rounded once after
        // all 16 products and missed 34 % of them. A compute shader's m16n8k16 keeps the simdgroup_matrix path above (an f32 sum of each block rounded once to f16, 1 ULP off
        // in about 0.03 % of the outputs): this model there, each lane summing its outputs in integers, made Ocarina of Time's NDSR networks 23 to 30 times slower. Returns
        // the 4 outputs: f32 bits, or f16 bits in the low half of each word
        private static Operand[] AHmmaExact(EmitterContext context, AmpereOp x, int k, int type, bool f32, Operand[,] aWord, Operand[,] bWord, Operand[] chain)
        {
            int blockSize = type == 2 ? 4 : 8;
            int special = type == 0 ? 31 : 255;   // the exponent field of an Inf or NaN

            var aPart = new (Operand Sig, Operand Exp, Operand Sign, Operand Field)[2, 16];
            var bPart = new (Operand Sig, Operand Exp, Operand Sign, Operand Field)[2, 16];
            Operand[] aMaxField = new Operand[2], bMaxField = new Operand[2];

            for (int r = 0; r < 2; r++)
            {
                for (int kk = 0; kk < k; kk++)
                {
                    aPart[r, kk] = AMmaParts(context, aWord[r, kk], kk & 1, type, a: true);
                    bPart[r, kk] = AMmaParts(context, bWord[r, kk], kk & 1, type, a: false);
                    aMaxField[r] = kk == 0 ? aPart[r, kk].Field : context.IMaximumU32(aMaxField[r], aPart[r, kk].Field);
                    bMaxField[r] = kk == 0 ? bPart[r, kk].Field : context.IMaximumU32(bMaxField[r], bPart[r, kk].Field);
                }
            }

            Operand[] d = new Operand[4];

            for (int i = 0; i < 4; i++)
            {
                int row = i >> 1, c = i & 1;
                Operand[] mag = new Operand[k], exp = new Operand[k], sign = new Operand[k];

                for (int kk = 0; kk < k; kk++)
                {
                    mag[kk] = context.IMultiply(aPart[row, kk].Sig, bPart[c, kk].Sig);    // < 2^26 with 24 fraction bits (A's significands are shifted to give them)
                    exp[kk] = context.IAdd(aPart[row, kk].Exp, bPart[c, kk].Exp);        // the f32 biased exponent less 97 (f16 biased exponents' sum)
                    sign[kk] = context.BitwiseExclusiveOr(aPart[row, kk].Sign, bPart[c, kk].Sign);
                }

                Operand cIn = f32 ? AGpr(x.Rc + i) : context.BitfieldExtractU32(AGpr(x.Rc + row), Const(c * 16), Const(16));
                Operand res = cIn;

                for (int blk = 0; blk < k; blk += blockSize)
                {
                    res = f32 ? AHmmaF32Block(context, mag[blk..(blk + blockSize)], exp[blk..(blk + blockSize)], sign[blk..(blk + blockSize)], res)
                              : AHmmaF16Block(context, mag[blk..(blk + blockSize)], exp[blk..(blk + blockSize)], sign[blk..(blk + blockSize)], res);
                }

                // An Inf or NaN (the format's largest exponent field) among this output's A row and B column, or an f32 (f16) one in C
                Operand isSpecial = context.BitwiseOr(
                    context.ICompareEqual(context.IMaximumU32(aMaxField[row], bMaxField[c]), Const(special)),
                    f32 ? context.ICompareEqual(context.BitwiseAnd(cIn, Const(0x7f800000)), Const(0x7f800000))
                        : context.ICompareEqual(context.BitwiseAnd(cIn, Const(0x7c00)), Const(0x7c00)));
                d[i] = context.ConditionalSelect(isSpecial, f32 ? chain[i] : context.BitwiseAnd(context.PackHalf2x16(chain[i], ConstF(0)), Const(0xffff)), res);
            }

            return d;
        }

        // nxnative: one source element of register w for AHmmaExact: half `half` of a packed f16 or bf16 pair, or a tf32 (bits 13..31). (significand, shifted in A so that
        // a product has 24 fraction bits; exponent field with denormals at 1 and zeros at -1024 (so a zero product never sets a block's exponent), in A less 224 for the
        // 8-bit exponents, so that a product's is the f32 biased exponent less 97 as with f16; sign as 0 or -1; the exponent field)
        private static (Operand Sig, Operand Exp, Operand Sign, Operand Field) AMmaParts(EmitterContext context, Operand w, int half, int type, bool a)
        {
            (int o, int fracBits, int expBits, int sigShift) = type switch
            {
                0 => (half * 16, 10, 5, 4),    // f16 x f16: 20 fraction bits
                1 => (half * 16, 7, 8, 10),    // bf16 x bf16: 14
                _ => (13, 10, 8, 4),           // tf32 x tf32: 20
            };

            sigShift = a ? sigShift : 0;
            Operand field = context.BitfieldExtractU32(w, Const(o + fracBits), Const(expBits));
            Operand frac = context.BitfieldExtractU32(w, Const(o), Const(fracBits));
            Operand sig = context.BitwiseOr(context.ShiftLeft(frac, Const(sigShift)), context.ShiftLeft(context.IMinimumU32(field, Const(1)), Const(fracBits + sigShift)));
            Operand exp = context.IMaximumU32(field, Const(1));
            exp = a && type != 0 ? context.ISubtract(exp, Const(224)) : exp;
            exp = context.ConditionalSelect(context.ICompareEqual(sig, Const(0)), Const(-1024), exp);
            Operand sign = context.BitfieldExtractS32(w, Const(o + fracBits + expBits), Const(1));

            return (sig, exp, sign, field);
        }

        // One block: 4 or 8 products (magnitude with 24 fraction bits below 2^exp, exponent on the products' scale, sign 0 / -1) and C (f32 bits) aligned to the largest
        // exponent, truncated, summed, and the sum truncated to f32 bits. |sum| < 9 * 2^26 fits in an int.
        private static Operand AHmmaF32Block(EmitterContext context, Operand[] mag, Operand[] exp, Operand[] sign, Operand c)
        {
            Operand cField = context.BitfieldExtractU32(c, Const(23), Const(8));
            Operand cMag = context.ShiftLeft(context.BitwiseOr(context.BitwiseAnd(c, Const(0x7fffff)), context.ShiftLeft(context.IMinimumU32(cField, Const(1)), Const(23))), Const(1));
            Operand cExp = context.ConditionalSelect(context.ICompareEqual(context.BitwiseAnd(c, Const(0x7fffffff)), Const(0)), Const(-512),
                                                     context.ISubtract(context.IMaximumU32(cField, Const(1)), Const(97)));
            Operand cSign = context.ShiftRightS32(c, Const(31));

            (Operand sum, Operand max) = AHmmaBlockSum(context, mag, exp, sign, cMag, cExp, cSign);

            // value = |sum| * 2^(max + 97 - 127 - 24); its leading bit p gives the f32 biased exponent max + 73 + p, here less 1 (the significand's bit 23 adds it back).
            // Below the normal range the f32 bits are |sum| * 2^(max + 95): the same shift with the exponent's deficit added
            Operand m = context.IAbsolute(sum);
            Operand p = context.FindMSBU32(m);
            Operand e1 = context.IAdd(max, context.IAdd(p, Const(72)));
            Operand shift = context.ISubtract(context.IAdd(Const(23), context.IMinimumS32(e1, Const(0))), p);
            // nxnative: a right shift of 32 or more (a sum below f32's denormals, which only 8-bit source exponents reach) is undefined in MSL, so at most 31 (m < 2^30 is then 0)
            Operand n = context.ShiftRightU32(context.ShiftLeft(m, context.IMaximumS32(shift, Const(0))),
                                              context.IMinimumS32(context.IMaximumS32(context.INegate(shift), Const(0)), Const(31)));
            Operand bits = context.IAdd(context.ShiftLeft(context.IMaximumS32(e1, Const(0)), Const(23)), n);
            bits = context.ConditionalSelect(context.ICompareGreaterOrEqual(e1, Const(254)), Const(0x7f800000), bits);
            bits = context.BitwiseOr(bits, context.BitwiseAnd(sum, Const(unchecked((int)0x80000000))));

            return context.ConditionalSelect(context.ICompareEqual(sum, Const(0)), Const(0), bits);
        }

        // nxnative: one block with an f16 C (its bits in the low half of c): the sum as AHmmaF32Block's, rounded to f16 bits (to nearest, even)
        private static Operand AHmmaF16Block(EmitterContext context, Operand[] mag, Operand[] exp, Operand[] sign, Operand c)
        {
            // C with 24 fraction bits; on the products' scale its exponent is the f16 field plus 15
            Operand cField = context.BitfieldExtractU32(c, Const(10), Const(5));
            Operand cMag = context.ShiftLeft(context.BitwiseOr(context.BitwiseAnd(c, Const(0x3ff)), context.ShiftLeft(context.IMinimumU32(cField, Const(1)), Const(10))), Const(14));
            Operand cExp = context.ConditionalSelect(context.ICompareEqual(context.BitwiseAnd(c, Const(0x7fff)), Const(0)), Const(-1024),
                                                     context.IAdd(context.IMaximumU32(cField, Const(1)), Const(15)));
            Operand cSign = context.BitfieldExtractS32(c, Const(15), Const(1));

            (Operand sum, Operand max) = AHmmaBlockSum(context, mag, exp, sign, cMag, cExp, cSign);

            // value = |sum| * 2^(max - 54); its leading bit p gives the f16 biased exponent e = max + p - 39. The significand keeps 11 bits (fewer below the normal range):
            // shift right by p - 10 + max(1 - e, 0), rounding to nearest even on the bits shifted out, or left when that is negative (exact)
            Operand m = context.IAbsolute(sum);
            Operand p = context.FindMSBU32(m);
            Operand e = context.IAdd(max, context.ISubtract(p, Const(39)));
            Operand shift = context.IAdd(context.ISubtract(p, Const(10)), context.IMaximumS32(context.ISubtract(Const(1), e), Const(0)));
            Operand r = context.IMinimumS32(context.IMaximumS32(shift, Const(0)), Const(31));
            Operand q = context.ShiftLeft(context.ShiftRightU32(m, r), context.IMaximumS32(context.INegate(shift), Const(0)));
            Operand one = context.ShiftLeft(Const(1), r);
            Operand rem = context.BitwiseAnd(m, context.ISubtract(one, Const(1)));
            Operand up = context.BitwiseAnd(context.ICompareNotEqual(r, Const(0)),
                                            context.ICompareGreaterUnsigned(context.IAdd(rem, context.BitwiseAnd(q, Const(1))), context.ShiftRightU32(one, Const(1))));
            q = context.IAdd(q, context.ConditionalSelect(up, Const(1), Const(0)));
            Operand bits = context.IAdd(context.ShiftLeft(context.ISubtract(context.IMaximumS32(e, Const(1)), Const(1)), Const(10)), q);
            bits = context.IMinimumU32(bits, Const(0x7c00));   // past the largest half: Inf
            bits = context.BitwiseOr(bits, context.BitwiseAnd(context.ShiftRightU32(sum, Const(16)), Const(0x8000)));

            return context.ConditionalSelect(context.ICompareEqual(sum, Const(0)), Const(0), bits);
        }

        // The terms of one block (the products and C, each magnitude with 24 fraction bits below 2^exp, sign 0 / -1) aligned to the largest exponent, each truncated, and
        // summed: (the sum, the largest exponent)
        private static (Operand Sum, Operand Max) AHmmaBlockSum(EmitterContext context, Operand[] mag, Operand[] exp, Operand[] sign, Operand cMag, Operand cExp, Operand cSign)
        {
            Operand max = cExp;

            for (int j = 0; j < mag.Length; j++)
            {
                max = context.IMaximumS32(max, exp[j]);
            }

            Operand Aligned(Operand m, Operand e, Operand s)
            {
                Operand v = context.ShiftRightU32(m, context.IMinimumU32(context.ISubtract(max, e), Const(31)));
                return context.ISubtract(context.BitwiseExclusiveOr(v, s), s);
            }

            Operand sum = Aligned(cMag, cExp, cSign);

            for (int j = 0; j < mag.Length; j++)
            {
                sum = context.IAdd(sum, Aligned(mag[j], exp[j], sign[j]));
            }

            return (sum, max);
        }

        // LDSM.16.M88.n / .MT88.n Rd, [Ra + UR + offset]: n (72..74: 1, 2, 4) 8 x 8 matrices of halves from shared memory; lanes 8i .. 8i + 7 give the addresses of
        // matrix i's rows. Lane L receives, of each matrix, row L / 4 at columns 2 (L % 4), + 1 (one register); transposed (78..80 = 1), column L / 4 at rows 2 (L % 4), + 1.
        public static void ALdsm(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            int count = 1 << x.Bits(72, 74);
            int mode = x.Bits(78, 80);

            if (count > 4 || mode > 1)
            {
                throw AFail(context, $"LDSM mode {mode} count {count}");
            }

            Operand addr = context.IAdd(ALocalAddress(context, x, 32, stride: false), Const((int)x.SignedField(40, 64)));
            int id = context.ResourceManager.SharedMemoryId;
            bool compute = context.TranslatorContext.Definitions.Stage == ShaderStage.Compute;
            Operand lane = compute ? null : ALaneId(context);
            Operand g = compute ? null : context.ShiftRightU32(lane, Const(2));
            Operand t = compute ? null : context.BitwiseAnd(lane, Const(3));

            // nxnative: LDSM reads rows that other lanes of the warp stored (STS) just before, often with no BAR between them (Ocarina of Time's NDVE movie networks and its
            // scene's compute). On the Switch a warp's shared memory accesses are in order; Metal orders another lane's threadgroup store and this lane's load only through a
            // barrier, and its compiler moves them past each other: dotted rows, every other pixel of a few rows wrong (the high halves of packed pairs), in other places each run.
            // LDSM is warp-collective (the whole warp runs it), so a subgroup barrier before its reads (the stores have landed) and one after (no lane's next store reaches the
            // rows before every lane has read them) is safe. In a compute shader the barriers bracket the MatrixLoads, so they bracket the per-lane code TensorPass puts in their
            // place; TensorPass does not count a subgroup barrier as a shared memory write, so an HMMA still reads its A straight from where the LDSM read it
            context.SubgroupBarrier();

            // nxnative: in a compute shader, one MatrixLoad a matrix (sources: shared memory id, mode, row address, matrix), which TensorPass lowers to the same per-lane code
            // (LdsmLane) once it knows whether the HMMAs it feeds read their A straight from where it reads (Ampere HMMA through simdgroup_matrix). One destination each:
            // the optimizer's dead code removal takes an operation whose destination nothing reads as dead, sources included
            for (int m = 0; m < count && x.Rd + m < RegisterConsts.RegisterZeroIndex; m++)
            {
                Operand result = compute ? context.Add(Instruction.MatrixLoad, Local(), Const(id), Const(mode), addr, Const(m)) : LdsmLane(context, g, t, addr, id, m, mode);

                context.Copy(Register(x.Rd + m, RegisterType.Gpr), result);
            }

            context.SubgroupBarrier();
        }

        // LDSM per lane: lane L's register of matrix m (g = L / 4, t = L % 4), from the row addresses (bytes in shared memory `id`) the lanes hold in addr
        public static Operand LdsmLane(EmitterContext context, Operand g, Operand t, Operand addr, int id, int m, int mode)
        {
            if (mode == 0)
            {
                Operand rowAddr = context.Shuffle(addr, context.IAdd(g, Const(8 * m)));
                Operand byteOffset = context.IAdd(rowAddr, context.ShiftLeft(t, Const(2)));

                return context.Load(StorageKind.SharedMemory, id, context.ShiftRightU32(byteOffset, Const(2)));
            }

            Operand[] h = new Operand[2];

            for (int j = 0; j < 2; j++)
            {
                Operand row = context.IAdd(context.ShiftLeft(t, Const(1)), Const(j));
                Operand rowAddr = context.Shuffle(addr, context.IAdd(row, Const(8 * m)));
                Operand byteOffset = context.IAdd(rowAddr, context.ShiftLeft(g, Const(1)));
                Operand word = context.Load(StorageKind.SharedMemory, id, context.ShiftRightU32(byteOffset, Const(2)));
                h[j] = context.BitfieldExtractU32(word, context.ShiftLeft(context.BitwiseAnd(byteOffset, Const(2)), Const(3)), Const(16));
            }

            return context.BitwiseOr(h[0], context.ShiftLeft(h[1], Const(16)));
        }

        // MOVM.16.MT88 Rd, Ra: the 8 x 8 matrix of halves the warp holds (lane L: row L / 4, columns 2 (L % 4), + 1), transposed into the same layout
        public static void AMovm(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x.Bits(78, 80) != 0)
            {
                throw AFail(context, $"MOVM mode {x.Bits(78, 80)}");
            }

            Operand lane = ALaneId(context);
            Operand r = context.ShiftRightU32(lane, Const(2));
            Operand t = context.BitwiseAnd(lane, Const(3));
            Operand src = AGpr(x.Ra);
            Operand[] h = new Operand[2];

            for (int j = 0; j < 2; j++)
            {
                // out (r, c = 2t + j) = in (c, r): held by lane 4c + r / 2, half r % 2
                Operand c = context.IAdd(context.ShiftLeft(t, Const(1)), Const(j));
                Operand v = context.Shuffle(src, context.IAdd(context.ShiftLeft(c, Const(2)), context.ShiftRightU32(r, Const(1))));
                h[j] = context.BitfieldExtractU32(v, context.ShiftLeft(context.BitwiseAnd(r, Const(1)), Const(4)), Const(16));
            }

            if (x.Rd < RegisterConsts.RegisterZeroIndex)
            {
                context.Copy(Register(x.Rd, RegisterType.Gpr), context.BitwiseOr(h[0], context.ShiftLeft(h[1], Const(16))));
            }
        }

        // .FTZ (80) of HSETP2 / HSET2: a subnormal half compares as zero, as FSETP.FTZ's float does (AFtz). The lanes are unpacked to 32 bits, where a half's subnormals
        // are normal floats, so FlushDenormal's bound would never flush them: the half's own bound is 2^-14
        private static (Operand[] A, Operand[] B) AHalfFtz(EmitterContext context, AmpereOp x, (Operand[] A, Operand[] B) src)
        {
            if (!x.Bit(80))
            {
                return src;
            }

            Operand[] Flush(Operand[] v) => [.. v.Select(l => context.ConditionalSelect(context.FPCompareLess(context.FPAbsolute(l), ConstF(6.1035156e-05f)), ConstF(0), l))];

            return (Flush(src.A), Flush(src.B));
        }

        // HSETP2 Pd0, Pd1, a, b, P: each half's comparison (76..80) combined with P (87..90) by the operation at 69..71; with .H_AND (71) the two halves' results ANDed
        // into Pd0 and its negation into Pd1
        public static void AHsetp2(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (Operand[] a, Operand[] b) = AHalfFtz(context, x, AHalfSecond(context, x));

            Operand lo = GetFPComparison(context, (FComp)x.Bits(76, 80), a[0], b[0]);
            Operand hi = GetFPComparison(context, (FComp)x.Bits(76, 80), a[1], b[1]);

            if (x.Bit(71))
            {
                lo = context.BitwiseAnd(lo, hi);
                hi = context.BitwiseNot(lo);
            }

            Operand p = APred(context, x, 87, 90);
            BoolOp bop = (BoolOp)x.Bits(69, 71);

            ASetPred(context, x.Bits(81, 84), GetPredLogicalOp(context, bop, lo, p));
            ASetPred(context, x.Bits(84, 87), GetPredLogicalOp(context, bop, hi, p));
        }

        // HSET2 Rd, a, b, P: each half 1.0 (.BF, 71) or all ones where (comparison bop P) holds, else 0
        public static void AHset2(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            (Operand[] a, Operand[] b) = AHalfFtz(context, x, AHalfSecond(context, x));
            Operand p = APred(context, x, 87, 90);
            BoolOp bop = (BoolOp)x.Bits(69, 71);
            Operand res = Const(0);

            for (int i = 0; i < 2; i++)
            {
                Operand cond = GetPredLogicalOp(context, bop, GetFPComparison(context, (FComp)x.Bits(76, 80), a[i], b[i]), p);
                Operand v = context.ConditionalSelect(cond, Const(x.Bit(71) ? 0x3c00 : 0xffff), Const(0));
                res = context.BitwiseOr(res, i == 0 ? v : context.ShiftLeft(v, Const(16)));
            }

            ASet(context, x, res);
        }

        // HMNMX2 Rd, a, b, P: each half's minimum where P (87..90) holds, else maximum
        public static void AHmnmx2(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);

            if (x.Bit(78) || x.Bit(81) || x.Bit(82) || x.Bit(85))
            {
                throw AFail(context, "HMNMX2 .F32 / .NAN / .XORSIGN / .BF16");
            }

            (Operand[] a, Operand[] b) = AHalfSecond(context, x);
            Operand p = APred(context, x, 87, 90);
            Operand[] r = new Operand[2];

            for (int i = 0; i < 2; i++)
            {
                r[i] = context.ConditionalSelect(p, context.FPMinimum(a[i], b[i]), context.FPMaximum(a[i], b[i]));
            }

            ASet(context, x, context.PackHalf2x16(r[0], r[1]));
        }
    }
}
