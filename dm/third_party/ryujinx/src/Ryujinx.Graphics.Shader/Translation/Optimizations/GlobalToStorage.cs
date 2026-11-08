using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Optimizations
{
    static class GlobalToStorage
    {
        private const int DriverReservedCb = 0;

        enum LsMemoryType
        {
            Local,
            Shared,
        }

        private class GtsContext
        {
            private readonly struct Entry
            {
                public readonly int FunctionId;
                public readonly Instruction Inst;
                public readonly StorageKind StorageKind;
                public readonly bool IsMultiTarget;
                public readonly IReadOnlyList<uint> TargetCbs;

                public Entry(
                    int functionId,
                    Instruction inst,
                    StorageKind storageKind,
                    bool isMultiTarget,
                    IReadOnlyList<uint> targetCbs)
                {
                    FunctionId = functionId;
                    Inst = inst;
                    StorageKind = storageKind;
                    IsMultiTarget = isMultiTarget;
                    TargetCbs = targetCbs;
                }
            }

            private readonly struct LsKey : IEquatable<LsKey>
            {
                public readonly Operand BaseOffset;
                public readonly int ConstOffset;
                public readonly LsMemoryType Type;

                public LsKey(Operand baseOffset, int constOffset, LsMemoryType type)
                {
                    BaseOffset = baseOffset;
                    ConstOffset = constOffset;
                    Type = type;
                }

                public override int GetHashCode()
                {
                    return HashCode.Combine(BaseOffset, ConstOffset, Type);
                }

                public override bool Equals(object obj)
                {
                    return obj is LsKey other && Equals(other);
                }

                public bool Equals(LsKey other)
                {
                    return other.BaseOffset == BaseOffset && other.ConstOffset == ConstOffset && other.Type == Type;
                }
            }

            private readonly List<Entry> _entries;
            private readonly Dictionary<LsKey, Dictionary<uint, SearchResult>> _sharedEntries;
            private readonly HelperFunctionManager _hfm;

            // nxnative: the raw global memory load helpers (TryGenerateRawStorageOp) by function id, their variants that take a pool resolved once for a run of loads
            // (ResolveRawRuns), and the helper that resolves a run's range
            public readonly Dictionary<int, StorageKind> RawLoadFunctions = new();
            public readonly Dictionary<StorageKind, int> ResolvedLoadFunctions = new();
            public int ResolveRangeFunction = -1;

            public int AddHelper(Function function)
            {
                return _hfm.AddFunction(function);
            }

            public GtsContext(HelperFunctionManager hfm)
            {
                _entries = [];
                _sharedEntries = new Dictionary<LsKey, Dictionary<uint, SearchResult>>();
                _hfm = hfm;
            }

            public int AddFunction(Operation baseOp, bool isMultiTarget, IReadOnlyList<uint> targetCbs, Function function)
            {
                int functionId = _hfm.AddFunction(function);

                _entries.Add(new Entry(functionId, baseOp.Inst, baseOp.StorageKind, isMultiTarget, targetCbs));

                return functionId;
            }

            public bool TryGetFunctionId(Operation baseOp, bool isMultiTarget, List<uint> targetCbs, out int functionId)
            {
                foreach (Entry entry in _entries)
                {
                    if (entry.Inst != baseOp.Inst ||
                        entry.StorageKind != baseOp.StorageKind ||
                        entry.IsMultiTarget != isMultiTarget ||
                        entry.TargetCbs.Count != targetCbs.Count)
                    {
                        continue;
                    }

                    bool allEqual = true;

                    for (int index = 0; index < targetCbs.Count; index++)
                    {
                        if (targetCbs[index] != entry.TargetCbs[index])
                        {
                            allEqual = false;
                            break;
                        }
                    }

                    if (allEqual)
                    {
                        functionId = entry.FunctionId;
                        return true;
                    }
                }

                functionId = -1;
                return false;
            }

            public void AddMemoryTargetCb(LsMemoryType type, Operand baseOffset, int constOffset, uint targetCb, SearchResult result)
            {
                LsKey key = new(baseOffset, constOffset, type);

                if (!_sharedEntries.TryGetValue(key, out Dictionary<uint, SearchResult> targetCbs))
                {
                    // No entry with this base offset, create a new one.

                    targetCbs = new Dictionary<uint, SearchResult>() { { targetCb, result } };

                    _sharedEntries.Add(key, targetCbs);
                }
                else if (targetCbs.TryGetValue(targetCb, out SearchResult existingResult))
                {
                    // If our entry already exists, but does not match the new result,
                    // we set the offset to null to indicate there are multiple possible offsets.
                    // This will be used on the multi-target access that does not need to know the offset.

                    if (existingResult.Offset != null &&
                        (existingResult.Offset != result.Offset ||
                        existingResult.ConstOffset != result.ConstOffset))
                    {
                        targetCbs[targetCb] = new SearchResult(result.SbCbSlot, result.SbCbOffset);
                    }
                }
                else
                {
                    // An entry for this base offset already exists, but not for the specified
                    // constant buffer region where the storage buffer base address and size
                    // comes from.

                    targetCbs.Add(targetCb, result);
                }
            }

            public bool TryGetMemoryTargetCb(LsMemoryType type, Operand baseOffset, int constOffset, out SearchResult result)
            {
                LsKey key = new(baseOffset, constOffset, type);

                if (_sharedEntries.TryGetValue(key, out Dictionary<uint, SearchResult> targetCbs) && targetCbs.Count == 1)
                {
                    SearchResult candidateResult = targetCbs.Values.First();

                    if (candidateResult.Found)
                    {
                        result = candidateResult;

                        return true;
                    }
                }

                result = default;

                return false;
            }
        }

        private readonly struct SearchResult
        {
            public static SearchResult NotFound => new(-1, 0);
            public bool Found => SbCbSlot != -1;
            public int SbCbSlot { get; }
            public int SbCbOffset { get; }
            public Operand Offset { get; }
            public int ConstOffset { get; }

            public SearchResult(int sbCbSlot, int sbCbOffset)
            {
                SbCbSlot = sbCbSlot;
                SbCbOffset = sbCbOffset;
            }

            public SearchResult(int sbCbSlot, int sbCbOffset, Operand offset, int constOffset = 0)
            {
                SbCbSlot = sbCbSlot;
                SbCbOffset = sbCbOffset;
                Offset = offset;
                ConstOffset = constOffset;
            }
        }

        public static void RunPass(
            HelperFunctionManager hfm,
            BasicBlock[] blocks,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            TargetLanguage targetLanguage)
        {
            GtsContext gtsContext = new(hfm);

            foreach (BasicBlock block in blocks)
            {
                for (LinkedListNode<INode> node = block.Operations.First; node != null; node = node.Next)
                {
                    if (node.Value is not Operation operation)
                    {
                        continue;
                    }

                    if (IsGlobalMemory(operation.StorageKind))
                    {
                        LinkedListNode<INode> nextNode = ReplaceGlobalMemoryWithStorage(
                            gtsContext,
                            resourceManager,
                            gpuAccessor,
                            targetLanguage,
                            block,
                            node);

                        if (nextNode == null)
                        {
                            // The returned value being null means that the global memory replacement failed,
                            // so we just make loads read 0 and stores do nothing.

                            gpuAccessor.Log($"Failed to reserve storage buffer for global memory operation \"{operation.Inst}\".");

                            if (operation.Dest != null)
                            {
                                operation.TurnIntoCopy(Const(0));
                            }
                            else
                            {
                                Utils.DeleteNode(node, operation);
                            }
                        }
                        else
                        {
                            node = nextNode;
                        }
                    }
                    else if (operation.Inst == Instruction.Store &&
                        (operation.StorageKind == StorageKind.SharedMemory ||
                        operation.StorageKind == StorageKind.LocalMemory))
                    {
                        // The NVIDIA compiler can sometimes use shared or local memory as temporary
                        // storage to place the base address and size on, so we need
                        // to be able to find such information stored in memory too.

                        if (TryGetMemoryOffsets(operation, out LsMemoryType type, out Operand baseOffset, out int constOffset))
                        {
                            Operand value = operation.GetSource(operation.SourcesCount - 1);

                            SearchResult result = FindUniqueBaseAddressCb(gtsContext, block, value, needsOffset: false);
                            if (result.Found)
                            {
                                uint targetCb = PackCbSlotAndOffset(result.SbCbSlot, result.SbCbOffset);
                                gtsContext.AddMemoryTargetCb(type, baseOffset, constOffset, targetCb, result);
                            }
                        }
                    }
                }
            }

            ResolveRawRuns(gtsContext, resourceManager, gpuAccessor, blocks);
        }

        private static bool IsGlobalMemory(StorageKind storageKind)
        {
            return storageKind is StorageKind.GlobalMemory or
                   StorageKind.GlobalMemoryS8 or
                   StorageKind.GlobalMemoryS16 or
                   StorageKind.GlobalMemoryU8 or
                   StorageKind.GlobalMemoryU16;
        }

        private static bool IsSmallInt(StorageKind storageKind)
        {
            return storageKind is StorageKind.GlobalMemoryS8 or
                   StorageKind.GlobalMemoryS16 or
                   StorageKind.GlobalMemoryU8 or
                   StorageKind.GlobalMemoryU16;
        }

        private static LinkedListNode<INode> ReplaceGlobalMemoryWithStorage(
            GtsContext gtsContext,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            TargetLanguage targetLanguage,
            BasicBlock block,
            LinkedListNode<INode> node)
        {
            Operation operation = node.Value as Operation;
            Operand globalAddress = operation.GetSource(0);
            SearchResult result = FindUniqueBaseAddressCb(gtsContext, block, globalAddress, needsOffset: true);

            if (result.Found)
            {
                // We found the storage buffer that is being accessed.
                // There are two possible paths here, if the operation is simple enough,
                // we just generate the storage access code inline.
                // Otherwise, we generate a function call (and the function if necessary).

                Operand offset = result.Offset;

                bool storageUnaligned = gpuAccessor.QueryHasUnalignedStorageBuffer();

                if (storageUnaligned)
                {
                    Operand baseAddress = Cbuf(result.SbCbSlot, result.SbCbOffset);

                    Operand baseAddressMasked = Local();
                    Operand hostOffset = Local();

                    int alignment = gpuAccessor.QueryHostStorageBufferOffsetAlignment();

                    Operation maskOp = new(Instruction.BitwiseAnd, baseAddressMasked, baseAddress, Const(-alignment));
                    Operation subOp = new(Instruction.Subtract, hostOffset, globalAddress, baseAddressMasked);

                    node.List.AddBefore(node, maskOp);
                    node.List.AddBefore(node, subOp);

                    offset = hostOffset;
                }
                else if (result.ConstOffset != 0)
                {
                    Operand newOffset = Local();

                    Operation addOp = new(Instruction.Add, newOffset, offset, Const(result.ConstOffset));

                    node.List.AddBefore(node, addOp);

                    offset = newOffset;
                }

                if (CanUseInlineStorageOp(operation, targetLanguage))
                {
                    return GenerateInlineStorageOp(resourceManager, node, operation, offset, result);
                }
                else
                {
                    if (!TryGenerateSingleTargetStorageOp(
                        gtsContext,
                        resourceManager,
                        targetLanguage,
                        operation,
                        result,
                        out int functionId))
                    {
                        return null;
                    }

                    return GenerateCallStorageOp(node, operation, offset, functionId);
                }
            }
            else
            {
                // Failed to find the storage buffer directly.
                // Try to walk through Phi chains and find all possible constant buffers where
                // the base address might be stored.
                // Generate a helper function that will check all possible storage buffers and use the right one.

                if (!TryGenerateMultiTargetStorageOp(
                    gtsContext,
                    resourceManager,
                    gpuAccessor,
                    targetLanguage,
                    block,
                    operation,
                    out int functionId))
                {
                    return null;
                }

                return GenerateCallStorageOp(node, operation, null, functionId);
            }
        }

        private static bool CanUseInlineStorageOp(Operation operation, TargetLanguage targetLanguage)
        {
            if (operation.StorageKind != StorageKind.GlobalMemory)
            {
                return false;
            }

            return (operation.Inst != Instruction.AtomicMaxS32 &&
                    operation.Inst != Instruction.AtomicMinS32) || targetLanguage == TargetLanguage.Spirv;
        }

        private static LinkedListNode<INode> GenerateInlineStorageOp(
            ResourceManager resourceManager,
            LinkedListNode<INode> node,
            Operation operation,
            Operand offset,
            SearchResult result)
        {
            bool isStore = operation.Inst == Instruction.Store || operation.Inst.IsAtomic();
            if (!resourceManager.TryGetStorageBufferBinding(result.SbCbSlot, result.SbCbOffset, isStore, out int binding))
            {
                return null;
            }

            Operand wordOffset = Local();

            Operand[] sources;

            if (operation.Inst == Instruction.AtomicCompareAndSwap)
            {
                sources =
                [
                    Const(binding),
                    Const(0),
                    wordOffset,
                    operation.GetSource(operation.SourcesCount - 2),
                    operation.GetSource(operation.SourcesCount - 1)
                ];
            }
            else if (isStore)
            {
                sources = [Const(binding), Const(0), wordOffset, operation.GetSource(operation.SourcesCount - 1)];
            }
            else
            {
                sources = [Const(binding), Const(0), wordOffset];
            }

            Operation shiftOp = new(Instruction.ShiftRightU32, wordOffset, offset, Const(2));
            Operation storageOp = new(operation.Inst, StorageKind.StorageBuffer, operation.Dest, sources);

            node.List.AddBefore(node, shiftOp);
            LinkedListNode<INode> newNode = node.List.AddBefore(node, storageOp);

            Utils.DeleteNode(node, operation);

            return newNode;
        }

        private static LinkedListNode<INode> GenerateCallStorageOp(LinkedListNode<INode> node, Operation operation, Operand offset, int functionId)
        {
            // Generate call to a helper function that will perform the storage buffer operation.

            Operand[] sources = new Operand[operation.SourcesCount - 1 + (offset == null ? 2 : 1)];

            sources[0] = Const(functionId);

            if (offset != null)
            {
                // If the offset was supplised, we use that and skip the global address.

                sources[1] = offset;

                for (int srcIndex = 2; srcIndex < operation.SourcesCount; srcIndex++)
                {
                    sources[srcIndex] = operation.GetSource(srcIndex);
                }
            }
            else
            {
                // Use the 64-bit global address which is split in 2 32-bit arguments.

                for (int srcIndex = 0; srcIndex < operation.SourcesCount; srcIndex++)
                {
                    sources[srcIndex + 1] = operation.GetSource(srcIndex);
                }
            }

            bool returnsValue = operation.Dest != null;
            Operand returnValue = returnsValue ? Local() : null;

            Operation callOp = new(Instruction.Call, returnValue, sources);

            LinkedListNode<INode> newNode = node.List.AddBefore(node, callOp);

            if (returnsValue)
            {
                operation.TurnIntoCopy(returnValue);

                return node;
            }
            else
            {
                Utils.DeleteNode(node, operation);

                return newNode;
            }
        }

        private static bool TryGenerateSingleTargetStorageOp(
            GtsContext gtsContext,
            ResourceManager resourceManager,
            TargetLanguage targetLanguage,
            Operation operation,
            SearchResult result,
            out int functionId)
        {
            List<uint> targetCbs = [PackCbSlotAndOffset(result.SbCbSlot, result.SbCbOffset)];

            if (gtsContext.TryGetFunctionId(operation, isMultiTarget: false, targetCbs, out functionId))
            {
                return true;
            }

            int inArgumentsCount = 1;

            if (operation.Inst == Instruction.AtomicCompareAndSwap)
            {
                inArgumentsCount = 3;
            }
            else if (operation.Inst == Instruction.Store || operation.Inst.IsAtomic())
            {
                inArgumentsCount = 2;
            }

            EmitterContext context = new();

            Operand offset = Argument(0);
            Operand compare = null;
            Operand value = null;

            if (inArgumentsCount == 3)
            {
                compare = Argument(1);
                value = Argument(2);
            }
            else if (inArgumentsCount == 2)
            {
                value = Argument(1);
            }

            if (!TryGenerateStorageOp(
                resourceManager,
                targetLanguage,
                context,
                operation.Inst,
                operation.StorageKind,
                offset,
                compare,
                value,
                result,
                out Operand resultValue))
            {
                functionId = 0;
                return false;
            }

            bool returnsValue = resultValue != null;

            if (returnsValue)
            {
                context.Return(resultValue);
            }
            else
            {
                context.Return();
            }

            string functionName = GetFunctionName(operation, isMultiTarget: false, targetCbs);

            Function function = new(
                ControlFlowGraph.Create(context.GetOperations()).Blocks,
                functionName,
                returnsValue,
                inArgumentsCount,
                0);

            functionId = gtsContext.AddFunction(operation, isMultiTarget: false, targetCbs, function);

            return true;
        }

        private static bool TryGenerateMultiTargetStorageOp(
            GtsContext gtsContext,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            TargetLanguage targetLanguage,
            BasicBlock block,
            Operation operation,
            out int functionId)
        {
            Queue<PhiNode> phis = new();
            HashSet<PhiNode> visited = [];
            List<uint> targetCbs = [];

            Operand globalAddress = operation.GetSource(0);

            if (globalAddress.AsgOp is Operation addOp && addOp.Inst == Instruction.Add)
            {
                Operand src1 = addOp.GetSource(0);
                Operand src2 = addOp.GetSource(1);

                if (src1.Type == OperandType.Constant && src2.Type == OperandType.LocalVariable)
                {
                    globalAddress = src2;
                }
                else if (src1.Type == OperandType.LocalVariable && src2.Type == OperandType.Constant)
                {
                    globalAddress = src1;
                }
            }

            if (globalAddress.AsgOp is PhiNode phi && visited.Add(phi))
            {
                phis.Enqueue(phi);
            }
            else
            {
                SearchResult result = FindUniqueBaseAddressCb(gtsContext, block, operation.GetSource(0), needsOffset: false);

                if (result.Found)
                {
                    targetCbs.Add(PackCbSlotAndOffset(result.SbCbSlot, result.SbCbOffset));
                }
            }

            while (phis.TryDequeue(out phi))
            {
                for (int srcIndex = 0; srcIndex < phi.SourcesCount; srcIndex++)
                {
                    BasicBlock phiBlock = phi.GetBlock(srcIndex);
                    Operand phiSource = phi.GetSource(srcIndex);

                    SearchResult result = FindUniqueBaseAddressCb(gtsContext, phiBlock, phiSource, needsOffset: false);

                    if (result.Found)
                    {
                        uint targetCb = PackCbSlotAndOffset(result.SbCbSlot, result.SbCbOffset);

                        if (!targetCbs.Contains(targetCb))
                        {
                            targetCbs.Add(targetCb);
                        }
                    }
                    else if (phiSource.AsgOp is PhiNode phi2 && visited.Add(phi2))
                    {
                        phis.Enqueue(phi2);
                    }
                }
            }

            targetCbs.Sort();

            if (targetCbs.Count == 0)
            {
                gpuAccessor.Log($"Global memory operation \"{operation.Inst}\" traced to no storage buffer: it goes through the memory pool table.");
            }

            if (gtsContext.TryGetFunctionId(operation, isMultiTarget: true, targetCbs, out functionId))
            {
                return true;
            }

            if (targetCbs.Count == 0)
            {
                return TryGenerateRawStorageOp(gtsContext, resourceManager, gpuAccessor, operation, out functionId);
            }

            int inArgumentsCount = 2;

            if (operation.Inst == Instruction.AtomicCompareAndSwap)
            {
                inArgumentsCount = 4;
            }
            else if (operation.Inst == Instruction.Store || operation.Inst.IsAtomic())
            {
                inArgumentsCount = 3;
            }

            EmitterContext context = new();

            Operand globalAddressLow = Argument(0);
            Operand globalAddressHigh = Argument(1);

            foreach (uint targetCb in targetCbs)
            {
                (int sbCbSlot, int sbCbOffset) = UnpackCbSlotAndOffset(targetCb);

                Operand baseAddrLow = Cbuf(sbCbSlot, sbCbOffset);
                Operand baseAddrHigh = Cbuf(sbCbSlot, sbCbOffset + 1);
                Operand size = Cbuf(sbCbSlot, sbCbOffset + 2);

                Operand offset = context.ISubtract(globalAddressLow, baseAddrLow);
                Operand borrow = context.ICompareLessUnsigned(globalAddressLow, baseAddrLow);

                Operand inRangeLow = context.ICompareLessUnsigned(offset, size);

                Operand addrHighBorrowed = context.IAdd(globalAddressHigh, borrow);

                Operand inRangeHigh = context.ICompareEqual(addrHighBorrowed, baseAddrHigh);

                Operand inRange = context.BitwiseAnd(inRangeLow, inRangeHigh);

                Operand lblSkip = Label();
                context.BranchIfFalse(lblSkip, inRange);

                Operand compare = null;
                Operand value = null;

                if (inArgumentsCount == 4)
                {
                    compare = Argument(2);
                    value = Argument(3);
                }
                else if (inArgumentsCount == 3)
                {
                    value = Argument(2);
                }

                SearchResult result = new(sbCbSlot, sbCbOffset);

                int alignment = gpuAccessor.QueryHostStorageBufferOffsetAlignment();

                Operand baseAddressMasked = context.BitwiseAnd(baseAddrLow, Const(-alignment));
                Operand hostOffset = context.ISubtract(globalAddressLow, baseAddressMasked);

                if (!TryGenerateStorageOp(
                    resourceManager,
                    targetLanguage,
                    context,
                    operation.Inst,
                    operation.StorageKind,
                    hostOffset,
                    compare,
                    value,
                    result,
                    out Operand resultValue))
                {
                    functionId = 0;
                    return false;
                }

                if (resultValue != null)
                {
                    context.Return(resultValue);
                }
                else
                {
                    context.Return();
                }

                context.MarkLabel(lblSkip);
            }

            bool returnsValue = operation.Dest != null;

            if (returnsValue)
            {
                context.Return(Const(0));
            }
            else
            {
                context.Return();
            }

            string functionName = GetFunctionName(operation, isMultiTarget: true, targetCbs);

            Function function = new(
                ControlFlowGraph.Create(context.GetOperations()).Blocks,
                functionName,
                returnsValue,
                inArgumentsCount,
                0);

            functionId = gtsContext.AddFunction(operation, isMultiTarget: true, targetCbs, function);

            return true;
        }

        // nxnative: the memory pool table the host binds for raw global memory (dmsh_abi.h DMSH_GMEM_MAP_*): a storage buffer reserved under this sentinel constant buffer
        // slot, which no constant buffer of the game has (dmsh_build.py turns it into DMSH_KIND_GMEM_MAP). 1 << GmemMapLog2 entries of GmemMapEntryWords words each,
        // {start lo, start hi, end lo, end hi, delta lo, delta hi, hint, 0}, sorted by start; entry 0 is empty (start 0, end 0) and unused entries start at ~0
        public const int GmemMapCbSlot = 0xff;
        private const int GmemMapLog2 = 13;
        private const int GmemMapEntryWords = 8;
        private const int GmemHintShift = 21;

        // nxnative: the write log (dmsh_abi.h DMSH_GMEM_LOG_*), reserved under the same sentinel slot at offset 1: a bitmap of the table entries the stores and atomics wrote through. The host
        // cannot know where a store through a loaded pointer lands before the GPU runs it, and it only wants the pools. Read first: a bit is set once, so nearly every store of a shader
        // that writes a whole frame costs a load, not an atomic (Ocarina of Time's movie compute ran at 41 ms a frame on a hash set of pages, which also ran out of slots)
        private const int GmemLogCbOffset = 1;

        // nxnative: a global memory access whose address the search traced to no storage buffer, an address the shader loaded from memory (Donkey Kong Bananza: gsys's vertex
        // shader that resets the terrain's indirect draw records reads each record's address from a storage buffer). Ryujinx drops such an access (a load reads 0). Here it
        // goes to the address itself: a tagged hint for its 2 MiB chunk, else a branchless binary search, finds the host's pool holding it, the address moves by that pool's delta (its Metal GPU address minus its
        // guest address) and is accessed through a physical storage buffer pointer. An address in no pool is still dropped
        private static bool TryGenerateRawStorageOp(
            GtsContext gtsContext,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            Operation operation,
            out int functionId,
            bool resolved = false)
        {
            functionId = 0;

            bool isSmall = IsSmallInt(operation.StorageKind);

            if (isSmall && operation.Inst.IsAtomic())
            {
                gpuAccessor.Log($"Global memory operation \"{operation.Inst}\" on a small integer traced to no storage buffer: not implemented.");
                return false;
            }

            if (!resourceManager.TryGetStorageBufferBinding(GmemMapCbSlot, 0, write: false, out int mapBinding))
            {
                return false;
            }

            bool writes = operation.Inst == Instruction.Store || operation.Inst.IsAtomic();
            int logBinding = 0;

            if (writes && !resourceManager.TryGetStorageBufferBinding(GmemMapCbSlot, GmemLogCbOffset, write: true, out logBinding))
            {
                return false;
            }

            resourceManager.Properties.UsesPhysicalPointers = true;

            int inArgumentsCount = 2;

            if (operation.Inst == Instruction.AtomicCompareAndSwap)
            {
                inArgumentsCount = 4;
            }
            else if (operation.Inst == Instruction.Store || operation.Inst.IsAtomic())
            {
                inArgumentsCount = 3;
            }

            if (resolved)
            {
                inArgumentsCount = 5;   // the address, then the run's entry (0: none holds the whole run) and its delta (ResolveRawRuns)
            }

            EmitterContext context = new();

            Operand addrLow = Argument(0);
            Operand addrHigh = Argument(1);

            Operand MapWord(Operand entry, int word)
            {
                return context.Load(StorageKind.StorageBuffer, mapBinding, Const(0), context.IAdd(entry, Const(word)));
            }

            Operand AtOrBelow(Operand high, Operand low)   // the address is at or above {low, high}
            {
                return context.BitwiseOr(
                    context.ICompareLessUnsigned(high, addrHigh),
                    context.BitwiseAnd(context.ICompareEqual(high, addrHigh), context.ICompareLessOrEqualUnsigned(low, addrLow)));
            }

            Operand Below(Operand high, Operand low)   // the address is below {low, high}
            {
                return context.BitwiseOr(
                    context.ICompareLessUnsigned(addrHigh, high),
                    context.BitwiseAnd(context.ICompareEqual(addrHigh, high), context.ICompareLessUnsigned(addrLow, low)));
            }

            // A load of a run whose pool was resolved once (ResolveRawRuns) moves by that pool's delta; a run no single entry holds takes the lookup below
            if (resolved)
            {
                Operand lblLookup = Label();
                context.BranchIfFalse(lblLookup, context.ICompareNotEqual(Argument(2), Const(0)));
                EmitAccess(Argument(3), Argument(4));
                context.MarkLabel(lblLookup);
            }

            Operand selected = Local();
            Operand lblFound = Label();
            Operand lblChosen = Label();

            // The entry this helper chose last in this invocation, if it holds the address: a shader reads a structure word after word from one pool, so all but the first access of a run
            // choose the entry the first one chose (Ocarina of Time's foliage mesh shader 73dc0b90c4082e43, 135 reads, took 10.9 ms of a 683x384 frame). The table's ranges are disjoint
            // (nvn_mtl.m gmem_map), so the search below would choose the same entry, and the host binds one table for a whole draw or dispatch. The word starts at 0, the empty entry
            int lastBinding = resourceManager.AddRawLastEntryMemory();
            Operand last = context.Load(StorageKind.LocalMemory, lastBinding);
            Operand lastEntry = context.IMultiply(last, Const(GmemMapEntryWords));
            Operand lastHolds = context.BitwiseAnd(AtOrBelow(MapWord(lastEntry, 1), MapWord(lastEntry, 0)), Below(MapWord(lastEntry, 3), MapWord(lastEntry, 2)));
            context.Copy(selected, last);
            context.BranchIfTrue(lblChosen, lastHolds);

            // The hint of the address's 2 MiB chunk names an entry holding the whole chunk (dmsh_abi.h DMSH_GMEM_HINT_SHIFT). An address too high for the tag searches
            Operand chunk = context.BitwiseOr(context.ShiftLeft(addrHigh, Const(32 - GmemHintShift)), context.ShiftRightU32(addrLow, Const(GmemHintShift)));
            Operand indexMask = Const((1 << GmemMapLog2) - 1);
            Operand hint = MapWord(context.IMultiply(context.BitwiseAnd(chunk, indexMask), Const(GmemMapEntryWords)), 6);
            Operand hintIndex = context.BitwiseAnd(hint, indexMask);
            Operand hintFits = context.BitwiseAnd(
                context.BitwiseAnd(context.ICompareNotEqual(hintIndex, Const(0)), context.ICompareEqual(context.ShiftRightU32(hint, Const(GmemMapLog2)), chunk)),
                context.ICompareLessUnsigned(addrHigh, Const(1 << (GmemHintShift - GmemMapLog2))));
            context.Copy(selected, hintIndex);
            context.BranchIfTrue(lblFound, hintFits);

            // The last entry whose start is at or below the address (entry 0 starts at 0, so there always is one)
            Operand index = EmitTableSearch(context, entry => AtOrBelow(MapWord(entry, 1), MapWord(entry, 0)));

            context.Copy(selected, index);
            context.MarkLabel(lblFound);
            context.Store(StorageKind.LocalMemory, lastBinding, selected);
            Operand found = context.IMultiply(selected, Const(GmemMapEntryWords));
            Operand lblSkip = Label();
            context.BranchIfFalse(lblSkip, Below(MapWord(found, 3), MapWord(found, 2)));   // an address in no pool: the entry below it ends below it

            // The last entry's range held it, or the hint's or the search's entry does
            context.MarkLabel(lblChosen);
            found = context.IMultiply(selected, Const(GmemMapEntryWords));

            if (writes)
            {
                Operand word = context.ShiftRightU32(selected, Const(5));
                Operand bit = context.ShiftLeft(Const(1), context.BitwiseAnd(selected, Const(31)));
                Operand lblLogged = Label();

                context.BranchIfTrue(lblLogged, context.ICompareNotEqual(context.BitwiseAnd(context.Load(StorageKind.StorageBuffer, logBinding, Const(0), word), bit), Const(0)));
                context.AtomicOr(StorageKind.StorageBuffer, logBinding, Const(0), word, bit);
                context.MarkLabel(lblLogged);
            }

            EmitAccess(MapWord(found, 4), MapWord(found, 5));

            context.MarkLabel(lblSkip);

            bool returnsValue = operation.Dest != null;

            if (returnsValue)
            {
                context.Return(Const(0));
            }
            else
            {
                context.Return();
            }

            Function function = new(
                ControlFlowGraph.Create(context.GetOperations()).Blocks,
                GetFunctionName(operation, isMultiTarget: true, []) + (resolved ? "RawResolved" : "Raw"),
                returnsValue,
                inArgumentsCount,
                0);

            if (resolved)
            {
                functionId = gtsContext.AddHelper(function);
                gtsContext.ResolvedLoadFunctions[operation.StorageKind] = functionId;
            }
            else
            {
                functionId = gtsContext.AddFunction(operation, isMultiTarget: true, [], function);

                if (operation.Inst == Instruction.Load)
                {
                    gtsContext.RawLoadFunctions[functionId] = operation.StorageKind;
                }
            }

            return true;

            // The access at the address moved by a pool's delta (its Metal GPU address minus its guest address), and its return
            void EmitAccess(Operand deltaLow, Operand deltaHigh)
            {
                // A small integer is read or written in its 32-bit word
                Operand wordLow = isSmall ? context.BitwiseAnd(addrLow, Const(~3)) : addrLow;
                Operand hostLow = context.IAdd(wordLow, deltaLow);
                Operand carry = context.ConditionalSelect(context.ICompareLessUnsigned(hostLow, wordLow), Const(1), Const(0));
                Operand hostHigh = context.IAdd(context.IAdd(addrHigh, deltaHigh), carry);

                Operand resultValue = null;

                if (operation.Inst == Instruction.Load)
                {
                    resultValue = context.Load(StorageKind.GlobalMemory, hostLow, hostHigh);

                    if (isSmall)
                    {
                        Operand bitOffset = HelperFunctionManager.GetBitOffset(context, addrLow);

                        resultValue = operation.StorageKind switch
                        {
                            StorageKind.GlobalMemoryS8 => context.BitfieldExtractS32(context.ShiftRightS32(resultValue, bitOffset), Const(0), Const(8)),
                            StorageKind.GlobalMemoryS16 => context.BitfieldExtractS32(context.ShiftRightS32(resultValue, bitOffset), Const(0), Const(16)),
                            StorageKind.GlobalMemoryU8 => context.BitwiseAnd(context.ShiftRightU32(resultValue, bitOffset), Const(byte.MaxValue)),
                            _ => context.BitwiseAnd(context.ShiftRightU32(resultValue, bitOffset), Const(ushort.MaxValue)),
                        };
                    }
                }
                else if (operation.Inst == Instruction.Store)
                {
                    Operand value = Argument(2);

                    if (isSmall)
                    {
                        int bitSize = operation.StorageKind is StorageKind.GlobalMemoryS8 or StorageKind.GlobalMemoryU8 ? 8 : 16;
                        Operand bitOffset = HelperFunctionManager.GetBitOffset(context, addrLow);

                        Operand lblLoopHead = Label();
                        context.MarkLabel(lblLoopHead);

                        Operand oldValue = context.Load(StorageKind.GlobalMemory, hostLow, hostHigh);
                        Operand newValue = context.BitfieldInsert(oldValue, value, bitOffset, Const(bitSize));
                        Operand casResult = context.AtomicCompareAndSwap(StorageKind.GlobalMemory, hostLow, hostHigh, oldValue, newValue);

                        context.BranchIfTrue(lblLoopHead, context.ICompareNotEqual(casResult, oldValue));
                    }
                    else
                    {
                        context.Store(StorageKind.GlobalMemory, hostLow, hostHigh, value);
                    }
                }
                else if (operation.Inst == Instruction.AtomicCompareAndSwap)
                {
                    resultValue = context.AtomicCompareAndSwap(StorageKind.GlobalMemory, hostLow, hostHigh, Argument(2), Argument(3));
                }
                else
                {
                    resultValue = context.Add(operation.Inst, StorageKind.GlobalMemory, Local(), hostLow, hostHigh, Argument(2));
                }

                if (resultValue != null)
                {
                    context.Return(resultValue);
                }
                else
                {
                    context.Return();
                }
            }
        }

        // nxnative: the binary search of the pool table as a loop of GmemMapLog2 steps: the last entry whose start passes startAtOrBelow (given an entry's first word). The search runs only
        // when the last entry and the hint both missed, but unrolled it was 13 copies of the step at every call site once Metal inlines the helpers: Ocarina of Time's compute shaders
        // 77e64b1d8200eac3 and d8abf8a34811f8c6 took 2.3 to 2.6 s each to become pipelines, and the 8 that its foliage's reaction to a sword needs froze the game for 11 s the first time
        private static Operand EmitTableSearch(EmitterContext context, Func<Operand, Operand> startAtOrBelow)
        {
            Operand index = Local();
            Operand step = Local();
            Operand lblStep = Label();

            context.Copy(index, Const(0));
            context.Copy(step, Const(1 << (GmemMapLog2 - 1)));
            context.MarkLabel(lblStep);

            Operand candidate = context.IAdd(index, step);
            context.Copy(index, context.ConditionalSelect(startAtOrBelow(context.IMultiply(candidate, Const(GmemMapEntryWords))), candidate, index));
            context.Copy(step, context.ShiftRightU32(step, Const(1)));
            context.BranchIfTrue(lblStep, context.ICompareNotEqual(step, Const(0)));

            return index;
        }

        // nxnative: loads through one untraced pointer at constant offsets resolve its pool once. A shader that reads a structure word after word through a pointer it loaded (Ocarina of Time's
        // colour-pass mesh shaders, 253 to 506 raw loads each, a pointer from a fixed word of a storage buffer and offsets +0, +4, +8 ...) paid each word the last-entry check, 4 table reads, and the
        // delta's 2 more: about 7 memory reads per word of data, and nearly all of the 683x384 colour pass's 5.8 ms of mesh work. Here the loads of one base {low, high} (SSA values) at
        // offsets c >= 0, as the decoder builds them (low = base low + c, high = base high + the carry of that add), are a run: at the nearest block that dominates all of them, one call looks up
        // the entry holding the run's whole range [base + min c, base + max c + 4) and reads its delta, and each load moves by that delta. A run no single entry holds (it spans two pools, or an
        // address in none, or a guard that leaves the pointer invalid) gives entry 0, and each of its loads takes the full lookup as before. The resolve only reads the table, so running it
        // where a guarded load would not run is harmless. Stores and atomics keep the per-access path: the write log is per entry
        private const int RawRunMinLoads = 2;
        private const int RawRunMaxOffset = 1 << 24;

        private readonly struct RawRunAccess
        {
            public readonly LinkedListNode<INode> Node;
            public readonly BasicBlock Block;
            public readonly Operand BaseLow;
            public readonly Operand BaseHigh;
            public readonly int Offset;

            public RawRunAccess(LinkedListNode<INode> node, BasicBlock block, Operand baseLow, Operand baseHigh, int offset)
            {
                Node = node;
                Block = block;
                BaseLow = baseLow;
                BaseHigh = baseHigh;
                Offset = offset;
            }
        }

        private static void ResolveRawRuns(GtsContext gtsContext, ResourceManager resourceManager, IGpuAccessor gpuAccessor, BasicBlock[] blocks)
        {
            if (gtsContext.RawLoadFunctions.Count == 0)
            {
                return;
            }

            // The runs in program order (first load first), keyed by the base's value (RawValueKey: the same SSA values, or the same pure operations of them), and where each node is
            Dictionary<string, List<RawRunAccess>> runs = new();
            List<string> order = [];
            Dictionary<INode, BasicBlock> blockOf = new();
            Dictionary<Operand, string> keys = new();

            foreach (BasicBlock block in blocks)
            {
                for (LinkedListNode<INode> node = block.Operations.First; node != null; node = node.Next)
                {
                    blockOf[node.Value] = block;

                    if (node.Value is not Operation call ||
                        call.Inst != Instruction.Call ||
                        call.SourcesCount != 3 ||
                        call.Dest == null ||
                        call.GetSource(0).Type != OperandType.Constant ||
                        !gtsContext.RawLoadFunctions.ContainsKey(call.GetSource(0).Value))
                    {
                        continue;
                    }

                    if (!TryGetRawBase(call.GetSource(1), call.GetSource(2), out Operand baseLow, out Operand baseHigh, out int offset))
                    {
                        continue;
                    }

                    string key = RawValueKey(baseLow, keys, RawKeyDepth) + "|" + RawValueKey(baseHigh, keys, RawKeyDepth);

                    if (!runs.TryGetValue(key, out List<RawRunAccess> run))
                    {
                        runs.Add(key, run = []);
                        order.Add(key);
                    }

                    run.Add(new RawRunAccess(node, block, baseLow, baseHigh, offset));
                }
            }

            int mapBinding = -1;

            foreach (string key in order)
            {
                List<RawRunAccess> run = runs[key];

                if (run.Count < RawRunMinLoads)
                {
                    continue;
                }

                // The nearest block that dominates every load of the run
                BasicBlock at = run[0].Block;

                foreach (RawRunAccess access in run)
                {
                    at = NearestCommonDominator(at, access.Block);

                    if (at == null)
                    {
                        break;
                    }
                }

                if (at == null)
                {
                    continue;
                }

                if (mapBinding < 0)
                {
                    if (!resourceManager.TryGetStorageBufferBinding(GmemMapCbSlot, 0, write: false, out mapBinding) ||
                        !TryGetResolveRangeFunction(gtsContext, resourceManager, out _))
                    {
                        return;
                    }
                }

                int min = int.MaxValue, end = 0;

                foreach (RawRunAccess access in run)
                {
                    min = Math.Min(min, access.Offset);
                    end = Math.Max(end, access.Offset + 4);
                }

                // Before the run's first load in that block, else at its end (before its branch)
                LinkedListNode<INode> before = null;

                foreach (RawRunAccess access in run)
                {
                    if (access.Block == at)
                    {
                        before = access.Node;
                        break;
                    }
                }

                // A load's base whose value is defined there: one SSA value dominates all its uses, but loads that compute the same value each on their own may not
                Operand baseLow = null, baseHigh = null;

                foreach (RawRunAccess access in run)
                {
                    if (IsAvailableAt(access.BaseLow, at, before, blockOf) && IsAvailableAt(access.BaseHigh, at, before, blockOf))
                    {
                        (baseLow, baseHigh) = (access.BaseLow, access.BaseHigh);
                        break;
                    }
                }

                if (baseLow == null)
                {
                    continue;
                }

                EmitterContext context = new();

                (Operand minLow, Operand minHigh) = AddOffset64(context, baseLow, baseHigh, min);
                (Operand endLow, Operand endHigh) = AddOffset64(context, baseLow, baseHigh, end);
                Operand entry = context.Call(gtsContext.ResolveRangeFunction, true, minLow, minHigh, endLow, endHigh);
                Operand entryWord = context.IMultiply(entry, Const(GmemMapEntryWords));
                Operand deltaLow = context.Load(StorageKind.StorageBuffer, mapBinding, Const(0), context.IAdd(entryWord, Const(4)));
                Operand deltaHigh = context.Load(StorageKind.StorageBuffer, mapBinding, Const(0), context.IAdd(entryWord, Const(5)));

                foreach (Operation op in context.GetOperations())
                {
                    if (before != null)
                    {
                        at.Operations.AddBefore(before, op);
                    }
                    else
                    {
                        at.Append(op);
                    }
                }

                foreach (RawRunAccess access in run)
                {
                    Operation call = (Operation)access.Node.Value;
                    StorageKind kind = gtsContext.RawLoadFunctions[call.GetSource(0).Value];

                    if (!gtsContext.ResolvedLoadFunctions.TryGetValue(kind, out int resolvedId))
                    {
                        Operation model = new(Instruction.Load, kind, Local(), Const(0), Const(0));

                        if (!TryGenerateRawStorageOp(gtsContext, resourceManager, gpuAccessor, model, out resolvedId, resolved: true))
                        {
                            continue;
                        }
                    }

                    call.SetSource(0, Const(resolvedId));
                    call.AppendSources([entry, deltaLow, deltaHigh]);
                }
            }
        }

        // The base and constant offset of a raw load's address, as the decoder builds an address plus an offset (InstEmitAmpereMem.AGlobalAddress, InstEmitMemory.Get40BitsAddress):
        // low = base low + c, high = base high + (low < base low ? 1 : 0), or the carry alone for a base high of 0. An address that is no such sum is its own base at offset 0
        private static bool TryGetRawBase(Operand low, Operand high, out Operand baseLow, out Operand baseHigh, out int offset)
        {
            low = SkipCopies(low);
            high = SkipCopies(high);

            if (low.AsgOp is Operation add && add.Inst == Instruction.Add && add.SourcesCount == 2)
            {
                for (int i = 0; i < 2; i++)
                {
                    Operand c = add.GetSource(i), b = SkipCopies(add.GetSource(1 - i));

                    if (c.Type != OperandType.Constant || c.Value <= 0 || c.Value >= RawRunMaxOffset || b.Type != OperandType.LocalVariable)
                    {
                        continue;
                    }

                    if (IsCarryOf(high, low, b))   // a high word of 0: the optimizer folded 0 + carry to the carry
                    {
                        baseLow = b;
                        baseHigh = Const(0);
                        offset = c.Value;

                        return true;
                    }

                    if (high.AsgOp is Operation addHigh && addHigh.Inst == Instruction.Add && addHigh.SourcesCount == 2)
                    {
                        for (int j = 0; j < 2; j++)
                        {
                            if (IsCarryOf(addHigh.GetSource(j), low, b))
                            {
                                baseLow = b;
                                baseHigh = SkipCopies(addHigh.GetSource(1 - j));
                                offset = c.Value;

                                return baseHigh.Type is OperandType.LocalVariable or OperandType.Constant;
                            }
                        }
                    }
                }
            }

            baseLow = low;
            baseHigh = high;
            offset = 0;

            return low.Type == OperandType.LocalVariable && high.Type is OperandType.LocalVariable or OperandType.Constant;
        }

        // carry = (sum < base) ? 1 : 0
        private static bool IsCarryOf(Operand carry, Operand sum, Operand baseLow)
        {
            if (SkipCopies(carry).AsgOp is not Operation select ||
                select.Inst != Instruction.ConditionalSelect ||
                select.SourcesCount != 3 ||
                select.GetSource(1).Type != OperandType.Constant || select.GetSource(1).Value != 1 ||
                select.GetSource(2).Type != OperandType.Constant || select.GetSource(2).Value != 0)
            {
                return false;
            }

            return SkipCopies(select.GetSource(0)).AsgOp is Operation compare &&
                   compare.Inst == Instruction.CompareLessU32 &&
                   compare.SourcesCount == 2 &&
                   SkipCopies(compare.GetSource(0)) == sum &&
                   SkipCopies(compare.GetSource(1)) == baseLow;
        }

        private const int RawKeyDepth = 6;

        // A key equal for two operands that hold the same value in an invocation: the same SSA value or constant, or the same pure integer operation of such values (a decoder computes
        // a pointer plus a uniform register again for each instruction). Anything else, a memory load among them, is keyed by its own identity
        private static string RawValueKey(Operand operand, Dictionary<Operand, string> keys, int depth)
        {
            operand = SkipCopies(operand);

            if (operand.Type == OperandType.Constant)
            {
                return "#" + operand.Value.ToString(System.Globalization.CultureInfo.InvariantCulture);
            }

            if (keys.TryGetValue(operand, out string known))
            {
                return known;
            }

            string key;

            if (depth > 0 && operand.AsgOp is Operation op && op.Dest == operand && op.DestsCount == 1 && op.SourcesCount is >= 1 and <= 3 && IsPureInteger(op.Inst))
            {
                string[] sources = new string[op.SourcesCount];

                for (int i = 0; i < sources.Length; i++)
                {
                    sources[i] = RawValueKey(op.GetSource(i), keys, depth - 1);
                }

                if (op.Inst is Instruction.Add or Instruction.BitwiseAnd or Instruction.BitwiseOr or Instruction.BitwiseExclusiveOr or Instruction.CompareEqual or Instruction.CompareNotEqual)
                {
                    Array.Sort(sources, StringComparer.Ordinal);
                }

                key = op.Inst.ToString() + "(" + string.Join(",", sources) + ")";
            }
            else
            {
                key = "v" + keys.Count.ToString(System.Globalization.CultureInfo.InvariantCulture);
            }

            keys[operand] = key;

            return key;
        }

        private static bool IsPureInteger(Instruction inst)
        {
            return inst is Instruction.Add or Instruction.Subtract or Instruction.Multiply or Instruction.ConditionalSelect or
                Instruction.CompareLessU32 or Instruction.CompareEqual or Instruction.CompareNotEqual or
                Instruction.BitwiseAnd or Instruction.BitwiseOr or Instruction.BitwiseExclusiveOr or Instruction.BitwiseNot or
                Instruction.ShiftLeft or Instruction.ShiftRightU32 or Instruction.ShiftRightS32;
        }

        // The operand's value exists at the insertion point: before node in block, or at block's end when node is null
        private static bool IsAvailableAt(Operand operand, BasicBlock block, LinkedListNode<INode> node, Dictionary<INode, BasicBlock> blockOf)
        {
            if (operand.Type == OperandType.Constant)
            {
                return true;
            }

            if (operand.Type != OperandType.LocalVariable || operand.AsgOp == null || !blockOf.TryGetValue(operand.AsgOp, out BasicBlock defBlock))
            {
                return false;
            }

            if (defBlock != block)
            {
                for (BasicBlock x = block.ImmediateDominator; x != null; x = x.ImmediateDominator == x ? null : x.ImmediateDominator)
                {
                    if (x == defBlock)
                    {
                        return true;
                    }
                }

                return false;
            }

            if (node == null)
            {
                return true;
            }

            for (LinkedListNode<INode> n = node.Previous; n != null; n = n.Previous)
            {
                if (n.Value == operand.AsgOp)
                {
                    return true;
                }
            }

            return false;
        }

        private static Operand SkipCopies(Operand operand)
        {
            while (operand.AsgOp is Operation copy && copy.Inst == Instruction.Copy && copy.SourcesCount == 1)
            {
                operand = copy.GetSource(0);
            }

            return operand;
        }

        private static (Operand, Operand) AddOffset64(EmitterContext context, Operand low, Operand high, int offset)
        {
            if (offset == 0)
            {
                return (low, high);
            }

            Operand sum = context.IAdd(low, Const(offset));
            Operand carry = context.ConditionalSelect(context.ICompareLessUnsigned(sum, low), Const(1), Const(0));

            return (sum, context.IAdd(high, carry));
        }

        private static BasicBlock NearestCommonDominator(BasicBlock a, BasicBlock b)
        {
            HashSet<BasicBlock> above = [];

            for (BasicBlock x = a; x != null; x = x.ImmediateDominator == x ? null : x.ImmediateDominator)
            {
                above.Add(x);
            }

            for (BasicBlock y = b; y != null; y = y.ImmediateDominator == y ? null : y.ImmediateDominator)
            {
                if (above.Contains(y))
                {
                    return y;
                }
            }

            return null;
        }

        // The entry of the pool table that holds a whole range [min, end): the entry this helper chose last in this invocation, else the hint of min's 2 MiB chunk, else the last entry
        // starting at or below min, if that entry also reaches end; else 0. A shader that loads its pointers again in each guarded block resolves the same pointer many times
        // (Ocarina of Time's colour-pass mesh shaders: 122 runs through 5 pointers), and all but the first take the remembered entry
        private static bool TryGetResolveRangeFunction(GtsContext gtsContext, ResourceManager resourceManager, out int functionId)
        {
            functionId = gtsContext.ResolveRangeFunction;

            if (functionId >= 0)
            {
                return true;
            }

            if (!resourceManager.TryGetStorageBufferBinding(GmemMapCbSlot, 0, write: false, out int mapBinding))
            {
                return false;
            }

            resourceManager.Properties.UsesPhysicalPointers = true;

            EmitterContext context = new();

            Operand minLow = Argument(0);
            Operand minHigh = Argument(1);
            Operand endLow = Argument(2);
            Operand endHigh = Argument(3);

            Operand MapWord(Operand entry, int word)
            {
                return context.Load(StorageKind.StorageBuffer, mapBinding, Const(0), context.IAdd(entry, Const(word)));
            }

            Operand NotAbove(Operand aHigh, Operand aLow, Operand bHigh, Operand bLow)   // {aLow, aHigh} <= {bLow, bHigh}
            {
                return context.BitwiseOr(
                    context.ICompareLessUnsigned(aHigh, bHigh),
                    context.BitwiseAnd(context.ICompareEqual(aHigh, bHigh), context.ICompareLessOrEqualUnsigned(aLow, bLow)));
            }

            Operand Holds(Operand index)
            {
                Operand found = context.IMultiply(index, Const(GmemMapEntryWords));

                return context.BitwiseAnd(
                    context.ICompareNotEqual(index, Const(0)),
                    context.BitwiseAnd(
                        NotAbove(MapWord(found, 1), MapWord(found, 0), minHigh, minLow),
                        NotAbove(endHigh, endLow, MapWord(found, 3), MapWord(found, 2))));
            }

            int lastBinding = resourceManager.AddRawLastEntryMemory();
            Operand last = context.Load(StorageKind.LocalMemory, lastBinding);
            Operand lblMiss = Label();
            context.BranchIfFalse(lblMiss, Holds(last));
            context.Return(last);
            context.MarkLabel(lblMiss);

            Operand selected = Local();
            Operand lblFound = Label();

            Operand chunk = context.BitwiseOr(context.ShiftLeft(minHigh, Const(32 - GmemHintShift)), context.ShiftRightU32(minLow, Const(GmemHintShift)));
            Operand indexMask = Const((1 << GmemMapLog2) - 1);
            Operand hint = MapWord(context.IMultiply(context.BitwiseAnd(chunk, indexMask), Const(GmemMapEntryWords)), 6);
            Operand hintIndex = context.BitwiseAnd(hint, indexMask);
            Operand hintFits = context.BitwiseAnd(
                context.BitwiseAnd(context.ICompareNotEqual(hintIndex, Const(0)), context.ICompareEqual(context.ShiftRightU32(hint, Const(GmemMapLog2)), chunk)),
                context.ICompareLessUnsigned(minHigh, Const(1 << (GmemHintShift - GmemMapLog2))));
            context.Copy(selected, hintIndex);
            context.BranchIfTrue(lblFound, hintFits);

            Operand index = EmitTableSearch(context, entry => NotAbove(MapWord(entry, 1), MapWord(entry, 0), minHigh, minLow));

            context.Copy(selected, index);
            context.MarkLabel(lblFound);

            Operand lblNone = Label();
            context.BranchIfFalse(lblNone, Holds(selected));
            context.Store(StorageKind.LocalMemory, lastBinding, selected);
            context.Return(selected);
            context.MarkLabel(lblNone);
            context.Return(Const(0));

            Function function = new(ControlFlowGraph.Create(context.GetOperations()).Blocks, "RawResolveRange", true, 4, 0);

            functionId = gtsContext.ResolveRangeFunction = gtsContext.AddHelper(function);

            return true;
        }

        private static uint PackCbSlotAndOffset(int cbSlot, int cbOffset)
        {
            return (uint)((ushort)cbSlot | ((ushort)cbOffset << 16));
        }

        private static (int, int) UnpackCbSlotAndOffset(uint packed)
        {
            return ((ushort)packed, (ushort)(packed >> 16));
        }

        private static string GetFunctionName(Operation baseOp, bool isMultiTarget, IReadOnlyList<uint> targetCbs)
        {
            StringBuilder nameBuilder = new();
            nameBuilder.Append(baseOp.Inst.ToString());

            nameBuilder.Append(baseOp.StorageKind switch
            {
                StorageKind.GlobalMemoryS8 => "S8",
                StorageKind.GlobalMemoryS16 => "S16",
                StorageKind.GlobalMemoryU8 => "U8",
                StorageKind.GlobalMemoryU16 => "U16",
                _ => string.Empty,
            });

            if (isMultiTarget)
            {
                nameBuilder.Append("Multi");
            }

            foreach (uint targetCb in targetCbs)
            {
                (int sbCbSlot, int sbCbOffset) = UnpackCbSlotAndOffset(targetCb);

                nameBuilder.Append($"_c{sbCbSlot}o{sbCbOffset}");
            }

            return nameBuilder.ToString();
        }

        private static bool TryGenerateStorageOp(
            ResourceManager resourceManager,
            TargetLanguage targetLanguage,
            EmitterContext context,
            Instruction inst,
            StorageKind storageKind,
            Operand offset,
            Operand compare,
            Operand value,
            SearchResult result,
            out Operand resultValue)
        {
            resultValue = null;
            bool isStore = inst.IsAtomic() || inst == Instruction.Store;

            if (!resourceManager.TryGetStorageBufferBinding(result.SbCbSlot, result.SbCbOffset, isStore, out int binding))
            {
                return false;
            }

            Operand wordOffset = context.ShiftRightU32(offset, Const(2));

            if (inst.IsAtomic())
            {
                if (IsSmallInt(storageKind))
                {
                    throw new NotImplementedException();
                }

                switch (inst)
                {
                    case Instruction.AtomicAdd:
                        resultValue = context.AtomicAdd(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicAnd:
                        resultValue = context.AtomicAnd(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicCompareAndSwap:
                        resultValue = context.AtomicCompareAndSwap(StorageKind.StorageBuffer, binding, Const(0), wordOffset, compare, value);
                        break;
                    case Instruction.AtomicMaxS32:
                        if (targetLanguage == TargetLanguage.Spirv)
                        {
                            resultValue = context.AtomicMaxS32(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        }
                        else
                        {
                            resultValue = GenerateAtomicCasLoop(context, wordOffset, binding, (memValue) =>
                            {
                                return context.IMaximumS32(memValue, value);
                            });
                        }

                        break;
                    case Instruction.AtomicMaxU32:
                        resultValue = context.AtomicMaxU32(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicMinS32:
                        if (targetLanguage == TargetLanguage.Spirv)
                        {
                            resultValue = context.AtomicMinS32(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        }
                        else
                        {
                            resultValue = GenerateAtomicCasLoop(context, wordOffset, binding, (memValue) =>
                            {
                                return context.IMinimumS32(memValue, value);
                            });
                        }

                        break;
                    case Instruction.AtomicMinU32:
                        resultValue = context.AtomicMinU32(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicOr:
                        resultValue = context.AtomicOr(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicSwap:
                        resultValue = context.AtomicSwap(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                    case Instruction.AtomicXor:
                        resultValue = context.AtomicXor(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                        break;
                }
            }
            else if (inst == Instruction.Store)
            {
                int bitSize = storageKind switch
                {
                    StorageKind.GlobalMemoryS8 or
                    StorageKind.GlobalMemoryU8 => 8,
                    StorageKind.GlobalMemoryS16 or
                    StorageKind.GlobalMemoryU16 => 16,
                    _ => 32,
                };

                if (bitSize < 32)
                {
                    Operand bitOffset = HelperFunctionManager.GetBitOffset(context, offset);

                    GenerateAtomicCasLoop(context, wordOffset, binding, (memValue) =>
                    {
                        return context.BitfieldInsert(memValue, value, bitOffset, Const(bitSize));
                    });
                }
                else
                {
                    context.Store(StorageKind.StorageBuffer, binding, Const(0), wordOffset, value);
                }
            }
            else
            {
                value = context.Load(StorageKind.StorageBuffer, binding, Const(0), wordOffset);

                if (IsSmallInt(storageKind))
                {
                    Operand bitOffset = HelperFunctionManager.GetBitOffset(context, offset);

                    switch (storageKind)
                    {
                        case StorageKind.GlobalMemoryS8:
                            value = context.ShiftRightS32(value, bitOffset);
                            value = context.BitfieldExtractS32(value, Const(0), Const(8));
                            break;
                        case StorageKind.GlobalMemoryS16:
                            value = context.ShiftRightS32(value, bitOffset);
                            value = context.BitfieldExtractS32(value, Const(0), Const(16));
                            break;
                        case StorageKind.GlobalMemoryU8:
                            value = context.ShiftRightU32(value, bitOffset);
                            value = context.BitwiseAnd(value, Const(byte.MaxValue));
                            break;
                        case StorageKind.GlobalMemoryU16:
                            value = context.ShiftRightU32(value, bitOffset);
                            value = context.BitwiseAnd(value, Const(ushort.MaxValue));
                            break;
                    }
                }

                resultValue = value;
            }

            return true;
        }

        private static Operand GenerateAtomicCasLoop(EmitterContext context, Operand wordOffset, int binding, Func<Operand, Operand> opCallback)
        {
            Operand lblLoopHead = Label();

            context.MarkLabel(lblLoopHead);

            Operand oldValue = context.Load(StorageKind.StorageBuffer, binding, Const(0), wordOffset);
            Operand newValue = opCallback(oldValue);

            Operand casResult = context.AtomicCompareAndSwap(
                StorageKind.StorageBuffer,
                binding,
                Const(0),
                wordOffset,
                oldValue,
                newValue);

            Operand casFail = context.ICompareNotEqual(casResult, oldValue);

            context.BranchIfTrue(lblLoopHead, casFail);

            return oldValue;
        }

        private static SearchResult FindUniqueBaseAddressCb(GtsContext gtsContext, BasicBlock block, Operand globalAddress, bool needsOffset)
        {
            globalAddress = Utils.FindLastOperation(globalAddress, block);

            if (globalAddress.Type == OperandType.ConstantBuffer)
            {
                return GetBaseAddressCbWithOffset(globalAddress, Const(0), 0);
            }

            Operation operation = globalAddress.AsgOp as Operation;

            if (operation == null || operation.Inst != Instruction.Add)
            {
                return FindBaseAddressCbFromMemory(gtsContext, operation, 0, needsOffset);
            }

            Operand src1 = operation.GetSource(0);
            Operand src2 = operation.GetSource(1);

            int constOffset = 0;

            if ((src1.Type == OperandType.LocalVariable && src2.Type == OperandType.Constant) ||
                (src2.Type == OperandType.LocalVariable && src1.Type == OperandType.Constant))
            {
                Operand baseAddr;
                Operand offset;

                if (src1.Type == OperandType.LocalVariable)
                {
                    baseAddr = Utils.FindLastOperation(src1, block);
                    offset = src2;
                }
                else
                {
                    baseAddr = Utils.FindLastOperation(src2, block);
                    offset = src1;
                }

                SearchResult result = GetBaseAddressCbWithOffset(baseAddr, offset, 0);
                if (result.Found)
                {
                    return result;
                }

                constOffset = offset.Value;
                operation = baseAddr.AsgOp as Operation;

                if (operation == null || operation.Inst != Instruction.Add)
                {
                    return FindBaseAddressCbFromMemory(gtsContext, operation, constOffset, needsOffset);
                }
            }

            src1 = operation.GetSource(0);
            src2 = operation.GetSource(1);

            // If we have two possible results, we give preference to the ones from
            // the driver reserved constant buffer, as those are the ones that
            // contains the base address.

            // If both are constant buffer, give preference to the second operand,
            // because constant buffer are always encoded as the second operand,
            // so the second operand will always be the one from the last instruction.
            // Unless only the first is from the driver reserved buffer: Ampere code adds uniform registers
            // (Donkey Kong Bananza's effect compute shaders: c0 storage entry + offset from c1, by UIMAD.WIDE),
            // and the IR keeps the order of their sources.

            if (src1.Type == OperandType.ConstantBuffer && src2.Type == OperandType.ConstantBuffer &&
                src1.GetCbufSlot() == DriverReservedCb && src2.GetCbufSlot() != DriverReservedCb)
            {
                return GetBaseAddressCbWithOffset(src1, src2, constOffset);
            }

            if (src1.Type != OperandType.ConstantBuffer ||
                (src1.Type == OperandType.ConstantBuffer && src2.Type == OperandType.ConstantBuffer) ||
                (src2.Type == OperandType.ConstantBuffer && src2.GetCbufSlot() == DriverReservedCb))
            {
                return GetBaseAddressCbWithOffset(src2, src1, constOffset);
            }

            return GetBaseAddressCbWithOffset(src1, src2, constOffset);
        }

        private static SearchResult FindBaseAddressCbFromMemory(GtsContext gtsContext, Operation operation, int constOffset, bool needsOffset)
        {
            if (operation != null)
            {
                if (TryGetMemoryOffsets(operation, out LsMemoryType type, out Operand bo, out int co) &&
                    gtsContext.TryGetMemoryTargetCb(type, bo, co, out SearchResult result) &&
                    (result.Offset != null || !needsOffset))
                {
                    if (constOffset != 0)
                    {
                        return new SearchResult(
                            result.SbCbSlot,
                            result.SbCbOffset,
                            result.Offset,
                            result.ConstOffset + constOffset);
                    }

                    return result;
                }
            }

            return SearchResult.NotFound;
        }

        private static SearchResult GetBaseAddressCbWithOffset(Operand baseAddress, Operand offset, int constOffset)
        {
            if (baseAddress.Type == OperandType.ConstantBuffer)
            {
                int sbCbSlot = baseAddress.GetCbufSlot();
                int sbCbOffset = baseAddress.GetCbufOffset();

                // We require the offset to be aligned to 1 word (64 bits),
                // since the address size is 64-bit and the GPU only supports aligned memory access.
                if ((sbCbOffset & 1) == 0)
                {
                    return new SearchResult(sbCbSlot, sbCbOffset, offset, constOffset);
                }
            }

            return SearchResult.NotFound;
        }

        private static bool TryGetMemoryOffsets(Operation operation, out LsMemoryType type, out Operand baseOffset, out int constOffset)
        {
            baseOffset = null;

            if (operation.Inst is Instruction.Load or Instruction.Store)
            {
                if (operation.StorageKind == StorageKind.SharedMemory)
                {
                    type = LsMemoryType.Shared;
                    return TryGetSharedMemoryOffsets(operation, out baseOffset, out constOffset);
                }
                else if (operation.StorageKind == StorageKind.LocalMemory)
                {
                    type = LsMemoryType.Local;
                    return TryGetLocalMemoryOffset(operation, out constOffset);
                }
            }

            type = default;
            constOffset = 0;
            return false;
        }

        private static bool TryGetSharedMemoryOffsets(Operation operation, out Operand baseOffset, out int constOffset)
        {
            baseOffset = null;
            constOffset = 0;

            // The byte offset is right shifted by 2 to get the 32-bit word offset,
            // so we want to get the byte offset back, since each one of those word
            // offsets are a new "local variable" which will not match.

            if (operation.GetSource(1).AsgOp is Operation shiftRightOp &&
                shiftRightOp.Inst == Instruction.ShiftRightU32 &&
                shiftRightOp.GetSource(1).Type == OperandType.Constant &&
                shiftRightOp.GetSource(1).Value == 2)
            {
                baseOffset = shiftRightOp.GetSource(0);
            }

            // Check if we have a constant offset being added to the base offset.

            if (baseOffset?.AsgOp is Operation addOp && addOp.Inst == Instruction.Add)
            {
                Operand src1 = addOp.GetSource(0);
                Operand src2 = addOp.GetSource(1);

                if (src1.Type == OperandType.Constant && src2.Type == OperandType.LocalVariable)
                {
                    constOffset = src1.Value;
                    baseOffset = src2;
                }
                else if (src1.Type == OperandType.LocalVariable && src2.Type == OperandType.Constant)
                {
                    baseOffset = src1;
                    constOffset = src2.Value;
                }
            }

            return baseOffset != null && baseOffset.Type == OperandType.LocalVariable;
        }

        private static bool TryGetLocalMemoryOffset(Operation operation, out int constOffset)
        {
            Operand offset = operation.GetSource(1);

            if (offset.Type == OperandType.Constant)
            {
                constOffset = offset.Value;
                return true;
            }

            constOffset = 0;
            return false;
        }
    }
}
