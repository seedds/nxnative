#!/usr/bin/env python3
# Reflection of a translated shader's MSL (spirv-cross's output), for dmsh_build.py: its vertex attributes, the bindings the host lays out (reflect, with the c2 handle decoding and
# the pool arrays it rewrites, pool_arrays), the driver segment words it reads (drv_reads) and those the replay does not fill (unfillable), and a pixel shader's raster facts
# (raster_facts). Functions of the MSL text and the library ABI (dmsh_abi.py) only, with no build state, so tests import them. Part of the translation chain (dmsh_chain.FILES)
# and of every shader's cache key (dmsh_build.py CODE_FP): what they find goes into the shader's library and its reflection json.
import re
import dmsh_abi as abi, dmsh_fetch, dmsh_geometry

# The NVN driver's constant segments have one layout in every game, which dm/gfx/nvn/dmsh_abi.h holds for the host and this build (through dmsh_abi.py); nvn_mtl.m fills them the same way at run time.
#   c2 C2_TEX + 8 r: handles bound with nvnCommandBufferBindTexture (combined texture and sampler), slot r. The driver has 32 such slots, but a game may bind higher ones, whose words lie in the
#     image table (Hades II binds slot 34, and no image), so a texture instruction's handle anywhere below the separate texture table names a combined slot. In the image
#     table that slot reads the image when one is bound there (nvn_mtl.m tex_slot_h): a size query of the image a compute shader writes (Unreal's GetDimensions on a RWTexture3D),
#     whose type the translator takes from the shader's surface instructions on that handle (dmsh_xlate ImageDims)
#   c2 C2_IMG + 8 r: images (nvnCommandBufferBindImage), slot r < C2_IMG_SLOTS
#   c2 C2_SEPTEX + 8 r: separate textures (nvnCommandBufferBindSeparateTexture), reported as slot SEPTEX_SLOT0 + r, where the host binds them (hle_nvn.c, NVN_SEPTEX_SLOT0)
#   c2 C2_SAMP + 8 r: separate samplers (nvnCommandBufferBindSeparateSampler), slot r < C2_SEP_SLOTS
#   c0 base + 0x10 r: storage buffer r (64-bit address, 32-bit size), base C0_SSBO_<stage> (the hull / domain stages' bases were never seen)
TEX_BASE, IMG_BASE, IMG_SLOTS, SEPTEX_BASE, SEPTEX_SLOT0, SAMP_BASE = abi.C2_TEX, abi.C2_IMG, abi.C2_IMG_SLOTS, abi.C2_SEPTEX, abi.SEPTEX_SLOT0, abi.C2_SAMP

SSBO = {'vs': abi.C0_SSBO_VS, 'cs': abi.C0_SSBO_CS, 'gs': abi.C0_SSBO_GS, 'ps': abi.C0_SSBO_PS, 'ts': abi.C0_SSBO_TS, 'ms': abi.C0_SSBO_MS}

def attributes(src):
    """The vertex attribute declarations, as matches of (type, name, location). Searched only in the lines from the first to the last: from any word of a 220 KB source the pattern costs 9 ms"""
    i, j = src.find('[[attribute('), src.rfind('[[attribute(')
    if i < 0: return []
    end = src.find('\n', j)
    return list(re.finditer(r'(\w+)\s+(\w+)\s*\[\[attribute\((\d+)\)\]\]', src[src.rfind('\n', 0, i) + 1:end if end >= 0 else len(src)]))

def in_word(src, i):   # whether a word character comes just before src[i]
    return i > 0 and (src[i - 1] == '_' or src[i - 1].isalnum())

def has_word(src, text):
    """Whether text, which starts with a word character, is in src with no word character before it: re.search(r'\\b' + text, src) found as text. A pattern that starts with \\b has no literal prefix to skip to"""
    i = src.find(text)
    while i >= 0:
        if not in_word(src, i): return True
        i = src.find(text, i + 1)
    return False

