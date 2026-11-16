using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;

using static Ryujinx.Graphics.Shader.StructuredIr.AstHelper;

namespace Ryujinx.Graphics.Shader.StructuredIr
{
    // nxnative: keep a loop exit's target block in the iteration that left the loop, when that block has subgroup operations.
    //
    // A conditional branch out of a loop to code past it is eliminated by GotoElimination.MoveOutward as `f = X; if (f) break;` in
    // the loop, then `if (!f) { ... }` over the code the branch skipped and `if (false || f) { f = false; B }` around the target.
    // For one thread that is the same program, but B now runs once for the lanes of every iteration together, while on NVIDIA the
    // lanes that branched in one iteration run it as one cohort before reconverging (BSSY / BSYNC). A warp-aggregated atomic (elect a
    // leader in a loop; the lanes whose key matches the leader's leave and reduce with VOTE / REDUX / SHFL) then mixes keys, and only
    // one leader's atomic lands (PRAGMATA 607ceeefb9478700, a SURED.MIN). Here B moves into the loop, `if (f) { B; break; }`, and the
    // block after the loop becomes the reset `f = false`. Only for a B with subgroup operations, and only in the exact form below,
    // where every thread runs the same statements in the same order as before:
    // - the loop is followed (as siblings) by `if (!f)` blocks only, then `if (f)` or `if (false || f)`, with no else;
    // - that block has the label `f = false` as a direct child, and no other mention of f; it has no break / continue of an
    //   enclosing loop;
    // - in the whole function f is only assigned the constant false, except once: `f = X` directly followed by `if (f) { break; }`
    //   whose break leaves this loop; and the function starts with `f = false` (the goto reset). So f is false whenever the loop is
    //   left any other way, and a thread that broke out runs nothing (the `if (!f)` blocks) before B.
    static class LoopExitCohort
    {
        public static void Apply(AstBlock mainBlock, Func<int, bool> calleeHasSubgroupOps)
        {
            List<AstBlock> loops = [];

            CollectLoops(mainBlock, loops);

            foreach (AstBlock loop in loops)
            {
                TryMove(mainBlock, loop, calleeHasSubgroupOps);
            }
        }

        // Whether a function body (the IR, before structuring) or a function it calls has subgroup operations
        public static bool[] SubgroupFunctions(IReadOnlyList<Function> functions)
        {
            bool[] has = new bool[functions.Count];
            List<int>[] callees = new List<int>[functions.Count];

            for (int index = 0; index < functions.Count; index++)
            {
                callees[index] = [];

                foreach (BasicBlock block in functions[index].Blocks)
                {
                    foreach (INode node in block.Operations)
                    {
                        if (node is not Operation operation)
                        {
                            continue;
                        }

                        Instruction inst = operation.Inst & Instruction.Mask;

                        if (IsSubgroupInst(inst))
                        {
                            has[index] = true;
                        }
                        else if (inst == Instruction.Call && operation.GetSource(0).Type == OperandType.Constant)
                        {
                            callees[index].Add(operation.GetSource(0).Value);
                        }
                    }
                }
            }

            for (bool changed = true; changed;)
            {
                changed = false;

                for (int index = 0; index < functions.Count; index++)
                {
                    if (!has[index] && callees[index].Exists(callee => callee >= 0 && callee < has.Length && has[callee]))
                    {
                        has[index] = changed = true;
                    }
                }
            }

            return has;
        }

        private static bool IsSubgroupInst(Instruction inst)
        {
            return inst is Instruction.Ballot or Instruction.Shuffle or Instruction.ShuffleDown or Instruction.ShuffleUp or Instruction.ShuffleXor or
                Instruction.VoteAll or Instruction.VoteAllEqual or Instruction.VoteAny or Instruction.SubgroupBarrier;
        }

        private static void CollectLoops(AstBlock block, List<AstBlock> loops)
        {
            foreach (IAstNode node in block)
            {
                if (node is AstBlock child)
                {
                    if (child.Type == AstBlockType.DoWhile)
                    {
                        loops.Add(child);
                    }

                    CollectLoops(child, loops);
                }
            }
        }

