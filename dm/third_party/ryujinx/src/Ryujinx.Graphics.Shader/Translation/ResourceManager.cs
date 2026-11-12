using Ryujinx.Common;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using System;
using System.Collections.Generic;
using System.Globalization;

namespace Ryujinx.Graphics.Shader.Translation
{
    class ResourceManager
    {
        // Those values are used if the shader as local or shared memory access,
        // but for some reason the supplied size was 0.
        private const int DefaultLocalMemorySize = 128;
        private const int DefaultSharedMemorySize = 4096;

        private static readonly string[] _stagePrefixes = new string[] { "cp", "vp", "tcp", "tep", "gp", "fp" };

        private readonly IGpuAccessor _gpuAccessor;
        private readonly ShaderStage _stage;
        private readonly string _stagePrefix;

        private readonly SetBindingPair[] _cbSlotToBindingMap;
        private readonly SetBindingPair[] _sbSlotToBindingMap;
        private uint _sbSlotWritten;

        private readonly Dictionary<int, int> _sbSlots;
        private readonly Dictionary<int, int> _sbSlotsReverse;

        private readonly HashSet<int> _usedConstantBufferBindings;

        private readonly record struct TextureInfo(int CbufSlot, int Handle, int ArrayLength, bool Separate, SamplerType Type, TextureFormat Format);

        private struct TextureMeta
        {
            public int Set;
            public int Binding;
            public bool AccurateType;
            public SamplerType Type;
            public TextureUsageFlags UsageFlags;
        }

        private readonly Dictionary<TextureInfo, TextureMeta> _usedTextures;
        private readonly Dictionary<TextureInfo, TextureMeta> _usedImages;

        public int LocalMemoryId { get; private set; }
        public int SharedMemoryId { get; private set; }

        public int LocalVertexDataMemoryId { get; private set; }
        public int LocalTopologyRemapMemoryId { get; private set; }
        public int LocalVertexIndexVertexRateMemoryId { get; private set; }
        public int LocalVertexIndexInstanceRateMemoryId { get; private set; }
        public int LocalGeometryOutputVertexCountMemoryId { get; private set; }
        public int LocalGeometryOutputIndexCountMemoryId { get; private set; }
        public int LocalGeometryPrimitiveOpenMemoryId { get; private set; }

        public ShaderProperties Properties { get; }

        public ResourceReservations Reservations { get; }

        // nxnative: TranslationFlags.MetalUniformBounds
        public bool UniformBounds { get; set; }

        // nxnative: TranslationFlags.MetalBufferImageBias
        public bool BufferImageBias { get; set; }

        public ResourceManager(ShaderStage stage, IGpuAccessor gpuAccessor, ResourceReservations reservations = null)
        {
            _gpuAccessor = gpuAccessor;
            Properties = new();
            Reservations = reservations;
            _stage = stage;
            _stagePrefix = GetShaderStagePrefix(stage);

            // Constant buffer banks 0-17 (Maxwell's), 24: NVN2's push constants (c[24]), 25: the uniform slots' sizes (Constants.NxUniformSizesBank), and 26: the
            // buffer images' offsets (Constants.NxBufferImageBiasBank)
            _cbSlotToBindingMap = new SetBindingPair[Constants.NxBufferImageBiasBank + 1];
            // Storage buffers the global-to-storage pass found: the stage's storage buffer table and, for compute, its uniform buffer address table (NVN 16 + 16 slots)
            _sbSlotToBindingMap = new SetBindingPair[32];
            _cbSlotToBindingMap.AsSpan().Fill(new(-1, -1));
            _sbSlotToBindingMap.AsSpan().Fill(new(-1, -1));

            _sbSlots = new();
            _sbSlotsReverse = new();

            _usedConstantBufferBindings = new();

            _usedTextures = new();
            _usedImages = new();

            Properties.AddOrUpdateConstantBuffer(new(BufferLayout.Std140, 0, SupportBuffer.Binding, "support_buffer", SupportBuffer.GetStructureType()));

            LocalMemoryId = -1;
            SharedMemoryId = -1;
        }

