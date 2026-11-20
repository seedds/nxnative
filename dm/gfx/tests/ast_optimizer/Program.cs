// Ordinary compute IR only: no explicit continuation mode or native/game code.
using System;
using System.IO;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.CodeGen;
using Ryujinx.Graphics.Shader.CodeGen.Spirv;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using Ryujinx.Graphics.Shader.Translation;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

static class Program
{
    sealed class Accessor : IGpuAccessor
    {
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) =>
            throw new InvalidOperationException("The IR fixture does not decode machine code.");
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, index + 2);
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, index + 1);
        public SetBindingPair CreateImageBinding(int count, bool buffer) => new(0, 10);
        public SetBindingPair CreateTextureBinding(int count, bool buffer) => new(0, 20);
        public int QuerySamplerArrayLengthFromPool() => 1;
        public int QueryTextureArrayLengthFromBuffer(int index) => 1;
        public int QueryTextureArrayLengthFromPool() => 1;
    }

    static Function Finish(EmitterContext context, string name, bool returns, int input, int output) =>
        new(ControlFlowGraph.Create(context.GetOperations()).Blocks, name, returns, input, output);

    static byte[] Generate(bool debug)
    {
        Accessor accessor = new();
        AttributeUsage usage = new(accessor);
        ShaderDefinitions definitions = new(ShaderStage.Compute, 1, 1, 1);
        ResourceManager resources = new(ShaderStage.Compute, accessor, new ResourceReservations(false, false));
        resources.Properties.AddOrUpdateStorageBuffer(new BufferDefinition(BufferLayout.Std430, 0, 1, "test_data",
            new StructureType([new(AggregateType.Array | AggregateType.U32, "words", 0)])));

        EmitterContext main = new(), outer = new(), inner = new(), effect = new();
        Operand result = Local(), output = Local(), unusedOutput = Local();
        main.Copy(unusedOutput, Const(999)); // Its value is dead; its call-output storage is required.
        Operand expression = main.IAdd(Const(3), Const(4));
        main.Add(new Operation(Instruction.Call, 0, [result, output, unusedOutput], [Const(1), Const(6), expression]));
        main.Add(new Operation(Instruction.Call, Local(), Const(3))); // Unused return, live storage effect.
        main.Store(StorageKind.StorageBuffer, 1, Const(0), Const(0), result);
        main.Store(StorageKind.StorageBuffer, 1, Const(0), Const(1), output);

        Operand snapshot = main.Load(StorageKind.StorageBuffer, 1, Const(0), Const(3));
        main.Store(StorageKind.StorageBuffer, 1, Const(0), Const(3), Const(99));
        main.Store(StorageKind.StorageBuffer, 1, Const(0), Const(4), main.IAdd(snapshot, Const(7)));
        Operand outputSnapshot = main.Copy(output);
        main.Add(new Operation(Instruction.Call, 0, [Local(), output, unusedOutput], [Const(1), Const(10), Const(11)]));
        main.Store(StorageKind.StorageBuffer, 1, Const(0), Const(5), outputSnapshot);
        main.Store(StorageKind.StorageBuffer, 1, Const(0), Const(6), output);
        main.AtomicAdd(StorageKind.StorageBuffer, 1, Const(0), Const(7), Const(5)); // Unused atomic result.
        main.Return();

        // Forward formal input values through another by-reference call. Its
        // outputs must be read after the call, even if the return is used later.
        Operand nestedReturn = Local(), nestedOutput = Local(), nestedUnused = Local();
        outer.Copy(nestedUnused, Const(123));
        outer.Add(new Operation(Instruction.Call, 0, [nestedReturn, nestedOutput, nestedUnused],
            [Const(2), Argument(0), Argument(1)]));
        outer.Copy(Argument(2), nestedOutput);
        outer.Copy(Argument(3), nestedUnused);
        outer.Return(nestedReturn);
        inner.Copy(Argument(2), inner.IAdd(Argument(0), Argument(1)));
        inner.Copy(Argument(3), inner.ISubtract(Argument(0), Argument(1)));
        inner.Return(inner.IMultiply(Argument(0), Argument(1)));
        effect.Store(StorageKind.StorageBuffer, 1, Const(0), Const(2), Const(77));
        effect.Return(Const(99));

        Function[] functions = [Finish(main, "main", false, 0, 0), Finish(outer, "outer", true, 2, 2),
            Finish(inner, "inner", true, 2, 2), Finish(effect, "effect", true, 0, 0)];
        StructuredProgramInfo info = StructuredProgram.MakeStructuredProgram(functions, usage, definitions,
            resources, TargetLanguage.Spirv, debug);
        HostCapabilities capabilities = new(false, false, false, false, false, false, false, false, false, false, false);
        return SpirvGenerator.Generate(info, new CodeGenParameters(usage, definitions, resources.Properties,
            capabilities, accessor, TargetApi.Vulkan));
    }

    static int Main(string[] args)
    {
        try
        {
            File.WriteAllBytes(Path.Combine(args[0], "optimized.spv"), Generate(false));
            File.WriteAllBytes(Path.Combine(args[0], "debug.spv"), Generate(true));
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 1;
        }
    }
}
