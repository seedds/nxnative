// Original Switch shaders (Maxwell machine code) → SPIR-V for the Vulkan target (the Ryujinx.Graphics.Shader translator), driven by dm/gfx/tools/dmsh_build_vk.py.
// A copy of dmsh_xlate/Program.cs, the Metal chain's translator, with an accessor that answers for Vulkan. The design wanted the two to share the accessor's fact lookups
// through one source file (docs/design/vulkan-backend.md, 2.4), but moving them out of dmsh_xlate/Program.cs changes DMSH_CHAIN (dmsh_chain.py hashes that file) and the
// dmsh_xlate.dll fingerprint in dmsh_chain.lock, and no Metal library can be rebuilt off a Mac. So the sharing waits for the next Metal rebuild on a Mac; until then a change
// to the parts both files hold (the job format, Get, Slot, QuerySamplerType, QueryTextureFormat, the image-type retry, Local, Attr, ConstantBuffer1Read, UsesFloat64) is made
// in both. What differs from dmsh_xlate (docs/design/vulkan-backend.md, 2.1 to 2.3):
//   bindings: by resource class and NVN slot in four descriptor sets (the binding model below), not one running number in set 0;
//   depth: z goes out unchanged with depth clip control reported (VK_EXT_depth_clip_control's negativeOneToOne gives the GL convention per pipeline), so there is no second entry point;
//   geometry shaders are ordinary geometry shaders: no passthrough stage (QueryHostSupportsGeometryShaderPassthrough false), no compute lowering, no <key>.reader.spv;
//   transform feedback is native: the job's "tf" layout (dmsh_build.py tf_layout) becomes the translator's varying locations and strides, which emit the Xfb decorations;
//   FP64 is native; when the module declares a 64-bit float, a second translation with FP64 lowered to FP32 (DoubleToFloat) is written as <key>_fp32.spv, for devices without
//     shaderFloat64 (Tears of the Kingdom 1.4.3 has 30 such shaders);
//   no TranslationFlags.MetalImageAtomics: image atomics take the translator's target-neutral path, without the coordinate guard Metal needs (out-of-bounds atomics are left to
//     Vulkan's image robustness [A: check with the validation layers]);
//   subgroup size 32 (Maxwell's warp); no fragment shader interlock and no viewport mask output (neither is in the Vulkan baseline, section 9.1); storage buffer offsets aligned to
//     64 bytes, the largest minStorageBufferOffsetAlignment of the devices of section 9.2 [A], recorded in the reflection as sbuf_align;
//   VertexCounterLowering (Tears of the Kingdom's vertex visibility counter) is not applied: Vulkan has subgroup operations in vertex shaders where subgroupSupportedStages has the stage.
// Window origin upper-left and no Y flip, as dmsh_xlate: the host's Vulkan viewport carries the flip (a negative height), so both targets keep the same shader conventions [A].
// Usage: dmsh_xlate_vk <job.json> <output dir>   or   dmsh_xlate_vk --source-sha (the first 16 digits of the sha256 of the embedded Program.cs + dmsh_xlate_vk.csproj)
//   Job: [{ "key", "stage": "vs|ps|cs|gs|hs|ds", "code", "tex", "img", "attr", "fout", "local", "shared", "c1", "debug", "reduced", "tf" }], as dmsh_xlate's (see its header), with
//        "tf": four entries, each {"stride": bytes, "outputs": [[location, components], ...]} or [] (the stream is not captured)
//   Output: <key>.spv and <key>.res.json (as dmsh_xlate's, each binding with its "set"; "sbuf_align"), <key>_fp32.spv and <key>_fp32.res.json for a module that declares doubles
using Ryujinx.Common.Memory;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

static class SwshXlateVk
{
    // NVN's handle tables in c[2], byte offsets, 8 bytes per slot (as dmsh_xlate): combined textures 0x20 + 8 r, images 0x120 + 8 r (8 slots), separate textures 0x168 + 8 r
    // (slot 128 + r), separate samplers 0x568 + 8 r
    const int TexBase = 0x20, ImgBase = 0x120, ImgSlots = 8, SepTexBase = 0x168, SepTexSlot0 = 128, SampBase = 0x568;

