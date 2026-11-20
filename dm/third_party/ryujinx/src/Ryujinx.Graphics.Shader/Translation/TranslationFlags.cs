using System;

namespace Ryujinx.Graphics.Shader.Translation
{
    [Flags]
    public enum TranslationFlags
    {
        None = 0,

        VertexA = 1 << 0,
        Compute = 1 << 1,
        DebugMode = 1 << 2,
        // Offline Metal: image atomics run natively on the texture (MSL 3.1), guarded for IGN bounds.
        MetalImageAtomics = 1 << 3,
        // Ampere (sm_86) machine code, as NVN2 (Switch 2) ships it: a 0x80-byte shader header (SPH version 4, its first 0x50 bytes laid out as version 3)
        // and 128-bit instructions, decoded and emitted by the Ampere frontend.
        Ampere = 1 << 4,
        // Offline Metal: a read of a game uniform slot (c[3] to c[16]) past the size the game bound gives 0, as Maxwell's constant buffers do; Metal has no bounds
        // of its own, so the read reached the guest memory past the binding. A read at a computed offset is compared with the size (Constants.NxUniformSizesBank);
        // the reads at fixed offsets are reported as an extent of the slot (BufferDescriptor.FixedSize), which the host makes readable
        MetalUniformBounds = 1 << 5,
        // Offline Metal: a buffer image's coordinate is moved by its view's element offset from the host's table (Constants.NxBufferImageBiasBank), so that the host can
        // bind it as a view from an aligned offset; a coordinate before the image stays outside it
        MetalBufferImageBias = 1 << 6,
        // Offline Metal: an access to a storage buffer whose address the shader takes from a game uniform slot or the push constants (an address with no size) gives 0,
        // or for a store or an atomic is dropped, outside the memory the host bound there, from the host's table (Constants.NxStorageLengthsBank). On the console
        // such an address reads the GPU's page table as far as the shader indexes, and an unmapped page reads 0; on Metal a read past the bound buffer reached
        // whatever lay after it in the GPU's address space
        MetalStorageBounds = 1 << 7,
    }
}
