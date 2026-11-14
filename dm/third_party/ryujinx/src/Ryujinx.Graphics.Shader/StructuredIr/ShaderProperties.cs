using System;
using System.Collections.Generic;

namespace Ryujinx.Graphics.Shader.StructuredIr
{
    class ShaderProperties
    {
        private readonly Dictionary<int, BufferDefinition> _constantBuffers;
        private readonly Dictionary<int, BufferDefinition> _storageBuffers;
        private readonly Dictionary<SetBindingPair, TextureDefinition> _textures;
        private readonly Dictionary<SetBindingPair, TextureDefinition> _images;
        private readonly Dictionary<int, MemoryDefinition> _localMemories;
        private readonly Dictionary<int, MemoryDefinition> _sharedMemories;

        public IReadOnlyDictionary<int, BufferDefinition> ConstantBuffers => _constantBuffers;
        public IReadOnlyDictionary<int, BufferDefinition> StorageBuffers => _storageBuffers;
        public IReadOnlyDictionary<SetBindingPair, TextureDefinition> Textures => _textures;
        public IReadOnlyDictionary<SetBindingPair, TextureDefinition> Images => _images;
        public IReadOnlyDictionary<int, MemoryDefinition> LocalMemories => _localMemories;
        public IReadOnlyDictionary<int, MemoryDefinition> SharedMemories => _sharedMemories;

        // nxnative: a global memory access traced to no storage buffer goes through a physical storage buffer pointer (GlobalToStorage.TryGenerateRawStorageOp)
        public bool UsesPhysicalPointers { get; set; }

        // nxnative: uniform bounds (TranslationFlags.MetalUniformBounds). The binding of each game uniform slot's buffer, whose reads past the bound size give 0, with its
        // index in the size table; the words of each slot the shader reads at fixed offsets (the end of the highest one), which the host makes readable; and the binding of
        // the size table (Constants.NxUniformSizesBank) that reads at computed offsets compare with, -1 when the shader has none
        private readonly Dictionary<int, int> _boundedConstantBuffers = new();
        public IReadOnlyDictionary<int, int> BoundedConstantBuffers => _boundedConstantBuffers;
        public int[] FixedUniformWords { get; } = new int[Constants.NxUniformSlots];
        public int UniformSizesBinding { get; set; } = -1;

        // nxnative: buffer texture offsets (TranslationFlags.MetalBufferImageBias): each buffer image's and each buffer texture's index in the offset table, and the
        // binding of the table (Constants.NxBufferImageBiasBank), -1 when the shader has none
        private readonly Dictionary<SetBindingPair, int> _bufferImageBiases = new();
        public IReadOnlyDictionary<SetBindingPair, int> BufferImageBiases => _bufferImageBiases;
        private readonly Dictionary<SetBindingPair, int> _bufferTextureBiases = new();
        public IReadOnlyDictionary<SetBindingPair, int> BufferTextureBiases => _bufferTextureBiases;
        public int BufferImageBiasBinding { get; set; } = -1;

        public void AddBufferImageBias(SetBindingPair image, int index)
        {
            _bufferImageBiases[image] = index;
        }

        public void AddBufferTextureBias(SetBindingPair texture, int index)
        {
            _bufferTextureBiases[texture] = index;
        }

        public void AddBoundedConstantBuffer(int binding, int sizeIndex)
        {
            _boundedConstantBuffers[binding] = sizeIndex;
        }

        public ShaderProperties()
        {
            _constantBuffers = new Dictionary<int, BufferDefinition>();
            _storageBuffers = new Dictionary<int, BufferDefinition>();
            _textures = new Dictionary<SetBindingPair, TextureDefinition>();
            _images = new Dictionary<SetBindingPair, TextureDefinition>();
            _localMemories = new Dictionary<int, MemoryDefinition>();
            _sharedMemories = new Dictionary<int, MemoryDefinition>();
        }

        public void AddOrUpdateConstantBuffer(BufferDefinition definition)
        {
            _constantBuffers[definition.Binding] = definition;
        }

        public void AddOrUpdateStorageBuffer(BufferDefinition definition)
        {
            _storageBuffers[definition.Binding] = definition;
        }

        public void AddOrUpdateTexture(TextureDefinition definition)
        {
            _textures[new(definition.Set, definition.Binding)] = definition;
        }

        public void AddOrUpdateImage(TextureDefinition definition)
        {
            _images[new(definition.Set, definition.Binding)] = definition;
        }

        public int AddLocalMemory(MemoryDefinition definition)
        {
            int id = _localMemories.Count;
            _localMemories.Add(id, definition);

            return id;
        }

        public int AddSharedMemory(MemoryDefinition definition)
        {
            int id = _sharedMemories.Count;
            _sharedMemories.Add(id, definition);

            return id;
        }

        public static TextureFormat GetTextureFormat(IGpuAccessor gpuAccessor, int handle, int cbufSlot = -1)
        {
            // When the formatted load extension is supported, we don't need to
            // specify a format, we can just declare it without a format and the GPU will handle it.
            if (gpuAccessor.QueryHostSupportsImageLoadFormatted())
            {
                return TextureFormat.Unknown;
            }

            TextureFormat format = gpuAccessor.QueryTextureFormat(handle, cbufSlot);

            if (format == TextureFormat.Unknown)
            {
                gpuAccessor.Log($"Unknown format for texture {handle}.");

                format = TextureFormat.R8G8B8A8Unorm;
            }

            return format;
        }

        private static bool FormatSupportsAtomic(TextureFormat format)
        {
            return format is TextureFormat.R32Sint or TextureFormat.R32Uint;
        }

        public static TextureFormat GetTextureFormatAtomic(IGpuAccessor gpuAccessor, int handle, int cbufSlot = -1, bool strict = false)
        {
            // Atomic image instructions do not support GL_EXT_shader_image_load_formatted,
            // and must have a type specified. Default to R32Sint if not available.

            TextureFormat format = gpuAccessor.QueryTextureFormat(handle, cbufSlot);

            if (!FormatSupportsAtomic(format))
            {
                if (strict)
                {
                    throw new NotSupportedException($"Metal image atomics require an R32 integer descriptor format, got {format}.");
                }

                gpuAccessor.Log($"Unsupported format for texture {handle}: {format}.");

                format = TextureFormat.R32Sint;
            }

            return format;
        }
    }
}
