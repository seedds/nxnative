using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.Instructions;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using System.Collections.Generic;
using System.Linq;

namespace Ryujinx.Graphics.Shader.Translation
{
    // Native RT entries return through the unchanged driver RPC in R4:R5.
    // NOINC alone is insufficient: validate both machine transfers and all
    // architectural effects before converting those returns into host returns.
    static class RayTracingControlFlow
    {
        private static bool UnconditionalSecondary(AmpereOp op)
            => op.Bits(87, 90) == RegisterConsts.PredicateTrueIndex && !op.Bit(90);

        public static void ValidateCall(InstOp instruction)
        {
            AmpereOp op = new(instruction);
            if (op.Opcode != AmpereOpcodes.Call || op.Bit(91) || op.Bit(85) || !op.Bit(86) ||
                !UnconditionalSecondary(op))
            {
                throw new InvalidOperationException("RT calls require direct CALL.REL.NOINC with secondary predicate PT.");
            }
            _ = DirectTarget(instruction);
        }

        public static void ValidateReturn(InstOp instruction)
        {
            AmpereOp op = new(instruction);
            if (op.Opcode != AmpereOpcodes.Ret || op.Bit(91) || !op.Bit(85) || !op.Bit(86) ||
                op.Ra != 4 || op.Field(32, 82) != 0 || !UnconditionalSecondary(op))
            {
                throw new InvalidOperationException("RT returns require RET.ABS.NODEC R4:R5 with zero offset and secondary predicate PT.");
            }
        }

        public static ulong DirectTarget(InstOp instruction)
        {
            AmpereOp op = new(instruction);
            if (op.Opcode is not (AmpereOpcodes.Bra or AmpereOpcodes.Call) ||
                (instruction.Address & 15) != 0 || instruction.Address > ulong.MaxValue - Decoder.AmpereInstSize)
            {
                throw new InvalidOperationException("Unsupported or overflowing RT direct branch.");
            }
            ulong next = instruction.Address + Decoder.AmpereInstSize;
            long offset = op.BranchOffset;
            ulong magnitude = offset >= 0 ? (ulong)offset : (ulong)(-(offset + 1)) + 1;
            if ((offset >= 0 && next > ulong.MaxValue - magnitude) || (offset < 0 && next < magnitude))
            {
                throw new InvalidOperationException("An RT direct branch address overflows.");
            }
            ulong target = offset >= 0 ? next + magnitude : next - magnitude;
            if ((target & 15) != 0)
            {
                throw new InvalidOperationException("An RT direct branch target is unaligned.");
            }
            return target;
        }

        public static void ValidateInstruction(InstOp instruction)
        {
            if (!instruction.IsAmpere || instruction.Name == InstName.Invalid)
            {
                throw new InvalidOperationException("Unsupported instruction in an RT native entry.");
            }
            AmpereOp op = new(instruction);
            switch (op.Opcode)
            {
                case AmpereOpcodes.Call: ValidateCall(instruction); break;
                case AmpereOpcodes.Ret: ValidateReturn(instruction); break;
                case AmpereOpcodes.Bra: _ = DirectTarget(instruction); break;
                // The ordinary emitter discards BMOV's barrier/GPR effects.
                // Such an instruction cannot participate in an RPC proof.
                case 0xf55:
                case 0xf56:
                    throw new InvalidOperationException("RT entries do not support barrier/register BMOV state transfers.");
                default:
                    if (instruction.Props.HasFlag(InstProps.Bra))
                    {
                        throw new InvalidOperationException("RT entries do not support EXIT, discard or indirect transfers.");
                    }
                    break;
            }
        }

        public static void ValidateDecoded(DecodedProgram decoded, RayTracingProgram program)
        {
            HashSet<ulong> entries = program.Entries.Select(x => x.Address).ToHashSet();
            if (decoded.Count() != entries.Count)
            {
                throw new InvalidOperationException("Every declared RT entry must be decoded exactly once.");
            }
            foreach (DecodedFunction function in decoded)
            {
                if (!entries.Contains(function.Address))
                {
                    throw new InvalidOperationException("An RT helper must remain inside its caller's control-flow graph.");
                }
                RayTracingModuleRange module = program.Modules.Single(x =>
                    function.Address >= x.Address && function.Address - x.Address < (ulong)x.Length);
                bool Inside(ulong address) => (address & 15) == 0 && address >= module.Address &&
                    address - module.Address < (ulong)module.Length;
                foreach (Block block in function.Blocks)
                {
                    if (!Inside(block.Address) || block.OpCodes.Count == 0 || block.EndAddress <= block.Address ||
                        block.EndAddress - module.Address > (ulong)module.Length || (block.EndAddress & 15) != 0)
                    {
                        throw new InvalidOperationException("An RT control-flow block escapes its native code module.");
                    }
                    foreach (InstOp instruction in block.OpCodes)
                    {
                        if (!Inside(instruction.Address))
                        {
                            throw new InvalidOperationException("An RT instruction escapes its native code module.");
                        }
                        ValidateInstruction(instruction);
                        int opcode = new AmpereOp(instruction).Opcode;
                        if ((opcode is AmpereOpcodes.Bra or AmpereOpcodes.Call) && !Inside(DirectTarget(instruction)))
                        {
                            throw new InvalidOperationException("An RT direct transfer escapes its native code module.");
                        }
                    }
                    foreach (Block successor in block.Successors)
                    {
                        if (!Inside(successor.Address))
                        {
                            throw new InvalidOperationException("An RT fallthrough escapes its native code module.");
                        }
                    }
                }
            }
        }

        public static void ValidateOperations(Operation[] operations)
        {
            foreach (Operation operation in operations)
            {
                if (operation.Inst is Instruction.Call or Instruction.Discard)
                {
                    throw new InvalidOperationException("RT native entries require proved driver returns and direct CFG transfers.");
                }
                bool Rpc(Operand operand) => operand is { Type: OperandType.Register } &&
                    operand.GetRegister() is { Type: RegisterType.Gpr, Index: 4 or 5 };
                for (int index = 0; index < operation.SourcesCount; index++)
                {
                    if (Rpc(operation.GetSource(index)))
                    {
                        throw new InvalidOperationException("An RT native entry reads driver RPC R4:R5 as data.");
                    }
                }
                for (int index = 0; index < operation.DestsCount; index++)
                {
                    if (Rpc(operation.GetDest(index)))
                    {
                        throw new InvalidOperationException("An RT native entry overwrites driver RPC R4:R5.");
                    }
                }
            }
        }
    }
}
