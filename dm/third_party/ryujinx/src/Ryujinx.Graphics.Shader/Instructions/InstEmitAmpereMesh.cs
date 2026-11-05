using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    static partial class InstEmit
    {
        // SM86 ISBE encoding: Mesa NAK sm70_encode.rs, OpIsberd/OpIsbewr.
        // Attribute offsets already address the packed 32-lane attribute lines.
        // MAP stores run backwards from their last byte; one uint per map byte
        // avoids a read/modify/write race between adjacent primitive indices.
        public static void RegisterAmpereMesh(AmpereRegistrar x)
        {
            x(0x923, InstName.Isberd, AMeshIsberd, InstProps.None);
            x(0x927, InstName.Ast, AMeshIsbewr, InstProps.None, writesRd: false);
        }

        private static bool IsAmpereMesh(EmitterContext context) =>
            context.TranslatorContext.Options.Flags.HasFlag(TranslationFlags.AmpereMesh);

        public static void AMeshIsberd(EmitterContext context)
        {
            if (!IsAmpereMesh(context)) { AIsberd(context); return; }
            AmpereOp x = new(context.CurrOp);
            if (x.Bits(76, 78) != 3 || x.Bits(74, 76) != 2)
                throw AFail(context, "ISBERD task/mesh access other than ATTR.32");
            Operand offset = context.ShiftRightU32(context.IAdd(AGpr(x.Ra), Const(x.Bits(40, 56))), Const(2));
            int memory = context.ResourceManager.GetIsbeMemoryId(x.Bit(79) ? 1 : 0);
            context.Copy(Register(x.Rd, RegisterType.Gpr), context.Load(StorageKind.SharedMemory, memory, offset));
        }

        public static void AMeshIsbewr(EmitterContext context)
        {
            if (!IsAmpereMesh(context)) throw AFail(context, "ISBEWR outside a task/mesh program");
            AmpereOp x = new(context.CurrOp);
            int access = x.Bits(76, 78), size = x.Bits(74, 76);
            if (!x.Bit(79) || (access != 0 && access != 3) || size > 2 || (access == 3 && size != 2))
                throw AFail(context, "ISBEWR task/mesh access other than output MAP or ATTR.32");
            Operand offset = context.IAdd(AGpr(x.Ra), Const(x.Bits(40, 56)));
            Operand value = AGpr(x.Rb);
            int memory = context.ResourceManager.GetIsbeMemoryId(access == 0 ? 2 : 1);
            if (access == 3)
            {
                context.Store(StorageKind.SharedMemory, memory, context.ShiftRightU32(offset, Const(2)), value);
            }
            else
            {
                for (int i = 0; i < (1 << size); i++)
                    context.Store(StorageKind.SharedMemory, memory, context.ISubtract(offset, Const(i)),
                        context.BitfieldExtractU32(value, Const(8 * i), Const(8)));
            }
        }
    }
}
