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
                    out int functionId,
                    out Operand site))
                {
                    return null;
                }

                return GenerateCallStorageOp(node, operation, null, functionId, site);
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

        private static LinkedListNode<INode> GenerateCallStorageOp(LinkedListNode<INode> node, Operation operation, Operand offset, int functionId, Operand site = null)
        {
            // Generate call to a helper function that will perform the storage buffer operation.

            Operand[] sources = new Operand[operation.SourcesCount - 1 + (offset == null ? 2 : 1) + (site != null ? 1 : 0)];

            if (site != null)
            {
                sources[^1] = site;   // nxnative: a raw load's bind-time pointer site (its last argument, TryGenerateRawStorageOp)
            }

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
            out int functionId,
            out Operand site)
        {
            site = null;
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

                if (operation.Inst == Instruction.Load)
                {
                    site = Const(FindPointerSite(resourceManager, gpuAccessor, operation));
                }
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

        // nxnative: bind-time pointer sites (dmsh_abi.h DMSH_GPTR_*): a storage buffer of entries laid out as the table's, one per site, under the same sentinel slot
        private const int GptrCbOffset = 2;
        private const int GptrMax = 64;

        // nxnative: a global memory access whose address the search traced to no storage buffer, an address the shader loaded from memory (Donkey Kong Bananza: gsys's vertex
        // shader that resets the terrain's indirect draw records reads each record's address from a storage buffer). Ryujinx drops such an access (a load reads 0). Here it
        // goes to the address itself: a tagged hint for its 2 MiB chunk, else a branchless binary search, finds the host's pool holding it, the address moves by that pool's delta (its Metal GPU address minus its
        // guest address) and is accessed through a physical storage buffer pointer. An address in no pool is still dropped
        private static bool TryGenerateRawStorageOp(
            GtsContext gtsContext,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            Operation operation,
            out int functionId)
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

            // A load takes its bind-time pointer site as a third argument (-1: none): the entry the host filled for it is tried before the table
            bool isLoad = operation.Inst == Instruction.Load;
            int siteBinding = 0;

            if (isLoad && !resourceManager.TryGetStorageBufferBinding(GmemMapCbSlot, GptrCbOffset, write: false, out siteBinding))
            {
                return false;
            }

            resourceManager.Properties.UsesPhysicalPointers = true;

            int inArgumentsCount = isLoad ? 3 : 2;

            if (operation.Inst == Instruction.AtomicCompareAndSwap)
            {
                inArgumentsCount = 4;
            }
            else if (operation.Inst == Instruction.Store || operation.Inst.IsAtomic())
            {
                inArgumentsCount = 3;
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

            Operand selected = Local();
            Operand lblFound = Label();
            Operand lblChosen = Label();

            // The delta of the pool holding the address: from the load's bind-time pointer site when the address is inside the entry the host filled for it, else from the table
            Operand deltaLow = Local();
            Operand deltaHigh = Local();
            Operand lblAccess = Label();

            if (isLoad)
            {
                Operand site = Argument(2);
                Operand lblNoSite = Label();
                context.BranchIfFalse(lblNoSite, context.ICompareNotEqual(site, Const(-1)));

                Operand siteEntry = context.IMultiply(site, Const(GmemMapEntryWords));

                Operand SiteWord(int word)
                {
                    return context.Load(StorageKind.StorageBuffer, siteBinding, Const(0), context.IAdd(siteEntry, Const(word)));
                }

                context.BranchIfFalse(lblNoSite, context.BitwiseAnd(AtOrBelow(SiteWord(1), SiteWord(0)), Below(SiteWord(3), SiteWord(2))));
                context.Copy(deltaLow, SiteWord(4));
                context.Copy(deltaHigh, SiteWord(5));
                context.Branch(lblAccess);
                context.MarkLabel(lblNoSite);
            }

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
            Operand index = Const(0);

            for (int step = 1 << (GmemMapLog2 - 1); step != 0; step >>= 1)
            {
                Operand candidate = context.IAdd(index, Const(step));
                Operand entry = context.IMultiply(candidate, Const(GmemMapEntryWords));
                index = context.ConditionalSelect(AtOrBelow(MapWord(entry, 1), MapWord(entry, 0)), candidate, index);
            }

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

            context.Copy(deltaLow, MapWord(found, 4));
            context.Copy(deltaHigh, MapWord(found, 5));
            context.MarkLabel(lblAccess);

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
                GetFunctionName(operation, isMultiTarget: true, []) + "Raw",
                returnsValue,
                inArgumentsCount,
                0);

            functionId = gtsContext.AddFunction(operation, isMultiTarget: true, [], function);

            return true;
        }

        // nxnative: a raw load's bind-time pointer site. Ocarina of Time's model shaders read five pointers at bytes 0-0x20 of storage slot 1 and load through each plus an index;
        // 99.5% of its raw-table accesses (2,173 shaders) have that form. The address's low word is the sum of a storage buffer word at a constant offset and index terms, and its high
        // word holds the next word of the same buffer: that pair is the pointer. When the high word names no such word, a single candidate in the low word is taken: the host's entry
        // only ever speeds up the lookup (the shader still checks the address against it), so a wrong guess costs a search, not a wrong read. Returns the site number, or -1
        private static int FindPointerSite(ResourceManager resourceManager, IGpuAccessor gpuAccessor, Operation operation)
        {
            List<(int, int)> low = [];
            List<(int, int)> high = [];
            CollectPointerWords(operation.GetSource(0), 0, low, []);
            CollectPointerWords(operation.GetSource(1), 0, high, []);

            (int, int)? pick = null;

            foreach ((int binding, int word) in low)
            {
                if (high.Contains((binding, word + 1)))
                {
                    if (pick != null && pick != (binding, word))
                    {
                        return -1;   // two pointers summed: none is the base
                    }

                    pick = (binding, word);
                }
            }

            if (pick == null && low.Count == 1)
            {
                pick = low[0];
            }

            if (pick is not (int pb, int pw) || !resourceManager.TryGetStorageBufferDescriptor(pb, out int cbSlot, out int cbOffset))
            {
                return -1;
            }

            var key = (cbSlot, cbOffset, pw * 4);

            if (!resourceManager.PointerSites.TryGetValue(key, out int site))
            {
                if (resourceManager.PointerSites.Count >= GptrMax)
                {
                    return -1;
                }

                site = resourceManager.PointerSites.Count;
                resourceManager.PointerSites.Add(key, site);
                gpuAccessor.Log($"gptr site {site}: c{cbSlot}[{cbOffset}] +{pw * 4}");
            }

            return site;
        }

        // The storage buffer words at constant offsets an address word is summed from: (binding, word offset), through copies, adds, ORs and masks
        private static void CollectPointerWords(Operand op, int depth, List<(int, int)> words, HashSet<INode> seen)
        {
            if (op == null || op.Type != OperandType.LocalVariable || depth > 16 || op.AsgOp is not Operation o || !seen.Add(o))
            {
                return;
            }

            switch (o.Inst)
            {
                case Instruction.Load:
                    if (o.StorageKind == StorageKind.StorageBuffer && o.SourcesCount == 3 && o.GetSource(0).Type == OperandType.Constant)
                    {
                        long? word = ConstValue(o.GetSource(2), 0);

                        if (word is long w && w >= 0 && !words.Contains((o.GetSource(0).Value, (int)w)))
                        {
                            words.Add((o.GetSource(0).Value, (int)w));
                        }
                    }

                    return;
                case Instruction.Copy:
                case Instruction.Add:
                case Instruction.BitwiseOr:
                    for (int i = 0; i < o.SourcesCount; i++)
                    {
                        CollectPointerWords(o.GetSource(i), depth + 1, words, seen);
                    }

                    return;
                case Instruction.Subtract:
                    CollectPointerWords(o.GetSource(0), depth + 1, words, seen);
                    return;
                case Instruction.BitwiseAnd:
                    if (o.GetSource(1).Type == OperandType.Constant)
                    {
                        CollectPointerWords(o.GetSource(0), depth + 1, words, seen);
                    }
                    else if (o.GetSource(0).Type == OperandType.Constant)
                    {
                        CollectPointerWords(o.GetSource(1), depth + 1, words, seen);
                    }

                    return;
            }
        }

        // A value made only of constants, or null
        private static long? ConstValue(Operand op, int depth)
        {
            if (op == null)
            {
                return 0;
            }

            if (op.Type == OperandType.Constant)
            {
                return op.Value;
            }

            if (op.Type != OperandType.LocalVariable || depth > 16 || op.AsgOp is not Operation o)
            {
                return null;
            }

            long? a = o.SourcesCount > 0 ? ConstValue(o.GetSource(0), depth + 1) : null;
            long? b = o.SourcesCount > 1 ? ConstValue(o.GetSource(1), depth + 1) : null;

            return o.Inst switch
            {
                Instruction.Copy => a,
                Instruction.Add when a.HasValue && b.HasValue => a + b,
                Instruction.Subtract when a.HasValue && b.HasValue => a - b,
                Instruction.Multiply when a.HasValue && b.HasValue => a * b,
                Instruction.ShiftLeft when a.HasValue && b.HasValue => a << (int)b,
                Instruction.ShiftRightU32 when a.HasValue && b.HasValue => (long)((uint)a >> (int)b),
                Instruction.BitwiseAnd when a.HasValue && b.HasValue => a & b,
                Instruction.BitwiseOr when a.HasValue && b.HasValue => a | b,
                _ => null,
            };
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
