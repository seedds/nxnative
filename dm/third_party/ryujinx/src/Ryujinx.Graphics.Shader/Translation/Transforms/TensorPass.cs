using Ryujinx.Graphics.Shader.Instructions;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation.Optimizations;
using System;
using System.Collections.Generic;
using System.Diagnostics;

using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Transforms
{
    // nxnative: MatrixMultiplyAdd (Ampere HMMA.16816 with an f16 accumulator, InstEmitAmpereTensor.AHmma) becomes a call of the helper dmsh_hmma16816_f16, whose body the
    // shader build replaces with simdgroup_matrix code in the Metal source (dmsh_build.py hmma_msl). Arguments: the lane, the subgroup's block offset in the scratch, A (4), B (2),
    // C (2), then what Prepare found: the flags below, and for FlagADirect the row address of the LDSM that read A, the offset of each of A's four matrices from lane 0's row and
    // the row stride (bytes); destinations: D (2), the first as the call's result, the second through its out argument (as ShufflePass does with its valid flag).
    // The translator's own body reads only the registers, so each flag is a promise about values that body computes anyway: the Metal body uses them to keep matrices in the
    // SIMD-group's registers from one call to the next (an accumulator through a chain of HMMAs, A across calls that share it) and to load A where its LDSM read it, instead of
    // taking every fragment through the scratch. MatrixLoad (one matrix of an LDSM in a compute shader) becomes its per-lane emulation here, after Prepare has seen which HMMAs
    // it feeds; the Metal compiler drops that code where every HMMA it feeds loads A by itself. On Ocarina of Time's NDVE movie networks (11 compute shaders, 14 dispatches a
    // frame, 2,217 HMMAs, 1,890 of them continuing a chain) the frame's compute went from 26.7 to 18.9 ms on an M4 (8 GPU cores)
    class TensorPass : ITransformPass
    {
        public const int FlagAReuse = 1;     // A is the previous call's (the call before it in the same block): its matrices are still loaded
        public const int FlagCSlot = 2;      // C is the D of an earlier call of the block, left in accumulator slot flags >> 8, and has no other use
        public const int FlagDSlot = 4;      // D is a later call's C and nothing else's: it stays in the slot, the registers are not written
        public const int FlagCZero = 8;      // C is zero
        public const int FlagADirect = 16;   // A is matrices of one LDSM of the block with no shared memory write between: rows at the LDSM's lane 0 address + offset, stride apart
        public const int FlagATrans = 32;    // that LDSM is .MT88 (A is the transpose of what lies in memory)
        public const int Slots = 8;          // accumulator slots, two 8 x 8 matrices each

        // lane, block, A (4), B (2), C (2), flags, LDSM address, A's four offsets, row stride
        public const int HmmaInArguments = 17;

        // DMSH_TENSOR_TRACE=1 (dmsh_xlate on one job): why each HMMA that loads its A does not load it where an LDSM read it
        private static readonly bool Trace = Environment.GetEnvironmentVariable("DMSH_TENSOR_TRACE") == "1";

        private static string DescribeA(Operation op)
        {
            string r = "";

            for (int k = 3; k < 7; k++)
            {
                Operand a = op.GetSource(k);
                r += a.Type == OperandType.LocalVariable && a.AsgOp is Operation d ? d.Inst + "/" + d.StorageKind + " " : a.Type + " ";
            }

            return r;
        }

        public static bool IsEnabled(IGpuAccessor gpuAccessor, ShaderStage stage, TargetLanguage targetLanguage, FeatureFlags usedFeatures)
        {
            return true;
        }

        // Before any other transform: the flags of each MatrixMultiplyAdd, appended to its sources
        public static void Prepare(TransformContext context)
        {
            Uniformity uniformity = null;

            foreach (BasicBlock block in context.Blocks)
            {
                List<Operation> mmas = [];
                Dictionary<Operation, int> epochAt = [];
                int epoch = 0;   // shared memory writes so far in the block

                foreach (INode node in block.Operations)
                {
                    if (node is not Operation op)
                    {
                        continue;
                    }

                    if (op.Inst == Instruction.MatrixLoad)
                    {
                        epochAt[op] = epoch;
                    }
                    else if (op.Inst == Instruction.MatrixMultiplyAdd)
                    {
                        mmas.Add(op);
                        epochAt[op] = epoch;
                    }
                    else if (WritesShared(op))
                    {
                        epoch++;
                    }
                }

                if (mmas.Count == 0)
                {
                    continue;
                }

                // Chains: a call whose C is the D of an earlier call that nothing else reads
                int n = mmas.Count;
                int[] prev = new int[n], next = new int[n], slot = new int[n];
                Array.Fill(prev, -1);
                Array.Fill(next, -1);
                Dictionary<Operation, int> index = [];

                for (int i = 0; i < n; i++)
                {
                    index[mmas[i]] = i;
                }

                for (int i = 0; i < n; i++)
                {
                    Operand c0 = mmas[i].GetSource(9), c1 = mmas[i].GetSource(10);

                    if (c0.Type == OperandType.LocalVariable && c0.AsgOp is Operation from && index.TryGetValue(from, out int j) && j < i &&
                        from.GetDest(0) == c0 && from.GetDest(1) == c1 && OnlyAsC(mmas[i], c0, c1))
                    {
                        prev[i] = j;
                        next[j] = i;
                    }
                }

                bool[] busy = new bool[Slots];

                for (int i = 0; i < n; i++)
                {
                    Operation op = mmas[i];

                    if (prev[i] >= 0)
                    {
                        slot[i] = slot[prev[i]];
                    }
                    else if (next[i] >= 0)
                    {
                        int free = Array.IndexOf(busy, false);

                        if (free < 0)
                        {
                            // More chains at once than slots: this one goes through the registers
                            prev[next[i]] = -1;
                            next[i] = -1;
                        }
                        else
                        {
                            busy[free] = true;
                            slot[i] = free;
                        }
                    }

                    if (prev[i] >= 0 && next[i] < 0)
                    {
                        busy[slot[i]] = false;
                    }

                    int flags = 0;

                    if (prev[i] >= 0)
                    {
                        flags |= FlagCSlot | slot[i] << 8;
                    }

                    if (next[i] >= 0)
                    {
                        flags |= FlagDSlot | slot[i] << 8;
                    }

                    if (prev[i] < 0 && IsZero(op.GetSource(9)) && IsZero(op.GetSource(10)))
                    {
                        flags |= FlagCZero;
                    }

                    if (i > 0 && SameA(mmas[i - 1], op))
                    {
                        flags |= FlagAReuse;
                    }

                    Operand addr = Const(0);
                    int[] offsets = new int[4];
                    int stride = 0;
                    Operation[] loads = LoadsOfA(op);
                    bool sameEpoch = loads != null && Array.TrueForAll(loads, l => epochAt.TryGetValue(l, out int e) && e == epochAt[op]);

                    if (Trace && (flags & FlagAReuse) == 0 && !sameEpoch)
                    {
                        Console.Error.WriteLine($"[tensor] block {block.Index} HMMA {i}: A {(loads == null ? "not the matrices of one LDSM: " + DescribeA(op) : Array.TrueForAll(loads, epochAt.ContainsKey) ? "read before a shared memory write" : "read by an LDSM in another block")}");
                    }

                    if (sameEpoch)
                    {
                        uniformity ??= new Uniformity(context, Trace);
                        Operand rows = loads[0].GetSource(2);
                        int count = 1;

                        foreach (Operation l in loads)
                        {
                            count = Math.Max(count, l.GetSource(3).Value + 1);
                        }

                        if (uniformity.RowStarts(rows, count, out int[] starts, out stride))
                        {
                            flags |= FlagADirect | (loads[0].GetSource(1).Value == 1 ? FlagATrans : 0);
                            addr = rows;

                            for (int k = 0; k < 4; k++)
                            {
                                offsets[k] = starts[loads[k].GetSource(3).Value];
                            }
                        }
                        else if (Trace)
                        {
                            Console.Error.WriteLine($"[tensor] block {block.Index} HMMA {i}: A's LDSM rows are not U + f(lane) with f strided: " + uniformity.Describe(rows, 0));
                        }
                    }

                    op.AppendSources([Const(flags), addr, Const(offsets[0]), Const(offsets[1]), Const(offsets[2]), Const(offsets[3]), Const(stride)]);
                }
            }
        }

        public static LinkedListNode<INode> RunPass(TransformContext context, LinkedListNode<INode> node)
        {
            Operation operation = (Operation)node.Value;

            if (operation.Inst == Instruction.MatrixLoad)
            {
                return LowerLoad(context, node, operation);
            }

            if (operation.Inst != Instruction.MatrixMultiplyAdd)
            {
                return node;
            }

            Operand memoryId = operation.GetSource(0);

            Debug.Assert(memoryId.Type == OperandType.Constant && operation.SourcesCount == 1 + HmmaInArguments);

            int functionId = context.Hfm.GetOrCreateFunctionId(HelperFunctionName.Hmma16816F16, memoryId.Value);

            Operand d0 = operation.GetDest(0);
            Operand d1 = operation.GetDest(1);

            operation.Dest = null;

            Operand[] callArgs = new Operand[HmmaInArguments + 2];
            callArgs[0] = Const(functionId);

            for (int i = 0; i < HmmaInArguments; i++)
            {
                callArgs[1 + i] = operation.GetSource(1 + i);
            }

            callArgs[1 + HmmaInArguments] = d1;

            LinkedListNode<INode> newNode = node.List.AddBefore(node, new Operation(Instruction.Call, 0, d0, callArgs));

            Utils.DeleteNode(node, operation);

            return newNode;
        }

        // MatrixLoad (shared memory id, mode, row address, matrix) → InstEmit.LdsmLane, as InstEmitAmpereTensor.ALdsm emits it outside compute shaders
        private static LinkedListNode<INode> LowerLoad(TransformContext context, LinkedListNode<INode> node, Operation operation)
        {
            EmitterContext emit = new();
            Operand lane = emit.Load(StorageKind.Input, IoVariable.SubgroupLaneId);

            if (context.GpuAccessor.QueryHostSubgroupSize() > 32)
            {
                lane = emit.BitwiseAnd(lane, Const(0x1f));
            }

            Operand g = emit.ShiftRightU32(lane, Const(2));
            Operand t = emit.BitwiseAnd(lane, Const(3));

            emit.Copy(operation.Dest, InstEmit.LdsmLane(emit, g, t, operation.GetSource(2), operation.GetSource(0).Value, operation.GetSource(3).Value, operation.GetSource(1).Value));

            LinkedListNode<INode> last = node;

            foreach (Operation op in emit.GetOperations())
            {
                last = node.List.AddBefore(node, op);
            }

            Utils.DeleteNode(node, operation);

            return last;
        }

        // Whether an operation may write shared memory, or let the other subgroups' writes show (a barrier); a call is taken to do either
        private static bool WritesShared(Operation op)
        {
            Instruction inst = op.Inst & Instruction.Mask;

            if (inst == Instruction.Store || inst.IsAtomic())
            {
                return op.StorageKind is StorageKind.SharedMemory or StorageKind.SharedMemory8 or StorageKind.SharedMemory16;
            }

            return inst is Instruction.Barrier or Instruction.GroupMemoryBarrier or Instruction.MemoryBarrier or Instruction.Call;
        }

        private static bool IsZero(Operand x) => x.Type == OperandType.Constant && x.Value == 0;

        // c0, c1 read by op as its C and by nothing else
        private static bool OnlyAsC(Operation op, Operand c0, Operand c1)
        {
            if (c0 == c1 || c0.UseOps.Count != 1 || c1.UseOps.Count != 1)
            {
                return false;
            }

            for (int s = 0; s < op.SourcesCount; s++)
            {
                if (s != 9 && s != 10 && (op.GetSource(s) == c0 || op.GetSource(s) == c1))
                {
                    return false;
                }
            }

            return true;
        }

        private static bool SameA(Operation a, Operation b)
        {
            for (int k = 3; k < 7; k++)
            {
                Operand x = a.GetSource(k), y = b.GetSource(k);

                if (x != y && !(x.Type == OperandType.Constant && y.Type == OperandType.Constant && x.Value == y.Value))
                {
                    return false;
                }
            }

            return true;
        }

        // The MatrixLoads of A's four registers, all of one LDSM (the same row address and mode), or null
        private static Operation[] LoadsOfA(Operation op)
        {
            Operation[] loads = new Operation[4];

            for (int k = 0; k < 4; k++)
            {
                Operand a = op.GetSource(3 + k);

                if (a.Type != OperandType.LocalVariable || a.AsgOp is not Operation def || def.Inst != Instruction.MatrixLoad ||
                    (k != 0 && (def.GetSource(2) != loads[0].GetSource(2) || def.GetSource(1).Value != loads[0].GetSource(1).Value)))
                {
                    return null;
                }

                loads[k] = def;
            }

            return loads;
        }

        // Which values are the same in every lane of a subgroup (where all 32 run together, as at an HMMA), and of the others, which are U + f(lane) with f known here:
        // the lane-dependent part of an LDSM's row addresses. Divergent: the lane, the thread index's components that change within a subgroup, local memory, atomics, calls,
        // textures, any operation not listed in Pure, phis where paths meet (all but the phis of a loop's header whose exits all branch on uniform conditions, and, when the loop
        // has several back edges, whose other branches do too: lanes that took different back edges bring the header different values), and what is computed from them
        private sealed class Uniformity
        {
            private readonly HashSet<Operand> _divergent = [];
            private readonly Dictionary<Operand, (int[] Lanes, bool Pure)?> _forms = [];
            private readonly Dictionary<Operand, string> _phiWhy;   // DMSH_TENSOR_TRACE: why a phi is divergent
            private readonly int _sizeX, _sizeY;

            public Uniformity(TransformContext context, bool trace)
            {
                _phiWhy = trace ? [] : null;
                _sizeX = context.Definitions.ComputeLocalSizeX;
                _sizeY = context.Definitions.ComputeLocalSizeY;

                Dictionary<BasicBlock, Loop> loops = [];

                foreach (BasicBlock block in context.Blocks)
                {
                    Loop loop = LoopOf(block);

                    if (loop != null)
                    {
                        loops[block] = loop;
                    }
                }

                bool changed;

                do
                {
                    changed = false;

                    foreach (BasicBlock block in context.Blocks)
                    {
                        foreach (INode node in block.Operations)
                        {
                            bool fresh = false;

                            for (int d = 0; d < node.DestsCount; d++)
                            {
                                Operand dest = node.GetDest(d);
                                fresh |= dest != null && dest.Type == OperandType.LocalVariable && !_divergent.Contains(dest);
                            }

                            if (fresh && Diverges(node, block, loops))
                            {
                                for (int d = 0; d < node.DestsCount; d++)
                                {
                                    Operand dest = node.GetDest(d);

                                    if (dest != null && dest.Type == OperandType.LocalVariable)
                                    {
                                        _divergent.Add(dest);
                                    }
                                }

                                changed = true;
                            }
                        }
                    }
                }
                while (changed);
            }

            // The blocks of a loop whose branches its header's phis depend on: those that branch out of it (Exits), and with several back edges the others (Inner)
            private sealed record Loop(List<BasicBlock> Exits, List<BasicBlock> Inner);

            // The loop headed by `header`, or null when it heads none
            private static Loop LoopOf(BasicBlock header)
            {
                HashSet<BasicBlock> body = [header];
                Stack<BasicBlock> work = [];
                int latches = 0;

                foreach (BasicBlock pred in header.Predecessors)
                {
                    if (Dominates(header, pred))
                    {
                        latches++;

                        if (body.Add(pred))
                        {
                            work.Push(pred);
                        }
                    }
                }

                if (latches == 0)
                {
                    return null;
                }

                while (work.Count != 0)
                {
                    foreach (BasicBlock pred in work.Pop().Predecessors)
                    {
                        if (body.Add(pred))
                        {
                            work.Push(pred);
                        }
                    }
                }

                List<BasicBlock> exits = [], inner = [];

                foreach (BasicBlock block in body)
                {
                    if ((block.Next != null && !body.Contains(block.Next)) || (block.Branch != null && !body.Contains(block.Branch)))
                    {
                        exits.Add(block);
                    }
                    else if (latches > 1)
                    {
                        inner.Add(block);
                    }
                }

                return new Loop(exits, inner);
            }

            private static bool Dominates(BasicBlock a, BasicBlock b)
            {
                for (BasicBlock x = b; x != null; x = x.ImmediateDominator == x ? null : x.ImmediateDominator)
                {
                    if (x == a)
                    {
                        return true;
                    }
                }

                return false;
            }

            // The first of the blocks that ends in a conditional branch on a divergent condition, or null
            private BasicBlock DivergentBranch(List<BasicBlock> blocks)
            {
                foreach (BasicBlock b in blocks)
                {
                    if (b.GetLastOp() is Operation branch && branch.Inst is Instruction.BranchIfTrue or Instruction.BranchIfFalse && IsDivergent(branch.GetSource(0)))
                    {
                        return b;
                    }
                }

                return null;
            }

            private bool IsDivergent(Operand x) => x != null && x.Type == OperandType.LocalVariable && _divergent.Contains(x);

            private bool AnySourceDivergent(INode node)
            {
                for (int s = 0; s < node.SourcesCount; s++)
                {
                    if (IsDivergent(node.GetSource(s)))
                    {
                        return true;
                    }
                }

                return false;
            }

            private bool Diverges(INode node, BasicBlock block, Dictionary<BasicBlock, Loop> loops)
            {
                if (node is PhiNode)
                {
                    if (!loops.TryGetValue(block, out Loop loop))
                    {
                        _phiWhy?.TryAdd(node.GetDest(0), $"paths meet in block {block.Index}");
                        return true;
                    }

                    if (DivergentBranch(loop.Exits) is BasicBlock exit)
                    {
                        _phiWhy?.TryAdd(node.GetDest(0), $"loop {block.Index} leaves in block {exit.Index} on " + Describe(((Operation)exit.GetLastOp()).GetSource(0), 0));
                        return true;
                    }

                    if (DivergentBranch(loop.Inner) is BasicBlock inner)
                    {
                        _phiWhy?.TryAdd(node.GetDest(0), $"loop {block.Index} has several back edges and branches in block {inner.Index} on " + Describe(((Operation)inner.GetLastOp()).GetSource(0), 0));
                        return true;
                    }

                    _phiWhy?.TryAdd(node.GetDest(0), $"a divergent source in loop {block.Index}");
                    return AnySourceDivergent(node);
                }

                Operation op = (Operation)node;

                if (op.Inst == Instruction.Load)
                {
                    return op.StorageKind switch
                    {
                        StorageKind.Input => !UniformInput(op),
                        StorageKind.ConstantBuffer or StorageKind.StorageBuffer or StorageKind.SharedMemory => AnySourceDivergent(op),
                        _ => true,
                    };
                }

                return !Pure(op.Inst) || AnySourceDivergent(op);
            }

            private bool UniformInput(Operation op)
            {
                IoVariable variable = (IoVariable)op.GetSource(0).Value;

                if (variable == IoVariable.CtaId)
                {
                    return true;
                }

                if (variable == IoVariable.ThreadId && op.SourcesCount == 2 && op.GetSource(1).Type == OperandType.Constant)
                {
                    return op.GetSource(1).Value switch
                    {
                        1 => _sizeX % 32 == 0,
                        2 => (_sizeX * _sizeY) % 32 == 0,
                        _ => false,
                    };
                }

                return false;
            }

            private static bool Pure(Instruction inst)
            {
                switch (inst & Instruction.Mask)
                {
                    case Instruction.Add:
                    case Instruction.Subtract:
                    case Instruction.Multiply:
                    case Instruction.MultiplyHighS32:
                    case Instruction.MultiplyHighU32:
                    case Instruction.Divide:
                    case Instruction.Modulo:
                    case Instruction.Negate:
                    case Instruction.Minimum:
                    case Instruction.MinimumU32:
                    case Instruction.Maximum:
                    case Instruction.MaximumU32:
                    case Instruction.Clamp:
                    case Instruction.ClampU32:
                    case Instruction.ShiftLeft:
                    case Instruction.ShiftRightS32:
                    case Instruction.ShiftRightU32:
                    case Instruction.BitwiseAnd:
                    case Instruction.BitwiseOr:
                    case Instruction.BitwiseExclusiveOr:
                    case Instruction.BitwiseNot:
                    case Instruction.BitfieldExtractS32:
                    case Instruction.BitfieldExtractU32:
                    case Instruction.BitfieldInsert:
                    case Instruction.BitCount:
                    case Instruction.FindLSB:
                    case Instruction.FindMSBS32:
                    case Instruction.FindMSBU32:
                    case Instruction.ConditionalSelect:
                    case Instruction.CompareEqual:
                    case Instruction.CompareNotEqual:
                    case Instruction.CompareLess:
                    case Instruction.CompareLessU32:
                    case Instruction.CompareLessOrEqual:
                    case Instruction.CompareLessOrEqualU32:
                    case Instruction.CompareGreater:
                    case Instruction.CompareGreaterU32:
                    case Instruction.CompareGreaterOrEqual:
                    case Instruction.CompareGreaterOrEqualU32:
                    case Instruction.LogicalAnd:
                    case Instruction.LogicalOr:
                    case Instruction.LogicalNot:
                    case Instruction.LogicalExclusiveOr:
                    case Instruction.ConvertFP32ToS32:
                    case Instruction.ConvertFP32ToU32:
                    case Instruction.ConvertS32ToFP32:
                    case Instruction.ConvertU32ToFP32:
                    case Instruction.ConvertFP32ToFP64:
                    case Instruction.ConvertFP64ToFP32:
                    case Instruction.ConvertFP64ToS32:
                    case Instruction.ConvertFP64ToU32:
                    case Instruction.ConvertS32ToFP64:
                    case Instruction.ConvertU32ToFP64:
                    case Instruction.Absolute:
                    case Instruction.Ceiling:
                    case Instruction.Floor:
                    case Instruction.Round:
                    case Instruction.Truncate:
                    case Instruction.FusedMultiplyAdd:
                    case Instruction.SquareRoot:
                    case Instruction.ReciprocalSquareRoot:
                    case Instruction.ExponentB2:
                    case Instruction.LogarithmB2:
                    case Instruction.Sine:
                    case Instruction.Cosine:
                    case Instruction.IsNan:
                    case Instruction.PackHalf2x16:
                    case Instruction.UnpackHalf2x16:
                    case Instruction.PackDouble2x32:
                    case Instruction.UnpackDouble2x32:
                    case Instruction.Copy:
                        return true;
                }

                return false;
            }

            // Lane L's row of an LDSM at x: U + starts[m] + r * stride for lane 8m + r of each of its `count` matrices, U the same in every lane. starts is relative to lane 0's row
            public bool RowStarts(Operand x, int count, out int[] starts, out int stride)
            {
                starts = new int[count];
                stride = 0;

                if (Form(x, 0) is not (int[] lanes, _))
                {
                    return false;
                }

                stride = lanes[1] - lanes[0];

                // An LDSM's rows are 16-byte aligned
                if (stride <= 0 || stride % 16 != 0)
                {
                    return false;
                }

                for (int m = 0; m < count; m++)
                {
                    starts[m] = lanes[8 * m] - lanes[0];

                    if (starts[m] % 16 != 0)
                    {
                        return false;
                    }

                    for (int r = 1; r < 8; r++)
                    {
                        if (lanes[8 * m + r] != lanes[8 * m] + r * stride)
                        {
                            return false;
                        }
                    }
                }

                return true;
            }

            // An operand as text for DMSH_TENSOR_TRACE: U for a uniform value, the operations of the others with their sources, eight levels deep
            public string Describe(Operand x, int depth)
            {
                if (x.Type == OperandType.Constant)
                {
                    return x.Value.ToString();
                }

                if (x.Type != OperandType.LocalVariable)
                {
                    return x.Type.ToString();
                }

                if (!_divergent.Contains(x))
                {
                    return "U";
                }

                if (depth > 8)
                {
                    return "…";
                }

                if (x.AsgOp is PhiNode phi)
                {
                    string r = "phi[" + (_phiWhy != null && _phiWhy.TryGetValue(x, out string why) ? why : "") + "](";

                    for (int s = 0; s < phi.SourcesCount; s++)
                    {
                        r += (s != 0 ? ", " : "") + Describe(phi.GetSource(s), depth + 1);
                    }

                    return r + ")";
                }

                if (x.AsgOp is not Operation op)
                {
                    return "?";
                }

                string t = op.Inst + (op.Inst == Instruction.Load ? "/" + op.StorageKind : "") + "(";

                for (int s = 0; s < op.SourcesCount; s++)
                {
                    t += (s != 0 ? ", " : "") + Describe(op.GetSource(s), depth + 1);
                }

                return t + ")";
            }

            // x = U + Lanes[lane] with U the same in every lane (zero when Pure), or null when it is not of that form
            private (int[] Lanes, bool Pure)? Form(Operand x, int depth)
            {
                if (x.Type == OperandType.Constant)
                {
                    return (Fill(x.Value), true);
                }

                if (x.Type != OperandType.LocalVariable || depth > 32)
                {
                    return null;
                }

                if (!_divergent.Contains(x))
                {
                    return (Fill(0), false);
                }

                if (!_forms.TryGetValue(x, out (int[] Lanes, bool Pure)? form))
                {
                    form = Compute(x, depth);
                    _forms[x] = form;
                }

                return form;
            }

            private (int[] Lanes, bool Pure)? Compute(Operand x, int depth)
            {
                if (x.AsgOp is not Operation op || (op.Inst & ~Instruction.Mask) != 0)
                {
                    return null;
                }

                if (op.Inst == Instruction.Load)
                {
                    return op.StorageKind == StorageKind.Input ? InputForm(op) : null;
                }

                if (op.Inst == Instruction.Copy)
                {
                    return Form(op.GetSource(0), depth + 1);
                }

                if (op.SourcesCount == 0 || op.SourcesCount > 2)
                {
                    return null;
                }

                (int[] Lanes, bool Pure)? fa = Form(op.GetSource(0), depth + 1);
                (int[] Lanes, bool Pure)? fb = op.SourcesCount == 2 ? Form(op.GetSource(1), depth + 1) : (Fill(0), true);

                if (fa is not (int[] a, bool pa) || fb is not (int[] b, bool pb))
                {
                    return null;
                }

                bool constA = op.GetSource(0).Type == OperandType.Constant;
                bool constB = op.SourcesCount == 2 && op.GetSource(1).Type == OperandType.Constant;

                // Linear in U: sums, and products and left shifts by a constant
                switch (op.Inst)
                {
                    case Instruction.Add:
                        return (Map(a, b, (u, v) => u + v), pa && pb);
                    case Instruction.Subtract:
                        return (Map(a, b, (u, v) => u - v), pa && pb);
                    case Instruction.Multiply when constA || constB:
                        return (Map(a, b, (u, v) => u * v), constA ? pb : pa);
                    case Instruction.ShiftLeft when constB:
                        return (Map(a, b, (u, v) => u << (v & 31)), pa);
                }

                if (!pa || !pb)
                {
                    return null;
                }

                Func<int, int, int> f = op.Inst switch
                {
                    Instruction.Multiply => (u, v) => u * v,
                    Instruction.ShiftLeft => (u, v) => u << (v & 31),
                    Instruction.ShiftRightU32 => (u, v) => (int)((uint)u >> (v & 31)),
                    Instruction.ShiftRightS32 => (u, v) => u >> (v & 31),
                    Instruction.BitwiseAnd => (u, v) => u & v,
                    Instruction.BitwiseOr => (u, v) => u | v,
                    Instruction.BitwiseExclusiveOr => (u, v) => u ^ v,
                    Instruction.BitwiseNot => (u, _) => ~u,
                    Instruction.Minimum => Math.Min,
                    Instruction.Maximum => Math.Max,
                    Instruction.MinimumU32 => (u, v) => (int)Math.Min((uint)u, (uint)v),
                    Instruction.MaximumU32 => (u, v) => (int)Math.Max((uint)u, (uint)v),
                    _ => null,
                };

                return f != null ? (Map(a, b, f), true) : null;
            }

            // The lane and the thread index: a subgroup is 32 consecutive threads of the workgroup in x, then y order
            private (int[] Lanes, bool Pure)? InputForm(Operation op)
            {
                IoVariable variable = (IoVariable)op.GetSource(0).Value;
                int[] lanes = new int[32];

                if (variable == IoVariable.SubgroupLaneId && op.SourcesCount == 1)
                {
                    for (int l = 0; l < 32; l++)
                    {
                        lanes[l] = l;
                    }

                    return (lanes, true);
                }

                if (variable != IoVariable.ThreadId || op.SourcesCount != 2 || op.GetSource(1).Type != OperandType.Constant)
                {
                    return null;
                }

                int component = op.GetSource(1).Value;

                if (component == 0 && _sizeX % 32 == 0)
                {
                    for (int l = 0; l < 32; l++)
                    {
                        lanes[l] = l;
                    }

                    return (lanes, false);
                }

                if (_sizeX < 32 && 32 % _sizeX == 0 && (_sizeX * _sizeY) % 32 == 0 && component < 2)
                {
                    for (int l = 0; l < 32; l++)
                    {
                        lanes[l] = component == 0 ? l % _sizeX : l / _sizeX;
                    }

                    return (lanes, component == 0);
                }

                return null;
            }

            private static int[] Fill(int value)
            {
                int[] lanes = new int[32];
                Array.Fill(lanes, value);

                return lanes;
            }

            private static int[] Map(int[] a, int[] b, Func<int, int, int> f)
            {
                int[] r = new int[32];

                for (int l = 0; l < 32; l++)
                {
                    r[l] = f(a[l], b[l]);
                }

                return r;
            }
        }
    }
}