        private static void TryMove(AstBlock mainBlock, AstBlock loop, Func<int, bool> calleeHasSubgroupOps)
        {
            if (loop.Parent == null)
            {
                return;
            }

            // The `if (!f)` blocks after the loop, then the target block
            AstOperand flag = null;
            List<AstBlock> skipped = [];
            IAstNode node = Next(loop);

            while (node is AstBlock { Type: AstBlockType.If } ifBlock && ifBlock.Condition is AstOperation { Inst: Instruction.LogicalNot } negation &&
                   negation.GetSource(0) is AstOperand { Type: OperandType.LocalVariable } notFlag && (flag == null || flag == notFlag))
            {
                flag = notFlag;
                skipped.Add(ifBlock);
                node = Next(node);
            }

            if (node is not AstBlock { Type: AstBlockType.If } target)
            {
                return;
            }

            AstOperand targetFlag = target.Condition switch
            {
                AstOperand { Type: OperandType.LocalVariable } operand => operand,
                AstOperation { Inst: Instruction.LogicalOr } either when IsConstFalse(either.GetSource(0)) &&
                    either.GetSource(1) is AstOperand { Type: OperandType.LocalVariable } operand => operand,
                _ => null,
            };

            if (targetFlag == null || (flag != null && flag != targetFlag) || targetFlag.VarType != AggregateType.Bool)
            {
                return;
            }

            flag = targetFlag;

            if (Next(target) is AstBlock { Type: AstBlockType.Else or AstBlockType.ElseIf })
            {
                return;
            }

            // The label, and no other mention of the flag in the target
            AstAssignment label = null;

            foreach (IAstNode child in target)
            {
                if (label == null && child is AstAssignment asg && asg.Destination == flag && IsConstFalse(asg.Source))
                {
                    label = asg;
                }
                else if (Mentions(child, flag))
                {
                    return;
                }
            }

            if (label == null || HasLoopExit(target) || !HasSubgroupOps(target, calleeHasSubgroupOps))
            {
                return;
            }

            // The flag's assignments in the function: constant false, but for one `f = X; if (f) { break; }` leaving this loop
            List<AstAssignment> writes = [];

            if (!CollectWrites(mainBlock, flag, writes))
            {
                return;
            }

            AstBlock breakIf = null;

            foreach (AstAssignment write in writes)
            {
                if (IsConstFalse(write.Source))
                {
                    continue;
                }

                if (breakIf != null || Next(write) is not AstBlock { Type: AstBlockType.If, Count: 1 } candidate || candidate.Condition != flag ||
                    candidate.First is not AstOperation { Inst: Instruction.LoopBreak } || EnclosingLoop(candidate) != loop)
                {
                    return;
                }

                breakIf = candidate;
            }

            if (breakIf == null || !StartsWithReset(mainBlock, flag, loop))
            {
                return;
            }

            // `if (f) { B; break; }`, and the reset where the target was
            IAstNode loopBreak = breakIf.First;

            for (IAstNode child = target.First; child != null;)
            {
                IAstNode next = Next(child);

                if (child != label)
                {
                    target.Remove(child);
                    breakIf.AddBefore(loopBreak, child);
                }

                child = next;
            }

            // A B that ends the program (return, or discard) leaves no path to the break, and the code generators take a block's last statement as its only
            // terminator: `return; break;` was SPIR-V with a branch outside any block (Sonic Frontiers' compute shader 0e6f8156033ae07e)
            if (Previous(loopBreak) is AstOperation { Inst: Instruction.Return or Instruction.Discard })
            {
                breakIf.Remove(loopBreak);
            }

            AstBlock parent = target.Parent;

            target.Remove(label);
            parent.AddBefore(target, label);
            parent.Remove(target);
            target.Condition = null;
        }

        private static bool IsConstFalse(IAstNode node)
        {
            return node is AstOperand { Type: OperandType.Constant } operand && operand.Value == IrConsts.False;
        }

