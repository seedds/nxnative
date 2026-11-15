#!/usr/bin/env python3
# The entry points dmsh_build.py adds to a translated shader's MSL, or rewrites in it: a vertex shader's outputs and its void entry for rasterizer discard (vs_writes, vs_discard),
# integer vertex inputs (int_inputs), outputs set before the body (vs_init), the base instance from Metal (vs_base_instance), the point size and the layer (drop_point_size,
# vs_layer, layer_out, vs_layer_entry), the depth entry point (vs_depth), and a pixel shader's integer and dual-source entry points (fs_uint, fs_dual). Functions of
# the MSL text only, with no build state, so tests import them. Part of the translation chain (dmsh_chain.FILES) and of every shader's cache key (dmsh_build.py CODE_FP).
import re, sys
import dmsh_abi, dmsh_side
from dmsh_reflect import attributes, in_word

def vs_writes(src, binds):
    """Whether a vertex shader has side effects that a draw with rasterizer discard still runs, so that it needs main0_discard: storage buffer writes (Xenoblade Chronicles 3's
    vertex-only work before its skinned models) or image stores (Metroid Prime Remastered's volumetric fog: 1cbc19f806a08166 fills its 100x57x64 RGBA16F froxel volume from
    5,700 points with rasterization off; without the entry the host skipped the draw, the volume stayed zero and the fog pass blended every scene to black)"""
    return any(b.get('rw') for b in binds) or bool(re.search(r'access::(read_)?write\b', src))

def vs_discard(src):
    """Metal requires a void vertex entry when rasterization is disabled.
    Preserve all original side effects and early returns; omit only outputs."""
    m = re.search(r'\bvertex\s+main0_out\s+main0\s*\((.*?)\)\s*\{', src, re.S)
    if not m: raise ValueError('vertex shader has no main0 entry for rasterizer discard')
    return '\nvertex void main0_discard(' + m.group(1) + ')\n{' + src[m.end():].replace('return out;', 'return;')

_CAST = ('as_type<int>(', 'as_type<uint>(')

def int_inputs(src):
    """Inputs that a vertex shader only uses as integers (under a float declaration every use is an as_type<int/uint> bit cast, or a plain copy) → {location: 'uint'}.
    Maxwell does no type conversion when reading vertex attribute registers: the bit pattern of integer formats (bone indices R8UI / RG8UI / RGBA8UI etc.) goes into the register as is and the shader uses it as an integer.
    The answer table has no attribute types (the machine code does not say them), so the translator's attribute type queries all defaulted to float: Metal converts the integer format to a float value, and after the bit cast it is garbage - every bone index is wrong and skinned models (the characters) are stretched into streaks or thrown off screen."""
    ins = {m.group(2): int(m.group(3)) for m in attributes(src)}
    i = src.find('vertex main0_out main0(')
    if i < 0: return {}
    body = src[i:]; out = {}
    for name, loc_ in ins.items():
        uses = list(re.finditer(r'in\.%s\b' % re.escape(name), body))
        nint = 0; bad = False
        for u in uses:
            ls = body.rfind('\n', 0, u.start()) + 1; le = body.find('\n', u.end())
            m = re.match(r'\s*float\s+(_\w+)\s*=\s*in\.%s\.[xyzw];$' % re.escape(name), body[ls:le])
            if not m:
                if body[:u.start()].endswith(_CAST): nint += 1; continue
                bad = True; break
            v = m.group(1)
            for o in list(re.finditer(r'\b%s\b' % re.escape(v), body))[1:]:
                if body[:o.start()].endswith(_CAST): nint += 1; continue
                l2s = body.rfind('\n', 0, o.start()) + 1; l2e = body.find('\n', o.end())
                if re.match(r'\s*[\w.\[\]]+\s*=\s*%s;$' % re.escape(v), body[l2s:l2e]): continue   # plain copy: the bit pattern is the same under either declaration
                bad = True; break
            if bad: break
        if not bad and nint: out[str(loc_)] = 'uint'
    return out

