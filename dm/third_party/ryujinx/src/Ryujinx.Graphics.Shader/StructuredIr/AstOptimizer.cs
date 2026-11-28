using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System.Collections.Generic;
using System.Linq;

using static Ryujinx.Graphics.Shader.StructuredIr.AstHelper;

namespace Ryujinx.Graphics.Shader.StructuredIr
{
    static class AstOptimizer
    {
        public static void Optimize(StructuredProgramContext context, bool flattenElseIf = true)
        {
            AstBlock mainBlock = context.CurrentFunction.MainBlock;

            // When debug mode is enabled, we disable expression propagation
            // (this makes comparison with the disassembly easier).
            if (!context.DebugMode)
            {
                AstBlockVisitor visitor = new(mainBlock);

                foreach (IAstNode node in visitor.Visit())
                {
                    if (node is AstAssignment assignment && assignment.Destination is AstOperand propVar && !propVar.AddressTaken)
                    {
                        bool isWorthPropagating = propVar.Uses.Count == 1 || IsWorthPropagating(assignment.Source);

                        if (propVar.Defs.Count == 1 && isWorthPropagating && CanPropagateExpression(assignment.Source))
                        {
                            PropagateExpression(propVar, assignment.Source);
                        }

                        if (propVar.Type == OperandType.LocalVariable && propVar.Uses.Count == 0 && !HasSideEffects(assignment.Source))
                        {
                            visitor.Block.Remove(assignment);

                            context.CurrentFunction.Locals.Remove(propVar);
                        }
                    }
                }
            }

            RemoveEmptyBlocks(mainBlock, flattenElseIf);
        }

        // Propagation changes evaluation time. Restrict it to value-only trees
        // whose leaves cannot be mutated: memory loads must retain their snapshot,
        // calls/atomics their order, and by-reference arguments their storage.
        // Unknown operations conservatively retain an explicit assignment.
        private static bool CanPropagateExpression(IAstNode source)
        {
            if (source is AstOperand operand)
            {
                return operand.Type is OperandType.Constant or OperandType.Undefined ||
                    operand.Type == OperandType.LocalVariable && !operand.AddressTaken && operand.Defs.Count == 1;
            }

            if (source is not AstOperation operation)
            {
                return false;
            }

            switch (operation.Inst & Instruction.Mask)
            {
                case Instruction.Absolute:
                case Instruction.Add:
                case Instruction.BitCount:
                case Instruction.BitfieldExtractS32:
                case Instruction.BitfieldExtractU32:
                case Instruction.BitfieldInsert:
                case Instruction.BitfieldReverse:
                case Instruction.BitwiseAnd:
                case Instruction.BitwiseExclusiveOr:
                case Instruction.BitwiseNot:
                case Instruction.BitwiseOr:
                case Instruction.Ceiling:
                case Instruction.Clamp:
                case Instruction.ClampU32:
                case Instruction.CompareEqual:
                case Instruction.CompareGreater:
                case Instruction.CompareGreaterOrEqual:
                case Instruction.CompareGreaterOrEqualU32:
                case Instruction.CompareGreaterU32:
                case Instruction.CompareLess:
                case Instruction.CompareLessOrEqual:
                case Instruction.CompareLessOrEqualU32:
                case Instruction.CompareLessU32:
                case Instruction.CompareNotEqual:
                case Instruction.ConditionalSelect:
                case Instruction.ConvertFP32ToFP64:
                case Instruction.ConvertFP64ToFP32:
                case Instruction.ConvertFP32ToS32:
                case Instruction.ConvertFP32ToU32:
                case Instruction.ConvertFP64ToS32:
                case Instruction.ConvertFP64ToU32:
                case Instruction.ConvertS32ToFP32:
                case Instruction.ConvertS32ToFP64:
                case Instruction.ConvertU32ToFP32:
                case Instruction.ConvertU32ToFP64:
                case Instruction.Copy:
                case Instruction.Cosine:
                case Instruction.Divide:
                case Instruction.ExponentB2:
                case Instruction.FindLSB:
                case Instruction.FindMSBS32:
                case Instruction.FindMSBU32:
                case Instruction.Floor:
                case Instruction.FusedMultiplyAdd:
                case Instruction.IsNan:
                case Instruction.LogarithmB2:
                case Instruction.LogicalAnd:
                case Instruction.LogicalExclusiveOr:
                case Instruction.LogicalNot:
                case Instruction.LogicalOr:
                case Instruction.Maximum:
                case Instruction.MaximumU32:
                case Instruction.Minimum:
                case Instruction.MinimumU32:
                case Instruction.Modulo:
                case Instruction.Multiply:
                case Instruction.MultiplyHighS32:
                case Instruction.MultiplyHighU32:
                case Instruction.Negate:
                case Instruction.PackDouble2x32:
                case Instruction.PackHalf2x16:
                case Instruction.ReciprocalSquareRoot:
                case Instruction.Round:
                case Instruction.ShiftLeft:
                case Instruction.ShiftRightS32:
                case Instruction.ShiftRightU32:
                case Instruction.Sine:
                case Instruction.SquareRoot:
                case Instruction.Subtract:
                case Instruction.Truncate:
                case Instruction.UnpackDouble2x32:
                case Instruction.UnpackHalf2x16:
                case Instruction.VectorExtract:
                    break;
                default:
                    return false;
            }

            for (int index = 0; index < operation.SourcesCount; index++)
            {
                if (!CanPropagateExpression(operation.GetSource(index)))
                {
                    return false;
                }
            }

            return true;
        }