        public void SetCurrentLocalMemory(int size, bool isUsed)
        {
            if (isUsed)
            {
                if (size <= 0)
                {
                    size = DefaultLocalMemorySize;
                }

                MemoryDefinition lmem = new MemoryDefinition("local_memory", AggregateType.Array | AggregateType.U32, BitUtils.DivRoundUp(size, sizeof(uint)));

                LocalMemoryId = Properties.AddLocalMemory(lmem);
            }
            else
            {
                LocalMemoryId = -1;
            }
        }

        public void SetCurrentSharedMemory(int size, bool isUsed)
        {
            if (isUsed)
            {
                if (size <= 0)
                {
                    size = DefaultSharedMemorySize;
                }

                MemoryDefinition smem = new MemoryDefinition("shared_memory", AggregateType.Array | AggregateType.U32, BitUtils.DivRoundUp(size, sizeof(uint)));

                SharedMemoryId = Properties.AddSharedMemory(smem);
            }
            else
            {
                SharedMemoryId = -1;
            }
        }

        // nxnative: the scratch of Ampere HMMA (InstEmitAmpereTensor.AHmma, helper dmsh_hmma16816_f16): a shared memory of its own, 256 words for each subgroup of the
        // workgroup, made the first time an HMMA needs it
        public int HmmaScratchMemoryId { get; private set; } = -1;

        public int GetHmmaScratchMemoryId(int subgroups)
        {
            if (HmmaScratchMemoryId < 0)
            {
                HmmaScratchMemoryId = Properties.AddSharedMemory(new MemoryDefinition("hmma_scratch", AggregateType.Array | AggregateType.U32, 256 * subgroups));
            }

            return HmmaScratchMemoryId;
        }

        public void SetVertexAsComputeLocalMemories(ShaderStage stage, InputTopology inputTopology)
        {
            LocalVertexDataMemoryId = AddMemoryDefinition("local_vertex_data", AggregateType.Array | AggregateType.FP32, Reservations.OutputSizePerInvocation);

            if (stage == ShaderStage.Vertex)
            {
                LocalVertexIndexVertexRateMemoryId = AddMemoryDefinition("local_vertex_index_vr", AggregateType.U32);
                LocalVertexIndexInstanceRateMemoryId = AddMemoryDefinition("local_vertex_index_ir", AggregateType.U32);
            }
            else if (stage == ShaderStage.Geometry)
            {
                LocalTopologyRemapMemoryId = AddMemoryDefinition("local_topology_remap", AggregateType.Array | AggregateType.U32, inputTopology.InputVertexCount);

                LocalGeometryOutputVertexCountMemoryId = AddMemoryDefinition("local_geometry_output_vertex", AggregateType.U32);
                LocalGeometryOutputIndexCountMemoryId = AddMemoryDefinition("local_geometry_output_index", AggregateType.U32);
                if (Reservations.GeometryInputFixed)
                    LocalGeometryPrimitiveOpenMemoryId = AddMemoryDefinition("local_geometry_primitive_open", AggregateType.U32);
            }
        }

        // nxnative: the pool table entry a raw global memory helper chose last in this invocation (GlobalToStorage.TryGenerateRawStorageOp), one word for each helper, zero at the start
        public int AddRawLastEntryMemory()
        {
            return Properties.AddLocalMemory(new MemoryDefinition($"gmem_last_{Properties.LocalMemories.Count}", AggregateType.U32, 1, zeroInit: true));
        }

        // nxnative: words 1 to 3 of a raw vector load (GlobalToStorage.TryGenerateRawStorageOp), one for the shader: the helper writes them and the loads of those words read them
        // right after its call, before any other access
        private int _rawVectorMemoryId = -1;

        public int GetRawVectorMemory()
        {
            if (_rawVectorMemoryId < 0)
            {
                _rawVectorMemoryId = Properties.AddLocalMemory(new MemoryDefinition("gmem_vec", AggregateType.Array | AggregateType.U32, 3));
            }

            return _rawVectorMemoryId;
        }

        private int AddMemoryDefinition(string name, AggregateType type, int arrayLength = 1)
        {
            return Properties.AddLocalMemory(new MemoryDefinition(name, type, arrayLength));
        }