def vs_init(src):
    """Every vertex shader output starts as (0,0,0,1) before the translated body runs: on the hardware that is what an unwritten component reads. The translator's source starts with out = {}, all zeros;
    Ryujinx initializes the components the next stage reads this way when it links stages, and since every shader is translated offline on its own without knowing what the next stage reads, all are
    initialized (components the body writes are overwritten as before). An output written only in xyz (a world position) is divided by its w in the pixel shader: with w = 0 every shadow-map
    compare value became infinite and Link's Awakening was in shadow everywhere outdoors (dim, no shadows from the fences)."""
    i = src.find('struct main0_out\n{\n'); j = src.find('\n};', i); k = src.find('main0_out out = {};')
    if i < 0 or j < 0 or k < 0: return src
    outs = re.findall(r'\n    float4 (\w+) \[\[user\(locn\d+\)\]\];', src[i:j])
    k += len('main0_out out = {};')
    return src[:k] + ''.join(f'\n    out.{n} = float4(0.0, 0.0, 0.0, 1.0);' for n in outs) + src[k:]

VP_BASE_INSTANCE = '(*spvDescriptorSet0.vp_c0)._m0[100].y'   # c0 + 0x644, the driver's base-instance word (row 100, second component)

def vs_base_instance(src):
    """The draw's base instance from Metal, not from the driver segment. GLSLC adds c0 + 0x644 to Maxwell's instance ID where a shader needs the absolute instance (gl_InstanceID +
    gl_BaseInstance); the host writes that word from the draw command (nvn_mtl.m encode_args), but an indirect draw's base instance is in its argument record, which the GPU may write
    earlier in the same submission. Metal passes every draw's base instance, direct or indirect, as [[base_instance]], so a vertex function reads it there. Super Mario Party Jamboree's
    GPU-driven instances (276504dc5c987ecf writes the records, 4b05556737e9535f draws them: record = instance + c0 + 0x644, 80 bytes each) all took the first records' transforms
    with 0 there, and stray grey triangles crossed the board. The segment's rows are float4 holding raw bits, so the word becomes the float with the base instance's bits; a function without
    the parameter gets it"""
    if VP_BASE_INSTANCE not in src: return src
    src = src.replace(VP_BASE_INSTANCE, 'as_type<float>(gl_BaseInstance)')
    def add(m):
        return m.group(0) if '[[base_instance]]' in m.group(0) else m.group(0)[:-1] + ', uint gl_BaseInstance [[base_instance]])'
    return re.sub(r'\bvertex\s+\w+\s+main0\s*\((?:[^()]|\([^()]*\))*\)', add, src)   # the parameter list, whose attributes hold parentheses ([[buffer(0)]])

def drop_point_size(src):
    """Take the point size output out of vertex functions whose output struct no longer has it. A write goes; where the translated body takes the output by reference (_88(out.gl_Position, out.gl_PointSize)), that function still needs something to write, so the
    vertex function gets a local float for it"""
    src = re.sub(r'\n    out\.gl_PointSize = [^;\n]*;', '', src)
    while 'out.gl_PointSize' in src:
        use = src.find('out.gl_PointSize')
        a = [m for m in re.finditer(r'\bvertex\s+\w+\s+\w+\s*\(', src) if m.start() < use]
        if not a: sys.exit('[RED] a point size output used outside a vertex function')
        b, depth = src.find('{', a[-1].end()), 0
        for e in range(b, len(src)):
            depth += src[e] == '{'; depth -= src[e] == '}'
            if not depth: break
        src = src[:b + 1] + '\n    float dmsh_point_size = 1.0;' + src[b + 1:e].replace('out.gl_PointSize', 'dmsh_point_size') + src[e:]
    return src

def vs_layer(src):
    """A vertex shader that writes the layer (render_target_array_index) loses the point size output. The translator writes gl_PointSize = 1.0 in every vertex shader; Metal builds a pipeline
    whose vertex function writes the layer only with an input primitive class, and one that writes a point size only with the point class (or none), so with both no pipeline can be built.
    Pokémon Legends: Z-A's layered full-screen passes (57246f9a04af8529, b29f08619f379a83: a triangle per instance, layer = instance + offset) draw triangles, where the point size is unused;
    the host gives such pipelines the triangle class (layer_out, nvn_mtl.m pso_desc). Shaders that do not write the layer are unchanged"""
    if not layer_out(src): return src
    i = src.find('struct main0_out\n{\n'); j = src.find('\n};', i)
    if '[[point_size]]' not in src[i:j]: return src
    src = src[:i] + re.sub(r'\n    float gl_PointSize \[\[point_size\]\];', '', src[i:j], count=1) + src[j:]
    return drop_point_size(src)

