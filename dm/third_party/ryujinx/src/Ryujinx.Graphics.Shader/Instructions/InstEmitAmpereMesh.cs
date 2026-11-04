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
        public static void RegisterAmpereMesh(AmpereRegistrar register)
        {
            register(0x923, InstName.Isberd, AMeshIsberd, InstProps.None);
            register(0x927, InstName.Ast, AMeshIsbewr, InstProps.None, writesRd: false);
        }

        private static bool IsAmpereMesh(EmitterContext context) =>
            context.TranslatorContext.Options.Flags.HasFlag(TranslationFlags.AmpereMesh);

        public static void AMeshIsberd(EmitterContext context)
        {
            if (!IsAmpereMesh(context))
            {
                AIsberd(context);
                return;
            }
            AmpereOp instruction = new(context.CurrOp);
            if (instruction.Bits(76, 78) != 3 || instruction.Bits(74, 76) != 2)
            {
                throw AFail(context, "ISBERD task/mesh access other than ATTR.32");
            }
            Operand offset = context.ShiftRightU32(context.IAdd(AGpr(instruction.Ra), Const(instruction.Bits(40, 56))), Const(2));
            int memoryId = context.ResourceManager.GetIsbeMemoryId(instruction.Bit(79) ? 1 : 0);
            context.Copy(Register(instruction.Rd, RegisterType.Gpr), context.Load(StorageKind.SharedMemory, memoryId, offset));
        }

        public static void AMeshIsbewr(EmitterContext context)
        {
            if (!IsAmpereMesh(context))
            {
                throw AFail(context, "ISBEWR outside a task/mesh program");
            }
            AmpereOp instruction = new(context.CurrOp);
            int accessKind = instruction.Bits(76, 78);
            int sizeLog2 = instruction.Bits(74, 76);
            if (!instruction.Bit(79) || (accessKind != 0 && accessKind != 3) || sizeLog2 > 2 || (accessKind == 3 && sizeLog2 != 2))
            {
                throw AFail(context, "ISBEWR task/mesh access other than output MAP or ATTR.32");
            }
            Operand offset = context.IAdd(AGpr(instruction.Ra), Const(instruction.Bits(40, 56)));
            Operand value = AGpr(instruction.Rb);
            int memoryId = context.ResourceManager.GetIsbeMemoryId(accessKind == 0 ? 2 : 1);
            if (accessKind == 3)
            {
                context.Store(StorageKind.SharedMemory, memoryId, context.ShiftRightU32(offset, Const(2)), value);
            }
            else
            {
                for (int byteIndex = 0; byteIndex < (1 << sizeLog2); byteIndex++)
                {
                    Operand byteOffset = context.ISubtract(offset, Const(byteIndex));
                    Operand byteValue = context.BitfieldExtractU32(value, Const(8 * byteIndex), Const(8));
                    context.Store(StorageKind.SharedMemory, memoryId, byteOffset, byteValue);
                }
            }
        }
    }
}
