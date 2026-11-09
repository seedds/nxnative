namespace Ryujinx.Graphics.Shader
{
    static class Constants
    {
        public const int ConstantBufferSize = 0x10000; // In bytes

        public const int MaxAttributes = 16;
        public const int AllAttributesMask = (int)(uint.MaxValue >> (32 - MaxAttributes));

        public const int NvnBaseVertexByteOffset = 0x640;
        public const int NvnBaseInstanceByteOffset = 0x644;
        public const int NvnDrawIndexByteOffset = 0x648;

        // nxnative: uniform bounds (TranslationFlags.MetalUniformBounds). Uniform slots c[3] to c[16] are the game's (NVN's 14 a stage); the host gives the bound size of each
        // as a u32 in the table of bank 25 (dm/gfx/nvn/dmsh_abi.h DMSH_CBSIZE_BANK, kind DMSH_KIND_CBSIZE), slot c[s] at index s - 3, in four uint4
        public const int NxFirstUniformSlot = 3;
        public const int NxUniformSlots = 14;
        public const int NxUniformSizesBank = 25;
    }
}
