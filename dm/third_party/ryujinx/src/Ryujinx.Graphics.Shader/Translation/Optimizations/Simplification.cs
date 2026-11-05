using Ryujinx.Graphics.Shader.IntermediateRepresentation;

using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Optimizations
{
    static class Simplification
    {
        private const int AllOnes = ~0;

        public static void RunPass(Operation operation)
        {
            switch (operation.Inst)
            {
                case Instruction.Add:
                    if (!TryEliminateHalfWordRecombine(operation))
                    {
                        TryEliminateBinaryOpCommutative(operation, 0);
                    }

                    break;

                case Instruction.BitwiseAnd:
                    TryEliminateBitwiseAnd(operation);
                    break;

                case Instruction.BitwiseExclusiveOr:
                    if (!TryEliminateXorSwap(operation))
                    {
                        TryEliminateBinaryOpCommutative(operation, 0);
                    }

                    break;

                case Instruction.BitwiseOr:
                    if (!TryEliminateHalfWordRecombine(operation))
                    {
                        TryEliminateBitwiseOr(operation);
                    }

                    break;

                case Instruction.CompareNotEqual:
                    TryEliminateCompareNotEqual(operation);
                    break;

                case Instruction.ConditionalSelect:
                    TryEliminateConditionalSelect(operation);
                    break;

                case Instruction.Divide:
                    TryEliminateBinaryOpY(operation, 1);
                    break;

                case Instruction.Multiply:
                    // nxnative: integer x * 0 == 0 (the XMAD expansion of a multiply by a 16-bit constant leaves one, see TryEliminateHalfWordRecombine)
                    if (IsConstEqual(operation.GetSource(0), 0) || IsConstEqual(operation.GetSource(1), 0))
                    {
                        operation.TurnIntoCopy(Const(0));
                    }
                    else
                    {
                        TryEliminateBinaryOpCommutative(operation, 1);
                    }

                    break;

                case Instruction.ShiftLeft:
                case Instruction.ShiftRightS32:
                case Instruction.ShiftRightU32:
                case Instruction.Subtract:
                    TryEliminateBinaryOpY(operation, 0);
                    break;
            }
        }

        private static void TryEliminateBitwiseAnd(Operation operation)
        {
            // Try to recognize and optimize those 3 patterns (in order):
            //  x & 0xFFFFFFFF == x,          0xFFFFFFFF & y == y,
            //  x & 0x00000000 == 0x00000000, 0x00000000 & y == 0x00000000

            Operand x = operation.GetSource(0);
            Operand y = operation.GetSource(1);

            if (IsConstEqual(x, AllOnes))
            {
                operation.TurnIntoCopy(y);
            }
            else if (IsConstEqual(y, AllOnes))
            {
                operation.TurnIntoCopy(x);
            }
            else if (IsConstEqual(x, 0) || IsConstEqual(y, 0))
            {
                operation.TurnIntoCopy(Const(0));
            }
        }

        private static bool TryEliminateXorSwap(Operation xCopyOp)
        {
            // Try to recognize XOR swap pattern:
            //  x = x ^ y
            //  y = x ^ y
            //  x = x ^ y
            // Or, in SSA:
            //  x2 = x ^ y
            //  y2 = x2 ^ y
            //  x3 = x2 ^ y2
            // Transform it into something more sane:
            //  temp = y
            //  y = x
            //  x = temp

            // Note that because XOR is commutative, there are actually
            // multiple possible combinations of this pattern, for
            // simplicity this only catches one of them.

            Operand x = xCopyOp.GetSource(0);
            Operand y = xCopyOp.GetSource(1);

            if (x.AsgOp is not Operation tCopyOp || tCopyOp.Inst != Instruction.BitwiseExclusiveOr ||
                y.AsgOp is not Operation yCopyOp || yCopyOp.Inst != Instruction.BitwiseExclusiveOr)
            {
                return false;
            }

            if (tCopyOp == yCopyOp)
            {
                return false;
            }

            if (yCopyOp.GetSource(0) != x ||
                yCopyOp.GetSource(1) != tCopyOp.GetSource(1) ||
                x.UseOps.Count != 2)
            {
                return false;
            }

            x = tCopyOp.GetSource(0);
            y = tCopyOp.GetSource(1);

            tCopyOp.TurnIntoCopy(y); // Temp = Y
            yCopyOp.TurnIntoCopy(x); // Y = X
            xCopyOp.TurnIntoCopy(tCopyOp.Dest); // X = Temp

            return true;
        }

        private static void TryEliminateBitwiseOr(Operation operation)
        {
            // Try to recognize and optimize those 3 patterns (in order):
            //  x | 0x00000000 == x,          0x00000000 | y == y,
            //  x | 0xFFFFFFFF == 0xFFFFFFFF, 0xFFFFFFFF | y == 0xFFFFFFFF

            Operand x = operation.GetSource(0);
            Operand y = operation.GetSource(1);

            if (IsConstEqual(x, 0))
            {
                operation.TurnIntoCopy(y);
            }
            else if (IsConstEqual(y, 0))
            {
                operation.TurnIntoCopy(x);
            }
            else if (IsConstEqual(x, AllOnes) || IsConstEqual(y, AllOnes))
            {
                operation.TurnIntoCopy(Const(AllOnes));
            }
        }

        private static bool TryEliminateHalfWordRecombine(Operation operation)
        {
            // nxnative: ((x >> 16) << 16) + (x & 0xFFFF) == x (also with |). It is what the XMAD sequence of x * 1 reduces to once x.lo * 0 folds away: Donkey Kong Bananza's
            // lowered compute kernels (04702b3dc0bc9cde) compute their buffer addresses as c0[ptr] * 1 + c1[0] + k, and the global-to-storage pass could not see c0[ptr] through it

            Operand x = operation.GetSource(0);
            Operand y = operation.GetSource(1);

            for (int pass = 0; pass < 2; pass++)
            {
                if (x.AsgOp is Operation shlOp && shlOp.Inst == Instruction.ShiftLeft && IsConstEqual(shlOp.GetSource(1), 16) &&
                    shlOp.GetSource(0).AsgOp is Operation shrOp && shrOp.Inst == Instruction.ShiftRightU32 && IsConstEqual(shrOp.GetSource(1), 16) &&
                    y.AsgOp is Operation andOp && andOp.Inst == Instruction.BitwiseAnd)
                {
                    Operand value = shrOp.GetSource(0);

                    if ((IsSameValue(andOp.GetSource(0), value) && IsConstEqual(andOp.GetSource(1), 0xFFFF)) ||
                        (IsSameValue(andOp.GetSource(1), value) && IsConstEqual(andOp.GetSource(0), 0xFFFF)))
                    {
                        operation.TurnIntoCopy(value);

                        return true;
                    }
                }

                (x, y) = (y, x);
            }

            return false;
        }

        private static void TryEliminateBinaryOpY(Operation operation, int comparand)
        {
            Operand x = operation.GetSource(0);
            Operand y = operation.GetSource(1);

            if (IsConstEqual(y, comparand))
            {
                operation.TurnIntoCopy(x);
            }
        }

        private static void TryEliminateBinaryOpCommutative(Operation operation, int comparand)
        {
            Operand x = operation.GetSource(0);
            Operand y = operation.GetSource(1);

            if (IsConstEqual(x, comparand))
            {
                operation.TurnIntoCopy(y);
            }
            else if (IsConstEqual(y, comparand))
            {
                operation.TurnIntoCopy(x);
            }
        }

        private static void TryEliminateCompareNotEqual(Operation operation)
        {
            // Comparison instruction returns 0 if the result is false, and -1 if true.
            // Doing a not equal zero comparison on the result is redundant, so we can just copy the first result in this case.

            Operand lhs = operation.GetSource(0);
            Operand rhs = operation.GetSource(1);

            if (lhs.Type == OperandType.Constant)
            {
                (lhs, rhs) = (rhs, lhs);
            }

            if (rhs.Type != OperandType.Constant || rhs.Value != 0)
            {
                return;
            }

            if (lhs.AsgOp is not Operation compareOp || !compareOp.Inst.IsComparison())
            {
                return;
            }

            operation.TurnIntoCopy(lhs);
        }

        private static void TryEliminateConditionalSelect(Operation operation)
        {
            Operand cond = operation.GetSource(0);

            if (cond.Type != OperandType.Constant)
            {
                return;
            }

            // The condition is constant, we can turn it into a copy, and select
            // the source based on the condition value.
            int srcIndex = cond.Value != 0 ? 1 : 2;

            Operand source = operation.GetSource(srcIndex);

            operation.TurnIntoCopy(source);
        }

        private static bool IsSameValue(Operand a, Operand b)
        {
            if (a == b)
            {
                return true;
            }

            return a.Type == OperandType.ConstantBuffer && b.Type == OperandType.ConstantBuffer &&
                   a.GetCbufSlot() == b.GetCbufSlot() && a.GetCbufOffset() == b.GetCbufOffset();
        }

        private static bool IsConstEqual(Operand operand, int comparand)
        {
            if (operand.Type != OperandType.Constant)
            {
                return false;
            }

            return operand.Value == comparand;
        }
    }
}
