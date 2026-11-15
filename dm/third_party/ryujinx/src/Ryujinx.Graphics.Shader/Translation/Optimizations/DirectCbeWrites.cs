using Ryujinx.Graphics.Shader.Instructions;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using System.Collections.Generic;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Optimizations
{
    // nxnative: a hull shader that writes its control points straight into the stage's circular buffer.
    //
    // NVIDIA's compiler can have a tessellation control shader write its per-control-point outputs with global stores (STG.E) into the circular buffer the next stage
    // reads, instead of storing them as attributes (AST). It is the same code in every such shader, around the shader's body:
    //   - the buffer's address comes from system registers 21 and 22 (S2R DirectCbeWriteAddressLow / High), and two words the shader reads from its own outputs
    //     (ISBERD with O at 0x7e and 0x7f): the count N of per-patch words, and the stride S between output words, in words;
    //   - output word k of the control point in lane L (k counts the header's output words, ShaderHeader.OutputWords: position xyzw, then the generic words, in attribute
    //     order) goes to  address + 0x80 * (1 + ceil(N / 4) + p) + 4 * L + 4 * S * k, where p is 1 when the shader writes a primitive ID;
    //   - after the body, the shader copies what it already wrote with AST (the per-patch words: tessellation factors and patch outputs), a header word and the primitive ID into
    //     the buffer, from ISBERD with O.
    // The translator has no such buffer: its outputs are the stage's outputs. So system registers 21 and 22 and ISBERD with O are marker loads (InstEmitMove.EmitSReg,
    // InstEmit.Isberd, only in a hull shader that reads register 21 or 22: FeatureFlags.DirectCbe). This pass answers N with the header's per-patch count and S with
    // StrideWords, so that once the optimizer has folded the address, k is its constant part shifted down by WordShift: 4 * S * k is above everything else in it (the rows,
    // at most 0x80 * 66, and 4 * L < 0x80). Each store to the buffer then becomes:
    //   - nothing, when it stores a value read with ISBERD with O (a copy of an output written with AST, or of the primitive ID, which is the system's);
    //   - otherwise, a store of output word k of the control point gl_InvocationID (the lane term 4 * L must be in the address).
    // An address that does not decode this way (another term in it, a k past the header's output words) is not guessed: the store is dropped with a note.
    static class DirectCbeWrites
    {
        public const int StrideWords = 0x2000;        // the answer to ISBERD with O at 0x7f; 4 * S fits XMAD's 16-bit operand, which the shader multiplies k by
        private const int WordShift = 15;             // log2(4 * StrideWords)
        private const int HeaderCountWord = 0x7e, HeaderStrideWord = 0x7f;
        private const int MaxDepth = 64;

        // First step, before the optimizer folds the addresses: the two header words become constants. True when the shader has any
        public static bool AnswerHeader(BasicBlock[] blocks, ShaderDefinitions definitions)
        {
            bool modified = false;

            foreach (BasicBlock block in blocks)
            {
                foreach (INode node in block.Operations)
                {
                    if (node is Operation operation && IsMarker(operation, IoVariable.IsbeOutput) && operation.SourcesCount == 2 && operation.GetSource(1).Type == OperandType.Constant)
                    {
                        int word = operation.GetSource(1).Value;

                        if (word == HeaderCountWord)
                        {
                            operation.TurnIntoCopy(Const(Math.Max(1, definitions.PerPatchAttributeCount)));
                            modified = true;
                        }
                        else if (word == HeaderStrideWord)
                        {
                            operation.TurnIntoCopy(Const(StrideWords));
                            modified = true;
                        }
                    }
                }
            }

            return modified;
        }

        // Second step, on the folded program: the stores to the buffer become outputs or go, and no marker is left
        public static void RunPass(BasicBlock[] blocks, ShaderDefinitions definitions, IGpuAccessor gpuAccessor)
        {
            int lifted = 0, copies = 0, refused = 0;
            HashSet<int> words = new();

            foreach (BasicBlock block in blocks)
            {
                for (LinkedListNode<INode> node = block.Operations.First; node != null;)
                {
                    LinkedListNode<INode> next = node.Next;

                    if (node.Value is Operation operation &&
                        operation.Inst == Instruction.Store &&
                        IsGlobalMemory(operation.StorageKind) &&
                        operation.SourcesCount == 3 &&
                        Reaches(operation.GetSource(1), IoVariable.DirectCbeAddressHigh, addressOps: true, 0))
                    {
                        Operand value = operation.GetSource(2);

                        if (Reaches(value, IoVariable.IsbeOutput, addressOps: false, 0))
                        {
                            Utils.DeleteNode(node, operation);
                            copies++;
                        }
                        else if (!TryOffset(operation.GetSource(0), 0, out uint offset, out bool hasBase, out bool hasLane) || !hasBase || !hasLane)
                        {
                            gpuAccessor.Log($"Hull shader store to the stage's circular buffer: its address is not the base plus a lane term and constants; dropped.");
                            Utils.DeleteNode(node, operation);
                            refused++;
                        }
                        else if ((int)(offset >> WordShift) >= definitions.OutputWords.Length)
                        {
                            gpuAccessor.Log($"Hull shader store to the stage's circular buffer: output word {offset >> WordShift} past the header's {definitions.OutputWords.Length}; dropped.");
                            Utils.DeleteNode(node, operation);
                            refused++;
                        }
                        else
                        {
                            int k = (int)(offset >> WordShift);
                            Operand invocationId = Local();
                            Operation load = new(Instruction.Load, StorageKind.Input, invocationId, Const((int)IoVariable.InvocationId));
                            Operation store = AttributeMap.CreateOutputStore(definitions, gpuAccessor, definitions.OutputWords[k], invocationId, value, out string reason);

                            if (store == null)
                            {
                                gpuAccessor.Log($"Hull shader store to the stage's circular buffer: output word {k}: {reason}; dropped.");
                                refused++;
                            }
                            else
                            {
                                block.Operations.AddBefore(node, load);
                                block.Operations.AddBefore(node, store);
                                words.Add(k);
                                lifted++;
                            }

                            Utils.DeleteNode(node, operation);
                        }
                    }

                    node = next;
                }
            }

            // What is left of the markers feeds nothing the program keeps (the copies' addresses and loop counts): the base reads as 0, and ISBERD as Ryujinx answers it, its offset
            foreach (BasicBlock block in blocks)
            {
                foreach (INode node in block.Operations)
                {
                    if (node is not Operation operation)
                    {
                        continue;
                    }

                    if (IsMarker(operation, IoVariable.DirectCbeAddressLow) || IsMarker(operation, IoVariable.DirectCbeAddressHigh))
                    {
                        operation.TurnIntoCopy(Const(0));
                    }
                    else if (IsMarker(operation, IoVariable.IsbeOutput))
                    {
                        operation.TurnIntoCopy(operation.SourcesCount == 2 ? operation.GetSource(1) : Const(0));
                    }
                }
            }

            gpuAccessor.Log($"Hull shader writes its control points to the stage's circular buffer: {lifted} stores became outputs ({words.Count} of the header's {definitions.OutputWords.Length} output words), {copies} copies of its outputs dropped{(refused != 0 ? $", {refused} stores not decoded" : "")}.");
        }

        private static bool IsMarker(Operation operation, IoVariable ioVariable)
        {
            return operation.Inst == Instruction.Load &&
                   operation.StorageKind == StorageKind.Input &&
                   operation.SourcesCount >= 1 &&
                   operation.GetSource(0).Type == OperandType.Constant &&
                   operation.GetSource(0).Value == (int)ioVariable;
        }

        private static bool IsGlobalMemory(StorageKind storageKind)
        {
            return storageKind is StorageKind.GlobalMemory or StorageKind.GlobalMemoryS8 or StorageKind.GlobalMemoryS16 or StorageKind.GlobalMemoryU8 or StorageKind.GlobalMemoryU16;
        }

        // Whether the value comes from the marker: through copies and phis, and for an address also through additions (the high word plus its carry) and selects
        private static bool Reaches(Operand operand, IoVariable marker, bool addressOps, int depth)
        {
            if (operand == null || operand.Type != OperandType.LocalVariable || depth > MaxDepth)
            {
                return false;
            }

            if (operand.AsgOp is PhiNode phi)
            {
                for (int index = 0; index < phi.SourcesCount; index++)
                {
                    if (Reaches(phi.GetSource(index), marker, addressOps, depth + 1))
                    {
                        return true;
                    }
                }

                return false;
            }

            if (operand.AsgOp is not Operation operation)
            {
                return false;
            }

            if (IsMarker(operation, marker))
            {
                return true;
            }

            if (operation.Inst == Instruction.Copy || (addressOps && operation.Inst is Instruction.Add or Instruction.ConditionalSelect))
            {
                for (int index = 0; index < operation.SourcesCount; index++)
                {
                    if (Reaches(operation.GetSource(index), marker, addressOps, depth + 1))
                    {
                        return true;
                    }
                }
            }

            return false;
        }

        // The low address as  base + 4 * lane + constant: the constant (wrapping as the shader's 32-bit adds do), and whether the base and the lane term are in it
        private static bool TryOffset(Operand operand, int depth, out uint offset, out bool hasBase, out bool hasLane)
        {
            offset = 0;
            hasBase = false;
            hasLane = false;

            if (operand == null || depth > MaxDepth)
            {
                return false;
            }

            if (operand.Type == OperandType.Constant)
            {
                offset = (uint)operand.Value;
                return true;
            }

            if (operand.Type != OperandType.LocalVariable)
            {
                return false;
            }

            if (operand.AsgOp is PhiNode phi)
            {
                // A predicated add (the row of the primitive ID, p above) joins two values with the same output word
                for (int index = 0; index < phi.SourcesCount; index++)
                {
                    if (!TryOffset(phi.GetSource(index), depth + 1, out uint o, out bool b, out bool l))
                    {
                        return false;
                    }

                    if (index == 0)
                    {
                        (offset, hasBase, hasLane) = (o, b, l);
                    }
                    else if ((o >> WordShift) != (offset >> WordShift) || b != hasBase || l != hasLane)
                    {
                        return false;
                    }
                }

                return phi.SourcesCount != 0;
            }

            if (operand.AsgOp is not Operation operation)
            {
                return false;
            }

            if (IsMarker(operation, IoVariable.DirectCbeAddressLow))
            {
                hasBase = true;
                return true;
            }

            if (IsLaneTimesFour(operation))
            {
                hasLane = true;
                return true;
            }

            if (operation.Inst == Instruction.Copy)
            {
                return TryOffset(operation.GetSource(0), depth + 1, out offset, out hasBase, out hasLane);
            }

            if (operation.Inst == Instruction.Add &&
                TryOffset(operation.GetSource(0), depth + 1, out uint a, out bool baseA, out bool laneA) &&
                TryOffset(operation.GetSource(1), depth + 1, out uint c, out bool baseC, out bool laneC) &&
                !(baseA && baseC) && !(laneA && laneC))
            {
                offset = a + c;
                hasBase = baseA || baseC;
                hasLane = laneA || laneC;
                return true;
            }

            return false;
        }

        // 4 * the subgroup lane (S2R LaneId, possibly masked to 5 bits), as SHL by 2 or a multiplication by 4
        private static bool IsLaneTimesFour(Operation operation)
        {
            if (operation.SourcesCount != 2)
            {
                return false;
            }

            Operand x = operation.GetSource(0), n = operation.GetSource(1);

            if (n.Type != OperandType.Constant || !((operation.Inst == Instruction.ShiftLeft && n.Value == 2) || (operation.Inst == Instruction.Multiply && n.Value == 4)))
            {
                return false;
            }

            if (x.Type == OperandType.LocalVariable && x.AsgOp is Operation lane)
            {
                if (lane.Inst == Instruction.BitwiseAnd && lane.SourcesCount == 2 && lane.GetSource(1).Type == OperandType.Constant && lane.GetSource(1).Value == 0x1f &&
                    lane.GetSource(0).Type == OperandType.LocalVariable && lane.GetSource(0).AsgOp is Operation masked)
                {
                    lane = masked;
                }

                return IsMarker(lane, IoVariable.SubgroupLaneId);
            }

            return false;
        }
    }
}