    // The binding model (docs/design/vulkan-backend.md, 2.3). Stage index s: vertex and compute 0, tessellation control 1, tessellation evaluation 2, geometry 3, fragment 4.
    //   set 0: uniform buffers, binding 1 + 18 s + the constant buffer slot (c0 the driver bank, c1 the shader's constants, c2 the handle table, c3 to c16 the game's uniform
    //          slots 0 to 13, c17); binding 0 is the translator's support buffer, one for every stage (the translator declares it at set 0, binding 0 itself)
    //   set 1: storage buffers, binding 16 s + the translator's storage buffer index (0 to 15, in order of first use; the NVN slot is in the reflection, as on Metal)
    //   set 2: textures with their samplers, binding 512 s + the texture slot (combined 0 to 127, separate 128 to 255); then 512 s + 256 + n for the others in the translator's
    //          order (arrays, handles in a uniform buffer, a slot that another variable took first, the separate samplers of a dynamic array)
    //   set 3: storage images, binding 512 s + the image slot (0 to 7); texel buffers (a buffer texture in texture slot t) 512 s + 8 + t; then 512 s + 264 + n for the others
    // The translator asks for a texture or image binding before it says which handle it serves, so the accessor hands out provisional numbers and Main renumbers them
    // from the program's descriptors (SlotBindings, Rebind). Which numbers the backend's pipeline layouts declare is decided by the descriptor benchmark of milestone P3.
    const int UboStride = 18, SsboStride = 16, ResStride = 512, TexRest = 256, ImgTexelBuffer0 = 8, ImgRest = 264, Provisional = 1 << 20, SbufAlign = 64;
    static int StageIndex(string stage) => stage switch { "hs" => 1, "ds" => 2, "gs" => 3, "ps" => 4, _ => 0 };

