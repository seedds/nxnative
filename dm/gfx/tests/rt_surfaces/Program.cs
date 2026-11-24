// Hand-built IR and synthetic instruction fields, with no game shader data.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.Instructions;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

static class Program
{
    static int checks;
    static void Check(bool condition, string message)
    {
        checks++; if (!condition) throw new InvalidOperationException(message);
    }
    static void Reject(Action action, string message)
    {
        try { action(); }
        catch (InvalidOperationException) { checks++; return; }
        throw new InvalidOperationException("Accepted " + message);
    }

    static (int Writes, int Kind, int Fault) Execute(Operation[] code, int componentClass, int enabled = 1, int initialFault = 0)
    {
        Dictionary<Operand, int> values = new();
        Dictionary<Operand, int> labels = new();
        for (int i = 0; i < code.Length; i++) if (code[i].Inst == Instruction.MarkLabel) labels.Add(code[i].Dest, i);
        int Read(Operand value) => value.Type == OperandType.Constant ? value.Value :
            value.Type == OperandType.Argument ? value.Value == 0 ? componentClass : enabled : values[value];
        int writes = 0, kind = -1, fault = initialFault;
        for (int pc = 0, steps = 0; pc < code.Length; pc++)
        {
            if (++steps > 100) throw new InvalidOperationException("Synthetic branch interpreter did not terminate.");
            Operation op = code[pc];
            switch (op.Inst)
            {
                case Instruction.CompareEqual: values[op.Dest] = Read(op.GetSource(0)) == Read(op.GetSource(1)) ? -1 : 0; break;
                case Instruction.BranchIfTrue: if (Read(op.GetSource(0)) != 0) pc = labels[op.Dest]; break;
                case Instruction.BranchIfFalse: if (Read(op.GetSource(0)) == 0) pc = labels[op.Dest]; break;
                case Instruction.Branch: pc = labels[op.Dest]; break;
                case Instruction.MarkLabel: break;
                case Instruction.Return: return (writes, kind, fault);
                case Instruction.Store when op.StorageKind == StorageKind.LocalMemory: fault = Read(op.GetSource(2)); break;
                case Instruction.Store when op.StorageKind == StorageKind.StorageBuffer: writes++; kind = Read(op.GetSource(3)); break;
                default: throw new InvalidOperationException("Unexpected synthetic operation " + op.Inst);
            }
        }
        return (writes, kind, fault);
    }

    static void DispatchProof()
    {
        EmitterContext c = new(); Operand end = Label();
        c.BranchIfFalse(end, Argument(1));
        List<TextureFormat> emitted = new();
        RayTracingSurfaces.EmitClassDispatch(c, Argument(0), 7, format =>
        {
            int kind = emitted.Count; emitted.Add(format);
            c.Store(StorageKind.StorageBuffer, 8, Const(0), Const(0), Const(kind));
        });
        c.MarkLabel(end); c.Return();
        Operation[] operations = c.GetOperations();
        Check(emitted.SequenceEqual(new[] { TextureFormat.R32G32B32A32Float, TextureFormat.R32G32B32A32Sint,
            TextureFormat.R32G32B32A32Uint }), "Three explicit representative component classes are emitted.");
        for (int kind = 0; kind < 3; kind++)
        {
            var result = Execute(operations, kind);
            Check(result == (1, kind, 0), "Exactly one matching typed store executes.");
            Check(Execute(operations, kind, initialFault: 17) == (1, kind, 17), "A valid store never clears an earlier native fault.");
        }
        foreach (int invalid in new[] { 3, -1, 4, int.MaxValue })
        {
            Check(Execute(operations, invalid) == (0, -1, 1), "Unbound or invalid executed class faults without a default store.");
            Check(Execute(operations, invalid, enabled: 0) == (0, -1, 0), "A predicated-off unbound image does not fault.");
        }
    }