def raster_facts(stage, src):
    """Static facts needed for the runtime proof that the load can be skipped (full-coverage first draw) (nvn_mtl.m first_draw_covers):
        Vertex: whether position x / y / z are taken unchanged from the .x / .y / .z of one and the same vertex attribute (w is the constant 1 or the same attribute's .w), with no branches and no clip distances in the main function - if so, pos_attr = attribute slot;
        Fragment: whether it may discard (discard_fragment), whether it writes sample_mask, and which color indices it writes. Anything unrecognized is treated as "unprovable" (pos_attr = -1 / discard = 1).
        Also, not for that proof: whether a fragment shader reads the primitive ID, which a specialized Metal 4 pipeline does not give it (nvn_mtl.m pso_build_ex), and a vertex shader's
        clip → screen varyings (screen, dmsh_fetch.py screen_varyings), the vertex side of the same-pixel proof for a projective sample's tile read (nvn_mtl.m fb_tile_plan)"""
    if stage == 'ps':
        out = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
        mem = out.group(1) if out else ''
        return {'discard': int('discard_fragment' in src), 'sample_mask': int('sample_mask' in mem), 'primitive_id': int('[[primitive_id]]' in src),
                'colors': sorted({int(x) for x in re.findall(r'\[\[color\((\d+)\)\]\]', mem)}),
                # the colour outputs main0 declares float (bitmap): the host's red line when one meets an integer attachment with no integer entry point fitting (nvn_mtl.m pso_desc)
                'float_colors': sum(1 << int(c) for ty, c in re.findall(r'^\s*(\w+)\s+\w+\s*\[\[color\((\d+)\)[^\]]*\]\];', mem, re.M) if ty.startswith(('float', 'half')))}
    if stage != 'vs': return {}
    scr = {'screen': dmsh_fetch.screen_varyings(src)}   # clip → screen varyings (dmsh_fetch.py): the vertex side of a projective sample's tile read
    body = re.search(r'\bvertex\s+\w+\s+\w+\s*\(.*?\)\s*\{(.*)\}\s*$', src, re.S)   # the main function is the last function in the file
    if not body or re.search(r'\b(if|for|while|switch|do)\b|clip_distance|ClipDistance', body.group(1)): return {'pos_attr': -1, **scr}
    b = body.group(1)
    attr = {m.group(2): int(m.group(3)) for m in attributes(src)}
    var = {m.group(1): (m.group(2), m.group(3)) for m in re.finditer(r'float\s+(_\w+)\s*=\s*in\.(\w+)\.([xyzw]);', b)}
    last = {}
    for m in re.finditer(r'out\.gl_Position\.([xyzw])\s*=\s*([^;]+);', b): last[m.group(1)] = m.group(2).strip()
    src_of = {c: var.get(v) for c, v in last.items()}
    m0 = src_of.get('x')
    if not m0 or m0[0] not in attr or any(src_of.get(c) != (m0[0], c) for c in 'xyz'): return {'pos_attr': -1, **scr}
    if last.get('w') == '1.0': w = 'one'
    elif src_of.get('w') == (m0[0], 'w'): w = 'attr'
    else: return {'pos_attr': -1, **scr}
    # uv pass-through (used for the "this-pixel addressing" proof in merge B step 2): the .x / .y of interpolated output locn K are taken unchanged from the .x / .y of vertex attribute A → uv_from_attr[K] = A
    ost = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    locn = {m.group(1): int(m.group(2)) for m in re.finditer(r'(m_\d+) \[\[user\(locn(\d+)\)\]\]', ost.group(1) if ost else '')}
    olast = {}
    for m in re.finditer(r'out\.(m_\d+)\.([xyzw])\s*=\s*([^;]+);', b): olast[(m.group(1), m.group(2))] = m.group(3).strip()
    uv, aff = {}, {}
    for mem, k in locn.items():
        sx, sy = var.get(olast.get((mem, 'x'), '')), var.get(olast.get((mem, 'y'), ''))
        if sx and sy and sx[0] == sy[0] and sx[0] in attr and sx[1] == 'x' and sy[1] == 'y': uv[str(k)] = attr[sx[0]]; continue
        # Affine: component = fma(attribute component, uniform[slot].vector[i].component, uniform[slot].vector[j].component) (vp_cN is uniform slot N − 3)
        def fm(v_):
            m = re.search(r'float %s = fma\((_\w+), \(\*spvDescriptorSet0\.vp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw]), \(\*spvDescriptorSet0\.vp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw])\);' % re.escape(v_), b)
            if not m or m.group(1) not in var: return None
            return var[m.group(1)], [int(m.group(2)) - 3, int(m.group(3)), 'xyzw'.index(m.group(4))], [int(m.group(5)) - 3, int(m.group(6)), 'xyzw'.index(m.group(7))]
        fx, fy = fm(olast.get((mem, 'x'), '')), fm(olast.get((mem, 'y'), ''))
        if fx and fy and fx[0][0] == fy[0][0] and fx[0][0] in attr and fx[0][1] == 'x' and fy[0][1] == 'y' and min(fx[1][0], fx[2][0], fy[1][0], fy[2][0]) >= 0:
            aff[str(k)] = {'attr': attr[fx[0][0]], 'sx': fx[1], 'ox': fx[2], 'sy': fy[1], 'oy': fy[2]}
    return {'pos_attr': attr[m0[0]], 'pos_w': w, 'uv_from_attr': uv, 'uv_affine': aff, **scr}

def decode_tex(name):
    """fp_t_cb{slots}_{handle} / fp_t_tcb_{handle}: texture slot, sampler slot (Ryujinx TextureHandle encoding; c[2] handle table). Handle type 3 (SeparateConstantSamplerHandle) carries a constant
    sampler ID instead of a second offset, so only the first offset can name the texture (const_sampler gives the ID)"""
    m = re.match(r'[a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)', name)
    if not m: return None
    slots = int(m.group(1)) if m.group(1) else -1; h = int(m.group(2), 16)
    tb = (slots & 0xffff) if slots >= 0 else 2; sb = ((slots >> 16) - 1) if slots >= 0 and (slots >> 16) else tb
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    def slot(word, base, cb): return (word * 4 - base) // abi.C2_ROW if cb == 2 and word * 4 >= base and (word * 4 - base) % abi.C2_ROW == 0 else None
    words = [o0] + ([o1] if ty else [])
    if re.match(r'[a-z]+_i', name):   # image
        return next((slot(x, IMG_BASE, tb) for x in words if IMG_BASE <= x * 4 < IMG_BASE + abi.C2_ROW * IMG_SLOTS and slot(x, IMG_BASE, tb) is not None), None), None, ty
    tw = [o0] if ty in (2, 3) else words
    t = next((slot(x, TEX_BASE, tb) for x in tw if slot(x, TEX_BASE, tb) is not None and x * 4 < SEPTEX_BASE), None)
    if t is None: t = next((SEPTEX_SLOT0 + slot(x, SEPTEX_BASE, tb) for x in tw if slot(x, SEPTEX_BASE, tb) is not None and x * 4 < SAMP_BASE), None)
    s = next((slot(x, SAMP_BASE, sb) for x in words if x * 4 >= SAMP_BASE and slot(x, SAMP_BASE, sb) is not None), None) if ty != 3 else None
    return t, s, ty

