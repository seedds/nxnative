using Ryujinx.Graphics.Shader.Instructions;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Linq;

namespace Ryujinx.Graphics.Shader.Decoders
{
    // Ampere (sm_86) machine code, as NVN2 (Switch 2) ships it: 128-bit instructions with no scheduling words, uniform registers and uniform predicates,
    // convergence barriers (BSSY / BSYNC) instead of Maxwell's SSY / SYNC stack, CALL / RET with the return offset in a register. The decoder builds the same
    // blocks and functions as the Maxwell one, so everything after decoding is shared: blocks end at BRA, BRX, EXIT, KILL, RET and CALL; a CALL's target is a
    // function and the call returns to the next instruction; convergence barriers only order the warp's threads and are not control flow per thread.
    // A CALL into code that never returns (no RET reachable: the compiler's predicated jump into the shader's EXIT tail) is a jump, decoded as a branch.
    static partial class Decoder
    {
        public const int AmpereInstSize = 16;

        public static DecodedProgram DecodeAmpere(ShaderDefinitions definitions, IGpuAccessor gpuAccessor, ulong startAddress)
        {
            HashSet<ulong> jumps = [];

            while (true)
            {
                DecodedProgram program = DecodeAmpere(definitions, gpuAccessor, startAddress, jumps);
                bool changed = false;

                foreach (DecodedFunction function in program)
                {
                    if (function != program.MainFunction &&
                        !function.Blocks.Any(b => b.OpCodes.Count != 0 && new AmpereOp(b.GetLastOp()).Opcode == AmpereOpcodes.Ret))
                    {
                        changed |= jumps.Add(function.Address);
                    }
                }

                if (!changed)
                {
                    return program;
                }
            }
        }

        // The CALLs that are jumps (their targets never return)
        public static bool IsAmpereJumpCall(DecodedProgram program, InstOp op)
        {
            return program.GetFunctionByAddress(AmpereTarget(op)) == null;
        }

        private static DecodedProgram DecodeAmpere(ShaderDefinitions definitions, IGpuAccessor gpuAccessor, ulong startAddress, HashSet<ulong> jumps)
        {
            Context context = new(gpuAccessor);
            Queue<DecodedFunction> functionsQueue = new();
            Dictionary<ulong, DecodedFunction> functionsVisited = new();

            DecodedFunction EnqueueFunction(ulong address)
            {
                if (!functionsVisited.TryGetValue(address, out DecodedFunction function))
                {
                    functionsVisited.Add(address, function = new DecodedFunction(address));
                    functionsQueue.Enqueue(function);
                }

                return function;
            }

            DecodedFunction mainFunction = EnqueueFunction(0);

            while (functionsQueue.TryDequeue(out DecodedFunction currentFunction))
            {
                List<Block> blocks = [];
                Queue<Block> workQueue = new();
                Dictionary<ulong, Block> visited = new();

                Block GetBlock(ulong blkAddress)
                {
                    if (!visited.TryGetValue(blkAddress, out Block block))
                    {
                        block = new Block(blkAddress);

                        workQueue.Enqueue(block);
                        visited.Add(blkAddress, block);
                    }

                    return block;
                }

                GetBlock(currentFunction.Address);

                bool hasNewTarget;

                do
                {
                    while (workQueue.TryDequeue(out Block currBlock))
                    {
                        if (BinarySearch(blocks, currBlock.Address, out int nBlkIndex))
                        {
                            Block nBlock = blocks[nBlkIndex];

                            if (nBlock.Address == currBlock.Address)
                            {
                                throw new InvalidOperationException("Found duplicate block address on the list.");
                            }

                            nBlock.Split(currBlock);
                            blocks.Insert(nBlkIndex + 1, currBlock);

                            continue;
                        }

                        ulong limitAddress = ulong.MaxValue;

                        if (nBlkIndex != blocks.Count)
                        {
                            Block nBlock = blocks[nBlkIndex];

                            int nextIndex = nBlkIndex + 1;

                            if (nBlock.Address < currBlock.Address && nextIndex < blocks.Count)
                            {
                                limitAddress = blocks[nextIndex].Address;
                            }
                            else if (nBlock.Address > currBlock.Address)
                            {
                                limitAddress = blocks[nBlkIndex].Address;
                            }
                        }

                        FillBlockAmpere(definitions, gpuAccessor, context, currBlock, limitAddress, startAddress);

                        if (currBlock.OpCodes.Count != 0)
                        {
                            InstOp lastOp = currBlock.GetLastOp();
                            AmpereOp last = new(lastOp);

                            bool jumpCall = last.Opcode == AmpereOpcodes.Call && jumps.Contains(AmpereTarget(lastOp));

                            if (last.Opcode == AmpereOpcodes.Call && !jumpCall)
                            {
                                EnqueueFunction(AmpereTarget(lastOp)).AddCaller(currentFunction);
                            }
                            else if (last.Opcode == AmpereOpcodes.Bra || jumpCall)
                            {
                                Block succBlock = GetBlock(AmpereTarget(lastOp));
                                currBlock.Successors.Add(succBlock);
                                succBlock.Predecessors.Add(currBlock);
                            }

                            if (!IsUnconditionalAmpere(last) && !(jumpCall && last.Pred == RegisterConsts.PredicateTrueIndex && !last.PredInv))
                            {
                                Block succBlock = GetBlock(currBlock.EndAddress);
                                currBlock.Successors.Insert(0, succBlock);
                                succBlock.Predecessors.Add(currBlock);
                            }
                        }

                        if (blocks.Count != 0)
                        {
                            Block nBlock = blocks[nBlkIndex];

                            blocks.Insert(nBlkIndex + (nBlock.Address < currBlock.Address ? 1 : 0), currBlock);
                        }
                        else
                        {
                            blocks.Add(currBlock);
                        }
                    }

                    hasNewTarget = FindBrxTargetsAmpere(context, gpuAccessor, startAddress, blocks, GetBlock);
                }
                while (hasNewTarget);

                currentFunction.SetBlocks(blocks.ToArray());
            }

            return new DecodedProgram(
                mainFunction,
                functionsVisited,
                context.AttributeUsage,
                context.UsedFeatures,
                context.ClipDistancesWritten,
                context.Cb1DataSize);
        }

