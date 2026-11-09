using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using System.Collections.Generic;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Optimizations
{
    static class BindlessToArray
    {
        private const int NvnTextureBufferIndex = 2;
        private const int HardcodedArrayLengthOgl = 4;

        // 1 and 0 elements are not considered arrays anymore.
        public const int MinimumArrayLength = 2;
        private const int NvnSlotArrayLength = 32;      // nxnative: as BindlessElimination's

        public static void RunPassOgl(BasicBlock block, ResourceManager resourceManager)
        {
            // We can turn a bindless texture access into a indexed access,
            // as long the following conditions are true:
            // - The handle is loaded using a LDC instruction.
            // - The handle is loaded from the constant buffer with the handles (CB2 for NVN).
            // - The load has a constant offset.
            // The base offset of the array of handles on the constant buffer is the constant offset.
            for (LinkedListNode<INode> node = block.Operations.First; node != null; node = node.Next)
            {
                if (node.Value is not TextureOperation texOp)
                {
                    continue;
                }

                if ((texOp.Flags & TextureFlags.Bindless) == 0)
                {
                    continue;
                }

                if (texOp.GetSource(0).AsgOp is not Operation handleAsgOp)
                {
                    continue;
                }

                if (handleAsgOp.Inst != Instruction.Load ||
                    handleAsgOp.StorageKind != StorageKind.ConstantBuffer ||
                    handleAsgOp.SourcesCount != 4)
                {
                    continue;
                }

                Operand ldcSrc0 = handleAsgOp.GetSource(0);

                if (ldcSrc0.Type != OperandType.Constant ||
                    !resourceManager.TryGetConstantBufferSlot(ldcSrc0.Value, out int src0CbufSlot) ||
                    src0CbufSlot != NvnTextureBufferIndex)
                {
                    continue;
                }

                Operand ldcSrc1 = handleAsgOp.GetSource(1);

                // We expect field index 0 to be accessed.
                if (ldcSrc1.Type != OperandType.Constant || ldcSrc1.Value != 0)
                {
                    continue;
                }

                Operand ldcSrc2 = handleAsgOp.GetSource(2);

                // FIXME: This is missing some checks, for example, a check to ensure that the shift value is 2.
                // Might be not worth fixing since if that doesn't kick in, the result will be no texture
                // to access anyway which is also wrong.
                // Plus this whole transform is fundamentally flawed as-is since we have no way to know the array size.
                // Eventually, this should be entirely removed in favor of a implementation that supports true bindless
                // texture access.
                if (ldcSrc2.AsgOp is not Operation shrOp || shrOp.Inst != Instruction.ShiftRightU32)
                {
                    continue;
                }

                if (shrOp.GetSource(0).AsgOp is not Operation shrOp2 || shrOp2.Inst != Instruction.ShiftRightU32)
                {
                    continue;
                }

                if (shrOp2.GetSource(0).AsgOp is not Operation addOp || addOp.Inst != Instruction.Add)
                {
                    continue;
                }

                Operand addSrc1 = addOp.GetSource(1);

                if (addSrc1.Type != OperandType.Constant)
                {
                    continue;
                }

                TurnIntoArray(resourceManager, texOp, NvnTextureBufferIndex, addSrc1.Value / 4, HardcodedArrayLengthOgl);

                Operand index = Local();

                Operand source = addOp.GetSource(0);

                Operation shrBy3 = new(Instruction.ShiftRightU32, index, source, Const(3));

                block.Operations.AddBefore(node, shrBy3);

                texOp.SetSource(0, index);
            }
        }

        public static void RunPass(BasicBlock block, ResourceManager resourceManager, IGpuAccessor gpuAccessor)
        {
            // We can turn a bindless texture access into a indexed access,
            // as long the following conditions are true:
            // - The handle is loaded using a LDC instruction.
            // - The handle is loaded from the constant buffer with the handles (CB2 for NVN).
            // - The load has a constant offset.
            // The base offset of the array of handles on the constant buffer is the constant offset.
            for (LinkedListNode<INode> node = block.Operations.First; node != null; node = node.Next)
            {
                if (node.Value is not TextureOperation texOp)
                {
                    continue;
                }

                if ((texOp.Flags & TextureFlags.Bindless) == 0)
                {
                    continue;
                }

                Operand bindlessHandle = Utils.FindLastOperation(texOp.GetSource(0), block);

                if (bindlessHandle.AsgOp is not Operation handleAsgOp)
                {
                    continue;
                }

                int secondaryCbufSlot = 0;
                int secondaryCbufOffset = 0;
                bool hasSecondaryHandle = false;

                if (handleAsgOp.Inst == Instruction.BitwiseOr)
                {
                    Operand src0 = Utils.FindLastOperation(handleAsgOp.GetSource(0), block);
                    Operand src1 = Utils.FindLastOperation(handleAsgOp.GetSource(1), block);

                    if (src0.Type == OperandType.ConstantBuffer && src1.AsgOp is Operation)
                    {
                        handleAsgOp = src1.AsgOp as Operation;
                        secondaryCbufSlot = src0.GetCbufSlot();
                        secondaryCbufOffset = src0.GetCbufOffset();
                        hasSecondaryHandle = true;
                    }
                    else if (src0.AsgOp is Operation && src1.Type == OperandType.ConstantBuffer)
                    {
                        handleAsgOp = src0.AsgOp as Operation;
                        secondaryCbufSlot = src1.GetCbufSlot();
                        secondaryCbufOffset = src1.GetCbufOffset();
                        hasSecondaryHandle = true;
                    }
                }

                if (handleAsgOp.Inst != Instruction.Load ||
                    handleAsgOp.StorageKind != StorageKind.ConstantBuffer ||
                    handleAsgOp.SourcesCount != 4)
                {
                    continue;
                }

                Operand ldcSrc0 = handleAsgOp.GetSource(0);

                if (ldcSrc0.Type != OperandType.Constant ||
                    !resourceManager.TryGetConstantBufferSlot(ldcSrc0.Value, out int src0CbufSlot))
                {
                    continue;
                }

                Operand ldcSrc1 = handleAsgOp.GetSource(1);

                // We expect field index 0 to be accessed.
                if (ldcSrc1.Type != OperandType.Constant || ldcSrc1.Value != 0)
                {
                    continue;
                }

                Operand ldcVecIndex = handleAsgOp.GetSource(2);
                Operand ldcElemIndex = handleAsgOp.GetSource(3);

                if (ldcVecIndex.Type != OperandType.LocalVariable || ldcElemIndex.Type != OperandType.LocalVariable)
                {
                    continue;
                }

                // nxnative: an NVN2 compute shader's handles at a computed index in its own c[0] layout (InstEmit.MapAmpereComputeC0), which the host does not fill: the array
                // moves to the handle table in c[2] the range maps to, the index shifted to match. Fast Fusion's volumetric lighting cded8d0899ba4a6d samples separate texture
                // c[0][0x40 + 8 i] (one of four) with the separate sampler at c[0][0x60]: c[2] 0x168 + 8 i, element 45 + i, with the sampler at c[2] 0x568
                if (src0CbufSlot == 0 && gpuAccessor.QueryAmpereComputeC0Layout() is int[] c0Layout && TryC0HandleBase(ldcVecIndex, out Operand c0Index, out int c0Offset))
                {
                    (int mappedBank, int mappedOffset) = Instructions.InstEmit.MapAmpereComputeC0(c0Layout, c0Offset);

                    if (mappedBank != NvnTextureBufferIndex || (mappedOffset & 7) != 0)
                    {
                        continue;
                    }

                    int c2Length = Math.Max(MinimumArrayLength, gpuAccessor.QueryTextureArrayLengthFromBuffer(NvnTextureBufferIndex));

                    TurnIntoArray(
                        resourceManager,
                        texOp,
                        hasSecondaryHandle ? TextureHandle.PackSlots(NvnTextureBufferIndex, secondaryCbufSlot) : NvnTextureBufferIndex,
                        hasSecondaryHandle ? TextureHandle.PackOffsets(0, secondaryCbufOffset, TextureHandleType.SeparateSamplerHandle) : 0,
                        c2Length);

                    Operand c0Element = Local();
                    Operand c2Element = Local();
                    Operand c2ElementMin = Local();

                    block.Operations.AddBefore(node, new Operation(Instruction.ShiftRightU32, c0Element, c0Index, Const(3)));
                    block.Operations.AddBefore(node, new Operation(Instruction.Add, c2Element, c0Element, Const(mappedOffset / 8)));
                    block.Operations.AddBefore(node, new Operation(Instruction.MinimumU32, c2ElementMin, c2Element, Const(c2Length - 1)));

                    texOp.SetSource(0, c2ElementMin);

                    continue;
                }

                int cbufSlot;
                int handleIndex;

                if (hasSecondaryHandle)
                {
                    cbufSlot = TextureHandle.PackSlots(src0CbufSlot, secondaryCbufSlot);
                    handleIndex = TextureHandle.PackOffsets(0, secondaryCbufOffset, TextureHandleType.SeparateSamplerHandle);
                }
                else
                {
                    cbufSlot = src0CbufSlot;
                    handleIndex = 0;
                }

                int length = Math.Max(MinimumArrayLength, gpuAccessor.QueryTextureArrayLengthFromBuffer(src0CbufSlot));

                // nxnative: the replay binds images and buffer textures element by element, so these stay short; the game's sampled textures take a table the host builds (dmsh_build.py pool_arrays)
                if (src0CbufSlot != NvnTextureBufferIndex && (texOp.Inst.IsImage() || (texOp.Type & SamplerType.Mask) == SamplerType.TextureBuffer))
                {
                    length = Math.Min(length, NvnSlotArrayLength);
                }

                // nxnative: handles at a computed index past a constant base, the base beyond the array's length: every read would clamp to its last element, which is no handle. Fire Emblem Fortune's
                // Weave's character shaders read their cube maps as c[14][8 i + 0x340] (LDC after a shift of 3). The array starts at the base instead (the host fills element k from base + 8k), so
                // the index is that of the handle within the table. A base inside the array keeps the array at 0, as before
                if (!hasSecondaryHandle && TryArrayBase(ldcVecIndex, out Operand tableIndex, out int baseOffset) && baseOffset / 8 >= length)
                {
                    TurnIntoArray(resourceManager, texOp, cbufSlot, baseOffset / 4, length);

                    Operand element = Local();
                    Operand elementMin = Local();

                    block.Operations.AddBefore(node, new Operation(Instruction.ShiftRightU32, element, tableIndex, Const(3)));
                    block.Operations.AddBefore(node, new Operation(Instruction.MinimumU32, elementMin, element, Const(length - 1)));

                    texOp.SetSource(0, elementMin);

                    continue;
                }

                TurnIntoArray(resourceManager, texOp, cbufSlot, handleIndex, length);

                Operand vecIndex = Local();
                Operand elemIndex = Local();
                Operand index = Local();
                Operand indexMin = Local();

                block.Operations.AddBefore(node, new Operation(Instruction.ShiftLeft, vecIndex, ldcVecIndex, Const(1)));
                block.Operations.AddBefore(node, new Operation(Instruction.ShiftRightU32, elemIndex, ldcElemIndex, Const(1)));
                block.Operations.AddBefore(node, new Operation(Instruction.Add, index, vecIndex, elemIndex));
                block.Operations.AddBefore(node, new Operation(Instruction.MinimumU32, indexMin, index, Const(length - 1)));

                texOp.SetSource(0, indexMin);
            }
        }

        // nxnative: the vector index of a constant buffer load whose address is (index + a constant byte offset) >> 2 >> 2: that index and offset
        private static bool TryArrayBase(Operand vecIndex, out Operand index, out int byteOffset)
        {
            index = null;
            byteOffset = 0;

            static bool ShiftBy2(Operand o, out Operand source)
            {
                source = null;

                if (o.AsgOp is Operation op && op.Inst == Instruction.ShiftRightU32 && op.GetSource(1).Type == OperandType.Constant && op.GetSource(1).Value == 2)
                {
                    source = op.GetSource(0);

                    return true;
                }

                return false;
            }

            if (!ShiftBy2(vecIndex, out Operand word))
            {
                return false;
            }

            if (word.AsgOp is Operation plus0 && plus0.Inst == Instruction.Add && plus0.GetSource(1).Type == OperandType.Constant && plus0.GetSource(1).Value == 0)
            {
                word = plus0.GetSource(0);
            }

            if (!ShiftBy2(word, out Operand address) || address.AsgOp is not Operation add || add.Inst != Instruction.Add || add.GetSource(1).Type != OperandType.Constant)
            {
                return false;
            }

            index = add.GetSource(0);
            byteOffset = add.GetSource(1).Value;

            return byteOffset > 0 && (byteOffset & 7) == 0;
        }

        // nxnative: the address of a 32-bit constant buffer load (vector index (address >> 2) >> 2, as EmitLoadConstant makes it, the word's + 0 included) taken apart into
        // a computed part and the constants added to it: (index, byte offset), the offset a multiple of 8 (a handle)
        private static bool TryC0HandleBase(Operand vecIndex, out Operand index, out int byteOffset)
        {
            index = null;
            byteOffset = 0;

            static Operand Shift2(Operand o) =>
                o.AsgOp is Operation op && op.Inst == Instruction.ShiftRightU32 && op.GetSource(1).Type == OperandType.Constant && op.GetSource(1).Value == 2 ? op.GetSource(0) : null;

            Operand word = Shift2(vecIndex);

            if (word?.AsgOp is Operation plus0 && plus0.Inst == Instruction.Add && plus0.GetSource(1).Type == OperandType.Constant && plus0.GetSource(1).Value == 0)
            {
                word = plus0.GetSource(0);
            }

            Operand address = word != null ? Shift2(word) : null;

            if (address == null)
            {
                return false;
            }

            for (int depth = 0; depth < 8 && address.AsgOp is Operation add && add.Inst == Instruction.Add; depth++)
            {
                if (add.GetSource(1).Type == OperandType.Constant)
                {
                    byteOffset += add.GetSource(1).Value;
                    address = add.GetSource(0);
                }
                else if (add.GetSource(0).Type == OperandType.Constant)
                {
                    byteOffset += add.GetSource(0).Value;
                    address = add.GetSource(1);
                }
                else
                {
                    break;
                }
            }

            index = address;

            return byteOffset > 0 && (byteOffset & 7) == 0;
        }

        private static void TurnIntoArray(ResourceManager resourceManager, TextureOperation texOp, int cbufSlot, int handleIndex, int length)
        {
            SetBindingPair setAndBinding = resourceManager.GetTextureOrImageBinding(
                texOp.Inst,
                texOp.Type,
                texOp.Format,
                texOp.Flags & ~TextureFlags.Bindless,
                cbufSlot,
                handleIndex,
                length);

            texOp.TurnIntoArray(setAndBinding);
        }
    }
}