        public int GetConstantBufferBinding(int slot)
        {
            SetBindingPair setAndBinding = _cbSlotToBindingMap[slot];
            if (setAndBinding.Binding < 0)
            {
                setAndBinding = _gpuAccessor.CreateConstantBufferBinding(slot);
                _cbSlotToBindingMap[slot] = setAndBinding;
                string slotNumber = slot.ToString(CultureInfo.InvariantCulture);
                AddNewConstantBuffer(setAndBinding.SetIndex, setAndBinding.Binding, $"{_stagePrefix}_c{slotNumber}");

                // nxnative: a game uniform slot's reads are bounded by the size the game bound (NoteUniformRead)
                if (UniformBounds && slot >= Constants.NxFirstUniformSlot && slot < Constants.NxFirstUniformSlot + Constants.NxUniformSlots)
                {
                    Properties.AddBoundedConstantBuffer(setAndBinding.Binding, slot - Constants.NxFirstUniformSlot);
                }
            }

            return setAndBinding.Binding;
        }

        // nxnative: each read of a game uniform slot, as the structured program meets it (TranslationFlags.MetalUniformBounds). A read at a fixed offset needs no code: the
        // slot's extent of such reads goes to the host (BufferDescriptor.FixedSize), which gives a binding shorter than it zeros up to there. A read at a computed offset is
        // compared with the slot's size in the size table, which only a shader with such a read declares (CodeGen/Spirv/Instructions.cs TryGenerateBoundedUniformLoad)
        public void NoteUniformRead(int binding, bool fixedOffset, int word)
        {
            if (!Properties.BoundedConstantBuffers.TryGetValue(binding, out int sizeIndex))
            {
                return;
            }

            if (fixedOffset)
            {
                Properties.FixedUniformWords[sizeIndex] = Math.Max(Properties.FixedUniformWords[sizeIndex], word + 1);
            }
            else
            {
                Properties.UniformSizesBinding = GetUniformSizesBinding();
            }
        }

        // nxnative: the size table, bank Constants.NxUniformSizesBank: one u32 a game uniform slot, in uint4 data[4] (a std140 array of u32 would take 16 bytes an element).
        // Declared at the shader's first read of a game uniform slot at a computed offset (NoteUniformRead)
        private int GetUniformSizesBinding()
        {
            int slot = Constants.NxUniformSizesBank;
            SetBindingPair setAndBinding = _cbSlotToBindingMap[slot];

            if (setAndBinding.Binding < 0)
            {
                setAndBinding = _gpuAccessor.CreateConstantBufferBinding(slot);
                _cbSlotToBindingMap[slot] = setAndBinding;

                StructureType type = new(new[]
                {
                    new StructureField(AggregateType.Array | AggregateType.Vector4 | AggregateType.U32, "data", (Constants.NxUniformSlots + 3) / 4),
                });

                Properties.AddOrUpdateConstantBuffer(new(BufferLayout.Std140, setAndBinding.SetIndex, setAndBinding.Binding, $"{_stagePrefix}_c{slot.ToString(CultureInfo.InvariantCulture)}", type));
                SetUsedConstantBufferBinding(setAndBinding.Binding);
            }

            return setAndBinding.Binding;
        }

        // nxnative: each image operation, as the structured program meets it (TranslationFlags.MetalBufferImageBias). A buffer image gets the next index of the offset
        // table at its first operation, and the shader declares the table with its first buffer image (CodeGen/Spirv/Instructions.cs BiasBufferImageCoordinate). The
        // index is the handle's: declarations of one handle in other formats are one texture at one offset, and the build finds an image's index by its slot and handle
        // (dmsh_reflect.py buffer_image_bias). An array of images keeps its coordinates: the host binds it as it is
        private readonly Dictionary<(int CbufSlot, int Handle), int> _bufferImageBiasByHandle = new();

        public void NoteBufferImage(int setIndex, int binding)
        {
            SetBindingPair image = new(setIndex, binding);

            if (!BufferImageBias || Properties.BufferImageBiases.ContainsKey(image) ||
                !Properties.Images.TryGetValue(image, out TextureDefinition definition) || definition.ArrayLength != 1)
            {
                return;
            }

            (int, int) handle = (-1, -1);

            foreach ((TextureInfo info, TextureMeta meta) in _usedImages)
            {
                if (meta.Set == setIndex && meta.Binding == binding)
                {
                    handle = (info.CbufSlot, info.Handle);
                    break;
                }
            }

            if (!_bufferImageBiasByHandle.TryGetValue(handle, out int index))
            {
                if (_bufferImageBiasByHandle.Count >= Constants.NxBufferImageBiases)
                {
                    return;
                }

                index = _bufferImageBiasByHandle.Count;
                _bufferImageBiasByHandle.Add(handle, index);
            }

            Properties.AddBufferImageBias(image, index);
            Properties.BufferImageBiasBinding = GetBufferImageBiasBinding();
        }