        // The target of BRA / CALL: the next instruction plus the signed offset.
        public static ulong AmpereTarget(InstOp op)
        {
            return (ulong)((long)op.Address + AmpereInstSize + new AmpereOp(op).BranchOffset);
        }

        // Whether execution never continues at the next instruction: an unconditional BRA, BRX, EXIT, KILL or RET. A CALL returns; a guarded or conditional
        // one may not be taken. BRA and EXIT carry a condition predicate (87..90) besides the guard.
        private static bool IsUnconditionalAmpere(AmpereOp op)
        {
            if (op.Opcode == AmpereOpcodes.Call)
            {
                return false;
            }

            if (op.Pred != RegisterConsts.PredicateTrueIndex || op.PredInv)
            {
                return false;
            }

            if (op.Opcode is AmpereOpcodes.Bra or AmpereOpcodes.Exit or AmpereOpcodes.Kill)
            {
                return op.Bits(87, 90) == RegisterConsts.PredicateTrueIndex && !op.Bit(90);
            }

            return true;
        }

        private static void FillBlockAmpere(
            ShaderDefinitions definitions,
            IGpuAccessor gpuAccessor,
            Context context,
            Block block,
            ulong limitAddress,
            ulong startAddress)
        {
            ulong address = block.Address;
            InstOp op;

            do
            {
                if (address + AmpereInstSize - 1 >= limitAddress)
                {
                    break;
                }

                ReadOnlySpan<ulong> words = gpuAccessor.GetCode(startAddress + address, AmpereInstSize);

                if (words.Length < 2)
                {
                    break;
                }

                op = AmpereTable.GetOp(address, words[0], words[1]);
                AmpereOp a = new(op);

                switch (a.Opcode)
                {
                    case AmpereOpcodes.Ipa:
                        SetUserAttributeUses(definitions, context, a.Bits(64, 72) << 2, 1, false, false, false);
                        break;
                    case AmpereOpcodes.IpaIdx:
                        SetUserAttributeUses(definitions, context, 0, 1, false, true, false);
                        break;
                    case AmpereOpcodes.Ald:
                        SetUserAttributeUses(definitions, context, a.Bits(40, 50), a.Bits(74, 76) + 1, a.Bit(79), a.Ra != RegisterConsts.RegisterZeroIndex, a.Bit(76));
                        break;
                    case AmpereOpcodes.Ast:
                        SetUserAttributeUses(definitions, context, a.Bits(40, 50), a.Bits(74, 76) + 1, true, a.Ra != RegisterConsts.RegisterZeroIndex, a.Bit(76));
                        break;
                    case 0x389:
                    case 0x589:
                    case 0x989:
                    case 0xf89:
                        context.SetUsedFeature(FeatureFlags.Shuffle);
                        break;
                    case AmpereOpcodes.Ldl:
                    case 0x387:
                    case 0x987:
                        context.SetUsedFeature(FeatureFlags.LocalMemory);
                        break;
                    case 0x984:
                    case 0x388:
                    case 0x988:
                    case 0x98c:
                    case 0x38c:
                        context.SetUsedFeature(FeatureFlags.SharedMemory);
                        break;
                    case 0x986:
                    case 0x386:
                    case 0x98e:
                    case 0x9a8:
                    case 0x3a8:
                    case 0x9a9:
                    case 0xb9b:
                    case 0x99b:
                    case 0x99d:
                    case 0x9a0:
                    case 0x9a2:
                        context.SetUsedFeature(FeatureFlags.Store);
                        break;
                }

                block.OpCodes.Add(op);

                address += AmpereInstSize;
            }
            while (!op.Props.HasFlag(InstProps.Bra));

            block.EndAddress = address;
        }