def layer_out(src):
    """Whether main0 (and so main0_z) writes the layer: render_target_array_index in main0's output. The reflection records it per library (layer_out) for the host and
    pso_offline.py, which read nothing else for it: a vertex shader that writes the layer makes its passes layered, and its pipelines take an input primitive class (nvn_mtl.m pso_desc),
    so they get no pulling pipeline and no base in the pipeline archive. Layer entry points (vs_layer_entry) do not count: the host draws with them only in place of a slice-writing
    geometry shader, and asks for them by name (entry_layer). Before the reflection held it, the host and pso_offline.py looked for the layer's name anywhere in the library, and
    left every pair of a library with layer entry points out of the archive (Ocarina of Time's effect vertex shaders, compiled at their first draw)"""
    i = src.find('struct main0_out\n{\n'); j = src.find('\n};', i)
    return i >= 0 and j >= 0 and '[[render_target_array_index]]' in src[i:j]

def vs_layer_entry(full):
    """Layer entry points for a vertex shader that writes integer bits taken from the instance index into an output component (Unreal Engine 4.18's WriteToSliceMainVS: the slice = instance −
    base instance + its first slice, DRAGON QUEST XI S 7ceb7898171a9ae9). main0_l and main0_lz are main0 and main0_z writing that component as the layer (render_target_array_index), without
    the point size (see vs_layer); the host draws with them when the program's geometry shader is a slice writer reading that component (gs_slice). → (source with the two entry points added,
    {'loc', 'comp'}), or (source, None) when no output qualifies"""
    i = full.find('struct main0_out\n{\n'); j = full.find('\n};', i)
    if i < 0 or j < 0 or 'gl_InstanceIndex' not in full or '[[render_target_array_index]]' in full[i:j]: return full, None
    locs = {m.group(1): int(m.group(2)) for m in re.finditer(r'\n    float4 (\w+) \[\[user\(locn(\d+)\)\]\];', full[i:j])}
    m = next((m for m in re.finditer(r'\n    out\.(\w+)\.([xyzw]) = as_type<float>\(', full) if m.group(1) in locs), None)
    if not m: return full, None
    name, comp = m.group(1), m.group(2)
    st = re.sub(r'\n    float gl_PointSize \[\[point_size\]\];', '', full[i:j + 3]).replace('struct main0_out', 'struct main0l_out', 1)
    st = st[:-3] + '\n    uint gl_Layer [[render_target_array_index]];\n};'
    add = '\n' + st + '\n'
    for ep, new in (('main0', 'main0_l'), ('main0_z', 'main0_lz')):
        a = re.search(rf'\bvertex\s+main0_out\s+{ep}\s*\(', full)
        if not a: return full, None
        b, depth = full.find('{', a.end()), 0
        for e in range(b, len(full)):
            depth += full[e] == '{'; depth -= full[e] == '}'
            if not depth: break
        fn = full[a.start():e + 1].replace('main0_out', 'main0l_out').replace(f' {ep}(', f' {new}(', 1)
        fn = drop_point_size(fn)
        add += '\n' + fn.replace('return out;', f'out.gl_Layer = as_type<uint>(out.{name}.{comp});\n    return out;') + '\n'
    return full + add, {'loc': locs[name], 'comp': comp}

def vs_depth(src):
    """Two entry points for the depth convention: the translator's vertex shader leaves z unchanged (NVN's near=0 convention); the main function is copied here - main0 gets the GL convention's (near=-w) z' = z/2 + w/2 before every return out
    (bit-identical to the formula Ryujinx used to emit in the translator), main0_z stays as is. The convention a game sets is runtime state (nvnDeviceSetDepthMode); the host picks the entry point by it, with nothing to configure per game.
    Metal function constants could do this too, but a function with function constants must always be specialized before a pipeline can be built (measured about 2.4 ms per vertex shader); two entry points only compile the vertex shaders twice (about 30% of the library).
    Returns (the source with only main0: reflection / raster facts are computed from it as before, the full source written into the metallib)."""
    m = re.search(r'\bvertex\s+main0_out\s+main0\s*\(', src)
    if not m: sys.exit('[RED] vertex shader without a main0 main function, cannot add the depth entry points')
    body = src[m.start():]   # the main function is the last function in the file
    gl = body.replace('return out;', 'out.gl_Position.z = fma(out.gl_Position.z, 0.5, out.gl_Position.w * 0.5);\n    return out;')
    single = src[:m.start()] + gl
    return single, single + '\n' + re.sub(r'\bmain0\s*\(', 'main0_z(', body, count=1)

