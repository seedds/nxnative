namespace Ryujinx.Graphics.Shader
{
    public readonly struct BufferDescriptor
    {
        // New fields should be added to the end of the struct to keep disk shader cache compatibility.

        public readonly int Set;
        public readonly int Binding;
        public readonly byte Slot;
        public readonly byte SbCbSlot;
        public readonly ushort SbCbOffset;
        public readonly BufferUsageFlags Flags;
        // nxnative: a game uniform slot's bytes that the shader reads at fixed offsets, from its start to the end of the highest such word (ResourceManager.NoteUniformRead)
        public readonly int FixedSize;

        public BufferDescriptor(int set, int binding, int slot, int fixedSize = 0)
        {
            Set = set;
            Binding = binding;
            Slot = (byte)slot;
            SbCbSlot = 0;
            SbCbOffset = 0;
            Flags = BufferUsageFlags.None;
            FixedSize = fixedSize;
        }

        public BufferDescriptor(int set, int binding, int slot, int sbCbSlot, int sbCbOffset, BufferUsageFlags flags)
        {
            Set = set;
            Binding = binding;
            Slot = (byte)slot;
            SbCbSlot = (byte)sbCbSlot;
            SbCbOffset = (ushort)sbCbOffset;
            Flags = flags;
            FixedSize = 0;
        }
    }
}