    sealed class Accessor : IGpuAccessor
    {
        readonly byte[] _code; readonly JsonObject _job; readonly int _stage; int _provisional = Provisional; public readonly List<string> Notes = new();
        readonly byte[] _c1;
        readonly byte[][] _tfLocations = new byte[4][]; readonly int[] _tfStrides = new int[4];
        public Accessor(byte[] code, JsonObject job, Dictionary<int, SamplerType> imageTypes = null)
        {
            _code = code; _job = job; _stage = StageIndex((string)job["stage"]); _imageTypes = imageTypes;
            _c1 = job["c1"] is JsonNode h ? Convert.FromHexString((string)h) : Array.Empty<byte>();
            // Transform feedback: stream b captures, in order, the components of its outputs; the translator wants one byte per captured 32-bit word, the output attribute's word
            // offset (the generic attributes start at byte 0x80: location l, component c is word 0x20 + 4 l + c)
            for (int b = 0; b < 4; b++)
            {
                var words = new List<byte>();
                if (job["tf"] is JsonArray tf && b < tf.Count && tf[b] is JsonObject e)
                {
                    _tfStrides[b] = (int)e["stride"];
                    foreach (JsonNode o in e["outputs"].AsArray())
                        for (int c = 0; c < (int)o[1]; c++) words.Add((byte)(0x20 + 4 * (int)o[0] + c));
                }
                _tfLocations[b] = words.ToArray();
            }
        }
        // A texture query on an image handle answered from the shader's own surface instructions on a second translation (as dmsh_xlate: see ImageQueryGuessed there)
        readonly Dictionary<int, SamplerType> _imageTypes; public bool ImageQueryGuessed;
        public static Dictionary<int, SamplerType> ImageTypesOf(ShaderProgram p)
        {
            var types = new Dictionary<int, SamplerType>();
            foreach (var im in p.Info.Images) if (im.CbufSlot == 2 || im.CbufSlot < 0) types[im.HandleIndex] = im.Type & (SamplerType.Mask | SamplerType.Array);
            return types;
        }
        // c1 constant segment: the jump tables of indirect branches (as dmsh_xlate)
        public uint ConstantBuffer1Read(int offset) { if (offset >= 0 && offset + 4 <= _c1.Length) return BitConverter.ToUInt32(_c1, offset); Notes.Add($"c1 read out of bounds +{offset:X} (c1 length {_c1.Length})"); return 0; }
        public void Log(string message) => Notes.Add("translator: " + message);
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, 1 + UboStride * _stage + index);
        public SetBindingPair CreateStorageBufferBinding(int index) => new(1, SsboStride * _stage + index);
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) => new(isBuffer ? 3 : 2, _provisional++);
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) => new(3, _provisional++);
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_code)[(int)address..]);
        public int QuerySamplerArrayLengthFromPool() => 32;
        public int QueryTextureArrayLengthFromBuffer(int slot) => 32;
        public int QueryTextureArrayLengthFromPool() => 32;

        string Get(string table, int slot) => _job[table] is JsonObject t && t[slot.ToString()] is JsonNode n ? (n is JsonObject o ? (string)o["type"] : (string)n) : null;
        public SamplerType QuerySamplerType(int handle, int cbufSlot = -1)
        {
            int slot = Slot(handle, cbufSlot, false), islot = Slot(handle, cbufSlot, true);
            string t = (slot >= 0 ? Get("tex", slot) : null) ?? (islot >= 0 ? Get("img", islot) : null);
            if (t == null && islot >= 0 && cbufSlot < 0)
            {
                if (_imageTypes != null && _imageTypes.TryGetValue(handle, out SamplerType it)) { Notes.Add($"texture handle {handle:X} is image slot {islot}: type {it} from the shader's image instructions"); return it; }
                ImageQueryGuessed = true;
            }
            if (t == null) { Notes.Add($"texture handle {handle:X} (constant buffer {cbufSlot}, slot {slot}) has no type in the answer table, using 2D"); return SamplerType.Texture2D; }
            return t switch
            {
                "1d" => SamplerType.Texture1D, "1darray" => SamplerType.Texture1D | SamplerType.Array,
                "buffer" => SamplerType.TextureBuffer, "2d" => SamplerType.Texture2D, "2darray" => SamplerType.Texture2D | SamplerType.Array,
                "2dms" => SamplerType.Texture2D | SamplerType.Multisample, "2dmsarray" => SamplerType.Texture2D | SamplerType.Multisample | SamplerType.Array,
                "3d" => SamplerType.Texture3D, "cube" => SamplerType.TextureCube, "cubearray" => SamplerType.TextureCube | SamplerType.Array,
                _ => SamplerType.Texture2D,
            };
        }
        // Texture slots read by a 1D integer fetch that the translator made a wrapped 2D read (as dmsh_xlate: res "fetch1d")
        public readonly SortedSet<int> Fetch1D = new();
        public void RecordTexelFetch1D(int handle) { int slot = Slot(handle, -1, false); if (slot >= 0) Fetch1D.Add(slot); }
        public TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1)
        {
            int slot = Slot(handle, cbufSlot, true);
            string f = _job["img"] is JsonObject t && slot >= 0 && t[slot.ToString()] is JsonObject o ? (string)o["fmt"] : null;
            if (f == null) { Notes.Add($"image handle {handle:X} slot {slot} has no format in the answer table, using R8G8B8A8Unorm"); return TextureFormat.R8G8B8A8Unorm; }
            return Enum.TryParse(f, true, out TextureFormat tf) ? tf : TextureFormat.R8G8B8A8Unorm;
        }
        int Local(int i) => _job["local"] is JsonArray a && a.Count > i ? (int)a[i] : 1;
        public int QueryComputeLocalSizeX() => Local(0);
        public int QueryComputeLocalSizeY() => Local(1);
        public int QueryComputeLocalSizeZ() => Local(2);
        public int QueryComputeSharedMemorySize() => _job["shared"] is JsonNode n ? (int)n : 0;
        static AttributeType Attr(string s) => s switch { "sint" => AttributeType.Sint, "uint" => AttributeType.Uint, _ => AttributeType.Float };
        public bool QueryHostSupportsGeometryShaderPassthrough() => false;   // VK_NV_geometry_shader_passthrough is NVIDIA's only: passthrough shaders become ordinary geometry shaders
        public GpuGraphicsState QueryGraphicsState()
        {
            Array32<AttributeType> attr = default; Array8<AttributeType> fout = default;
            if (_job["attr"] is JsonObject a) foreach (var kv in a) attr[int.Parse(kv.Key)] = Attr((string)kv.Value);
            if (_job["fout"] is JsonObject f) foreach (var kv in f) fout[int.Parse(kv.Key)] = Attr((string)kv.Value);
            return new GpuGraphicsState(false, InputTopology.Triangles, false, TessPatchType.Triangles, TessSpacing.EqualSpacing,
                false, false, false, false /* depth not rewritten: the host sets the depth convention per pipeline (depth clip control) */, false, 1f, AlphaTestOp.Always, 0f, attr, false, fout, false,
                false /* no Y flip */, true /* origin upper-left */, false);
        }
        public bool QueryHostSupportsTextureShadowLod() => true;
        public bool QueryHostSupportsDepthClipControl() => true;
        public bool QueryHostSupportsFragmentShaderInterlock() => false;
        public bool QueryHostSupportsViewportMask() => false;
        public int QueryHostSubgroupSize() => 32;
        public int QueryHostStorageBufferOffsetAlignment() => SbufAlign;
        public bool QueryHostSupportsTransformFeedback() => true;
        public bool QueryTransformFeedbackEnabled() => _job["tf"] is JsonArray;
        public ReadOnlySpan<byte> QueryTransformFeedbackVaryingLocations(int bufferIndex) => bufferIndex is >= 0 and < 4 ? new ReadOnlySpan<byte>(_tfLocations[bufferIndex]) : ReadOnlySpan<byte>.Empty;
        public int QueryTransformFeedbackStride(int bufferIndex) => bufferIndex is >= 0 and < 4 ? _tfStrides[bufferIndex] : 0;
        // Job reduced = true: relaxed precision (as dmsh_xlate); the translator then also marks the position Invariant
        public bool QueryHostReducedPrecision() => _job["reduced"] is JsonNode n && (bool)n;
        // Job softfloat64 = true: FP64 lowered to FP32 (DoubleToFloat), for the <key>_fp32 variant only (see Main)
        public bool QueryHostSupportsShaderFloat64() => !(_job["softfloat64"] is JsonNode n && (bool)n);
    }

    // A handle's texture or image slot, numbered as dmsh_build.py decode_tex numbers the reflection's (as dmsh_xlate): both c[2] words are tried; -1 when neither is in a table
    static int Slot(int handle, int cbufSlot, bool image)
    {
        if (cbufSlot >= 0 && (cbufSlot & 0xffff) != 2) return -1;
        (int w0, int w1, TextureHandleType ty) = TextureHandle.UnpackOffsets(handle);
        foreach (int w in ty == TextureHandleType.CombinedSampler || ty == TextureHandleType.SeparateConstantSamplerHandle ? new[] { w0 } : new[] { w0, w1 })
        {
            int b = w * 4;
            if (b % 8 != 0) continue;
            if (image && b >= ImgBase && b < ImgBase + 8 * ImgSlots) return (b - ImgBase) / 8;
            if (!image && b >= TexBase && b < SepTexBase) return (b - TexBase) / 8;
            if (!image && b >= SepTexBase && b < SampBase) return SepTexSlot0 + (b - SepTexBase) / 8;
        }
        return -1;
    }

    // The final bindings of sets 2 and 3 (the binding model above), from the provisional ones the accessor handed out: (set, provisional binding) → binding
    static Dictionary<(int, int), int> SlotBindings(ShaderProgram p, int stage)
    {
        var all = new List<(TextureDescriptor d, bool image)>();
        foreach (var d in p.Info.Textures) all.Add((d, false));
        foreach (var d in p.Info.Images) all.Add((d, true));
        all.Sort((a, b) => a.d.Binding.CompareTo(b.d.Binding));   // the order the translator asked for them in
        var map = new Dictionary<(int, int), int>(); var taken = new HashSet<(int, int)>(); var rest = new List<(int, int)>();
        foreach (var (d, image) in all)
        {
            if (d.Binding < Provisional || map.ContainsKey((d.Set, d.Binding))) continue;
            bool buffer = (d.Type & SamplerType.Mask) == SamplerType.TextureBuffer;
            int slot = d.ArrayLength == 1 && d.Type != SamplerType.None ? Slot(d.HandleIndex, d.CbufSlot, image) : -1;
            int want = slot < 0 ? -1 : image ? slot : buffer ? ImgTexelBuffer0 + slot : slot;
            if (want >= 0 && taken.Add((d.Set, want))) map[(d.Set, d.Binding)] = ResStride * stage + want;
            else rest.Add((d.Set, d.Binding));
        }
        var next = new Dictionary<int, int> { [2] = TexRest, [3] = ImgRest };
        foreach (var (set, b) in rest)
        {
            if (next[set] >= ResStride) throw new NotSupportedException($"more than {ResStride - (set == 2 ? TexRest : ImgRest)} set {set} bindings without a slot of their own");
            map[(set, b)] = ResStride * stage + next[set]++;
        }
        return map;
    }

    // The SPIR-V with its Binding decorations in sets 2 and 3 renumbered (SlotBindings); no provisional number may be left
    static byte[] Rebind(byte[] spirv, Dictionary<(int, int), int> map)
    {
        uint[] w = new uint[spirv.Length / 4]; Buffer.BlockCopy(spirv, 0, w, 0, spirv.Length);
        var set = new Dictionary<uint, uint>();
        for (int i = 5; i < w.Length; i += Math.Max(1, (int)(w[i] >> 16)))
            if ((w[i] & 0xffff) == 71 && w[i + 2] == 34) set[w[i + 1]] = w[i + 3];   // OpDecorate DescriptorSet
        for (int i = 5; i < w.Length; i += Math.Max(1, (int)(w[i] >> 16)))
        {
            if ((w[i] & 0xffff) != 71 || w[i + 2] != 33) continue;   // OpDecorate Binding
            if (set.TryGetValue(w[i + 1], out uint s) && map.TryGetValue(((int)s, (int)w[i + 3]), out int b)) w[i + 3] = (uint)b;
            else if (w[i + 3] >= Provisional) throw new InvalidOperationException($"binding {w[i + 3]} of set {s} has no slot binding");
        }
        byte[] output = new byte[spirv.Length]; Buffer.BlockCopy(w, 0, output, 0, output.Length);
        return output;
    }

    // True when the SPIR-V declares a 64-bit float type (OpTypeFloat 64) (as dmsh_xlate)
    static bool UsesFloat64(byte[] spirv)
    {
        for (int i = 20; i + 12 <= spirv.Length; )
        {
            uint word = BitConverter.ToUInt32(spirv, i);
            if ((word & 0xffff) == 22 && (word >> 16) == 3 && BitConverter.ToUInt32(spirv, i + 8) == 64) return true;
            i += Math.Max(1, (int)(word >> 16)) * 4;
        }
        return false;
    }

    // One translation of a job: the program (with the image-type retry of dmsh_xlate), its accessor's notes and fetch1d slots
    static (ShaderProgram, Accessor) Translate(byte[] code, JsonObject job, TranslationFlags flags)
    {
        var acc = new Accessor(code, job);
        ShaderProgram p = Translator.CreateContext(0, acc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags)).Translate();
        if (acc.ImageQueryGuessed && Accessor.ImageTypesOf(p) is { Count: > 0 } imageTypes)   // a query on an image handle went unanswered: again with the images' types
        {
            acc = new Accessor(code, job, imageTypes);
            p = Translator.CreateContext(0, acc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags)).Translate();
        }
        return (p, acc);
    }

    // <name>.spv and <name>.res.json of one translation
    static void Write(string outDir, string name, string key, string stage, ShaderProgram p, Accessor acc)
    {
        var slots = SlotBindings(p, StageIndex(stage));
        File.WriteAllBytes(Path.Combine(outDir, name + ".spv"), Rebind(p.BinaryCode, slots));
        int Final(TextureDescriptor t) => slots.TryGetValue((t.Set, t.Binding), out int b) ? b : t.Binding;
        var res = new JsonObject
        {
            ["key"] = key, ["stage"] = stage, ["target"] = "vk", ["notes"] = new JsonArray(acc.Notes.ConvertAll(n => (JsonNode)n).ToArray()),
            ["cbufs"] = new JsonArray(), ["sbufs"] = new JsonArray(), ["textures"] = new JsonArray(), ["images"] = new JsonArray(),
            ["uses_frag_coord"] = p.Info.UsesFragCoord, ["frag_out_map"] = p.Info.FragmentOutputMap, ["sbuf_align"] = SbufAlign,
        };
        foreach (var b in p.Info.CBuffers) res["cbufs"].AsArray().Add(new JsonObject { ["set"] = b.Set, ["binding"] = b.Binding, ["slot"] = b.Slot });
        foreach (var b in p.Info.SBuffers) res["sbufs"].AsArray().Add(new JsonObject { ["set"] = b.Set, ["binding"] = b.Binding, ["slot"] = b.Slot, ["cb_slot"] = b.SbCbSlot, ["cb_offset"] = b.SbCbOffset, ["flags"] = (int)b.Flags });
        foreach (var (list, field) in new[] { (p.Info.Textures, "textures"), (p.Info.Images, "images") })
            foreach (var t in list) res[field].AsArray().Add(new JsonObject { ["set"] = t.Set, ["binding"] = Final(t), ["type"] = (int)t.Type, ["format"] = t.Format.ToString(), ["cbuf_slot"] = t.CbufSlot, ["handle"] = t.HandleIndex, ["array"] = t.ArrayLength, ["separate"] = t.Separate, ["flags"] = (int)t.Flags });
        if (acc.Fetch1D.Count > 0) { var f1 = new JsonArray(); foreach (int h in acc.Fetch1D) f1.Add(h); res["fetch1d"] = f1; }
        File.WriteAllText(Path.Combine(outDir, name + ".res.json"), res.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
    }

    static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--source-sha")   // first 16 digits of the sha256 of the embedded Program.cs + csproj (same convention as dmsh_chain_vk.py)
        {
            var asm = typeof(SwshXlateVk).Assembly; var ms = new MemoryStream();
            foreach (var n in new[] { "src0", "src1" }) asm.GetManifestResourceStream(n).CopyTo(ms);
            Console.WriteLine(Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(ms.ToArray()))[..16].ToLowerInvariant());
            return 0;
        }
        if (args.Length != 2) { Console.Error.WriteLine("usage: dmsh_xlate_vk <job.json> <output dir> | --source-sha"); return 2; }
        var jobs = JsonNode.Parse(File.ReadAllText(args[0])).AsArray(); string outDir = args[1]; Directory.CreateDirectory(outDir);
        int ok = 0, bad = 0;
        foreach (JsonObject job in jobs)
        {
            string key = (string)job["key"], stage = (string)job["stage"];
            try
            {
                byte[] code = File.ReadAllBytes((string)job["code"]);
                // Job debug = false: turn off DebugMode (as dmsh_xlate); no MetalImageAtomics (see the file header)
                var flags = (job["debug"] is JsonNode dbg && !(bool)dbg ? 0 : TranslationFlags.DebugMode) | (stage == "cs" ? TranslationFlags.Compute : 0);
                job.Remove("softfloat64");
                var (p, acc) = Translate(code, job, flags);
                bool fp64 = UsesFloat64(p.BinaryCode);
                if (fp64) acc.Notes.Add("FP64 native; the lowered variant is <key>_fp32");
                Write(outDir, key, key, stage, p, acc);
                if (fp64)   // the variant for devices without shaderFloat64
                {
                    var lowered = (JsonObject)job.DeepClone(); lowered["softfloat64"] = true;
                    try
                    {
                        var (p32, acc32) = Translate(code, lowered, flags); acc32.Notes.Add("FP64 lowered to FP32 (DoubleToFloat)");
                        Write(outDir, key + "_fp32", key, stage, p32, acc32);
                    }
                    catch (Exception e) { Console.Error.WriteLine($"[RED] {key}: the FP32 variant failed, devices without shaderFloat64 cannot run it: {e.GetType().Name}: {e.Message.Split('\n')[0]}"); }
                }
                ok++;
            }
            catch (Exception e) { bad++; Console.Error.WriteLine($"[RED] {key}: {e.GetType().Name}: {e.Message.Split('\n')[0]}"); if (Environment.GetEnvironmentVariable("DMSH_STACK") != null) Console.Error.WriteLine(e.StackTrace); }
        }
        Console.WriteLine($"translated: {ok} ok, {bad} failed");
        return bad == 0 ? 0 : 1;
    }
}
