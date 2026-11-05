using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System.Collections.Generic;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation
{
    // nxnative: a compute thread that exits before a barrier the rest of its threadgroup still executes. NVIDIA's BAR counts exited threads as arrived; Metal does
    // not (all threads of a threadgroup must execute each threadgroup_barrier), and on Apple GPUs such a kernel can leave its command buffer running forever: PRAGMATA
    // (156 of its compute shaders, e.g. "if (lid * 4 >= count) return; barrier") left one that never completed, and twice the Mac's WindowServer stopped minutes later.
    // The exits before the last barrier become "dead = true; go to the next barrier": a dead thread runs no other instruction but still takes part in every barrier,
    // and returns after the last one. Functions with a barrier inside a loop (a backward branch around it) are left as they are
    static class BarrierExits
    {
        public static Operation[] Apply(Operation[] ops)
        {
            var labelAt = new Dictionary<Operand, int>();
            var barriers = new List<int>();

            for (int i = 0; i < ops.Length; i++)
            {
                if (ops[i].Inst == Instruction.MarkLabel) labelAt[ops[i].Dest] = i;
                else if (ops[i].Inst == Instruction.Barrier) barriers.Add(i);
            }

            if (barriers.Count == 0) return ops;

            int last = barriers[^1];
            bool exitBefore = false;

            for (int i = 0; i < last; i++) if (ops[i].Inst == Instruction.Return) { exitBefore = true; break; }

            if (!exitBefore) return ops;

            for (int i = 0; i < ops.Length; i++)
            {
                Instruction inst = ops[i].Inst;

                if ((inst == Instruction.Branch || inst == Instruction.BranchIfTrue || inst == Instruction.BranchIfFalse) && ops[i].Dest != null &&
                    labelAt.TryGetValue(ops[i].Dest, out int t) && t < i)
                {
                    foreach (int b in barriers) if (t < b && b < i) return ops;   // a barrier inside a loop
                }
            }

            // A register, not a Local(): locals are single-assignment temporaries the SSA pass does not merge across blocks (a Local here was folded to false). The
            // Ampere frontend never uses Maxwell's condition flags, so flag register 3 is free
            Operand dead = Register(3, RegisterType.Flag);
            var before = new Operand[barriers.Count];

            for (int k = 0; k < before.Length; k++) before[k] = Label();

            var outOps = new List<Operation>(ops.Length + 4 * barriers.Count + 4) { new Operation(Instruction.Copy, dead, Const(IrConsts.False)) };
            int next = 0;

            for (int i = 0; i < ops.Length; i++)
            {
                while (next < barriers.Count && barriers[next] < i) next++;

                if (ops[i].Inst == Instruction.Return && i < last)
                {
                    outOps.Add(new Operation(Instruction.Copy, dead, Const(IrConsts.True)));
                    outOps.Add(new Operation(Instruction.Branch, before[next]));
                    continue;
                }

                if (next < barriers.Count && barriers[next] == i)
                {
                    outOps.Add(new Operation(Instruction.MarkLabel, before[next]));
                    outOps.Add(ops[i]);

                    if (next + 1 < barriers.Count)
                    {
                        outOps.Add(new Operation(Instruction.BranchIfTrue, before[next + 1], dead));
                    }
                    else
                    {
                        Operand live = Label();
                        outOps.Add(new Operation(Instruction.BranchIfFalse, live, dead));
                        outOps.Add(new Operation(Instruction.Return, (Operand)null));
                        outOps.Add(new Operation(Instruction.MarkLabel, live));
                    }

                    continue;
                }

                outOps.Add(ops[i]);
            }

            return outOps.ToArray();
        }
    }
}
