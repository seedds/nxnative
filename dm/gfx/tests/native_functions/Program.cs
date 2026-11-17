// Synthetic IR only. No game instructions, metadata, keys or captures.
using System;
using System.Collections.Generic;
using System.IO;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.CodeGen;
using Ryujinx.Graphics.Shader.CodeGen.Spirv;
using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using Ryujinx.Graphics.Shader.Translation;
using Ryujinx.Graphics.Shader.Translation.Optimizations;
using Ryujinx.Graphics.Shader.Translation.Transforms;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

static class Program
{
    sealed class Accessor : IGpuAccessor
    {
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => throw new InvalidOperationException("IR only");
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, index + RayTracingProgram.ReservedBindings);
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, index + 1);
        public SetBindingPair CreateImageBinding(int count, bool buffer) => new(0, 10);
        public SetBindingPair CreateTextureBinding(int count, bool buffer) => new(0, 20);
        public int QuerySamplerArrayLengthFromPool() => 1;
        public int QueryTextureArrayLengthFromBuffer(int index) => 1;
        public int QueryTextureArrayLengthFromPool() => 1;
    }

    static byte[] Generate(bool debug, bool trace)
    {
        Accessor accessor = new();
        AttributeUsage usage = new(accessor);
        ShaderDefinitions definitions = new(ShaderStage.Compute, 1, 1, 1);
        ResourceManager resources = new(ShaderStage.Compute, accessor, new ResourceReservations(false, false));
        resources.Properties.AddOrUpdateStorageBuffer(new BufferDefinition(BufferLayout.Std430, 0, 1, "test_data",
            new StructureType([new(AggregateType.Array | AggregateType.U32, "words", 0)])));
        EmitterContext main = new();
        Operand invocation = main.Load(StorageKind.Input, IoVariable.GlobalId, Const(0));
        List<Operation[]> code = new();

        if (trace)
        {
            int hit = resources.Properties.AddLocalMemory(new MemoryDefinition("current_hit", AggregateType.Array | AggregateType.U32, 22));
            resources.Properties.RayQueryBinding = new(0, 2); // Support UBO0 and test storage1 keep their bindings.
            Operand first = main.IMultiply(invocation, Const(64));
            Operand Word(int word) => main.Load(StorageKind.StorageBuffer, 1, Const(0), main.IAdd(first, Const(word)));
            main.Add(new Operation(Instruction.RayQueryTrace, null, Const(hit), Const(1), Const(255),
                Word(0), Word(1), Word(2), Word(3), Word(4), Word(5), Word(6), Word(7)));
            for (int word = 0; word < 22; word++)
                main.Store(StorageKind.StorageBuffer, 1, Const(0), main.IAdd(first, Const(16 + word)),
                    main.Load(StorageKind.LocalMemory, hit, Const(word)));
            main.Return();
            code.Add(main.GetOperations());
        }
        else
        {
            int state = NativeFunctionState.Allocate(resources);
            int local = resources.Properties.AddLocalMemory(new MemoryDefinition("native_local", AggregateType.Array | AggregateType.U32, 8));
            int fault = resources.Properties.AddLocalMemory(new MemoryDefinition("native_fault", AggregateType.Array | AggregateType.U32, 1));
            main.Store(StorageKind.LocalMemory, fault, Const(0), Const(0));
            Operand first = main.IMultiply(invocation, Const(352));
            for (int word = 0; word < NativeFunctionState.Words; word++)
                main.Store(StorageKind.LocalMemory, state, Const(word),
                    main.Load(StorageKind.StorageBuffer, 1, Const(0), main.IAdd(first, Const(word))));
            for (int word = 0; word < 8; word++)
                main.Store(StorageKind.LocalMemory, local, Const(word),
                    main.Load(StorageKind.StorageBuffer, 1, Const(0), main.IAdd(first, Const(336 + word))));
            main.Call(1, false);
            for (int word = 0; word < NativeFunctionState.Words; word++)
                main.Store(StorageKind.StorageBuffer, 1, Const(0), main.IAdd(first, Const(word)),
                    main.Load(StorageKind.LocalMemory, state, Const(word)));
            for (int word = 0; word < 8; word++)
                main.Store(StorageKind.StorageBuffer, 1, Const(0), main.IAdd(first, Const(336 + word)),
                    main.Load(StorageKind.LocalMemory, local, Const(word)));
            main.Store(StorageKind.StorageBuffer, 1, Const(0), main.IAdd(first, Const(344)),
                main.Load(StorageKind.LocalMemory, fault, Const(0)));
            main.Return();

            EmitterContext caller = new(), empty = new(), leaf = new();
            Operand R(int index) => Register(index, RegisterType.Gpr);
            Operand early = Label();
            caller.Copy(R(7), caller.IAdd(R(0), Const(13)));
            caller.Call(2, false); // Callee has no direct architectural writes.
            caller.BranchIfTrue(early, caller.ICompareEqual(R(0), Const(100)));
            caller.Copy(R(9), caller.IAdd(R(7), R(3))); // Future write on only one path.
            caller.Return();
            caller.MarkLabel(early);
            caller.Return();
            empty.Call(3, false);
            empty.Return();
            leaf.Copy(R(3), leaf.IAdd(R(7), Const(19)));
            leaf.Copy(Register(2, RegisterType.Uniform), leaf.IAdd(Register(2, RegisterType.Uniform), Const(23)));
            leaf.Copy(Register(0, RegisterType.Flag), Const(1));
            Operand address = leaf.ConditionalSelect(leaf.ICompareEqual(R(0), Const(200)), Const(0x10000 / 4 + 8), Const(0x10000 / 4 + 3));
            leaf.Store(StorageKind.LocalMemory, local, address, R(3));
            // Must not execute on the out-of-range path, including in callers.
            leaf.Copy(R(11), Const(0x1234));
            leaf.Return();
            code.Add(main.GetOperations());
            foreach (EmitterContext function in new[] { caller, empty, leaf })
                code.Add(NativeFunctionState.Lower(NativeFunctionState.CheckLocalMemory(
                    function.GetOperations(), local, fault, 0x10000, 32), state, fault));
        }

        // Same register-SSA/optimization path used by TranslatorContext. In
        // particular the native functions receive no inferred C ABI arguments.
        List<Function> functions = new();
        for (int i = 0; i < code.Count; i++) functions.Add(null);
        HelperFunctionManager helpers = new(functions, ShaderStage.Compute);
        FeatureFlags features = FeatureFlags.LocalMemory;
        for (int i = 0; i < code.Count; i++)
        {
            ControlFlowGraph cfg = ControlFlowGraph.Create(code[i]);
            if (i != 0) NativeFunctionState.ValidateExits(cfg);
            Dominance.FindDominators(cfg);
            Dominance.FindDominanceFrontiers(cfg.Blocks);
            Ssa.Rename(cfg.Blocks);
            TransformContext context = new(helpers, cfg.Blocks, definitions, resources, accessor,
                TargetApi.Vulkan, TargetLanguage.Spirv, ShaderStage.Compute, true, ref features);
            Optimizer.RunPass(context);
            TransformPasses.RunPass(context);
            functions[i] = new(cfg.Blocks, $"function{i}", false, 0, 0);
        }
        resources.UnifyArrayTypes(functions);
        StructuredProgramInfo info = StructuredProgram.MakeStructuredProgram(functions, usage, definitions,
            resources, TargetLanguage.Spirv, debug);
        HostCapabilities capabilities = new(false, false, false, false, false, false, false, false, false, false, false);
        return SpirvGenerator.Generate(info, new CodeGenParameters(usage, definitions, resources.Properties,
            capabilities, accessor, TargetApi.Vulkan));
    }

    static void ResourceInputs()
    {
        Accessor accessor = new();
        ResourceManager resources = new(ShaderStage.Compute, accessor, new ResourceReservations(false, false));
        RayTracingProgram program = new([new(0, 1, 1, RayTracingStage.RayGeneration, 32)], [new(0, 256), new(256, 256)], []);
        RayTracingResources inputs = new(resources, program);
        int c3 = resources.GetConstantBufferBinding(3), c0 = resources.GetConstantBufferBinding(0);
        int slot = (program.DescriptorOffset + 0x430 + 0x668 + 14 * 8) / 4;
        EmitterContext c = new();
        c.Load(StorageKind.ConstantBuffer, c3, Const(0), c.IAdd(Const(slot / 4 - 1), Const(1)), Const(slot & 3));
        inputs.RecordInputs(0, c.GetOperations(), program);
        inputs.RecordInputs(0, [new Operation(Instruction.Copy, Local(), Cbuf(3, slot + 1))], program);
        inputs.RecordInputs(256, [new Operation(Instruction.Copy, Local(), Cbuf(3, slot - 16))], program);
        inputs.RecordInputs(0, [new Operation(Instruction.Load, StorageKind.ConstantBuffer, Local(),
            Const(RayTracingProgram.ParametersBinding), Const(0), Const(RayTracingProgram.ImageClassBase / 4), Const(0))], program);
        var slots = inputs.GetAccelerationSlots();
        if (slots[0].Length != 1 || slots[0][0] != 14 || slots[256].Length != 1 || slots[256][0] != 6)
            throw new InvalidOperationException("Native AS dependencies did not preserve module ownership/deduplicate both address words.");
        int rejected = 0;
        void Reject(Operation op)
        {
            try { inputs.RecordInputs(0, [op], program); }
            catch (InvalidOperationException) { rejected++; return; }
            throw new InvalidOperationException("Unsupported native runtime input was accepted.");
        }
        Reject(new Operation(Instruction.Load, StorageKind.ConstantBuffer, Local(), Const(c3), Const(0), Local(), Const(0)));
        Reject(new Operation(Instruction.Load, StorageKind.ConstantBuffer, Local(), Const(c0), Const(0), Const(0), Const(0)));
        Reject(new Operation(Instruction.Load, StorageKind.ConstantBuffer, Local(), Const(99), Const(0), Const(0), Const(0)));
        Reject(new Operation(Instruction.Copy, Local(), Cbuf(2, 0)));
        Reject(new Operation(Instruction.Copy, Local(), Cbuf(3, 0xffff / 4)));
        Reject(new Operation(Instruction.Load, StorageKind.ConstantBuffer, Local(), Const(RayTracingProgram.ParametersBinding), Const(0), Const(0), Const(0)));
        if (rejected != 6) throw new InvalidOperationException("Native resource-input rejection coverage is incomplete.");
        Console.WriteLine("Native resource inputs: module AS slots and 6 unsupported forms checked");
    }

    static int Main(string[] args)
    {
        try
        {
            ResourceInputs();
            int rejected = 0;
            void Reject(Action check)
            {
                try { check(); }
                catch (InvalidOperationException) { rejected++; return; }
                throw new InvalidOperationException("An unsupported native-state operation was accepted.");
            }
            Reject(() => NativeFunctionState.Lower([new Operation(Instruction.Copy, Register(256, RegisterType.Gpr), Const(1))], 0));
            Reject(() => NativeFunctionState.Lower([new Operation(Instruction.Call, null, Local())], 0));
            Reject(() => NativeFunctionState.Lower([new Operation(Instruction.Return, null, Const(1))], 0));
            Reject(() => NativeFunctionState.Lower([new Operation(Instruction.Discard, null)], 0));
            Reject(() => NativeFunctionState.ValidateExits(ControlFlowGraph.Create([new Operation(Instruction.Copy, Local(), Const(1))])));
            Console.WriteLine($"Native validation: {rejected} unsupported operations rejected");
            foreach (bool trace in new[] { false, true })
                foreach (bool debug in new[] { false, true })
                    File.WriteAllBytes(Path.Combine(args[0], $"{(trace ? "trace" : "state")}-{(debug ? "debug" : "optimized")}.spv"), Generate(debug, trace));
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
}
