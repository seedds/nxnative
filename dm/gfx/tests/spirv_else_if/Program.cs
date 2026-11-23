// test_spirv_else_if.py's harness: translate one synthetic sm86 pixel program (a 0x80-byte header and 128-bit
// instructions, built by the test) to SPIR-V with this tree's shader translator, in normal mode and in debug mode, with
// the flags dmsh_xlate passes for a pixel shader without ray tracing (Program.cs: MetalImageAtomics | MetalUniformBounds |
// MetalBufferImageBias | MetalStorageBounds, Ampere for sm86, DebugMode only for a debug job) and its graphics state
// (origin upper-left, no Y flip).
//   ElseIf <program.bin> <out dir>   -> <out dir>/else_if_normal.spv, <out dir>/else_if_debug.spv
// Exit 0 translated, 1 the translator threw (the message is printed), 2 usage.
using System;
using System.IO;
using System.Runtime.InteropServices;
using Ryujinx.Common.Memory;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;

namespace DmshElseIf
{
    sealed class ProgramAccessor : IGpuAccessor
    {
        private readonly ulong[] _code;
        private int _cb, _sb, _tex, _img;

        public ProgramAccessor(byte[] program)
        {
            byte[] padded = new byte[(program.Length + 0x200 + 7) & ~7];   // the decoder may read a little past the end
            program.CopyTo(padded, 0);
            _code = MemoryMarshal.Cast<byte, ulong>(padded).ToArray();
        }

        public void Log(string message) => Console.Error.WriteLine("translator: " + message);

        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => _code.AsSpan((int)(address / 8));

        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, _cb++);
        public SetBindingPair CreateStorageBufferBinding(int index) => new(1, _sb++);
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) { int b = _tex; _tex += count; return new(2, b); }
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) { int b = _img; _img += count; return new(3, b); }
        public int QuerySamplerArrayLengthFromPool() => 1;
        public int QueryTextureArrayLengthFromBuffer(int slot) => 1;
        public int QueryTextureArrayLengthFromPool() => 1;

        // dmsh_xlate's answer for a pixel shader with no job attributes: no Y flip, origin upper-left (the default state
        // gives OriginLowerLeft, which the Vulkan environment does not allow)
        public GpuGraphicsState QueryGraphicsState()
        {
            Array32<AttributeType> attr = default;
            Array8<AttributeType> fout = default;
            return new GpuGraphicsState(false, InputTopology.Triangles, false, TessPatchType.Triangles, TessSpacing.EqualSpacing,
                false, false, false, false, false, 1f, AlphaTestOp.Always, 0f, attr, false, fout, false,
                yNegateEnabled: false, originUpperLeft: true, halvePrimitiveId: false);
        }
    }

    static class Program
    {
        const TranslationFlags XlateFlags = TranslationFlags.Ampere | TranslationFlags.MetalImageAtomics |
            TranslationFlags.MetalUniformBounds | TranslationFlags.MetalBufferImageBias | TranslationFlags.MetalStorageBounds;

        static int Main(string[] args)
        {
            if (args.Length != 2)
            {
                Console.Error.WriteLine("usage: ElseIf <program.bin> <out dir>");
                return 2;
            }

            byte[] code = File.ReadAllBytes(args[0]);
            Directory.CreateDirectory(args[1]);

            foreach ((string mode, TranslationFlags flags) in new[] { ("normal", XlateFlags), ("debug", XlateFlags | TranslationFlags.DebugMode) })
            {
                try
                {
                    TranslatorContext context = Translator.CreateContext(0, new ProgramAccessor(code), new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
                    ShaderProgram program = context.Translate();
                    File.WriteAllBytes(Path.Combine(args[1], $"else_if_{mode}.spv"), program.BinaryCode);
                    Console.WriteLine($"{mode}: {program.BinaryCode.Length} bytes of SPIR-V");
                }
                catch (Exception e)
                {
                    Console.WriteLine($"{mode}: {e.GetType().Name}: {e.Message}");
                    return 1;
                }
            }

            return 0;
        }
    }
}