def fs_uint(src):
    """Integer colour targets. With no PC version the translator declares every fragment output float; a shader writes an integer as the float with its bits (as_type<float>(…)), and Metal
    refuses a pipeline whose float output meets an integer attachment. Luigi's Mansion 3 draws its whole scene into an R32Uint + RGBA8Uint G-buffer (1,377 of its 4,307 pixel shaders write
    an output from integer bits): a hand-written row per shader (shader_facts_manual.tsv) does not scale to that, and a G-buffer may mix the two (Pokémon Sword's scene shaders write colours
    0 and 2 as floats and colour 1 into an RGBA16Uint target). The hardware writes an output's bits to an integer target whatever the shader computed, so which outputs are integers is the
    pipeline's to say, not the shader's: such a shader gets a second entry point, main0_u, the same body (dmsh_side.shared_body) returning each float colour output n as a float or as the
    unsigned integer vector of the same bits (as_type, nothing converted) by the bool function constant DMSH_UINT_FC + n (dm_ui<n>); its output struct holds both members on [[color(n)]],
    each present by its constant. The host specializes it with the pipeline's integer targets (nvn_mtl.m uint_fn). Telling integer outputs from float bits in the code instead took four
    rules and still missed (Trails in the Sky 2nd Chapter: a uniform word, zeros, a register reused by two branches, a texel read as it is, each beside integer outputs into integer targets).
    → main0_u, to append to the source with its body shared, or None"""
    m = re.search(r'^.*\bfragment\s+main0_out\s+main0\s*\(', src, re.M); st = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    if not m or not st: return None
    mem = re.findall(r'^\s*(\w+)\s+(\w+)\s*(\[\[[^\]]*\]\])?;', st.group(1), re.M)
    cols = {nm: ty for ty, nm, at in mem if 'color(' in at and ty.startswith('float')}
    # main0_u is right for any pixel shader; only those that look like integer writers get one (the library stays small, and a shader with none still names itself in the host's red
    # line): an output from integer bits, directly or through a float local that holds them (PRAGMATA's visibility shaders write float _439 = as_type<float>(int(gl_PrimitiveID)) into an
    # RG32Uint target), nothing but constants (a clear drawn into an integer target, 35e1865e313f0553 writes 0 into Luigi's Mansion 3's R32Uint target), or nothing but flat stage inputs
    # passed through (the bits its vertex or mesh stage wrote: Mario Kart World's dcb0504c844f1ecc copies a primitive's id into an R8Uint target) and constants (Unreal's virtual texture
    # page table update, 17eda07e4bac39f8 in DRAGON BALL: Sparking! ZERO, writes a flat input's x and 0 to y, z, w of an R16Uint target)
    stores = [m for m in re.finditer(r'out\.(\w+)(?:\.\w+)? = ([^;]+);', src) if m.group(1) in cols and not in_word(src, m.start())]   # one pass over the text, not one for each output and each question
    writes = [m.group(2).strip() for m in stores]
    bits = set(re.findall(r'\b(_\d+(?:\[\d+\])?) = as_type<float\d?>\(', src))   # locals given integer bits somewhere
    flat = set(re.findall(r'\b(\w+) \[\[user\(locn\d+\), flat\]\]', src)) if ', flat]]' in src else ()
    def passed(w):   # a flat input, or a local that holds one
        d = re.fullmatch(r'\w+', w) and re.search(r'\bfloat\d? %s = (in\.\w+(?:\.\w+)?);' % w, src)
        i = re.fullmatch(r'in\.(\w+)(?:\.\w+)?', d[1] if d else w)
        return bool(i and i[1] in flat)
    def constant(w): return re.fullmatch(r'-?[0-9.]+f?|float\d\([-0-9., ]*\)', w)
    if not any(w.startswith('as_type<float') or w in bits for w in writes) and not (writes and all(constant(w) or passed(w) for w in writes)): return None
    return uint_entry(src)