def const_sampler(name):
    """The constant sampler ID of a type-3 handle (fp_t_cb65538_3041005C: the texture handle at c2 word 0x5C ORed with sampler 0x104), or None"""
    m = re.match(r'[a-z]+_t\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)', name)
    if not m: return None
    h = int(m.group(2), 16)
    return (h >> 14) & 0x3fff if h >> 28 == 3 else None

def decode_ubo_tex(name):
    """Bindless texture: the handle is written by the game into its own uniform buffer (fp_t_cb7_2A = c[7] word 0x2A). Returns (texture: (uniform slot, byte offset), sampler: (slot, offset));
    handle type 0 = combined handle (one 32-bit word: low 20 bits texture ID, high 12 bits sampler ID), 1 = the sampler handle is in another word (its slot is the high 16 bits of slots − 1). This game's whole library uses only type 0 (1850 shaders)
    Type 3: the sampler is a constant ID (Monster Hunter Rise: a texture handle at c[5] word 0x28 with sampler 0x104); the sampler part is then ('id', ID).
    Type 1 with the sampler word in the driver table: the texture handles are the game's, one separate sampler the shader's own (Kirby Air Riders: array<depth2d<float>, 32> fp_t2ds_cb196614_10570000, c[6] and the
    word at c2 byte 0x570, about 5,700 pixel and compute shaders, its shadows and lights); the sampler part is then ('c2', byte offset)"""
    m = re.match(r'[a-z]+_t\w*?_cb(\d+)_([0-9A-F]+)', name)
    if not m: return None
    slots, h = int(m.group(1)), int(m.group(2), 16)
    tb = slots & 0xffff; sb = (slots >> 16) - 1 if slots >> 16 else tb
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    if not 3 <= tb <= 16 or ty not in (0, 1, 3) or (ty == 1 and sb != 2 and not 3 <= sb <= 16): return None
    return (tb - 3, o0 * 4), (('id', o1) if ty == 3 else ('c2', o1 * 4) if ty and sb == 2 else (sb - 3, o1 * 4) if ty else (tb - 3, o0 * 4))

def decode_ubo_samp_id(name):
    """Handle type 2 (Ryujinx's SeparateSamplerId: the shader ORs the texture word with a second word shifted left by 20) with the texture in the driver table c2 and the second word, the sampler ID
    itself, in the game's own uniform buffer (its bank the high 16 bits of slots, less one). The Witcher 3: fp_t_cb720898_2000005A, texture at c2 word 0x5A, sampler ID at c[10] word 0.
    Returns the sampler word's (uniform slot, byte offset), or None. A texture in a uniform buffer with such a sampler, and arrays of them (the handle tables, nvn_mtl.m htab_snap), are
    not decoded: no game is known to have one"""
    m = re.fullmatch(r'[a-z]+_t\w*?_cb(\d+)_([0-9A-F]+)', name)
    if not m: return None
    slots, h = int(m.group(1)), int(m.group(2), 16)
    sb = (slots >> 16) - 1
    return (sb - 3, ((h >> 14) & 0x3fff) * 4) if h >> 28 == 2 and slots & 0xffff == 2 and 3 <= sb <= 16 else None

def decode_ubo_img(name):
    """An image whose handle the game keeps in its own uniform buffer (<stage>_i_cb<n>_<word>_<format>: Unreal Engine 5.4 on the Switch 2 keeps every image in c[3]). Returns (uniform slot, byte offset)"""
    m = re.match(r'[a-z]+_i\w*?_cb(\d+)_([0-9A-F]+)', name)
    return (int(m.group(1)) - 3, int(m.group(2), 16) * 4) if m and 3 <= int(m.group(1)) <= 16 else None

def buffer_image_bias(name, res):
    """A buffer image's or buffer texture's index in the offset table (dmsh_abi.h DMSH_TBIAS_BANK), which the translator gives it as "tbx" (TranslationFlags.MetalBufferImageBias),
    found by the constant buffer slot and the handle its name carries (<stage>_i_cb<slots>_<handle>_<format> for an image, <stage>_t_cb<slots>_<handle> for a texture, <stage>_tb_...
    for an array of buffer textures, _tcb_<handle> for a handle in no constant buffer) in the translator's images or textures; None for any other binding. An array's is its first
    element's, the rest following it"""
    m = re.match(r'[a-z]+_([ti])[a-z0-9]*_(?:cb(\d+)|tcb)_([0-9A-F]+)(?:_|$)', name)
    if not m: return None
    slots, h = int(m.group(2)) if m.group(2) else None, int(m.group(3), 16)
    return next((i['tbx'] for i in res.get('images' if m.group(1) == 'i' else 'textures', ()) if 'tbx' in i and i['handle'] == h and (i['cbuf_slot'] == slots if slots is not None else i['cbuf_slot'] < 0)), None)

def buffer_bias_entries(res):
    """The offset table's length in entries, as the translator declares it (uint4 data[], ResourceManager.GetBufferImageBiasBinding): every entry given out, an array's one per element"""
    n = max((i['tbx'] + max(i.get('array', 1), 1) for k in ('textures', 'images') for i in res.get(k, ()) if 'tbx' in i), default=0)
    return (n + 3) // 4 * 4