        // nxnative: the offset table, bank Constants.NxBufferImageBiasBank: one u32 a buffer image, in uint4 data[4]
        private int GetBufferImageBiasBinding()
        {
            int slot = Constants.NxBufferImageBiasBank;
            SetBindingPair setAndBinding = _cbSlotToBindingMap[slot];

            if (setAndBinding.Binding < 0)
            {
                setAndBinding = _gpuAccessor.CreateConstantBufferBinding(slot);
                _cbSlotToBindingMap[slot] = setAndBinding;

                StructureType type = new(new[]
                {
                    new StructureField(AggregateType.Array | AggregateType.Vector4 | AggregateType.U32, "data", (Constants.NxBufferImageBiases + 3) / 4),
                });

                Properties.AddOrUpdateConstantBuffer(new(BufferLayout.Std140, setAndBinding.SetIndex, setAndBinding.Binding, $"{_stagePrefix}_c{slot.ToString(CultureInfo.InvariantCulture)}", type));
                SetUsedConstantBufferBinding(setAndBinding.Binding);
            }

            return setAndBinding.Binding;
        }

        public bool TryGetStorageBufferBinding(int sbCbSlot, int sbCbOffset, bool write, out int binding)
        {
            if (!TryGetSbSlot((byte)sbCbSlot, (ushort)sbCbOffset, out int slot))
            {
                binding = 0;
                return false;
            }

            SetBindingPair setAndBinding = _sbSlotToBindingMap[slot];

            if (setAndBinding.Binding < 0)
            {
                setAndBinding = _gpuAccessor.CreateStorageBufferBinding(slot);
                _sbSlotToBindingMap[slot] = setAndBinding;
                string slotNumber = slot.ToString(CultureInfo.InvariantCulture);
                AddNewStorageBuffer(setAndBinding.SetIndex, setAndBinding.Binding, $"{_stagePrefix}_s{slotNumber}");
            }

            if (write)
            {
                _sbSlotWritten |= 1u << slot;
            }

            binding = setAndBinding.Binding;
            return true;
        }

        private bool TryGetSbSlot(byte sbCbSlot, ushort sbCbOffset, out int slot)
        {
            int key = PackSbCbInfo(sbCbSlot, sbCbOffset);

            if (!_sbSlots.TryGetValue(key, out slot))
            {
                slot = _sbSlots.Count;

                if (slot >= _sbSlotToBindingMap.Length)
                {
                    return false;
                }

                _sbSlots.Add(key, slot);
                _sbSlotsReverse.Add(slot, key);
            }

            return true;
        }

        public bool TryGetConstantBufferSlot(int binding, out int slot)
        {
            for (slot = 0; slot < _cbSlotToBindingMap.Length; slot++)
            {
                if (_cbSlotToBindingMap[slot].Binding == binding)
                {
                    return true;
                }
            }

            slot = 0;
            return false;
        }

        public SetBindingPair GetTextureOrImageBinding(
            Instruction inst,
            SamplerType type,
            TextureFormat format,
            TextureFlags flags,
            int cbufSlot,
            int handle,
            int arrayLength = 1,
            bool separate = false)
        {
            inst &= Instruction.Mask;
            bool isImage = inst.IsImage();
            bool isWrite = inst.IsImageStore();
            bool accurateType = !inst.IsTextureQuery();
            bool intCoords = isImage || flags.HasFlag(TextureFlags.IntCoords) || inst == Instruction.TextureQuerySize;
            bool coherent = flags.HasFlag(TextureFlags.Coherent);

            if (!isImage)
            {
                format = TextureFormat.Unknown;
            }

            SetBindingPair setAndBinding = GetTextureOrImageBinding(
                cbufSlot,
                handle,
                arrayLength,
                type,
                format,
                isImage,
                intCoords,
                isWrite,
                accurateType,
                coherent,
                separate);

            _gpuAccessor.RegisterTexture(handle, cbufSlot);

            return setAndBinding;
        }

