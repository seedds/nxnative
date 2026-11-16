using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System.Collections.Generic;

using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Transforms
{
    // nxnative: warp-synchronous shared memory. Ampere code relies on a warp's lanes running in lockstep and leaves the barrier out where a lane stores to shared memory and then
    // loads a word another lane stored (Unreal's GPU radix sort downsweep, d032ce5090303e01 at 0x2eb0: STS / LDS [x-4] / STS / LDS [x-8]). For one thread that load and the earlier
    // store do not alias, so Metal's compiler moves the load above the store and reads the other lane's old value (Orbitals: Niagara's sorted index list wrong every frame, PR #1775).
    // A fence between them keeps the order; it is not an execution barrier, so it is safe in divergent code.
    //
    // The fence goes only where it is needed: before a shared memory load that some path reaches from a shared memory store with no barrier or fence in between. A forward pass over
    // each function's blocks carries "a store is pending" to a fixed point, so loops (a store at the bottom of a body, read at the top of the next iteration) and branches are followed
    // as the program runs. One fence orders every store before it, so a run of stores takes none until the load that follows them. The end of the shader needs none: a compute kernel
    // has nothing left to read, and the wrapper of a task or mesh shader puts a threadgroup barrier after its body (dmsh_meshstage.py). A call is followed through its callee's
    // summary (Summary), so it runs once all of the shader's functions are translated. The text rule this replaces put a fence after every store: 25,108 in 699 of Ocarina of Time's
    // shaders, 17,641 of them in its 500 mesh shaders after the stage output writes, and one foliage mesh pipeline crashed Apple's compiler on an M2 in every run until they were
    // removed (GitHub issue #2481).
    //
    // The fence is a call of the helper dmsh_tg_fence (HelperFunctionManager.GenerateSharedFence), whose body the shader build makes the simdgroup-scope fence (dmsh_rewrite.py tg_fence).
    // Stage memory (a task or mesh shader's inputs and outputs, InstEmitAmpereMem.AIsberd / AStageOut) lies in shared memory and is treated as shared memory.
    static class SharedMemoryFence
    {
        // What a call does to the pending state. EntryRead: a load in the callee can follow a store made before the call (so a pending store needs a fence before the call);
        // FromClean / FromPending: whether a store is pending when it returns, called with none pending or with one
        private readonly record struct Summary(bool EntryRead, bool FromClean, bool FromPending);

        private static readonly Summary _transparent = new(false, false, true);   // touches no shared memory
        private static readonly Summary _unknown = new(true, true, true);

        public static void RunPass(List<Function> functions, int ownCount, HelperFunctionManager hfm, ShaderStage stage, FeatureFlags usedFeatures)
        {
            if (stage != ShaderStage.Compute || !usedFeatures.HasFlag(FeatureFlags.SharedMemory))
            {
                return;
            }

            Dictionary<Function, Summary> summaries = new();
            List<LinkedListNode<INode>> fenced = [];

            for (int i = 0; i < ownCount; i++)
            {
                if (functions[i] != null && functions[i].Blocks.Length != 0)
                {
                    Analyse(functions, summaries, functions[i], false, fenced);   // its fences as called with nothing pending: a caller fences before a call whose callee needs it
                }
            }

            if (fenced.Count == 0)
            {
                return;
            }

            int fenceId = hfm.GetOrCreateFunctionId(HelperFunctionName.SharedFence);

            foreach (LinkedListNode<INode> node in fenced)
            {
                node.List.AddBefore(node, new Operation(Instruction.Call, 0, (Operand)null, Const(fenceId)));
            }
        }

        // Runs the pass over a function entered with the given pending state → whether a store is pending at any of its returns. fenced (when given) collects the nodes that need a
        // fence before them; reads (when given) collects the loads that see a pending store
        private static bool Analyse(List<Function> functions, Dictionary<Function, Summary> summaries, Function function, bool entry, List<LinkedListNode<INode>> fenced, HashSet<LinkedListNode<INode>> reads = null)
        {
            BasicBlock[] blocks = function.Blocks;
            Dictionary<BasicBlock, bool> pendingIn = new();

            foreach (BasicBlock block in blocks)
            {
                pendingIn[block] = false;
            }

            pendingIn[blocks[0]] = entry;

            for (bool changed = true; changed;)
            {
                changed = false;

                foreach (BasicBlock block in blocks)
                {
                    if (!Transfer(functions, summaries, block, pendingIn[block], null))
                    {
                        continue;
                    }

                    foreach (BasicBlock successor in new[] { block.Next, block.Branch })
                    {
                        if (successor != null && pendingIn.TryGetValue(successor, out bool pending) && !pending)
                        {
                            pendingIn[successor] = true;
                            changed = true;
                        }
                    }
                }
            }

            bool exit = false;
            List<LinkedListNode<INode>> seen = [];

            foreach (BasicBlock block in blocks)
            {
                bool pendingOut = Transfer(functions, summaries, block, pendingIn[block], seen);

                if (block.Next == null && block.Branch == null)
                {
                    exit |= pendingOut;
                }
            }

            fenced?.AddRange(seen);
            reads?.UnionWith(seen);

            return exit;
        }

        // The pending state at the block's end from the one at its start; fenced (when given) collects the nodes that need a fence before them
        private static bool Transfer(List<Function> functions, Dictionary<Function, Summary> summaries, BasicBlock block, bool pending, List<LinkedListNode<INode>> fenced)
        {
            for (LinkedListNode<INode> node = block.Operations.First; node != null; node = node.Next)
            {
                if (node.Value is not Operation operation)
                {
                    continue;
                }

                Instruction inst = operation.Inst & Instruction.Mask;

                if (IsOrdering(inst))
                {
                    pending = false;
                }
                else if (inst == Instruction.Call)
                {
                    Summary s = Summarise(functions, summaries, operation.GetSource(0).Value);

                    if (pending && s.EntryRead)
                    {
                        fenced?.Add(node);
                        pending = false;
                    }

                    pending = pending ? s.FromPending : s.FromClean;
                }
                else if (inst == Instruction.MatrixLoad || (IsShared(operation.StorageKind) && (inst == Instruction.Load || inst.IsAtomic())))
                {
                    if (pending)
                    {
                        fenced?.Add(node);
                    }

                    pending = inst.IsAtomic();   // a load leaves nothing pending; an atomic stored
                }
                else if (inst == Instruction.Store && IsShared(operation.StorageKind))
                {
                    pending = true;
                }
            }

            return pending;
        }

        private static Summary Summarise(List<Function> functions, Dictionary<Function, Summary> summaries, int functionId)
        {
            Function callee = functionId >= 0 && functionId < functions.Count ? functions[functionId] : null;

            if (callee == null || callee.Blocks.Length == 0)
            {
                return callee == null ? _unknown : _transparent;
            }

            if (summaries.TryGetValue(callee, out Summary known))
            {
                return known;
            }

            // Helpers whose bodies the shader build replaces in the Metal source, by the names that are its contract
            Summary s;

            if (callee.Name == HelperFunctionManager.SharedFenceName)
            {
                s = new(false, false, false);
            }
            else if (callee.Name.StartsWith("dmsh_shared_store"))
            {
                s = new(false, true, true);   // a plain byte or half-word store (dmsh_rewrite.py shared_store_msl)
            }
            else if (callee.Name.StartsWith("dmsh_hmma"))
            {
                // Reads A straight from the shader's shared memory (dmsh_rewrite.py hmma_msl, FlagADirect) and stores only to a scratch of its own, whose order it keeps itself
                s = new(true, false, false);
            }
            else
            {
                summaries[callee] = _unknown;   // a call cycle meets the conservative answer

                HashSet<LinkedListNode<INode>> clean = [], dirty = [];
                bool fromClean = Analyse(functions, summaries, callee, false, null, clean);
                bool fromPending = Analyse(functions, summaries, callee, true, null, dirty);

                s = new(dirty.Count > clean.Count, fromClean, fromPending);
            }

            summaries[callee] = s;

            return s;
        }

        // A barrier or memory barrier orders this lane's shared memory accesses on both sides (Barrier: BAR.SYNC; GroupMemoryBarrier / MemoryBarrier: MEMBAR, which the shader build
        // makes fences on threadgroup memory too, membar_fence; SubgroupBarrier: LDSM's, InstEmitAmpereTensor.ALdsm)
        private static bool IsOrdering(Instruction inst)
        {
            return inst is Instruction.Barrier or Instruction.GroupMemoryBarrier or Instruction.MemoryBarrier or Instruction.SubgroupBarrier;
        }

        private static bool IsShared(StorageKind kind)
        {
            return kind is StorageKind.SharedMemory or StorageKind.SharedMemory8 or StorageKind.SharedMemory16;
        }
    }
}