def uint_entry(src):
    """main0_u for main0 of src, whatever main0 writes (fs_uint) → its text, to append to the source with its body shared, or None when main0 is not of a shape dmsh_side.entry knows"""
    st = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    try: e = dmsh_side.entry(src, 'main0')
    except ValueError: e = None
    if not e or not st: return None
    mem = re.findall(r'^\s*(\w+)\s+(\w+)\s*(\[\[[^\]]*\]\])?;', st.group(1), re.M)
    cols = {nm: ty for ty, nm, at in mem if 'color(' in at and ty.startswith('float')}
    uty = lambda ty: 'u' + ty.replace('float', 'int')   # float4 → uint4, float → uint
    col = {nm: int(re.search(r'color\((\d+)\)', at).group(1)) for _, nm, at in mem if nm in cols}   # pyright: ignore[reportOptionalMemberAccess]  (every colour member has its index)
    consts = ''.join(f'constant bool dm_ui{n} [[function_constant({dmsh_abi.UINT_FC + n})]];\nconstant bool dm_uf{n} = !dm_ui{n};\n' for n in sorted(set(col.values())))
    struct = 'struct main0_out_u\n{\n' + ''.join(f'    {ty} {nm} {at[:-2]}, function_constant(dm_uf{col[nm]})]];\n    {uty(ty)} {nm}_ui {at[:-2]}, function_constant(dm_ui{col[nm]})]];\n'
                                                if nm in cols else f'    {ty} {nm} {at};\n' for ty, nm, at in mem) + '};\n\n'
    conv = 'main0_out_u r = {};\n' + ''.join(f'    if (dm_ui{col[nm]}) r.{nm}_ui = as_type<{uty(cols[nm])}>(out.{nm}); else r.{nm} = out.{nm};\n' if nm in cols else f'    r.{nm} = out.{nm};\n'
                                          for _, nm, _ in mem) + '    return r;'
    return '\n' + consts + '\n' + struct + dmsh_side.calling(e, 'main0_u', 'main0_out_u', conv)


def fs_dual(src):
    """Dual-source blending. NVN's SRC1 blend factors read the pixel shader's output 1 as target 0's second source (Maxwell feeds MRT 1 to it), and the translator declares that
    output [[color(1)]]; Metal reads the second source only from [[color(0), index(1)]], and its compiler crashes building a pipeline that blends with an absent one (Pokémon Legends:
    Arceus's ha_post_effects, f4f36a03b0c6691d). A shader in the profile's dual_want.tsv that writes exactly colours 0 and 1 gets a second entry point, main0_d, the same body with
    output 1 as index 1 of colour 0; the host takes it for a pipeline whose target 0 blends with a SRC1 factor (nvn_mtl.m pso_desc). Not every such shader gets one: most write a
    second render target (5,474 of Arceus's 6,156 pixel shaders write exactly colours 0 and 1). It calls main0's body (dmsh_side.shared_body): a copy of the body was compiled again, and
    a readable-tile rewrite of its samples would not build there (dmsh_fetch.fold). → main0_d, to append to the source with its body shared (with its other entry points), or None"""
    st = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    if not st: return None
    try: e = dmsh_side.entry(src, 'main0')
    except ValueError: e = None
    if not e: return None
    mem = re.findall(r'^\s*(\w+)\s+(\w+)\s*(\[\[[^\]]*\]\])?;', st.group(1), re.M)
    if sorted(at for _, _, at in mem if 'color(' in at) != ['[[color(0)]]', '[[color(1)]]']: return None
    struct = 'struct main0_out_d\n{\n' + ''.join(f'    {ty} {nm} {"[[color(0), index(1)]]" if at == "[[color(1)]]" else at};\n' for ty, nm, at in mem) + '};\n\n'
    return '\n' + struct + dmsh_side.calling(e, 'main0_d', 'main0_out_d', 'return main0_out_d{' + ', '.join(f'out.{nm}' for _, nm, _ in mem) + '};')
