using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System;

using static Ryujinx.Graphics.Shader.Instructions.InstEmitHelper;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    static partial class InstEmit
    {
        public static void PrmtR(EmitterContext context)
        {
            InstPrmtR op = context.GetOp<InstPrmtR>();
            EmitPrmt(context, GetSrcReg(context, op.SrcA), GetSrcReg(context, op.SrcB), GetSrcReg(context, op.SrcC), op.Dest, op.PMode);
        }

        public static void PrmtI(EmitterContext context)
        {
            InstPrmtI op = context.GetOp<InstPrmtI>();
            EmitPrmt(context, GetSrcReg(context, op.SrcA), GetSrcImm(context, Imm20ToSInt(op.Imm20)), GetSrcReg(context, op.SrcC), op.Dest, op.PMode);
        }

        public static void PrmtC(EmitterContext context)
        {
            InstPrmtC op = context.GetOp<InstPrmtC>();
            EmitPrmt(context, GetSrcReg(context, op.SrcA), GetSrcCbuf(context, op.CbufSlot, op.CbufOffset), GetSrcReg(context, op.SrcC), op.Dest, op.PMode);
        }

        public static void PrmtRc(EmitterContext context)
        {
            InstPrmtRc op = context.GetOp<InstPrmtRc>();
            // The register at bit 39 is the selector; the constant buffer supplies the upper four bytes.
            EmitPrmt(context, GetSrcReg(context, op.SrcA), GetSrcReg(context, op.SrcC), GetSrcCbuf(context, op.CbufSlot, op.CbufOffset), op.Dest, op.PMode);
        }

        private static void EmitPrmt(EmitterContext context, Operand a, Operand selector, Operand b, int rd, PMode mode)
        {
            // Maxwell PRMT Rd, a, selector, b: PTX prmt's {b:a} byte pair, with selector as PTX's final operand.
            // Generic mode uses four nibbles (bit 3 replicates the selected byte's sign). The six specialized modes
            // use selector[1:0], with no sign replication: NVIDIA PTX ISA, Data Movement and Conversion / prmt.
            if ((int)mode > (int)PMode.Rc16)
            {
                throw new InvalidOperationException($"Maxwell PRMT mode {(int)mode} is not supported.");
            }

            Operand select = context.BitwiseAnd(selector, Const(3));
            Operand result = Const(0);
            for (int i = 0; i < 4; i++)
            {
                Operand index = mode switch
                {
                    0 => context.BitfieldExtractU32(selector, Const(i * 4), Const(4)),
                    PMode.F4e => context.IAdd(select, Const(i)),
                    PMode.B4e => context.BitwiseAnd(context.ISubtract(select, Const(i)), Const(7)),
                    PMode.Rc8 => select,
                    PMode.Ecl => context.IMaximumU32(select, Const(i)),
                    PMode.Ecr => context.IMinimumU32(select, Const(i)),
                    PMode.Rc16 => context.IAdd(context.ShiftLeft(context.BitwiseAnd(select, Const(1)), Const(1)), Const(i & 1)),
                    _ => throw new InvalidOperationException(),
                };
                Operand source = context.ConditionalSelect(context.ICompareNotEqual(context.BitwiseAnd(index, Const(4)), Const(0)), b, a);
                Operand shift = context.ShiftLeft(context.BitwiseAnd(index, Const(3)), Const(3));
                Operand value = context.BitfieldExtractU32(source, shift, Const(8));
                if (mode == 0)
                {
                    Operand sign = context.ShiftRightS32(context.ShiftLeft(value, Const(24)), Const(31));
                    value = context.ConditionalSelect(context.ICompareNotEqual(context.BitwiseAnd(index, Const(8)), Const(0)), sign, value);
                }
                result = context.BitwiseOr(result, context.ShiftLeft(context.BitwiseAnd(value, Const(255)), Const(i * 8)));
            }
            context.Copy(GetDest(rd), result);
        }
    }
}