def c2_array(name, ty):
    """An array of handles in the driver table c2 that the shader indexes at run time (Ryujinx's texture array from a buffer, here bank 2: <stage>_t<type>_cb<slots>_<handle>, declared
    array<element, n>, one element per c2 handle word; the index is the handle's byte offset ÷ 8). Luigi's Mansion 3: 586 pixel shaders sample their shadow cascades from
    array<texture2d_array<float>, n> fp_t2das_cb2_0, the index (0x80 + a cascade offset) ÷ 8. Handle type 1 ORs one fixed separate sampler word into every element's handle:
    EA SPORTS FC 27's lights, fp_t2d_cb196610_10570000 (texture words from c2 byte 0, the sampler word at c2 byte 0x570), indexed by separate texture i at element 45 + i.
    → (the c2 byte offset of element 0 (element k = the handle at that offset + 8k), the c2 byte offset of the sampler word or None), or None"""
    m = re.fullmatch(r'[a-z]+_t\w*?_cb(\d+)_([0-9A-F]+)', name[:-5] if name.endswith('Smplr') else name)
    if not m or not ty.startswith('array<'): return None
    slots, h = int(m.group(1)), int(m.group(2), 16)
    if h >> 28 == 0 and slots == 2: return (h & 0x3fff) * 4, None
    if h >> 28 == 1 and slots & 0xffff == 2 and (slots >> 16) - 1 == 2: return (h & 0x3fff) * 4, ((h >> 14) & 0x3fff) * 4
    return None

POOL_MIN = 33   # arrays at least this long are pools: Program.cs sizes Ryujinx's handle fallback by the pools, the vendored BindlessElimination keeps 32-entry slot arrays (NvnSlotArrayLength)

# The test is a plain load: an atomic one, even relaxed, goes to the shared cache every time, and the reads of a pass name a few IDs, so its pixels queue on the same word.
# A 1536x864 pass of 16 marks a pixel over 16 IDs: 2.60 ms atomic, 0.34 plain, 0.18 with no mark (Apple M6). A stale 0 costs an OR that changes nothing; a bit only goes
# from 0 to 1 while a submission runs, as the host clears one (pool_seen_clear) only for an ID registered anew, which work committed after it reads
POOL_MARK = """template<typename P> static inline __attribute__((always_inline)) uint dm_pool_mark(constant P* p, uint i)
{
    device uint* w = reinterpret_cast<device uint*>(p->seen) + (i >> 5);
    uint b = 1u << (i & 31u);
    if (!(*w & b)) atomic_fetch_or_explicit(reinterpret_cast<device atomic_uint*>(w), b, memory_order_relaxed);
    return i;
}

"""

def pool_arrays(src):
    """Ryujinx's arrays for a texture handle it cannot trace to one slot (<stage>_<t|s>_cb0_40000000, indexed by the handle's texture / sampler ID), sized to the whole ID ranges
    (Program.cs QueryTextureArrayLengthFromPool, 2^20 and 2^12) → a pointer to a pool the replay keeps per element type (nvn_mtl.m pool_gpu): struct spvPool_<name> { uint n; ulong seen;
    array<element, 1> e; }, read as (&e[0])[min(index, n)], n being the highest ID the host filled. A read of a texture pool also sets the ID's bit in the bitmap at seen (dm_pool_mark), as which IDs a
    shader reads is known only here: the host makes those textures instead of all of them. The word is read first, with a plain load: a bit is set once, so nearly every read costs a load, not an atomic. The struct declares one element: Metal's argument encoder walks every element
    of a declared array (AGX IndirectArgumentLayoutGen2::parseArray), and for 2^20 textures that took 57 ms and 66 MB per encoder (9.7 GB in Mario Party Superstars' prewarm);
    the elements are 8-byte resource IDs, so the pointer reaches the rest (checked on the GPU). Mario Party Superstars' characters read their light textures' handles from a
    storage buffer (6,370 pixel shaders). The argument IDs are then renumbered in member order, a pointer taking one and any other array its length, as the host lays the buffer out.
    Buffer textures and images keep their 32-entry slot arrays (tex_dyn_slots). ValueError on a use of a pool array the rewrite does not cover.
    An array of the handles in one of the game's uniform buffers (Ryujinx's texture array from a buffer, as long as a 64 KB buffer's handles, Program.cs QueryTextureArrayLengthFromBuffer) takes
    the same form: the host keeps a table of that buffer's handles, as long as its last handle (nvn_mtl.m htab_gpu), so the element index is unchanged and only the textures the table names are made.
    A table's reads are not marked: its index is an element of the table, not a texture ID, and the host has made every texture the table names already (it leaves seen 0). Marked through that
    null address, Fire Emblem Fortune's Weave's lighting pass took 197 ms a frame on an M1 Pro (PR #1738)"""
    body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', src, re.S)
    if not body: return src
    pools = {}
    def to_pointer(m):
        if int(m.group(3)) < POOL_MIN or not (m.group(4).endswith('_cb0_40000000') or decode_ubo_tex(m.group(4).removesuffix('Smplr'))): return m.group(0)
        pools[m.group(4)] = (m.group(2), int(m.group(3)), m.group(2) != 'sampler' and m.group(4).endswith('_cb0_40000000'))   # element, length, marked (a texture pool's reads)
        return f'{m.group(1)}constant spvPool_{m.group(4)}* {m.group(4)} [[id('
    members = re.sub(r'^(\s*)array<(.+), (\d+)> (\w+) \[\[id\(', to_pointer, body.group(1), flags=re.M)
    if not pools: return src
    aid = 0
    def renumber(m):
        nonlocal aid
        a = re.fullmatch(r'array<.+, (\d+)>', m.group(2).strip())
        out = f'{m.group(1)}{m.group(2)} {m.group(3)} [[id({aid}){m.group(5)}]];'
        aid += int(a.group(1)) if a else 1
        return out
    members = re.sub(r'^(\s*)(.+?)\s+(\w+)\s*\[\[id\((\d+)\)((?:\s*,\s*\w+\(\d+\))*)\]\];', renumber, members, flags=re.M)
    structs = ''.join(f'struct spvPool_{nm}\n{{\n    uint n;\n    ulong seen;\n    array<{el}, 1> e;\n}};\n\n' for nm, (el, n, _) in pools.items())
    marked = any(mk for _, _, mk in pools.values())
    if marked: structs = POOL_MARK + structs
    i = body.start()
    src = src[:i] + structs + src[i:body.start(1)] + members + src[body.end(1):]
    for nm in pools:   # base.name[index] → (&base.name->e[0])[min(uint(index), base.name->n)], the index taken with its brackets balanced
        out, pos = [], 0
        for m in re.finditer(r'([\w.]+)\.%s\[' % re.escape(nm), src):
            if m.start() < pos: continue
            depth, j = 1, m.end()
            while depth:
                if j >= len(src): raise ValueError(f'{nm}: unbalanced index')
                depth += {'[': 1, ']': -1}.get(src[j], 0); j += 1
            base = f'{m.group(1)}.{nm}'
            index = f'min(uint({src[m.end():j - 1]}), {base}->n)'
            out.append(src[pos:m.start()] + f'(&{base}->e[0])[{f"dm_pool_mark({base}, {index})" if pools[nm][2] else index}]'); pos = j
        src = ''.join(out) + src[pos:]
        names = 3 if pools[nm][2] else 2   # each read names it twice, and once more in a mark; the member once (spvPool_<name> is one word)
        left = len(re.findall(r'\b%s\b' % re.escape(nm), src)) - names * len(re.findall(r'\.%s->e\[0\]\)\[' % re.escape(nm), src)) - 1
        if left: raise ValueError(f'{nm}: {left} uses of the pool array the rewrite does not cover')
    # The mark stores to memory, so Metal would test this fragment shader against depth after running it: every covered pixel is shaded (no hidden surface removal) and the depth it tests differs by
    # an ulp from a prepass's, which fails an EQUAL pass along contours. The mark is the host's own, so it must not change when the test runs: early, unless the shader discards or writes depth, which
    # a test before it would get wrong (then it stays late, and only costs the shading)
    own = src.replace(POOL_MARK, '')   # what the shader does itself: a store of its own keeps the test where the game's own shader has it
    if marked and not re.search(r'\bdiscard_fragment\b|\[\[\s*depth\(|\[\[\s*sample_mask\s*\]\]|\[\[\s*stencil\s*\]\]|\.write\(|\batomic_\w+_explicit\b|^\s*device\s', own, re.M):
        src = re.sub(r'^(?<!\]\] )fragment\s', '[[early_fragment_tests]] fragment ', src, flags=re.M)
    return src

