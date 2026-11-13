using Ryujinx.Graphics.Shader.Translation;

namespace Ryujinx.Graphics.Shader.StructuredIr
{
    readonly struct MemoryDefinition
    {
        public string Name { get; }
        public AggregateType Type { get; }
        public int ArrayLength { get; }

        // nxnative: the memory starts at zero in each invocation (a SPIR-V null initializer), for state a shader reads before it writes (GlobalToStorage.TryGenerateRawStorageOp's last entry)
        public bool ZeroInit { get; }

        public MemoryDefinition(string name, AggregateType type, int arrayLength = 1, bool zeroInit = false)
        {
            Name = name;
            Type = type;
            ArrayLength = arrayLength;
            ZeroInit = zeroInit;
        }
    }
}
