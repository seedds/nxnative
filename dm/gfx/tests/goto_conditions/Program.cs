// Ordinary forward CFGs with skipped condition definitions. No RT/native code.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.CodeGen;
using Ryujinx.Graphics.Shader.CodeGen.Spirv;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using Ryujinx.Graphics.Shader.Translation;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

static class Program
{
    const int Variants = 64, Blocks = 12, Output = 1024;
    static long checks;
    readonly record struct Edge(int Target, bool IfTrue, bool Unconditional);
    sealed class Accessor : IGpuAccessor
    {
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => throw new InvalidOperationException();
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, index + 2);
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, 1);
        public SetBindingPair CreateImageBinding(int count, bool buffer) => new(0, 10);
        public SetBindingPair CreateTextureBinding(int count, bool buffer) => new(0, 20);
        public int QuerySamplerArrayLengthFromPool() => 1;
        public int QueryTextureArrayLengthFromBuffer(int index) => 1;
        public int QueryTextureArrayLengthFromPool() => 1;
    }

    // Interpret the structured AST with eager logical operands, like its
    // SPIR-V operations. A read before definition fails even when another
    // operand could mask its value. This checks more than the output alone.
    static void Execute(AstBlock root, int[] memory)
    {
        Dictionary<AstOperand, int> locals = new();
        int Eval(IAstNode node)
        {
            if (node is AstOperand operand)
            {
                if (operand.Type == OperandType.Constant) return operand.Value;
                if (operand.Type == OperandType.LocalVariable && locals.TryGetValue(operand, out int value)) return value;
                throw new InvalidOperationException("Structured control flow reads an undefined local.");
            }
            AstOperation operation = (AstOperation)node;
            // Evaluate every source before the operation, deliberately avoiding
            // the host language's short-circuit behavior for AND/OR below.
            int[] s = Enumerable.Range(0, operation.SourcesCount).Select(i => Eval(operation.GetSource(i))).ToArray();
            int Bool(bool value) => value ? -1 : 0;
            return operation.Inst switch
            {
                Instruction.Load when operation.StorageKind == StorageKind.StorageBuffer => memory[s[2]],
                Instruction.CompareNotEqual => Bool(s[0] != s[1]),
                Instruction.LogicalNot => Bool(s[0] == 0),
                Instruction.LogicalAnd => Bool((s[0] != 0) & (s[1] != 0)),
                Instruction.LogicalOr => Bool((s[0] != 0) | (s[1] != 0)),
                Instruction.Copy => s[0],
                _ => throw new InvalidOperationException("Unexpected fixture expression " + operation.Inst),
            };
        }
        bool Run(AstBlock block)
        {
            bool taken = false;
            foreach (IAstNode node in block)
            {
                if (node is AstAssignment assignment) locals[(AstOperand)assignment.Destination] = Eval(assignment.Source);
                else if (node is AstBlock child)
                {
                    bool execute = child.Type switch
                    {
                        AstBlockType.If => Eval(child.Condition) != 0,
                        AstBlockType.ElseIf => !taken && Eval(child.Condition) != 0,
                        AstBlockType.Else => !taken,
                        _ => throw new InvalidOperationException("Unexpected fixture block " + child.Type),
                    };
                    taken = child.Type == AstBlockType.If ? execute : taken || execute;
                    if (execute && Run(child)) return true;
                }
                else if (node is AstOperation operation)
                {
                    if (operation.Inst == Instruction.Return) return true;
                    if (operation.Inst != Instruction.Store || operation.StorageKind != StorageKind.StorageBuffer)
                        throw new InvalidOperationException("Unexpected fixture statement " + operation.Inst);
                    memory[Eval(operation.GetSource(2))] = Eval(operation.GetSource(3));
                }
                else throw new InvalidOperationException("Unexpected fixture node.");
            }
            return false;
        }
        Run(root);
    }

    static void Validate(StructuredProgramInfo info, Edge[][] graphs)
    {
        int[] memory = new int[Output + Variants * Blocks];
        for (int pattern = 0; pattern < 1 << (Blocks - 1); pattern++)
        {
            for (int i = 0; i < Blocks - 1; i++) memory[i] = (pattern >> i) & 1;
            for (int v = 0; v < Variants; v++)
            {
                Array.Fill(memory, -123, Output, Variants * Blocks);
                Execute(info.Functions[v + 1].MainBlock, memory);
                bool[] visited = new bool[Blocks];
                // Independent original-CFG interpreter: it never sees the AST
                // or the compiler's flags, transformations or generated text.
                for (int i = 0; i < Blocks;)
                {
                    visited[i] = true;
                    if (i == Blocks - 1) break;
                    Edge edge = graphs[v][i];
                    i = edge.Unconditional || (memory[i] != 0) == edge.IfTrue ? edge.Target : i + 1;
                }
                for (int i = 0; i < Blocks; i++)
                {
                    checks++;
                    if (memory[Output + v * Blocks + i] != (visited[i] ? i + 1 : -123))
                        throw new InvalidOperationException($"Control-flow mismatch: variant {v}, pattern {pattern}, block {i}.");
                }
            }
        }
    }

    static byte[] Generate(bool debug)
    {
        Accessor accessor = new(); AttributeUsage usage = new(accessor);
        ShaderDefinitions definitions = new(ShaderStage.Compute, 1, 1, 1);
        ResourceManager resources = new(ShaderStage.Compute, accessor, new ResourceReservations(false, false));
        resources.Properties.AddOrUpdateStorageBuffer(new BufferDefinition(BufferLayout.Std430, 0, 1, "test_data",
            new StructureType([new(AggregateType.Array | AggregateType.U32, "words", 0)])));
        EmitterContext main = new(); Function[] functions = new Function[Variants + 1]; Edge[][] graphs = new Edge[Variants][];
        for (int v = 0; v < Variants; v++)
        {
            EmitterContext c = new(); Operand[] labels = Enumerable.Range(0, Blocks).Select(_ => Label()).ToArray();
            graphs[v] = new Edge[Blocks - 1]; Random random = new(v + 19);
            for (int i = 0; i < Blocks; i++)
            {
                c.MarkLabel(labels[i]); c.Store(StorageKind.StorageBuffer, 1, Const(0), Const(Output + v * Blocks + i), Const(i + 1));
                if (i == Blocks - 1) { c.Return(); continue; }
                Edge edge = new(random.Next(i + 1, Blocks), random.Next(2) == 0, v >= Variants / 2 && random.Next(4) == 0);
                graphs[v][i] = edge;
                if (edge.Unconditional) { c.Branch(labels[edge.Target]); continue; }
                Operand condition = c.ICompareNotEqual(c.Load(StorageKind.StorageBuffer, 1, Const(0), Const(i)), Const(0));
                if (edge.IfTrue) c.BranchIfTrue(labels[edge.Target], condition);
                else c.BranchIfFalse(labels[edge.Target], condition);
            }
            ControlFlowGraph cfg = ControlFlowGraph.Create(c.GetOperations());
            Dominance.FindDominators(cfg); Dominance.FindDominanceFrontiers(cfg.Blocks); Ssa.Rename(cfg.Blocks);
            functions[v + 1] = new(cfg.Blocks, "case" + v, false, 0, 0);
            main.Add(new Operation(Instruction.Call, null, Const(v + 1)));
        }
        main.Return(); functions[0] = new(ControlFlowGraph.Create(main.GetOperations()).Blocks, "main", false, 0, 0);
        StructuredProgramInfo info = StructuredProgram.MakeStructuredProgram(functions, usage, definitions, resources, TargetLanguage.Spirv, debug);
        Validate(info, graphs);
        HostCapabilities capabilities = new(false, false, false, false, false, false, false, false, false, false, false);
        return SpirvGenerator.Generate(info, new CodeGenParameters(usage, definitions, resources.Properties, capabilities, accessor, TargetApi.Vulkan));
    }
    static int Main(string[] args)
    {
        try
        {
            Directory.CreateDirectory(args[0]);
            foreach (bool debug in new[] { false, true })
                File.WriteAllBytes(Path.Combine(args[0], debug ? "debug.spv" : "optimized.spv"), Generate(debug));
            Console.WriteLine($"Goto entry conditions: {checks} path/output checks passed; no undefined AST reads."); return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
}
