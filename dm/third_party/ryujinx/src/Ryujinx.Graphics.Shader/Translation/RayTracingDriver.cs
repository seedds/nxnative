using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using System;
using System.Collections.Generic;
using System.Linq;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation
{
    // One SPIR-V module owns the scheduler, every native region, checked global
    // resources and the per-invocation bank/arena/hit context. SPIRV-Cross lowers
    // traversal itself: no generated-MSL function extraction or diagnostic ABI.
    sealed class RayTracingResources
    {
        public const int SceneBinding = RayTracingProgram.SceneBinding, ParametersBinding = RayTracingProgram.ParametersBinding,
            ErrorsBinding = RayTracingProgram.ErrorsBinding, InstanceSbtBinding = RayTracingProgram.InstanceSbtBinding;
        public int Registers { get; }
        public int Hit { get; }
        public int Fault { get; }
        public int ParameterWords { get; }
        private readonly ResourceManager _resources;
        private readonly Dictionary<ulong, SortedSet<int>> _accelerationSlots = new();

        public RayTracingResources(ResourceManager resources, RayTracingProgram program)
        {
            _resources = resources;
            foreach (RayTracingModuleRange module in program.Modules) _accelerationSlots.Add(module.Address, new());
            Registers = NativeFunctionState.Allocate(resources);
            Hit = resources.Properties.AddLocalMemory(new MemoryDefinition("current_hit", AggregateType.Array | AggregateType.U32, 22));
            Fault = resources.Properties.AddLocalMemory(new MemoryDefinition("continuation_fault", AggregateType.Array | AggregateType.U32, 1));
            // Fixed fields, eight runtime image classes, then a membership bit
            // per backend logical ID. No image format is guessed at build time.
            ParameterWords = (RayTracingProgram.MembershipBase + (program.Entries.Max(x => x.LogicalId) / 32 + 1) + 3) & ~3;
            resources.Properties.RayQueryBinding = new(0, SceneBinding);
            resources.Properties.AddOrUpdateConstantBuffer(new BufferDefinition(BufferLayout.Std140, 0, ParametersBinding, "rt_dispatch",
                new StructureType([new(AggregateType.Array | AggregateType.Vector4 | AggregateType.U32, "words", ParameterWords / 4)])));
            resources.Properties.AddOrUpdateStorageBuffer(new BufferDefinition(BufferLayout.Std430, 0, ErrorsBinding, "rt_errors",
                new StructureType([new(AggregateType.Array | AggregateType.U32, "words", 0)])));
            resources.Properties.AddOrUpdateStorageBuffer(new BufferDefinition(BufferLayout.Std430, 0, InstanceSbtBinding, "rt_instance_sbt",
                new StructureType([new(AggregateType.Array | AggregateType.U32, "words", 0)])));
        }

        private static uint? Constant(Operand value, HashSet<Operand> visiting)
        {
            if (value.Type == OperandType.Constant) return unchecked((uint)value.Value);
            if (value.Type != OperandType.LocalVariable || value.AsgOp is not Operation op || !visiting.Add(value)) return null;
            uint? a = op.SourcesCount > 0 ? Constant(op.GetSource(0), visiting) : null;
            uint? b = op.SourcesCount > 1 ? Constant(op.GetSource(1), visiting) : null;
            visiting.Remove(value);
            if (!a.HasValue) return null;
            if (op.Inst == Instruction.Copy) return a;
            if (!b.HasValue) return null;
            return op.Inst switch
            {
                Instruction.Add => unchecked(a.Value + b.Value),
                Instruction.Subtract => unchecked(a.Value - b.Value),
                Instruction.BitwiseAnd => a.Value & b.Value,
                Instruction.BitwiseOr => a.Value | b.Value,
                Instruction.ShiftLeft => a.Value << (int)(b.Value & 31),
                Instruction.ShiftRightU32 => a.Value >> (int)(b.Value & 31),
                _ => null,
            };
        }

        // Collect only original native reads, before the software driver/state
        // imports are lowered. This lets a program acquire the AS slot its code
        // actually names, without choosing an arbitrary bound AS or a title rule.
        public void RecordInputs(ulong address, Operation[] code, RayTracingProgram program)
        {
            RayTracingModuleRange module = program.Modules.Single(m => address >= m.Address && address - m.Address < (ulong)m.Length);
            int bank3 = _resources.GetConstantBufferBinding(3), bank0 = _resources.GetConstantBufferBinding(0);
            if (bank3 < RayTracingProgram.ReservedBindings || bank0 < RayTracingProgram.ReservedBindings || bank3 == bank0)
                throw new InvalidOperationException("Native constant banks overlap reserved ray-tracing bindings.");
            void Read(int bank, ulong offset)
            {
                if (bank is not (0 or 3)) throw new InvalidOperationException("Unsupported native runtime constant bank.");
                if (bank == 0 && offset != 0x28)
                    throw new InvalidOperationException("Unsupported native c0 input beyond the software local-memory cursor.");
                if (bank == 3 && !((offset >= (ulong)program.CoreOffset && offset - (ulong)program.CoreOffset < 0x140) ||
                                  (offset >= (ulong)program.DescriptorOffset && offset - (ulong)program.DescriptorOffset < 0x1430)))
                    throw new InvalidOperationException("Native c3 input is outside its metadata-owned runtime resources.");
                ulong first = (ulong)program.DescriptorOffset + 0x430 + 0x668;
                if (bank == 3 && offset >= first && offset - first < 32 * 8)
                    _accelerationSlots[module.Address].Add((int)((offset - first) / 8));
            }
            foreach (Operation op in code)
            {
                for (int index = 0; index < op.SourcesCount; index++)
                {
                    Operand source = op.GetSource(index);
                    if (source.Type == OperandType.ConstantBuffer) Read(source.GetCbufSlot(), (ulong)source.GetCbufOffset() * 4);
                }
                if (op is { Inst: Instruction.Load, StorageKind: StorageKind.ConstantBuffer })
                {
                    if (op.SourcesCount != 4 || op.GetSource(0).Type != OperandType.Constant)
                        throw new InvalidOperationException("Unsupported native runtime constant-bank resource.");
                    uint? vector = Constant(op.GetSource(2), new()), component = Constant(op.GetSource(3), new());
                    if (!vector.HasValue || !component.HasValue || component.Value >= 4)
                        throw new InvalidOperationException("Unsupported dynamic native runtime-resource-bank read.");
                    // Formatted SUST lowering already emits its backend class
                    // read here. Reserved resource IDs cannot be native banks;
                    // do not mistake that explicit driver input for an LDC.
                    if (op.GetSource(0).Value == ParametersBinding)
                    {
                        ulong word = (ulong)vector.Value * 4 + component.Value;
                        if (word < RayTracingProgram.ImageClassBase || word >= RayTracingProgram.MembershipBase)
                            throw new InvalidOperationException("Unsupported generated native image-class input.");
                        continue;
                    }
                    if (op.GetSource(0).Value != bank0 && op.GetSource(0).Value != bank3)
                        throw new InvalidOperationException("Unsupported native runtime constant-bank resource.");
                    Read(op.GetSource(0).Value == bank0 ? 0 : 3, ((ulong)vector.Value * 4 + component.Value) * 4);
                }
            }
        }

        public IReadOnlyDictionary<ulong, int[]> GetAccelerationSlots() =>
            _accelerationSlots.ToDictionary(pair => pair.Key, pair => pair.Value.ToArray());
    }

    static class RayTracingDriver
    {
        public static Operation[] Emit(RayTracingProgram program, RayTracingResources resources, DecodedProgram decoded)
        {
            EmitterContext c = new();
            Operand P(int word) => c.Load(StorageKind.ConstantBuffer, RayTracingResources.ParametersBinding, Const(0),
                Const(word / 4), Const(word & 3));
            Operand DynamicP(Operand word) => c.Load(StorageKind.ConstantBuffer, RayTracingResources.ParametersBinding, Const(0),
                c.ShiftRightU32(word, Const(2)), c.BitwiseAnd(word, Const(3)));
            Operand Core(int offset) => Cbuf(3, (program.CoreOffset + offset) / 4);
            Operand R(int slot) => c.Load(StorageKind.LocalMemory, resources.Registers, Const(slot));
            void SetR(int slot, Operand value) => c.Store(StorageKind.LocalMemory, resources.Registers, Const(slot), value);
            Operand Hit(int word) => c.Load(StorageKind.LocalMemory, resources.Hit, Const(word));
            Operand And(Operand a, Operand b) => c.BitwiseAnd(a, b);
            Operand Or(Operand a, Operand b) => c.BitwiseOr(a, b);
            Operand Equal(Operand a, Operand b) => c.ICompareEqual(a, b);

            Operand x = c.Load(StorageKind.Input, IoVariable.GlobalId, Const(0));
            Operand y = c.Load(StorageKind.Input, IoVariable.GlobalId, Const(1));
            Operand z = c.Load(StorageKind.Input, IoVariable.GlobalId, Const(2));
            Operand inside = And(c.ICompareLessUnsigned(x, Core(0x20)),
                And(c.ICompareLessUnsigned(y, Core(0x24)), c.ICompareLessUnsigned(z, Core(0x28))));
            Operand active = Label();
            c.BranchIfTrue(active, inside); c.Return(); c.MarkLabel(active);
            Operand packedLaunch = Or(x, Or(c.ShiftLeft(z, Core(0x78)), c.ShiftLeft(y, Core(0x74))));

            void Abort(int code, Operand detail, Operand auxiliary = null)
            {
                Operand written = c.AtomicCompareAndSwap(StorageKind.StorageBuffer, RayTracingResources.ErrorsBinding,
                    Const(0), Const(0), Const(0), Const(code));
                Operand done = Label();
                c.BranchIfTrue(done, c.ICompareNotEqual(written, Const(0)));
                c.Store(StorageKind.StorageBuffer, RayTracingResources.ErrorsBinding, Const(0), Const(1), packedLaunch);
                c.Store(StorageKind.StorageBuffer, RayTracingResources.ErrorsBinding, Const(0), Const(2), detail);
                if (auxiliary != null) c.Store(StorageKind.StorageBuffer, RayTracingResources.ErrorsBinding, Const(0), Const(3), auxiliary);
                c.MarkLabel(done); c.Return();
            }
            void Require(Operand condition, int code, Operand detail, Operand auxiliary = null)
            {
                Operand valid = Label();
                c.BranchIfTrue(valid, condition); Abort(code, detail, auxiliary); c.MarkLabel(valid);
            }
            (Operand Low, Operand High) AddAddress(Operand low, Operand high, Operand offset)
            {
                Operand sum = c.IAdd(low, offset);
                Operand carry = c.ConditionalSelect(c.ICompareLessUnsigned(sum, low), Const(1), Const(0));
                Operand upper = c.IAdd(high, carry);
                Require(c.ICompareGreaterOrEqualUnsigned(upper, high), 3, offset);
                return (sum, upper);
            }
            Operand ReadHandle(int baseOffset, Operand byteOffset, RayTracingStage stage, int extentWord)
            {
                Require(Equal(And(byteOffset, Const(31)), Const(0)), 3, byteOffset);
                if (extentWord >= 0)
                {
                    Operand enough = Or(c.ICompareNotEqual(P(extentWord + 1), Const(0)),
                        And(c.ICompareGreaterOrEqualUnsigned(P(extentWord), Const(16)),
                            c.ICompareLessOrEqualUnsigned(byteOffset, c.IAdd(P(extentWord), Const(-16)))));
                    Require(enough, 3, byteOffset);
                }
                var address = AddAddress(Core(baseOffset), Core(baseOffset + 4), byteOffset);
                Require(Equal(And(address.Low, Const(3)), Const(0)), 3, address.Low);
                Operand Word(int index)
                {
                    var p = AddAddress(address.Low, address.High, Const(index * 4));
                    return c.Load(StorageKind.GlobalMemory, p.Low, p.High);
                }
                Operand header = Word(0), reserved = Word(1), tagLow = Word(2), tagHigh = Word(3);
                Operand id = And(header, Const(0xffff));
                Require(And(Equal(c.ShiftRightU32(header, Const(16)), Const((int)stage)),
                    And(Equal(reserved, Const(0)), And(Equal(tagLow, P(0)), Equal(tagHigh, P(1))))), 4, header);
                Operand primary = Const(0);
                foreach (RayTracingEntry entry in program.Entries.Where(e => e.Stage == stage && e.OwnerId == e.LogicalId))
                    primary = Or(primary, Equal(id, Const(entry.LogicalId)));
                Require(primary, 4, id);
                Operand maskWord = DynamicP(c.IAdd(Const(RayTracingProgram.MembershipBase), c.ShiftRightU32(id, Const(5))));
                Require(c.ICompareNotEqual(And(c.ShiftRightU32(maskWord, And(id, Const(31))), Const(1)), Const(0)), 4, id);
                return id;
            }

            for (int slot = 0; slot < NativeFunctionState.Words; slot++) SetR(slot, Const(0));
            c.Store(StorageKind.LocalMemory, resources.Fault, Const(0), Const(0));
            SetR(8, Const(program.LocalMemoryBias + program.LocalMemoryBytes));
            SetR(22, packedLaunch);
            Operand launch = ReadHandle(0x38, Const(0), RayTracingStage.RayGeneration, -1);
            SetR(21, launch);
            Operand loop = Label(), returned = Label(), trace = Label(), resume = Label(), doneLabel = Label();
            c.MarkLabel(loop);
            Operand selected = R(21);
            // Local IR operands are SSA values. Derive the executed stage from
            // the immutable selected ID instead of assigning one Local operand
            // in multiple call blocks (which bypasses register SSA renaming).
            Operand executedRaygen = Const(0);
            foreach (RayTracingEntry entry in program.Entries.Where(e => e.Stage == RayTracingStage.RayGeneration))
                executedRaygen = Or(executedRaygen, Equal(selected, Const(entry.LogicalId)));
            var calls = program.Entries.Select(entry => (Entry: entry, Label: Label())).ToArray();
            foreach (var call in calls) c.BranchIfTrue(call.Label, Equal(selected, Const(call.Entry.LogicalId)));
            Abort(5, selected, Const(-1));
            foreach (var call in calls)
            {
                c.MarkLabel(call.Label);
                c.Call(decoded.GetFunctionByAddress(call.Entry.Address).Id, false);
                Operand nativeFault = c.Load(StorageKind.LocalMemory, resources.Fault, Const(0));
                Operand noFault = Label();
                c.BranchIfFalse(noFault, nativeFault); Abort(1, nativeFault, selected); c.MarkLabel(noFault);
                c.Branch(returned);
            }
            c.MarkLabel(returned);
            Operand action = R(21);
            c.BranchIfTrue(doneLabel, Equal(action, Const(-7)));
            c.BranchIfTrue(trace, Equal(action, Const(-1)));
            c.BranchIfTrue(resume, Equal(action, Const(-3)));
            Abort(5, action, selected);

            c.MarkLabel(trace);
            Require(executedRaygen, 6, selected); // Trace depth one.
            Require(Equal(Or(c.BitwiseExclusiveOr(R(18), P(2)), c.BitwiseExclusiveOr(R(19), P(3))), Const(0)), 7, R(18));
            // Supported native trace variant: mask in8..15, zero SBT offset/
            // stride and no unknown high flags. Never guess those encodings.
            Require(Equal(And(R(37), Const(unchecked((int)0xffff00ff))), Const(0)), 6, R(37));
            c.Add(new Operation(Instruction.RayQueryTrace, null, Const(resources.Hit), Const(1),
                And(c.ShiftRightU32(R(37), Const(8)), Const(255)), R(10), R(11), R(12), R(13), R(14), R(15), R(16), R(17)));
            Require(Equal(Hit(21), Const(0)), 8, Hit(0));
            Operand miss = Label();
            c.BranchIfFalse(miss, Equal(Hit(0), Const(1)));
            Require(Equal(Hit(3), Const(0)), 6, Hit(3)); // Native geometry-index join is not established yet.
            Require(c.ICompareLessUnsigned(Hit(4), P(6)), 7, Hit(4));
            Operand contribution = c.Load(StorageKind.StorageBuffer, RayTracingResources.InstanceSbtBinding, Const(0), Hit(4));
            Require(Equal(contribution, Const(0)), 6, contribution);
            Require(c.ICompareLessOrEqualUnsigned(Hit(2), Const(0x3fffffff)), 6, Hit(2));
            SetR(17, Hit(1)); SetR(20, Const(0)); SetR(38, Hit(2));
            SetR(39, c.ConditionalSelect(c.ICompareNotEqual(Hit(6), Const(0)), Const(unchecked((int)0xfe000000)), Const(unchecked((int)0xff000000))));
            SetR(40, Hit(4)); SetR(41, Hit(7)); SetR(42, Hit(8));
            SetR(21, ReadHandle(0x48, Const(0), RayTracingStage.ClosestHit, 10));
            c.Branch(loop);
            c.MarkLabel(miss);
            SetR(21, ReadHandle(0x40, R(20), RayTracingStage.Miss, 8));
            c.Branch(loop);

            c.MarkLabel(resume);
            Require(Equal(executedRaygen, Const(0)), 5, selected, action);
            Operand continuation = And(R(23), Const(0xffff));
            Operand allowed = Const(0);
            foreach (RayTracingEntry entry in program.Entries.Where(e => e.Stage == RayTracingStage.RayGeneration && e.OwnerId != e.LogicalId))
                allowed = Or(allowed, And(Equal(launch, Const(entry.OwnerId)), Equal(continuation, Const(entry.LogicalId))));
            Require(allowed, 5, continuation, launch);
            SetR(21, continuation); c.Branch(loop);
            c.MarkLabel(doneLabel);
            Require(executedRaygen, 5, selected, action);
            c.Return();
            return c.GetOperations();
        }
    }
}