        private SetBindingPair GetTextureOrImageBinding(
            int cbufSlot,
            int handle,
            int arrayLength,
            SamplerType type,
            TextureFormat format,
            bool isImage,
            bool intCoords,
            bool write,
            bool accurateType,
            bool coherent,
            bool separate)
        {
            int dimensions = type == SamplerType.None ? 0 : type.Dimensions;
            Dictionary<TextureInfo, TextureMeta> dict = isImage ? _usedImages : _usedTextures;

            TextureUsageFlags usageFlags = TextureUsageFlags.None;

            if (intCoords)
            {
                usageFlags |= TextureUsageFlags.NeedsScaleValue;

                bool canScale = _stage.SupportsRenderScale && arrayLength == 1 && !write && dimensions == 2;

                if (!canScale)
                {
                    // Resolution scaling cannot be applied to this texture right now.
                    // Flag so that we know to blacklist scaling on related textures when binding them.
                    usageFlags |= TextureUsageFlags.ResScaleUnsupported;
                }
            }

            if (write)
            {
                usageFlags |= TextureUsageFlags.ImageStore;
            }

            if (coherent)
            {
                usageFlags |= TextureUsageFlags.ImageCoherent;
            }

            // For array textures, we also want to use type as key,
            // since we may have texture handles stores in the same buffer, but for textures with different types.
            SamplerType keyType = arrayLength > 1 ? type : SamplerType.None;
            TextureInfo info = new TextureInfo(cbufSlot, handle, arrayLength, separate, keyType, format);
            TextureMeta meta = new TextureMeta()
            {
                AccurateType = accurateType,
                Type = type,
                UsageFlags = usageFlags,
            };

            int setIndex;
            int binding;

            if (dict.TryGetValue(info, out TextureMeta existingMeta))
            {
                dict[info] = MergeTextureMeta(meta, existingMeta);
                setIndex = existingMeta.Set;
                binding = existingMeta.Binding;
            }
            else
            {
                if (arrayLength > 1 && (setIndex = _gpuAccessor.CreateExtraSet()) >= 0)
                {
                    // We reserved an "extra set" for the array.
                    // In this case the binding is always the first one (0).
                    // Using separate sets for array is better as we need to do less descriptor set updates.

                    binding = 0;
                }
                else
                {
                    bool isBuffer = (type & SamplerType.Mask) == SamplerType.TextureBuffer;

                    SetBindingPair setAndBinding = isImage
                        ? _gpuAccessor.CreateImageBinding(arrayLength, isBuffer)
                        : _gpuAccessor.CreateTextureBinding(arrayLength, isBuffer);

                    setIndex = setAndBinding.SetIndex;
                    binding = setAndBinding.Binding;
                }

                meta.Set = setIndex;
                meta.Binding = binding;

                dict.Add(info, meta);
            }

            string nameSuffix;
            string prefix = isImage ? "i" : "t";

            if (arrayLength != 1 && type != SamplerType.None)
            {
                prefix += type.ShortTypeName;
            }

            if (isImage)
            {
                nameSuffix = cbufSlot < 0
                    ? $"{prefix}_tcb_{handle:X}_{format.ToGlslFormat()}"
                    : $"{prefix}_cb{cbufSlot}_{handle:X}_{format.ToGlslFormat()}";
            }
            else if (type == SamplerType.None)
            {
                nameSuffix = cbufSlot < 0 ? $"s_tcb_{handle:X}" : $"s_cb{cbufSlot}_{handle:X}";
            }
            else
            {
                nameSuffix = cbufSlot < 0 ? $"{prefix}_tcb_{handle:X}" : $"{prefix}_cb{cbufSlot}_{handle:X}";
            }

            TextureDefinition definition = new TextureDefinition(
                setIndex,
                binding,
                arrayLength,
                separate,
                $"{_stagePrefix}_{nameSuffix}",
                meta.Type,
                info.Format,
                meta.UsageFlags);

            if (isImage)
            {
                Properties.AddOrUpdateImage(definition);
            }
            else
            {
                Properties.AddOrUpdateTexture(definition);
            }

            return new SetBindingPair(setIndex, binding);
        }