def reflect(key, stage, msl, res):
    """MSL argument buffer struct member → replay reflection: kind (same as nvn_refl.h) + NVN slot number + argument buffer member index"""
    body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', msl, re.S)
    binds = []
    # A member may carry attributes after its id: SPIRV-Cross marks the resources a fragment shader writes inside its interlock [[id(n), raster_order_group(0)]]
    # (Mario Party Superstars' light_primitive_point_cluster.bnsh, a storage buffer and two images); a member the pattern skipped left the host's argument buffer longer than the reflection
    members = list(re.finditer(r'^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)(?:\s*,\s*\w+\(\d+\))*\]\];', body.group(1) if body else '', re.M))
    if body and len(members) != body.group(1).count('[[id('): raise ValueError(f'{key}: an argument buffer member the reflection cannot read')
    for m in members:
        ty, nm, aid = m.group(1).strip(), m.group(2), int(m.group(3))
        e = {'msl_name': nm, 'msl_type': ty, 'arg_id': aid}
        pool = re.search(r'struct spvPool_%s\s*\{\s*uint n;\s*ulong seen;\s*array<(.+), \d+> e;' % re.escape(nm), msl) if ty == f'constant spvPool_{nm}*' else None
        if ty == 'sampler' and nm.endswith('Smplr' + abi.BORDER_TWIN):   # a sampler's twin (border_emu): the same sampler, its bit in the block in the order of the twins
            nm = nm[:-len(abi.BORDER_TWIN)]; e['border_bit'] = sum(1 for b in binds if 'border_bit' in b)
        if nm == abi.BORDER_MEMBER: e.update(kind=abi.KIND_BORDER)
        elif nm in dmsh_geometry.HELPERS: e.update(kind=abi.KIND_GEOMETRY, reg=dmsh_geometry.HELPERS[nm])
        elif pool and not nm.endswith('_cb0_40000000'):   # a table of a uniform buffer's handles (pool_arrays): reg and off as an array of them, below
            ut, us = decode_ubo_tex(nm.removesuffix('Smplr'))   # pyright: ignore[reportGeneralTypeIssues]  (not None: pool_arrays made this pool only for a name it decodes)
            e.update(kind=abi.KIND_SAMP_HTAB if pool.group(1) == 'sampler' else abi.KIND_TEX_HTAB, elem=pool.group(1), reg=ut[0], off=ut[1], **({'soff': us[1], 'sreg': abi.SREG_C2} if us[0] == 'c2' else {}))
        elif pool: e.update(kind=abi.KIND_SAMP_POOL if pool.group(1) == 'sampler' else abi.KIND_TEX_POOL, elem=pool.group(1))   # see pool_arrays
        elif nm == 'support_buffer': e.update(kind=abi.KIND_SUPPORT)
        elif c := re.fullmatch(r'[a-z]+_c(\d+)', nm):
            n = int(c.group(1))
            if 3 <= n <= 16:   # fixed: the slot's bytes the shader reads at fixed offsets, which the host makes readable (zeros past a shorter binding); the translator's own count (Program.cs)
                e.update(kind=abi.KIND_CBUF, reg=n - 3, fixed=next((b['fixed'] for b in res['cbufs'] if b['slot'] == n), 0))
            elif n == abi.CBSIZE_BANK: e.update(kind=abi.KIND_CBSIZE)   # the uniform slots' bound sizes, which the translator's reads at computed offsets compare with
            elif n == abi.TBIAS_BANK: e.update(kind=abi.KIND_TBIAS, n=buffer_bias_entries(res))   # the buffer textures' element offsets, which the translator adds to their coordinates (buffer_image_bias)
            elif n == abi.SBLEN_BANK: e.update(kind=abi.KIND_SBLEN)   # the bounded storage buffers' lengths, which their accesses compare with (Ryujinx TranslationFlags.MetalStorageBounds)
            else: e.update(kind=abi.KIND_C1 if n == 1 else abi.KIND_DRVCB, bank=n)
        elif sb := re.fullmatch(r'[a-z]+_s(\d+)', nm):
            e.update(kind=abi.KIND_SBUF, index=int(sb.group(1)))
        elif ty == 'sampler' and nm.endswith('Smplr') and (si := decode_ubo_samp_id(nm[:-5])):   # a sampler ID itself in a uniform buffer word (handle type 2): the replay shifts it into a handle
            e.update(kind=abi.KIND_SAMP_UBO, reg=si[0], off=si[1], raw_id=True)
        elif u := decode_ubo_tex(nm[:-5] if nm.endswith('Smplr') else nm):   # bindless: the handle is in a uniform buffer (reg = uniform slot, off = byte offset)
            ut, us = u
            other = () if us[0] in ('id', 'c2') or us == ut else (ut, us)   # handle type 1: the word of the other half, which the host ORs in (the compiler puts texture and sampler words in either order)
            if nm.endswith('Smplr') and us[0] == 'c2': e.update(kind=abi.KIND_SAMP_UBO, reg=ut[0], off=ut[1], soff=us[1], sreg=abi.SREG_C2)
            elif nm.endswith('Smplr'): e.update(kind=abi.KIND_SAMP_ID, reg=us[1]) if us[0] == 'id' else e.update(kind=abi.KIND_SAMP_UBO, reg=us[0], off=us[1], **({'soff': ut[1], 'sreg': ut[0]} if other else {}))
            elif 'texture_buffer' in ty:
                e.update(kind=abi.KIND_TBUF_UBO, reg=ut[0], off=ut[1])
                if not ty.startswith('array<') and (x := buffer_image_bias(nm, res)) is not None: e['tbx'] = x   # a buffer texture: its index in the offset table (the host binds one texture here)
            else: e.update(kind=abi.KIND_TEX_UBO, reg=ut[0], off=ut[1], **({'soff': us[1], 'sreg': us[0]} if other else {}))
        elif decode_ubo_img(nm):
            e.update(kind=abi.KIND_IMG_UBO, reg=decode_ubo_img(nm)[0], off=decode_ubo_img(nm)[1])   # pyright: ignore[reportOptionalSubscript]  (not None: tested above)
            if (x := buffer_image_bias(nm, res)) is not None: e['tbx'] = x   # as KIND_IMG below
            if has_word(msl, nm + '.atomic_'): e['atomic'] = True   # as KIND_IMG below
        elif c2_array(nm, ty) is not None:   # an array of the driver table's handles (c2_array): the replay fills element k from the slot whose c2 word is at off + 8k
            off, soff = c2_array(nm, ty)   # pyright: ignore[reportGeneralTypeIssues]  (not None: tested above)
            e.update(kind=abi.KIND_SAMP_C2A if nm.endswith('Smplr') else abi.KIND_TEX_C2A, off=off, **({'soff': soff} if soff is not None else {}))
            if not nm.endswith('Smplr') and 'texture_buffer' in ty and (x := buffer_image_bias(nm, res)) is not None: e['tbx'] = x   # an array of buffer textures: its first entry
        elif nm.endswith('Smplr'):   # handle type 0 = texture and sampler combined in one handle: the sampler index is taken from the high bits of that texture slot's handle
            t, s, ty = decode_tex(nm[:-5]); e.update(  # pyright: ignore[reportGeneralTypeIssues]  (a sampler whose name does not decode stops the shader here)
                kind=abi.KIND_SAMP_ID, reg=const_sampler(nm[:-5])) if ty == 3 else e.update(kind=abi.KIND_SAMP, reg=s) if ty else e.update(kind=abi.KIND_SAMP_TEX, reg=t)
        else:
            d = decode_tex(nm)
            if not d: e.update(kind=abi.KIND_UNKNOWN)
            else: e.update(kind=abi.KIND_IMG if re.match(r'[a-z]+_i', nm) else (abi.KIND_TBUF if 'texture_buffer' in ty else abi.KIND_TEX), reg=d[0])
            # native texture atomics (dmsh_atomic.py): Metal allows them only on an R32Uint / R32Sint texture made with the shader-atomic usage, which the host checks for this slot
            if e.get('kind') == abi.KIND_IMG and has_word(msl, nm + '.atomic_'): e['atomic'] = True
            if e.get('kind') in (abi.KIND_IMG, abi.KIND_TBUF) and (x := buffer_image_bias(nm, res)) is not None: e['tbx'] = x   # a buffer image or texture: its index in the offset table
            # a 1D integer fetch the translator read as 2D rows of 16384 texels (Program.cs RecordTexelFetch1D): the host binds a texture buffer in this slot as such rows (nvn_mtl.m tbuf_as_2d)
            if e.get('kind') == abi.KIND_TEX and e.get('reg') in res.get('fetch1d', ()):
                e['fetch1d'] = True
                if has_word(msl, nm + '.get_width('): e['fetch1d_size'] = True   # also size-queried: a texture buffer of more than 16384 elements reports 16384 there
        binds.append(e)
    # Storage buffers: SPIR-V binding number → c0 offset → NVN storage buffer slot (c0 base SSBO[stage], C0_ROW bytes per slot)
    base = SSBO.get(stage)   # storage buffer base for the hull / domain stages has not been measured: a storage buffer there is reported with no slot
    sb_sorted = sorted(res['sbufs'], key=lambda b: b['binding'])
    for e in binds:
        if e['kind'] == abi.KIND_SBUF:
            b = sb_sorted[e['index']] if e['index'] < len(sb_sorted) else None
            off = b['cb_offset'] * 4 if b and b['cb_slot'] == 0 else None
            if b and b['cb_slot'] == abi.GMEM_MAP_CB_SLOT:   # not a game buffer: for addresses traced to no storage buffer, the host's pool table (offset 0) or the write log (DMSH_GMEM_LOG_CB_OFFSET)
                if b['cb_offset'] == abi.GMEM_LOG_CB_OFFSET: e.update(kind=abi.KIND_GMEM_LOG, rw=True); continue
                # rw: one of those accesses writes (the translator notes each one with its operation: Load, Store or an atomic)
                e.update(kind=abi.KIND_GMEM_MAP, rw=any(re.search(r'operation "(Store|Atomic\w*)" traced to no storage buffer', n) for n in res.get('notes', ()))); continue
            if stage == 'cs' and off is not None and abi.C0_CS_UBO <= off < abi.C0_CS_UBO + abi.C0_ROW * abi.C0_CS_UBO_SLOTS and (off - abi.C0_CS_UBO) % abi.C0_ROW == 0:
                e.update(kind=abi.KIND_UBO_PTR, reg=(off - abi.C0_CS_UBO) // abi.C0_ROW)   # compute-stage uniform slot r: address at c0[C0_CS_UBO + 0x10 r], read through global memory (checked against cs_lut_combine_c reading cb10[66])
            elif b and b['cb_slot'] == abi.PUSH_BANK:   # the address is one the game pushed (c[24]; PRAGMATA's compute buffer entries in push block 1): the host reads it from its push blocks
                e.update(kind=abi.KIND_SBUF_PUSH, off=b['cb_offset'] * 4)
            elif b and b['cb_slot'] >= 3:   # the address is in the game's own uniform buffer c[n] = uniform slot n - 3 (Monster Hunter Rise's compute shaders: c3 + 0x38); the replay reads it there
                e.update(kind=abi.KIND_SBUF_UBO, reg=b['cb_slot'] - 3, off=b['cb_offset'] * 4)
            else: e['reg'] = (off - base) // abi.C0_ROW if off is not None and base is not None and 0 <= off - base < abi.C0_ROW * abi.C0_SSBO_SLOTS and (off - base) % abi.C0_ROW == 0 else None
            e['rw'] = bool(b and b['flags'] & 1)   # BufferUsageFlags.Write; Program.cs serializes the translator enum unchanged (atomics count as writes too)
            if b and b['flags'] & 2:   # BufferUsageFlags.Bounded: its accesses compare with its entry in the length table (dmsh_abi.h DMSH_SBLEN_BANK), the translator's storage slot
                if e['kind'] not in (abi.KIND_SBUF_UBO, abi.KIND_SBUF_PUSH) or not 0 <= b['slot'] < abi.SBLEN_SLOTS:
                    raise ValueError(f"{key}: storage buffer {e['msl_name']} is bounded but is a {e['kind']} binding, slot {b['slot']}")
                e['sbx'] = b['slot']
    if any('sbx' in e for e in binds) and not any(e['kind'] == abi.KIND_SBLEN for e in binds):
        raise ValueError(f'{key}: bounded storage buffers without their length table')
    return binds

DRV_FILL = {0: [(b, b + abi.C0_ROW * abi.C0_SSBO_SLOTS) for k, b in sorted(SSBO.items(), key=lambda kv: kv[1]) if k not in ('ts', 'ms')],   # c0: storage buffer address tables (vertex / compute / geometry / pixel; a task or mesh stage's: unfillable())
            2: [(TEX_BASE, SEPTEX_BASE), (IMG_BASE, IMG_BASE + abi.C2_ROW * IMG_SLOTS), (SEPTEX_BASE, SAMP_BASE), (SAMP_BASE, SAMP_BASE + abi.C2_ROW * abi.C2_SEP_SLOTS)]}   # c2: the handle tables above, all filled by the replay layer

INDEX_BUILTINS = {'gl_SampleID': (0, abi.C0_SAMPLE_BYTES - 1)}   # a computed index's inputs known to be bounded: the sample index, below the sample count (at most the table's bytes)
INDEX_DEF = re.compile(r'^\s*(?:int|uint)\s+(_\d+)\s*=\s*([^;\n]+);', re.M)
INDEX_SET = re.compile(r'^\s*(_\d+)\s*=', re.M)

def index_range(src, expr):
    """The range [lo, hi] of a computed vector index in the MSL, or None: an integer expression of literals and bounded builtins (INDEX_BUILTINS), through SPIRV-Cross's
    single-assignment int / uint temporaries and bit-casts, with + - * & | ^ << >> on non-negative values. Anything else (a float operation, a uniform read, a temporary
    assigned twice) gives None. PRAGMATA's 156 pixel shaders read the sample position table byte by byte at c0 0x620 + gl_SampleID (a689530d00090ebf: row 98)"""
    defs, sets = {}, set(INDEX_SET.findall(src))   # a temporary assigned again after its declaration (a loop's phi) has no single value
    for name, e in INDEX_DEF.findall(src): defs.setdefault(name, []).append(e)
    float_defs = dict(re.findall(r'^\s*float\s+(_\d+)\s*=\s*as_type<float>\(([\w.]+)\);', src, re.M))   # a bit-cast of a builtin into a float temporary keeps its bits
    import ast as _ast   # here, so the reflection tests can lift this function alone (test_dmsh_reflect.py builder_unfillable)
    def ev(n, depth):
        if depth > 64: return None
        if isinstance(n, _ast.Constant) and isinstance(n.value, int): return (n.value, n.value)
        if isinstance(n, _ast.Name):
            if n.id in INDEX_BUILTINS: return INDEX_BUILTINS[n.id]
            if n.id in float_defs and float_defs[n.id] in INDEX_BUILTINS and n.id not in sets: return INDEX_BUILTINS[float_defs[n.id]]
            if len(defs.get(n.id, ())) != 1 or n.id in sets: return None
            return parse(defs[n.id][0], depth + 1)
        if isinstance(n, _ast.Call) and isinstance(n.func, _ast.Name) and n.func.id == '_c' and len(n.args) == 1 and not n.keywords: return ev(n.args[0], depth + 1)
        if not isinstance(n, _ast.BinOp): return None
        a, b = ev(n.left, depth + 1), ev(n.right, depth + 1)
        if a is None or b is None: return None
        op = type(n.op)
        if op is _ast.Add: r = (a[0] + b[0], a[1] + b[1])
        elif op is _ast.Sub: r = (a[0] - b[1], a[1] - b[0])
        elif op is _ast.Mult: r = (a[0] * b[0], a[1] * b[1])
        elif op is _ast.RShift and b[0] == b[1] and b[0] < 32: r = (a[0] >> b[0], a[1] >> b[0])
        elif op is _ast.LShift and b[0] == b[1] and b[0] < 32: r = (a[0] << b[0], a[1] << b[0])
        elif op is _ast.BitAnd: r = (0, min(a[1], b[1]))
        elif op in (_ast.BitOr, _ast.BitXor): r = (0, (1 << max(a[1], b[1]).bit_length()) - 1)
        else: return None
        return r if 0 <= r[0] <= r[1] < 1 << 31 else None
    def parse(e, depth):
        e = re.sub(r'\b(?:as_type<\s*u?int\s*>|u?int)\s*\(', '_c(', e)   # integer casts and bit-casts keep a non-negative value's bits
        e = re.sub(r'\b(\d+)u\b', r'\1', e)
        try: tree = _ast.parse(e, mode='eval')
        except SyntaxError: return None
        return ev(tree.body, depth)
    return parse(expr, 0)

def drv_reads(src):
    """Reads of driver segments c0 / c2 in the MSL: constant index → byte range; a computed index whose range is known (index_range) → the rows it can read;
    any other non-constant index is recorded as dynamic"""
    out = {}
    for bank in (0, 2):
        offs, dyn = set(), False
        for m in re.finditer(r'fp_c%d\)\._m0\[([^\]]+)\]' % bank, src):
            idx = m.group(1).rstrip('u')
            if idx.isdigit(): offs.add(int(idx) * 16); continue
            r = index_range(src, m.group(1))
            if r and r[1] - r[0] < 16: offs.update(range(r[0] * 16, r[1] * 16 + 16, 16))
            else: dyn = True
        if offs or dyn: out[str(bank)] = {'vec4_offsets': sorted(offs), 'dynamic': dyn}
    return out

SAMPLE_ROWS = [(abi.C0_SAMPLE_TABLE, abi.C0_SAMPLE_TABLE + abi.C0_SAMPLE_BYTES), (abi.C0_SAMPLE_INFO, abi.C0_SAMPLE_INFO + abi.C0_ROW)]   # c0: the pixel stage's sample position rows

def unfillable(dr, stage):
    bad = []
    for bank, d in dr.items():
        fill = DRV_FILL[int(bank)] + (SAMPLE_ROWS if bank == '0' and stage == 'ps' else []) + ([(SSBO[stage], SSBO[stage] + abi.C0_ROW * abi.C0_SSBO_SLOTS)] if bank == '0' and stage in ('ts', 'ms') else [])
        if bank == '0' and stage == 'cs': fill = fill + [(abi.C0_CS_UBO, abi.C0_CS_UBO + abi.C0_ROW * abi.C0_CS_UBO_SLOTS)]   # compute's uniform address table, which the host fills
        # a computed index into c2 reads one of its handle tables, which the replay layer fills whole (nvn_mtl.m DRV2: EA SPORTS FC 27's lights read the separate texture words
        # at 0x168 + 8i to test each light's handle); c0 is filled only in its tables and words above, so a computed index there may read what the replay does not write.
        # The one computed c0 index known is GLSLC's sample position lookup in a pixel shader: a byte of the table at 0x620, at an index made of the sample index (PIXLD.MYINDEX)
        # and the pixel's place in the location grid masked by the words of the 0x630 row, at most 15 (Metroid Prime Remastered: 30 of its 438 shaders that read the sample index)
        if d['dynamic'] and bank == '0' and not (stage == 'ps' and abi.C0_SAMPLE_INFO in d['vec4_offsets']): bad.append(f'c{bank} dynamic index')
        for o in d['vec4_offsets']:
            # a read is a 16-byte row: overlapping any table is enough (Engage reads the c2+0x160 row for texture slot 0 at 0x168 in it)
            if not any(o < b and a < o + 16 for a, b in fill): bad.append(f'c{bank}+{o:#x}')
    return bad
