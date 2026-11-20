using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using System;
using System.Collections.Generic;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation
{
    // Callable machine-code regions share architectural state, rather than an
    // inferred C function ABI. Keep ordinary register SSA inside each region;
    // publish its direct writes at calls/returns and import every callee effect.
    static class NativeFunctionState
    {
        public const int Words = 336; // R0..254, P0..6, CC0..3, UR0..62, UP0..6.

        public static int Allocate(ResourceManager resources)
        {
            return resources.Properties.AddLocalMemory(new MemoryDefinition(
                "native_registers", AggregateType.Array | AggregateType.U32, Words));
        }

        private static int Slot(Register register)
        {
            return register.Type switch
            {
                RegisterType.Gpr when (uint)register.Index < 255 => register.Index,
                RegisterType.Predicate when (uint)register.Index < 7 => 255 + register.Index,
                RegisterType.Flag when (uint)register.Index < 4 => 262 + register.Index,
                RegisterType.Uniform when (uint)register.Index < 63 => 266 + register.Index,
                RegisterType.UniformPredicate when (uint)register.Index < 7 => 329 + register.Index,
                // Emitter temporaries are function-local. Zero/true pseudo
                // registers have already become constants in OperandHelper.
                RegisterType.Shadow => -1,
                _ => throw new InvalidOperationException($"Unsupported native register {register.Type}:{register.Index}."),
            };
        }

        private static Register FromSlot(int slot)
        {
            return slot < 255 ? new(slot, RegisterType.Gpr) :
                   slot < 262 ? new(slot - 255, RegisterType.Predicate) :
                   slot < 266 ? new(slot - 262, RegisterType.Flag) :
                   slot < 329 ? new(slot - 266, RegisterType.Uniform) :
                                new(slot - 329, RegisterType.UniformPredicate);
        }

        // Local load/store emission has already expanded vector/subword access
        // into its word operations. A positive, word-aligned logical bias keeps
        // a downward-growing native stack from wrapping into a valid array index.
        // Check each actual word before touching the private arena. Fault exits
        // are explicit returns, so Lower publishes all preceding native effects.
        public static Operation[] CheckLocalMemory(Operation[] code, int localMemory, int faultMemory, int biasBytes, int lengthBytes)
        {
            if (localMemory < 0 || faultMemory < 0 || localMemory == faultMemory ||
                biasBytes <= 0 || lengthBytes <= 0 || ((biasBytes | lengthBytes) & 3) != 0 ||
                (long)biasBytes + lengthBytes > uint.MaxValue)
            {
                throw new ArgumentException("Native local memory needs a distinct fault word and a bounded, positively biased arena.");
            }
            EmitterContext result = new();
            foreach (Operation operation in code)
            {
                if (operation.StorageKind == StorageKind.LocalMemory &&
                    operation.GetSource(0) is Operand { Type: OperandType.Constant } memory && memory.Value == localMemory)
                {
                    if (operation.Inst is not (Instruction.Load or Instruction.Store) ||
                        operation.SourcesCount != (operation.Inst == Instruction.Load ? 2 : 3))
                    {
                        throw new InvalidOperationException("Unsupported native local-memory operation.");
                    }
                    Operand relative = result.IAdd(operation.GetSource(1), Const(-(biasBytes / 4)));
                    Operand valid = Label();
                    result.BranchIfTrue(valid, result.ICompareLessUnsigned(relative, Const(lengthBytes / 4)));
                    result.Store(StorageKind.LocalMemory, faultMemory, Const(0), Const(1));
                    result.Return();
                    result.MarkLabel(valid);
                    operation.SetSource(1, relative);
                }
                result.Add(operation);
            }
            return result.GetOperations();
        }

        public static Operation[] Lower(Operation[] code, int stateMemory, int faultMemory = -1)
        {
            SortedSet<int> written = new();
            foreach (Operation operation in code)
            {
                for (int index = 0; index < operation.SourcesCount; index++)
                {
                    if (operation.GetSource(index).Type == OperandType.Register)
                    {
                        _ = Slot(operation.GetSource(index).GetRegister());
                    }
                }
                for (int index = 0; index < operation.DestsCount; index++)
                {
                    if (operation.GetDest(index) is Operand { Type: OperandType.Register } destination)
                    {
                        int slot = Slot(destination.GetRegister());
                        if (slot >= 0) written.Add(slot);
                    }
                }
            }

            List<Operation> result = new();
            void Import()
            {
                for (int slot = 0; slot < Words; slot++)
                {
                    result.Add(new Operation(Instruction.Load, StorageKind.LocalMemory,
                        Register(FromSlot(slot)), Const(stateMemory), Const(slot)));
                }
            }
            void Flush()
            {
                foreach (int slot in written)
                {
                    result.Add(new Operation(Instruction.Store, StorageKind.LocalMemory,
                        null, Const(stateMemory), Const(slot), Register(FromSlot(slot))));
                }
            }

            // Before the first label: a back edge to native entry must not
            // reload the bank halfway through execution of this function.
            Import();
            foreach (Operation operation in code)
            {
                if (operation.Inst == Instruction.Discard)
                {
                    throw new InvalidOperationException("A callable native function cannot discard its invocation.");
                }
                if (operation.Inst == Instruction.Call)
                {
                    if (operation.DestsCount != 0 || operation.SourcesCount != 1 ||
                        operation.GetSource(0).Type != OperandType.Constant || operation.GetSource(0).Value <= 0)
                    {
                        throw new InvalidOperationException("Native calls require a direct void target and shared architectural state.");
                    }
                    Flush();
                    result.Add(operation);
                    Import();
                    if (faultMemory >= 0)
                    {
                        Operand fault = Local(), valid = Label();
                        result.Add(new Operation(Instruction.Load, StorageKind.LocalMemory, fault, Const(faultMemory), Const(0)));
                        result.Add(new Operation(Instruction.BranchIfFalse, valid, fault));
                        Flush();
                        result.Add(new Operation(Instruction.Return, null));
                        result.Add(new Operation(Instruction.MarkLabel, valid));
                    }
                }
                else if (operation.Inst == Instruction.Return)
                {
                    if (operation.DestsCount != 0 || operation.SourcesCount != 0)
                    {
                        throw new InvalidOperationException("Native returns cannot have C ABI values.");
                    }
                    // Callee-only effects stay in the bank, even for a caller
                    // which writes no architectural registers of its own.
                    Flush();
                    result.Add(operation);
                }
                else
                {
                    result.Add(operation);
                }
            }
            return result.ToArray();
        }

        public static void ValidateExits(ControlFlowGraph cfg)
        {
            foreach (BasicBlock block in cfg.PostOrderBlocks)
            {
                if (block.Next == null && block.Branch == null &&
                    block.GetLastOp() is not Operation { Inst: Instruction.Return })
                {
                    throw new InvalidOperationException("A reachable native function exit has no explicit return.");
                }
            }
        }
    }
}
