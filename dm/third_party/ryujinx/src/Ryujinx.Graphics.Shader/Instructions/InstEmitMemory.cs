using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Numerics;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitHelper;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    static partial class InstEmit
    {
        public static void Atom(EmitterContext context)
        {
            InstAtom op = context.GetOp<InstAtom>();

            int sOffset = (op.Imm20 << 12) >> 12;

            (Operand addrLow, Operand addrHigh) = Get40BitsAddress(context, new Register(op.SrcA, RegisterType.Gpr), op.E, sOffset);

            Operand value = GetSrcReg(context, op.SrcB);

            Operand res = EmitAtomicOp(context, StorageKind.GlobalMemory, op.Op, op.Size, addrLow, addrHigh, value);

            context.Copy(GetDest(op.Dest), res);
        }

        // nxnative: ATOM.CAS Rd, [Ra + imm], Rb, Rb+1 (CUDA atomicCAS): compares the word with Rb, swaps in Rb+1, Rd the old word (whether or not it swapped). The fields, from
        // envytools' gm107.c (its 0xeef0 row: atommem0_imm, tabeef0sz) and nvdisasm's sm_61 records in CuAssembler: a signed byte displacement at 28..47, E at 48, .64 at 49.
        // Every compiled form seen has bits 50..51 (Ryujinx's BcRz) clear and an even Rb. The 64-bit form (Metal has no 64-bit compare-and-swap), set bits 50..51 and an odd
        // or RZ Rb fail translation, as SUATOM's unsupported forms do
        public static void AtomCas(EmitterContext context)
        {
            InstAtomCas op = context.GetOp<InstAtomCas>();

            if (op.Size64 || op.BcRz != 0 || (op.SrcB & 1) != 0)
            {
                throw new NotSupportedException($"ATOM.CAS at 0x{context.CurrOp.Address:x}: only the 32-bit form with bits 50..51 clear and an even Rb ({context.CurrOp.RawOpCode:x16}).");
            }

            int sOffset = (op.Imm20 << 12) >> 12;

            (Operand addrLow, Operand addrHigh) = Get40BitsAddress(context, new Register(op.SrcA, RegisterType.Gpr), op.E, sOffset);

            Operand res = context.AtomicCompareAndSwap(StorageKind.GlobalMemory, addrLow, addrHigh, GetSrcReg(context, op.SrcB), GetSrcReg(context, op.SrcB + 1));

            context.Copy(GetDest(op.Dest), res);
        }

        public static void Atoms(EmitterContext context)
        {
            if (context.TranslatorContext.Definitions.Stage != ShaderStage.Compute)
            {
                context.TranslatorContext.GpuAccessor.Log($"Atoms instruction is not valid on \"{context.TranslatorContext.Definitions.Stage}\" stage.");
                return;
            }

            InstAtoms op = context.GetOp<InstAtoms>();

            Operand offset = context.ShiftRightU32(GetSrcReg(context, op.SrcA), Const(2));

            int sOffset = (op.Imm22 << 10) >> 10;

            offset = context.IAdd(offset, Const(sOffset));

            Operand value = GetSrcReg(context, op.SrcB);

            AtomSize size = op.AtomsSize switch
            {
                AtomsSize.S32 => AtomSize.S32,
                AtomsSize.U64 => AtomSize.U64,
                AtomsSize.S64 => AtomSize.S64,
                _ => AtomSize.U32,
            };

            Operand id = Const(context.ResourceManager.SharedMemoryId);
            Operand res = EmitAtomicOp(context, StorageKind.SharedMemory, op.AtomOp, size, id, offset, value);

            context.Copy(GetDest(op.Dest), res);
        }

        public static void Ldc(EmitterContext context)
        {
            InstLdc op = context.GetOp<InstLdc>();

            if (op.LsSize > LsSize2.B64)
            {
                context.TranslatorContext.GpuAccessor.Log($"Invalid LDC size: {op.LsSize}.");
                return;
            }

            bool isSmallInt = op.LsSize < LsSize2.B32;

            int count = op.LsSize == LsSize2.B64 ? 2 : 1;

            Operand slot = Const(op.CbufSlot);
            Operand srcA = GetSrcReg(context, op.SrcA);

            if (op.AddressMode is AddressMode.Is or AddressMode.Isl)
            {
                slot = context.IAdd(slot, context.BitfieldExtractU32(srcA, Const(16), Const(16)));
                srcA = context.BitwiseAnd(srcA, Const(0xffff));
            }

            // nxnative: added to a register, the immediate is signed and the sum 32 bits, as nvdisasm prints it (`LDC R11, c[0x3][R5+-0x4]` for 0xfffc) and as
            // Xenoblade Chronicles 2's pixel shaders use it (c6 at R - 8 to R - 128); to reach past 32 KiB the compiler adds 0x10000 to the register (Ampere:
            // Yoshi and the Mysterious Book's `IADD3 R40, R26, 0x10000` before c[6][R40 - 0x5578]). Alone (Ra = RZ), the immediate is the address as it is:
            // Mortal Kombat 11 reads c[14][0x8448] that way, which sign-extended became the word 0x3FFFE112 and overflowed into the operand's slot field as c15
            // (Operand.PackCbufInfo)
            int immediate = op.SrcA == RegisterConsts.RegisterZeroIndex ? op.CbufOffset : Imm16ToSInt(op.CbufOffset);
            Operand addr = context.IAdd(srcA, Const(immediate));
            Operand wordOffset = context.ShiftRightU32(addr, Const(2));

            for (int index = 0; index < count; index++)
            {
                Register dest = new(op.Dest + index, RegisterType.Gpr);

                if (dest.IsRZ)
                {
                    break;
                }

                Operand offset = context.IAdd(wordOffset, Const(index));
                Operand value = EmitLoadConstant(context, slot, offset);

                if (isSmallInt)
                {
                    value = ExtractSmallInt(context, (LsSize)op.LsSize, GetBitOffset(context, addr), value);
                }

                context.Copy(Register(dest), value);
            }
        }

        public static void Ldg(EmitterContext context)
        {
            InstLdg op = context.GetOp<InstLdg>();

            EmitLdg(context, op.LsSize, op.SrcA, op.Dest, Imm24ToSInt(op.Imm24), op.E);
        }

        public static void Ldl(EmitterContext context)
        {
            InstLdl op = context.GetOp<InstLdl>();

            EmitLoad(context, StorageKind.LocalMemory, op.LsSize, GetSrcReg(context, op.SrcA), op.Dest, Imm24ToSInt(op.Imm24));
        }

        public static void Lds(EmitterContext context)
        {
            if (context.TranslatorContext.Definitions.Stage != ShaderStage.Compute)
            {
                context.TranslatorContext.GpuAccessor.Log($"Lds instruction is not valid on \"{context.TranslatorContext.Definitions.Stage}\" stage.");
                return;
            }

            InstLds op = context.GetOp<InstLds>();

            EmitLoad(context, StorageKind.SharedMemory, op.LsSize, GetSrcReg(context, op.SrcA), op.Dest, Imm24ToSInt(op.Imm24));
        }

        public static void Red(EmitterContext context)
        {
            InstRed op = context.GetOp<InstRed>();

            // nxnative: the displacement is signed, as ATOM's (envytools gm107.c: RED's address is atommem0_imm too)
            int sOffset = (op.Imm20 << 12) >> 12;

            (Operand addrLow, Operand addrHigh) = Get40BitsAddress(context, new Register(op.SrcA, RegisterType.Gpr), op.E, sOffset);

            EmitAtomicOp(context, StorageKind.GlobalMemory, (AtomOp)op.RedOp, op.RedSize, addrLow, addrHigh, GetDest(op.SrcB));
        }

        public static void Stg(EmitterContext context)
        {
            InstStg op = context.GetOp<InstStg>();

            EmitStg(context, op.LsSize, op.SrcA, op.Dest, Imm24ToSInt(op.Imm24), op.E);
        }

        public static void Stl(EmitterContext context)
        {
            InstStl op = context.GetOp<InstStl>();

            EmitStore(context, StorageKind.LocalMemory, op.LsSize, GetSrcReg(context, op.SrcA), op.Dest, Imm24ToSInt(op.Imm24));
        }

        public static void Sts(EmitterContext context)
        {
            if (context.TranslatorContext.Definitions.Stage != ShaderStage.Compute)
            {
                context.TranslatorContext.GpuAccessor.Log($"Sts instruction is not valid on \"{context.TranslatorContext.Definitions.Stage}\" stage.");
                return;
            }

            InstSts op = context.GetOp<InstSts>();

            EmitStore(context, StorageKind.SharedMemory, op.LsSize, GetSrcReg(context, op.SrcA), op.Dest, Imm24ToSInt(op.Imm24));
        }

        // banks: the banks a run-time slot can be, when the caller knows them (Ampere's LDC .IS, ARuntimeBanks); else the accessor's bound banks
        private static Operand EmitLoadConstant(EmitterContext context, Operand slot, Operand offset, uint? banks = null)
        {
            Operand vecIndex = context.ShiftRightU32(offset, Const(2));
            Operand elemIndex = context.BitwiseAnd(offset, Const(3));

            if (slot.Type == OperandType.Constant)
            {
                int binding = context.ResourceManager.GetConstantBufferBinding(slot.Value);
                return context.Load(StorageKind.ConstantBuffer, binding, Const(0), vecIndex, elemIndex);
            }
            else
            {
                Operand value = Const(0);

                uint cbUseMask = banks ?? context.TranslatorContext.GpuAccessor.QueryConstantBufferUse();

                while (cbUseMask != 0)
                {
                    int cbIndex = BitOperations.TrailingZeroCount(cbUseMask);
                    int binding = context.ResourceManager.GetConstantBufferBinding(cbIndex);

                    Operand isCurrent = context.ICompareEqual(slot, Const(cbIndex));
                    Operand currentValue = context.Load(StorageKind.ConstantBuffer, binding, Const(0), vecIndex, elemIndex);

                    value = context.ConditionalSelect(isCurrent, currentValue, value);

                    cbUseMask &= ~(1u << cbIndex);
                }

                return value;
            }
        }

        // nxnative: a float atomic add (RED.F32.FTZ.RN and ATOM's float form), as a compare-and-swap loop on the word: neither SPIR-V as the chain uses it nor Metal's texture atomics have one. It was
        // dropped with a log line, so agl's scene analysis summed nothing: Ocarina of Time's probe_analyze reads the probes' luminance into a buffer the CPU turns into exposure, and at 0 the game took
        // its full correction, several times too bright. Returns the value before the add
        private static Operand EmitAtomicAddF32(EmitterContext context, StorageKind storageKind, Operand e0, Operand e1, Operand value)
        {
            Operand lblRetry = Label();
            context.MarkLabel(lblRetry);

            Operand expected = context.Load(storageKind, e0, e1);
            Operand sum = context.FPAdd(expected, value);
            Operand found = context.AtomicCompareAndSwap(storageKind, e0, e1, expected, sum);

            context.BranchIfTrue(lblRetry, context.ICompareNotEqual(found, expected));

            return found;
        }

        private static Operand EmitAtomicOp(
            EmitterContext context,
            StorageKind storageKind,
            AtomOp op,
            AtomSize type,
            Operand e0,
            Operand e1,
            Operand value)
        {
            Operand res = Const(0);

            switch (op)
            {
                case AtomOp.Add:
                    if (type is AtomSize.S32 or AtomSize.U32)
                    {
                        res = context.AtomicAdd(storageKind, e0, e1, value);
                    }
                    else if (type == AtomSize.F32FtzRn)
                    {
                        res = EmitAtomicAddF32(context, storageKind, e0, e1, value);
                    }
                    else
                    {
                        context.TranslatorContext.GpuAccessor.Log($"Invalid reduction type: {type}.");
                    }

                    break;
                case AtomOp.Min:
                    if (type == AtomSize.S32)
                    {
                        res = context.AtomicMinS32(storageKind, e0, e1, value);
                    }
                    else if (type == AtomSize.U32)
                    {
                        res = context.AtomicMinU32(storageKind, e0, e1, value);
                    }
                    else
                    {
                        context.TranslatorContext.GpuAccessor.Log($"Invalid reduction type: {type}.");
                    }

                    break;
                case AtomOp.Max:
                    if (type == AtomSize.S32)
                    {
                        res = context.AtomicMaxS32(storageKind, e0, e1, value);
                    }
                    else if (type == AtomSize.U32)
                    {
                        res = context.AtomicMaxU32(storageKind, e0, e1, value);
                    }
                    else
                    {
                        context.TranslatorContext.GpuAccessor.Log($"Invalid reduction type: {type}.");
                    }

                    break;
                case AtomOp.And:
                    if (type is AtomSize.S32 or AtomSize.U32)
                    {
                        res = context.AtomicAnd(storageKind, e0, e1, value);
                    }
                    else
                    {
                        context.TranslatorContext.GpuAccessor.Log($"Invalid reduction type: {type}.");
                    }

                    break;
                case AtomOp.Or:
                    if (type is AtomSize.S32 or AtomSize.U32)
                    {
                        res = context.AtomicOr(storageKind, e0, e1, value);
                    }
                    else
                    {
                        context.TranslatorContext.GpuAccessor.Log($"Invalid reduction type: {type}.");
                    }

                    break;
                case AtomOp.Xor:
                    if (type is AtomSize.S32 or AtomSize.U32)
                    {
                        res = context.AtomicXor(storageKind, e0, e1, value);
                    }
                    else
                    {
                        context.TranslatorContext.GpuAccessor.Log($"Invalid reduction type: {type}.");
                    }

                    break;
                case AtomOp.Exch:
                    if (type is AtomSize.S32 or AtomSize.U32)
                    {
                        res = context.AtomicSwap(storageKind, e0, e1, value);
                    }
                    else
                    {
                        context.TranslatorContext.GpuAccessor.Log($"Invalid reduction type: {type}.");
                    }

                    break;
                default:
                    context.TranslatorContext.GpuAccessor.Log($"Invalid atomic operation: {op}.");
                    break;
            }

            return res;
        }

        private static void EmitLoad(
            EmitterContext context,
            StorageKind storageKind,
            LsSize2 size,
            Operand srcA,
            int rd,
            int offset,
            int memoryId = -1)
        {
            if (size > LsSize2.B128)
            {
                context.TranslatorContext.GpuAccessor.Log($"Invalid load size: {size}.");
                return;
            }

            int id = memoryId >= 0
                ? memoryId
                : storageKind == StorageKind.LocalMemory
                    ? context.ResourceManager.LocalMemoryId
                    : context.ResourceManager.SharedMemoryId;
            bool isSmallInt = size < LsSize2.B32;

            int count = size switch
            {
                LsSize2.B64 => 2,
                LsSize2.B128 => 4,
                _ => 1,
            };

            Operand baseOffset = context.Copy(srcA);

            for (int index = 0; index < count; index++)
            {
                Register dest = new(rd + index, RegisterType.Gpr);

                if (dest.IsRZ)
                {
                    break;
                }

                Operand byteOffset = context.IAdd(baseOffset, Const(offset + index * 4));
                Operand wordOffset = context.ShiftRightU32(byteOffset, Const(2)); // Word offset = byte offset / 4 (one word = 4 bytes).
                Operand bitOffset = GetBitOffset(context, byteOffset);
                Operand value = context.Load(storageKind, id, wordOffset);

                if (isSmallInt)
                {
                    value = ExtractSmallInt(context, (LsSize)size, bitOffset, value);
                }

                context.Copy(Register(dest), value);
            }
        }

        private static void EmitLdg(
            EmitterContext context,
            LsSize size,
            int ra,
            int rd,
            int offset,
            bool extended)
        {
            int count = GetVectorCount(size);
            StorageKind storageKind = GetStorageKind(size);

            (_, Operand addrHigh) = Get40BitsAddress(context, new Register(ra, RegisterType.Gpr), extended, offset);

            Operand srcA = context.Copy(new Operand(new Register(ra, RegisterType.Gpr)));

            // nxnative: the words of one LDG.64 / LDG.128 are marked as one access (Operation.GlobalVectorCount, as in InstEmitAmpereMem.ALdg)
            bool vector = count > 1 && storageKind == StorageKind.GlobalMemory && rd + count <= RegisterConsts.RegisterZeroIndex;
            int group = vector ? ++context.GlobalVectorGroups : 0;

            for (int index = 0; index < count; index++)
            {
                Register dest = new(rd + index, RegisterType.Gpr);

                if (dest.IsRZ)
                {
                    break;
                }

                Operand value = context.Load(storageKind, context.IAdd(srcA, Const(offset + index * 4)), addrHigh);

                if (vector && value.AsgOp is Operation load)
                {
                    load.GlobalVectorCount = count;
                    load.GlobalVectorWord = index;
                    load.GlobalVectorGroup = group;
                }

                context.Copy(Register(dest), value);
            }
        }

        private static void EmitStore(
            EmitterContext context,
            StorageKind storageKind,
            LsSize2 size,
            Operand srcA,
            int rd,
            int offset)
        {
            if (size > LsSize2.B128)
            {
                context.TranslatorContext.GpuAccessor.Log($"Invalid store size: {size}.");
                return;
            }

            int id = storageKind == StorageKind.LocalMemory
                ? context.ResourceManager.LocalMemoryId
                : context.ResourceManager.SharedMemoryId;
            bool isSmallInt = size < LsSize2.B32;

            int count = size switch
            {
                LsSize2.B64 => 2,
                LsSize2.B128 => 4,
                _ => 1,
            };

            Operand baseOffset = context.Copy(srcA);

            for (int index = 0; index < count; index++)
            {
                bool isRz = rd + index >= RegisterConsts.RegisterZeroIndex;

                Operand value = Register(isRz ? rd : rd + index, RegisterType.Gpr);
                Operand byteOffset = context.IAdd(baseOffset, Const(offset + index * 4));
                Operand wordOffset = context.ShiftRightU32(byteOffset, Const(2));
                Operand bitOffset = GetBitOffset(context, byteOffset);

                if (isSmallInt && storageKind == StorageKind.LocalMemory)
                {
                    Operand word = context.Load(storageKind, id, wordOffset);

                    value = InsertSmallInt(context, (LsSize)size, bitOffset, word, value);
                }

                if (storageKind == StorageKind.LocalMemory)
                {
                    context.Store(storageKind, id, wordOffset, value);
                }
                else if (storageKind == StorageKind.SharedMemory)
                {
                    switch (size)
                    {
                        case LsSize2.U8:
                        case LsSize2.S8:
                            context.Store(StorageKind.SharedMemory8, id, byteOffset, value);
                            break;
                        case LsSize2.U16:
                        case LsSize2.S16:
                            context.Store(StorageKind.SharedMemory16, id, byteOffset, value);
                            break;
                        default:
                            context.Store(storageKind, id, wordOffset, value);
                            break;
                    }
                }
            }
        }

        private static void EmitStg(
            EmitterContext context,
            LsSize2 size,
            int ra,
            int rd,
            int offset,
            bool extended)
        {
            if (size > LsSize2.B128)
            {
                context.TranslatorContext.GpuAccessor.Log($"Invalid store size: {size}.");
                return;
            }

            int count = GetVectorCount((LsSize)size);
            StorageKind storageKind = GetStorageKind((LsSize)size);

            (_, Operand addrHigh) = Get40BitsAddress(context, new Register(ra, RegisterType.Gpr), extended, offset);

            Operand srcA = context.Copy(new Operand(new Register(ra, RegisterType.Gpr)));

            for (int index = 0; index < count; index++)
            {
                bool isRz = rd + index >= RegisterConsts.RegisterZeroIndex;

                Operand value = Register(isRz ? rd : rd + index, RegisterType.Gpr);

                Operand addrLowOffset = context.IAdd(srcA, Const(offset + index * 4));

                context.Store(storageKind, addrLowOffset, addrHigh, value);
            }
        }

        private static StorageKind GetStorageKind(LsSize size)
        {
            return size switch
            {
                LsSize.U8 => StorageKind.GlobalMemoryU8,
                LsSize.S8 => StorageKind.GlobalMemoryS8,
                LsSize.U16 => StorageKind.GlobalMemoryU16,
                LsSize.S16 => StorageKind.GlobalMemoryS16,
                _ => StorageKind.GlobalMemory,
            };
        }

        private static int GetVectorCount(LsSize size)
        {
            return size switch
            {
                LsSize.B64 => 2,
                LsSize.B128 or LsSize.UB128 => 4,
                _ => 1,
            };
        }

        private static (Operand, Operand) Get40BitsAddress(
            EmitterContext context,
            Register ra,
            bool extended,
            int offset)
        {
            Operand addrLow = Register(ra);
            Operand addrHigh;

            if (extended && !ra.IsRZ)
            {
                addrHigh = Register(ra.Index + 1, RegisterType.Gpr);
            }
            else
            {
                addrHigh = Const(0);
            }

            Operand offs = Const(offset);

            addrLow = context.IAdd(addrLow, offs);

            if (extended)
            {
                Operand carry = context.ICompareLessUnsigned(addrLow, offs);

                // nxnative: the displacement is signed (LDG / STG 24 bits, ATOM / RED 20), so a negative one adds -1 to the high word as well as the carry out of the low word.
                // Adding the carry alone moved every access with a negative displacement 4 GB up (the low word's add carries unless it borrows), out of the storage buffer's range
                addrHigh = context.IAdd(addrHigh, context.ConditionalSelect(carry, Const(offset < 0 ? 0 : 1), Const(offset < 0 ? -1 : 0)));
            }

            return (addrLow, addrHigh);
        }

        private static Operand GetBitOffset(EmitterContext context, Operand baseOffset)
        {
            // Note: bit offset = (baseOffset & 0b11) * 8.
            // Addresses should be always aligned to the integer type,
            // so we don't need to take unaligned addresses into account.
            return context.ShiftLeft(context.BitwiseAnd(baseOffset, Const(3)), Const(3));
        }

        private static Operand ExtractSmallInt(
            EmitterContext context,
            LsSize size,
            Operand bitOffset,
            Operand value)
        {
            value = context.ShiftRightU32(value, bitOffset);

            switch (size)
            {
                case LsSize.U8:
                    value = ZeroExtendTo32(context, value, 8);
                    break;
                case LsSize.U16:
                    value = ZeroExtendTo32(context, value, 16);
                    break;
                case LsSize.S8:
                    value = SignExtendTo32(context, value, 8);
                    break;
                case LsSize.S16:
                    value = SignExtendTo32(context, value, 16);
                    break;
            }

            return value;
        }

        private static Operand InsertSmallInt(
            EmitterContext context,
            LsSize size,
            Operand bitOffset,
            Operand word,
            Operand value)
        {
            switch (size)
            {
                case LsSize.U8:
                case LsSize.S8:
                    value = context.BitwiseAnd(value, Const(0xff));
                    value = context.BitfieldInsert(word, value, bitOffset, Const(8));
                    break;

                case LsSize.U16:
                case LsSize.S16:
                    value = context.BitwiseAnd(value, Const(0xffff));
                    value = context.BitfieldInsert(word, value, bitOffset, Const(16));
                    break;
            }

            return value;
        }
    }
}