        // nxnative: the types other instructions gave the texture whose handle word is at the given offset of constant buffer cbufSlot (whatever sampler it was paired with),
        // for BindlessElimination.TryTableHandle; arrays of handles are left out
        public List<SamplerType> GetTextureTypes(int cbufSlot, int textureOffset, bool isImage)
        {
            List<SamplerType> types = new();

            foreach ((TextureInfo info, TextureMeta meta) in isImage ? _usedImages : _usedTextures)
            {
                (int offset, _, _) = TextureHandle.UnpackOffsets(info.Handle);
                (int slot, _) = TextureHandle.UnpackSlots(info.CbufSlot, cbufSlot);

                if (info.ArrayLength == 1 && meta.AccurateType && slot == cbufSlot && offset == textureOffset)
                {
                    types.Add(meta.Type);
                }
            }

            return types;
        }

        // nxnative: one texture read as an array by some instructions and as a plain texture by others. A handle has one binding, declared with one type: the declaration took the
        // last instruction's and the descriptor the first accurate one's, and each instruction built its coordinates for its own, so a 2D read of a binding declared a 2D array had
        // no layer (SPIRV-Cross then wrote `coord.z` of a float2, which Metal refuses: PRAGMATA, 24 pixel shaders, cb65542 handle 0x3041000E sampled with TEX 2D at level 0 and with
        // TXD ARRAY_2D). The GPU reads the texture the handle names with the coordinates each instruction gives, and without a layer that is layer 0. So such a binding is an
        // array for every instruction, and those that give no layer read layer 0. 1D and 2D only (a cube array's layer goes with the reference of a shadow read, and 3D has none);
        // a level-of-detail query takes no layer either way. Runs once every function is translated (TranslatorContext)
        public void UnifyArrayTypes(IReadOnlyList<Function> funcs)
        {
            Dictionary<SetBindingPair, List<TextureOperation>> ops = new();

            foreach (Function func in funcs)
            {
                foreach (BasicBlock block in func.Blocks)
                {
                    foreach (INode node in block.Operations)
                    {
                        if (node is TextureOperation texOp && !texOp.Inst.IsImage() && (texOp.Flags & TextureFlags.Bindless) == 0 && texOp.Type != SamplerType.None)
                        {
                            SetBindingPair pair = new(texOp.Set, texOp.Binding);

                            if (!ops.TryGetValue(pair, out List<TextureOperation> list))
                            {
                                ops.Add(pair, list = new());
                            }

                            list.Add(texOp);
                        }
                    }
                }
            }

            foreach ((SetBindingPair pair, List<TextureOperation> list) in ops)
            {
                if (!Properties.Textures.TryGetValue(pair, out TextureDefinition definition) || definition.ArrayLength != 1)
                {
                    continue;   // an array of textures: its instructions' first source is the index
                }

                SamplerType kind = list[0].Type & SamplerType.Mask;
                bool sameKind = list.TrueForAll(o => (o.Type & SamplerType.Mask) == kind && (o.Type & SamplerType.Multisample) == 0);
                bool mixed = list.Exists(o => (o.Type & SamplerType.Array) != 0) && list.Exists(o => (o.Type & SamplerType.Array) == 0);

                if (!sameKind || !mixed || kind is not (SamplerType.Texture1D or SamplerType.Texture2D))
                {
                    continue;
                }

                foreach (TextureOperation texOp in list)
                {
                    if ((texOp.Type & SamplerType.Array) != 0)
                    {
                        continue;
                    }

                    if (texOp.Inst == Instruction.TextureSample)
                    {
                        bool intCoords = (texOp.Flags & TextureFlags.IntCoords) != 0;

                        texOp.InsertSource(texOp.Type.Dimensions, intCoords ? OperandHelper.Const(0) : OperandHelper.ConstF(0f));
                    }

                    texOp.Type |= SamplerType.Array;
                }

                Properties.AddOrUpdateTexture(new TextureDefinition(
                    definition.Set, definition.Binding, definition.ArrayLength, definition.Separate, definition.Name, definition.Type | SamplerType.Array, definition.Format, definition.Flags));

                foreach ((TextureInfo info, TextureMeta meta) in new List<KeyValuePair<TextureInfo, TextureMeta>>(_usedTextures))
                {
                    if (meta.Set == pair.SetIndex && meta.Binding == pair.Binding && meta.Type != SamplerType.None)
                    {
                        TextureMeta array = meta;
                        array.Type |= SamplerType.Array;
                        _usedTextures[info] = array;
                    }
                }
            }
        }