    sealed class Accessor(byte[] code) : IGpuAccessor
    {
        int binding = RayTracingProgram.ReservedBindings;
        public int FormatQueries;
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => address >= (ulong)code.Length ?
            default : MemoryMarshal.Cast<byte, ulong>(code.AsSpan((int)address));
        public bool QueryRawGlobalMemory() => true;
        public bool QueryHostSupportsImageLoadFormatted() => false;
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, binding++);
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, binding++);
        public SetBindingPair CreateImageBinding(int count, bool buffer) { int b = binding; binding += count; return new(0, b); }
        public SetBindingPair CreateTextureBinding(int count, bool buffer) { int b = binding; binding += count; return new(0, b); }
        public int QuerySamplerArrayLengthFromPool() => 1;
        public int QueryTextureArrayLengthFromBuffer(int index) => 1;
        public int QueryTextureArrayLengthFromPool() => 1;
        public TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1) { FormatQueries++; return TextureFormat.R8G8B8A8Unorm; }
    }

    static UInt128 Base(int opcode) => (UInt128)(uint)opcode | (UInt128)7 << 12;
    static UInt128 Store(int handle, int mask = 15, int opcode = 0xb9b) => Base(opcode) | (UInt128)10 << 32 |
        (UInt128)(uint)handle << 40 | (UInt128)3 << 54 | (UInt128)3 << 61 | (UInt128)(uint)mask << 72;
    static UInt128 Ret() => Base(AmpereOpcodes.Ret) | (UInt128)4 << 24 | (UInt128)3 << 85 | (UInt128)7 << 87;
    static UInt128 Move(int destination, int value) => Base(0x802) | (UInt128)(uint)destination << 16 | (UInt128)(uint)value << 32;
    static byte[] Code(params UInt128[] words)
    {
        byte[] bytes = new byte[words.Length * 16];
        for (int i = 0; i < words.Length; i++)
        {
            BitConverter.GetBytes((ulong)words[i]).CopyTo(bytes, i * 16);
            BitConverter.GetBytes((ulong)(words[i] >> 64)).CopyTo(bytes, i * 16 + 8);
        }
        return bytes;
    }
    static RayTracingProgram Manifest(int length, int descriptorOffset = 0x140) => new(
        [new(0, 1, 1, RayTracingStage.RayGeneration, 0)], [new(0, length)], [], descriptorOffset: descriptorOffset);

    static void SlotProof()
    {
        foreach (int offset in new[] { 0x140, 0x200 })
        {
            RayTracingProgram p = Manifest(16, offset);
            for (int slot = 0; slot < RayTracingProgram.ImageClassCount; slot++)
                Check(RayTracingSurfaces.ImageSlot(p, (offset + 0x550 + slot * 8) / 4, 3) == slot, "Image slot follows metadata-derived descriptor placement.");
            foreach (var bad in new[] { ((offset + 0x550) / 4, 2), ((offset + 0x550 + 4) / 4, 3),
                ((offset + 0x550 - 8) / 4, 3), ((offset + 0x550 + RayTracingProgram.ImageClassCount * 8) / 4, 3), (int.MaxValue, 3) })
                Reject(() => RayTracingSurfaces.ImageSlot(p, bad.Item1, bad.Item2), "unsupported descriptor image address");
        }
    }

    static void TranslationProof(string output)
    {
        byte[] bytes = Code(Store(0x1a4), Store(0x1a6), Move(21, -7), Ret());
        Accessor accessor = new(bytes);
        TranslationOptions options = new(TargetLanguage.Spirv, TargetApi.Vulkan, TranslationFlags.Ampere | TranslationFlags.Compute);
        ShaderProgram shader = Translator.CreateRayTracingContext(accessor, options, Manifest(bytes.Length)).Translate();
        Check(accessor.FormatQueries == 0, "RT formatted stores never ask for a guessed image format.");
        Check(shader.Info.Images.Count == 6 && shader.Info.Images.Select(x => x.Binding).Distinct().Count() == 6,
            "Two image slots have six distinct class bindings, not exponential shader variants.");
        foreach (int handle in new[] { 0x1a4, 0x1a6 })
            Check(shader.Info.Images.Where(x => x.HandleIndex == handle).Select(x => x.Format).Order().SequenceEqual(
                new[] { TextureFormat.R32G32B32A32Float, TextureFormat.R32G32B32A32Sint, TextureFormat.R32G32B32A32Uint }.Order()),
                "Each handle reflects float/sint/uint component classes separately.");
        if (output != null) { Directory.CreateDirectory(output); File.WriteAllBytes(Path.Combine(output, "surfaces.spv"), shader.BinaryCode); }
        foreach (int mask in new[] { 1, 3 })
        {
            byte[] narrow = Code(Store(0x1a4, mask), Move(21, -7), Ret());
            Check(Translator.CreateRayTracingContext(new Accessor(narrow), options, Manifest(narrow.Length)).Translate().Info.Images.Count == 3,
                "Formatted R/RG vectors retain the same runtime component-class contract.");
        }
        foreach (UInt128 unsupported in new[] { Store(0x1a4, mask: 2), Store(0x1a4, opcode: 0xb9d), Store(0x1a4, opcode: 0x99c), Store(0x1a4, opcode: 0xb97) })
        {
            byte[] bad = Code(unsupported, Move(21, -7), Ret());
            Reject(() => Translator.CreateRayTracingContext(new Accessor(bad), options, Manifest(bad.Length)).Translate(),
                "unsupported RT surface variant");
        }
        // The same frontend outside RT still uses the ordinary format query
        // and emits one store, preserving existing renderer behavior.
        ShaderDefinitions definitions = new(ShaderStage.Compute, 1, 1, 1);
        Accessor ordinary = new([]);
        TranslatorContext context = new(0, 0, 0, definitions, ordinary, options, default);
        ResourceManager resources = new(ShaderStage.Compute, ordinary, new ResourceReservations(false, false));
        EmitterContext emission = new(context, resources, default, false, false) { CurrOp = AmpereTable.GetOp(0, (ulong)Store(0x1a4), (ulong)(Store(0x1a4) >> 64)) };
        InstEmit.ASurface(emission);
        Check(ordinary.FormatQueries == 1 && emission.GetOperations().Count(x => x.Inst == Instruction.ImageStore) == 1,
            $"Non-RT formatted stores retain the existing format query and single-store path (queries={ordinary.FormatQueries}, stores={emission.GetOperations().Count(x => x.Inst == Instruction.ImageStore)}).");
    }

    static int Main(string[] args)
    {
        try { DispatchProof(); SlotProof(); TranslationProof(args.FirstOrDefault()); Console.WriteLine($"RT formatted surfaces: {checks} checks passed"); return 0; }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
}
