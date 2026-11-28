using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation
{
    // SUST.P interprets each source word according to the bound descriptor:
    // float for normalized/float storage, signed or unsigned integer otherwise.
    // Keep that choice at dispatch time. Representative formats choose Metal's
    // component type; the actual bound image retains its width/normalization.
    static class RayTracingSurfaces
    {
        public const int FloatClass = 0, SintClass = 1, UintClass = 2, UnboundClass = 3;
        private const int NativeImageHandlesOffset = 0x430 + 0x120;

        public static int ImageSlot(RayTracingProgram program, int handle, int cbufSlot)
        {
            ArgumentNullException.ThrowIfNull(program);
            long offset = (long)handle * 4 - program.DescriptorOffset - NativeImageHandlesOffset;
            if (cbufSlot != 3 || offset < 0 || (offset & 7) != 0 || offset / 8 >= RayTracingProgram.ImageClassCount)
                throw new InvalidOperationException("RT formatted image store does not name a supported descriptor image slot.");
            return (int)(offset / 8);
        }

        public static void EmitFormattedStore(EmitterContext context, int handle, int cbufSlot, Action<TextureFormat> emitTyped)
        {
            RayTracingProgram program = context.TranslatorContext?.RayTracingProgram ??
                throw new InvalidOperationException("RT formatted surface dispatch requires a ray-tracing program.");
            int slot = ImageSlot(program, handle, cbufSlot);
            int fault = context.TranslatorContext.Continuations?.Fault ??
                throw new InvalidOperationException("RT formatted surface dispatch has no private native fault context.");
            int word = RayTracingProgram.ImageClassBase + slot;
            Operand componentClass = context.Load(StorageKind.ConstantBuffer, RayTracingResources.ParametersBinding,
                Const(0), Const(word / 4), Const(word & 3));
            EmitClassDispatch(context, componentClass, fault, emitTyped);
        }

        // Also accepts ordinary IR input for focused tests. This emits one
        // reachable typed store, or a fault/return; it never emits a default
        // store or guesses a missing image's format.
        public static void EmitClassDispatch(EmitterContext context, Operand componentClass,
            int faultMemory, Action<TextureFormat> emitTyped)
        {
            ArgumentNullException.ThrowIfNull(context);
            ArgumentNullException.ThrowIfNull(componentClass);
            ArgumentNullException.ThrowIfNull(emitTyped);
            if (faultMemory < 0) throw new ArgumentOutOfRangeException(nameof(faultMemory));
            TextureFormat[] formats = [TextureFormat.R32G32B32A32Float,
                TextureFormat.R32G32B32A32Sint, TextureFormat.R32G32B32A32Uint];
            Operand[] cases = [Label(), Label(), Label()];
            Operand done = Label();
            for (int kind = FloatClass; kind <= UintClass; kind++)
                context.BranchIfTrue(cases[kind], context.ICompareEqual(componentClass, Const(kind)));
            context.Store(StorageKind.LocalMemory, faultMemory, Const(0), Const(1));
            context.Return();
            for (int kind = FloatClass; kind <= UintClass; kind++)
            {
                context.MarkLabel(cases[kind]);
                emitTyped(formats[kind]);
                context.Branch(done);
            }
            context.MarkLabel(done);
        }
    }
}