        private static bool HasSideEffects(IAstNode source)
        {
            if (source is not AstOperation operation)
            {
                return false;
            }

            Instruction inst = operation.Inst & Instruction.Mask;
            if (inst.IsAtomic() || inst is Instruction.Call or Instruction.ImageAtomic or Instruction.ImageStore or Instruction.Store)
            {
                return true;
            }

            for (int index = 0; index < operation.SourcesCount; index++)
            {
                if (HasSideEffects(operation.GetSource(index)))
                {
                    return true;
                }
            }

            return false;
        }

        private static bool IsWorthPropagating(IAstNode source)
        {
            if (source is not AstOperation srcOp)
            {
                return false;
            }

            if (!InstructionInfo.IsUnary(srcOp.Inst))
            {
                return false;
            }

            return srcOp.GetSource(0) is AstOperand || srcOp.Inst == Instruction.Copy;
        }

        private static void PropagateExpression(AstOperand propVar, IAstNode source)
        {
            IAstNode[] uses = propVar.Uses.ToArray();

            foreach (IAstNode useNode in uses)
            {
                if (useNode is AstBlock useBlock)
                {
                    useBlock.Condition = source;
                }
                else if (useNode is AstOperation useOperation)
                {
                    for (int srcIndex = 0; srcIndex < useOperation.SourcesCount; srcIndex++)
                    {
                        if (useOperation.GetSource(srcIndex) == propVar)
                        {
                            useOperation.SetSource(srcIndex, source);
                        }
                    }
                }
                else if (useNode is AstAssignment useAssignment)
                {
                    useAssignment.Source = source;
                }
            }
        }

        private static void RemoveEmptyBlocks(AstBlock mainBlock, bool flattenElseIf)
        {
            Queue<AstBlock> pending = new();

            pending.Enqueue(mainBlock);

            while (pending.TryDequeue(out AstBlock block))
            {
                foreach (IAstNode node in block)
                {
                    if (node is AstBlock childBlock)
                    {
                        pending.Enqueue(childBlock);
                    }
                }

                AstBlock parent = block.Parent;

                if (parent == null)
                {
                    continue;
                }

                AstBlock nextBlock = Next(block) as AstBlock;

                bool hasElse = nextBlock != null && nextBlock.Type == AstBlockType.Else;

                bool isIf = block.Type == AstBlockType.If;

                if (block.Count == 0)
                {
                    if (isIf)
                    {
                        if (hasElse)
                        {
                            nextBlock.TurnIntoIf(InverseCond(block.Condition));
                        }

                        parent.Remove(block);
                    }
                    else if (block.Type == AstBlockType.Else)
                    {
                        parent.Remove(block);
                    }
                }
                else if (flattenElseIf && isIf && parent.Type == AstBlockType.Else && parent.Count == (hasElse ? 2 : 1))
                {
                    AstBlock parentOfParent = parent.Parent;

                    parent.Remove(block);

                    parentOfParent.AddAfter(parent, block);

                    if (hasElse)
                    {
                        parent.Remove(nextBlock);

                        parentOfParent.AddAfter(block, nextBlock);
                    }

                    parentOfParent.Remove(parent);

                    block.TurnIntoElseIf();
                }
            }
        }
    }
}