        // BRX Ra, imm: to the next instruction + Ra + imm. Ra comes from a jump table in the c1 constant segment, loaded by an LDC c[0x1][Rx + base] before the
        // BRX; the table's entries that land on an instruction inside the program are the targets, up to the first that does not, and up to the next table another
        // BRX of the program reads. The compiler lays a shader's switch tables out back to back in c1 (Donkey Kong Bananza's SilverStarMaterial vertex shaders: tables
        // of 8, 6, ... entries from c1 0x0, each index clamped by IMNMX first), and their entries are all code offsets: read on to the end, the first table took
        // every later switch's targets as its own, 400 and more, whose nested comparisons went past the Metal compiler's 256 nested brackets
        private static bool FindBrxTargetsAmpere(Context context, IGpuAccessor gpuAccessor, ulong startAddress, IEnumerable<Block> blocks, Func<ulong, Block> getBlock)
        {
            bool hasNewTarget = false;
            SortedSet<int> tableBases = [];

            foreach (Block block in blocks)
            {
                AmpereOp op = new(block.GetLastOp());

                if (op.Opcode is AmpereOpcodes.Brx or AmpereOpcodes.Brxu)
                {
                    int b = FindBrxTableAmpere(block, op.Ra, op.Opcode == AmpereOpcodes.Brxu);

                    if (b >= 0)
                    {
                        tableBases.Add(b);
                    }
                }
            }

            foreach (Block block in blocks)
            {
                InstOp lastOp = block.GetLastOp();
                AmpereOp brx = new(lastOp);
                bool hasNext = block.HasNext();

                if (brx.Opcode is not (AmpereOpcodes.Brx or AmpereOpcodes.Brxu) || block.Successors.Count != (hasNext ? 1 : 0))
                {
                    continue;
                }

                int baseOffset = FindBrxTableAmpere(block, brx.Ra, brx.Opcode == AmpereOpcodes.Brxu);

                if (baseOffset < 0)
                {
                    continue;
                }

                HashSet<ulong> seen = [];
                long next = (long)lastOp.Address + AmpereInstSize + brx.BranchOffset;

                int tableEnd = gpuAccessor.QueryConstantBuffer1Size();

                foreach (int b in tableBases)
                {
                    if (b > baseOffset)
                    {
                        tableEnd = Math.Min(tableEnd, b);
                        break;
                    }
                }

                for (int k = 0; baseOffset + k * 4 + 4 <= tableEnd; k++)
                {
                    int value = (int)context.ConstantBuffer1Read(baseOffset + k * 4);
                    long target = next + value;

                    if (target < 0 || (target % AmpereInstSize) != 0 || gpuAccessor.GetCode(startAddress + (ulong)target, AmpereInstSize).Length < 2)
                    {
                        break;
                    }

                    if (seen.Add((ulong)target))
                    {
                        Block t = getBlock((ulong)target);
                        t.Predecessors.Add(block);
                        block.Successors.Add(t);
                        hasNewTarget = true;
                    }
                }
            }

            return hasNewTarget;
        }

        // The c[0x1] byte offset of the LDC that last wrote reg in this block (or a predecessor chain of single blocks), or -1. BRXU's register is a uniform one,
        // loaded by the uniform LDC with a uniform index (ULDC 0xabb); a write to the register of the same number in the other file does not count.
        private static int FindBrxTableAmpere(Block block, int reg, bool uniform)
        {
            HashSet<Block> visitedBlocks = [];

            while (block != null && visitedBlocks.Add(block))
            {
                for (int i = block.OpCodes.Count - 1; i >= 0; i--)
                {
                    AmpereOp op = new(block.OpCodes[i]);

                    if (op.Opcode == (uniform ? 0xabb : AmpereOpcodes.Ldc) && op.Rd == reg)
                    {
                        return op.Bits(54, 59) == 1 && op.Bits(78, 80) == 0 ? (int)op.SignedField(38, 54) : -1;
                    }

                    if (op.Rd == reg && AmpereTable.WritesRd(op.Opcode) && AmpereTable.HasUniformGuard(op.Opcode) == uniform)
                    {
                        return -1;
                    }
                }

                block = block.Predecessors.Count == 1 ? block.Predecessors[0] : null;
            }

            return -1;
        }
    }
}