        private static TextureMeta MergeTextureMeta(TextureMeta meta, TextureMeta existingMeta)
        {
            meta.Set = existingMeta.Set;
            meta.Binding = existingMeta.Binding;
            meta.UsageFlags |= existingMeta.UsageFlags;

            // If the texture we have has inaccurate type information, then
            // we prefer the most accurate one.
            if (existingMeta.AccurateType)
            {
                meta.AccurateType = true;
                meta.Type = existingMeta.Type;
            }

            return meta;
        }

        public void SetUsageFlagsForTextureQuery(int binding, SamplerType type)
        {
            TextureInfo selectedInfo = default;
            TextureMeta selectedMeta = default;
            bool found = false;

            foreach ((TextureInfo info, TextureMeta meta) in _usedTextures)
            {
                if (meta.Binding == binding)
                {
                    selectedInfo = info;
                    selectedMeta = meta;
                    found = true;
                    break;
                }
            }

            if (found)
            {
                selectedMeta.UsageFlags |= TextureUsageFlags.NeedsScaleValue;
                
                int dimensions = type.Dimensions;
                bool canScale = _stage.SupportsRenderScale && selectedInfo.ArrayLength == 1 && dimensions == 2;

                if (!canScale)
                {
                    // Resolution scaling cannot be applied to this texture right now.
                    // Flag so that we know to blacklist scaling on related textures when binding them.
                    selectedMeta.UsageFlags |= TextureUsageFlags.ResScaleUnsupported;
                }

                _usedTextures[selectedInfo] = selectedMeta;
            }
        }

        public void SetUsedConstantBufferBinding(int binding)
        {
            _usedConstantBufferBindings.Add(binding);
        }

        public BufferDescriptor[] GetConstantBufferDescriptors()
        {
            BufferDescriptor[] descriptors = new BufferDescriptor[_usedConstantBufferBindings.Count];

            int descriptorIndex = 0;

            for (int slot = 0; slot < _cbSlotToBindingMap.Length; slot++)
            {
                SetBindingPair setAndBinding = _cbSlotToBindingMap[slot];

                if (setAndBinding.Binding >= 0 && _usedConstantBufferBindings.Contains(setAndBinding.Binding))
                {
                    // nxnative: the bytes of a game uniform slot read at fixed offsets (NoteUniformRead)
                    int fixedSize = Properties.BoundedConstantBuffers.TryGetValue(setAndBinding.Binding, out int sizeIndex) ? Properties.FixedUniformWords[sizeIndex] * 4 : 0;

                    descriptors[descriptorIndex++] = new BufferDescriptor(setAndBinding.SetIndex, setAndBinding.Binding, slot, fixedSize);
                }
            }

            if (descriptors.Length != descriptorIndex)
            {
                Array.Resize(ref descriptors, descriptorIndex);
            }

            return descriptors;
        }

        public BufferDescriptor[] GetStorageBufferDescriptors()
        {
            BufferDescriptor[] descriptors = new BufferDescriptor[_sbSlots.Count];

            int descriptorIndex = 0;

            foreach ((int key, int slot) in _sbSlots)
            {
                SetBindingPair setAndBinding = _sbSlotToBindingMap[slot];

                if (setAndBinding.Binding >= 0)
                {
                    (int sbCbSlot, int sbCbOffset) = UnpackSbCbInfo(key);
                    BufferUsageFlags flags = (_sbSlotWritten & (1u << slot)) != 0 ? BufferUsageFlags.Write : BufferUsageFlags.None;
                    descriptors[descriptorIndex++] = new BufferDescriptor(setAndBinding.SetIndex, setAndBinding.Binding, slot, sbCbSlot, sbCbOffset, flags);
                }
            }

            if (descriptors.Length != descriptorIndex)
            {
                Array.Resize(ref descriptors, descriptorIndex);
            }

            return descriptors;
        }