        private static AstBlock EnclosingLoop(IAstNode node)
        {
            AstBlock block = node.Parent;

            while (block != null && block.Type != AstBlockType.DoWhile)
            {
                block = block.Parent;
            }

            return block;
        }

        // A direct child `f = false` of the function's block before the statement that holds the loop
        private static bool StartsWithReset(AstBlock mainBlock, AstOperand flag, AstBlock loop)
        {
            IAstNode top = loop;

            while (top.Parent != mainBlock)
            {
                top = top.Parent;
            }

            for (IAstNode node = mainBlock.First; node != null && node != top; node = Next(node))
            {
                if (node is AstAssignment asg && asg.Destination == flag && IsConstFalse(asg.Source))
                {
                    return true;
                }
            }

            return false;
        }

        // False when an operation writes the flag (an out argument): not the goto form
        private static bool CollectWrites(AstBlock block, AstOperand flag, List<AstAssignment> writes)
        {
            foreach (IAstNode node in block)
            {
                if (node is AstBlock child)
                {
                    if (!CollectWrites(child, flag, writes))
                    {
                        return false;
                    }
                }
                else if (node is AstAssignment asg && asg.Destination == flag)
                {
                    writes.Add(asg);
                }
                else if (node is AstOperation operation && WritesOperand(operation, flag))
                {
                    return false;
                }
            }

            return true;
        }

        private static bool WritesOperand(AstOperation operation, AstOperand flag)
        {
            return flag.Defs.Contains(operation);
        }

        private static bool Mentions(IAstNode node, AstOperand flag)
        {
            switch (node)
            {
                case AstOperand operand:
                    return operand == flag;
                case AstAssignment asg:
                    return Mentions(asg.Destination, flag) || Mentions(asg.Source, flag);
                case AstOperation operation:
                    for (int index = 0; index < operation.SourcesCount; index++)
                    {
                        if (Mentions(operation.GetSource(index), flag))
                        {
                            return true;
                        }
                    }

                    return WritesOperand(operation, flag);
                case AstBlock block:
                    if (block.Condition != null && Mentions(block.Condition, flag))
                    {
                        return true;
                    }

                    foreach (IAstNode child in block)
                    {
                        if (Mentions(child, flag))
                        {
                            return true;
                        }
                    }

                    return false;
                default:
                    return false;
            }
        }

        // A break or continue that would leave the block's enclosing loop (not one inside the block)
        private static bool HasLoopExit(AstBlock block)
        {
            foreach (IAstNode node in block)
            {
                if (node is AstOperation { Inst: Instruction.LoopBreak or Instruction.LoopContinue })
                {
                    return true;
                }

                if (node is AstBlock { Type: not AstBlockType.DoWhile } child && HasLoopExit(child))
                {
                    return true;
                }
            }

            return false;
        }

        private static bool HasSubgroupOps(IAstNode node, Func<int, bool> calleeHasSubgroupOps)
        {
            switch (node)
            {
                case AstAssignment asg:
                    return HasSubgroupOps(asg.Source, calleeHasSubgroupOps);
                case AstOperation operation:
                    Instruction inst = operation.Inst & Instruction.Mask;

                    if (IsSubgroupInst(inst) ||
                        (inst == Instruction.Call && operation.GetSource(0) is AstOperand { Type: OperandType.Constant } callee && calleeHasSubgroupOps(callee.Value)))
                    {
                        return true;
                    }

                    for (int index = 0; index < operation.SourcesCount; index++)
                    {
                        if (HasSubgroupOps(operation.GetSource(index), calleeHasSubgroupOps))
                        {
                            return true;
                        }
                    }

                    return false;
                case AstBlock block:
                    if (block.Condition != null && HasSubgroupOps(block.Condition, calleeHasSubgroupOps))
                    {
                        return true;
                    }

                    foreach (IAstNode child in block)
                    {
                        if (HasSubgroupOps(child, calleeHasSubgroupOps))
                        {
                            return true;
                        }
                    }

                    return false;
                default:
                    return false;
            }
        }
    }
}