        public TextureDescriptor[] GetTextureDescriptors(bool includeArrays = true)
        {
            return GetDescriptors(_usedTextures, includeArrays);
        }

        public TextureDescriptor[] GetImageDescriptors(bool includeArrays = true)
        {
            // nxnative: with each buffer image's index in the offset table (NoteBufferImage)
            return GetDescriptors(_usedImages, includeArrays, meta => Properties.BufferImageBiases.TryGetValue(new(meta.Set, meta.Binding), out int index) ? index : -1);
        }

        private static TextureDescriptor[] GetDescriptors(IReadOnlyDictionary<TextureInfo, TextureMeta> usedResources, bool includeArrays, Func<TextureMeta, int> bufferBias = null)
        {
            List<TextureDescriptor> descriptors = new();

            bool hasAnyArray = false;

            foreach ((TextureInfo info, TextureMeta meta) in usedResources)
            {
                if (info.ArrayLength > 1)
                {
                    hasAnyArray = true;
                    continue;
                }

                descriptors.Add(new TextureDescriptor(
                    meta.Set,
                    meta.Binding,
                    meta.Type,
                    info.Format,
                    info.CbufSlot,
                    info.Handle,
                    info.ArrayLength,
                    info.Separate,
                    meta.UsageFlags,
                    bufferBias?.Invoke(meta) ?? -1));
            }

            if (hasAnyArray && includeArrays)
            {
                foreach ((TextureInfo info, TextureMeta meta) in usedResources)
                {
                    if (info.ArrayLength <= 1)
                    {
                        continue;
                    }

                    descriptors.Add(new TextureDescriptor(
                        meta.Set,
                        meta.Binding,
                        meta.Type,
                        info.Format,
                        info.CbufSlot,
                        info.Handle,
                        info.ArrayLength,
                        info.Separate,
                        meta.UsageFlags));
                }
            }

            return descriptors.ToArray();
        }

        public bool TryGetCbufSlotAndHandleForTexture(int binding, out int cbufSlot, out int handle)
        {
            foreach ((TextureInfo info, TextureMeta meta) in _usedTextures)
            {
                if (meta.Binding == binding)
                {
                    cbufSlot = info.CbufSlot;
                    handle = info.Handle;

                    return true;
                }
            }

            cbufSlot = 0;
            handle = 0;
            return false;
        }

        private static int FindDescriptorIndex(TextureDescriptor[] array, int binding)
        {
            return Array.FindIndex(array, x => x.Binding == binding);
        }

        public int FindTextureDescriptorIndex(int binding)
        {
            return FindDescriptorIndex(GetTextureDescriptors(), binding);
        }

        public int FindImageDescriptorIndex(int binding)
        {
            return FindDescriptorIndex(GetImageDescriptors(), binding);
        }

        public bool IsArrayOfTexturesOrImages(int binding, bool isImage)
        {
            foreach ((TextureInfo info, TextureMeta meta) in isImage ? _usedImages : _usedTextures)
            {
                if (meta.Binding == binding)
                {
                    return info.ArrayLength != 1;
                }
            }

            return false;
        }

        private void AddNewConstantBuffer(int setIndex, int binding, string name)
        {
            StructureType type = new(new[]
            {
                new StructureField(AggregateType.Array | AggregateType.Vector4 | AggregateType.FP32, "data", Constants.ConstantBufferSize / 16),
            });

            Properties.AddOrUpdateConstantBuffer(new(BufferLayout.Std140, setIndex, binding, name, type));
        }

        private void AddNewStorageBuffer(int setIndex, int binding, string name)
        {
            StructureType type = new(new[]
            {
                new StructureField(AggregateType.Array | AggregateType.U32, "data", 0),
            });

            Properties.AddOrUpdateStorageBuffer(new(BufferLayout.Std430, setIndex, binding, name, type));
        }

        public static string GetShaderStagePrefix(ShaderStage stage)
        {
            uint index = (uint)stage;

            return index >= _stagePrefixes.Length ? "invalid" : _stagePrefixes[index];
        }

        private static int PackSbCbInfo(int sbCbSlot, int sbCbOffset)
        {
            return sbCbOffset | (sbCbSlot << 16);
        }

        private static (int, int) UnpackSbCbInfo(int key)
        {
            return ((byte)(key >> 16), (ushort)key);
        }
    }
}
