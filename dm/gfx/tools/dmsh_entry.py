#!/usr/bin/env python3
# The entry points dmsh_build.py adds to a translated shader's MSL, or rewrites in it: a vertex shader's outputs and its void entry for rasterizer discard (vs_writes, vs_discard),
# integer vertex inputs (int_inputs), outputs set before the body (vs_init), the base instance from Metal (vs_base_instance), the point size and the layer (drop_point_size,
# vs_layer, layer_out, vs_layer_entry), the depth entry point (vs_depth), and a pixel shader's typed-output and dual-source entry points (fs_uint, fs_dual). Functions of
# the MSL text only, with no build state, so tests import them. Part of the translation chain (dmsh_chain.FILES) and of every shader's cache key (dmsh_build.py CODE_FP).
import re, sys
import dmsh_abi, dmsh_side, dmsh_alpha, dmsh_logic
from dmsh_reflect import attributes

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
    """The typed-output entry point main0_u, for every pixel shader. Maxwell writes an output register's bits to its target, and the target's format, not the shader, says what they are: a
    float, unsigned or signed integer. With no PC version the translator declares every colour output float, and Metal refuses a pipeline whose float output meets an integer attachment
    (a pipeline from a Metal 4 base is not refused, and the target takes undefined values). Luigi's Mansion 3 draws its whole scene into an R32Uint + RGBA8Uint G-buffer, Pokémon Sword
    mixes float colours 0 and 2 with an RGBA16Uint colour 1, Kirby and the Forgotten Land writes an R8Uint colour 5 beside float ones (GitHub issue #2760), Mortal Kombat 11 needs signed and
    unsigned ones (PR #2807). So the pipeline says which outputs are integers: main0_u is the same body (dmsh_side.shared_body) returning each colour output n as a float, an unsigned or a
    signed vector of the same bits (as_type, nothing converted), by the bool function constants DMSH_UINT_FC + n (dm_ui<n>) and DMSH_SINT_FC + n (dm_si<n>), a float where neither is set
    (dm_uf<n>); its output struct holds the three members on [[color(n)]], each present by its constant. It also holds the fixed-function work Metal lacks, each by a constant and with its
    arguments present only then: NVN's alpha test (DMSH_TYPED_ALPHA_FC, dm_at: dmsh_alpha.test, on colour 0's alpha before the outputs are typed) and logic operation
    (DMSH_TYPED_LOGIC_FC, dm_lo: dmsh_logic.parts, on the typed outputs, reading the attachments DMSH_LOGIC_FC + n names), so one function serves every variant a pipeline asks for, and the
    side library holds one more body than before, not three (dmsh_side.unit gives main0 an alpha test entry point, which needs no specialization, and no logic one). The host specializes it
    with the pipeline's targets and state (nvn_mtl.m typed_fn) when a target's class is not the one main0 declares or the pipeline has a logic operation. A shader got an integer entry
    point only when its code looked like an integer writer, which took five rules and still missed (Trails in the Sky 2nd Chapter: a uniform word, zeros, a register reused by two branches,
    a texel read as it is; Kirby's 76 shaders of #2760): every pixel shader gets it, and the build cuts it into the side library <key>_bx that the host loads on first need
    (dmsh_build.py ps_libraries), so the base library every shader loads stays as it was. A colour member of another type (half) or shape (an index, a function constant) stays as it is.
    → main0_u (with the helpers it uses), to append to the source with its body shared, or None when main0 is not of a shape dmsh_side.entry knows or has no colour output"""
    st = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    try: e = dmsh_side.entry(src, 'main0')
    except ValueError: e = None
    if not e or not st or e['ret'] != 'main0_out': return None
    mem = re.findall(r'^\s*(\w+)\s+(\w+)\s*(\[\[[^\]]*\]\])?;', st.group(1), re.M)
    col = {}   # member → (scalar type, width suffix, colour)
    for ty, nm, at in mem:
        c, t = re.fullmatch(r'\[\[color\(([0-7])\)\]\]', at or ''), re.fullmatch(r'(float|uint|int)([234]?)', ty)
        if c and t: col[nm] = (t.group(1), t.group(2), int(c.group(1)))
    if not col: return None
    ns = sorted({n for _, _, n in col.values()})
    consts = ''.join(f'constant bool dm_ui{n} [[function_constant({dmsh_abi.UINT_FC + n})]];\nconstant bool dm_si{n} [[function_constant({dmsh_abi.SINT_FC + n})]];\n'
                     f'constant bool dm_uf{n} = !dm_ui{n} && !dm_si{n};\n' for n in ns)
    kinds = (('float', '', 'dm_uf'), ('uint', '_ui', 'dm_ui'), ('int', '_si', 'dm_si'))
    def member(ty, nm, at):
        if nm not in col: return f'    {ty} {nm} {at};\n' if at else f'    {ty} {nm};\n'
        _, w, n = col[nm]
        return ''.join(f'    {k}{w} {nm}{sfx} [[color({n}), function_constant({fc}{n})]];\n' for k, sfx, fc in kinds)
    def store(nm):
        if nm not in col: return f'    r.{nm} = out.{nm};\n'
        k0, w, n = col[nm]
        put = lambda k, sfx: f'r.{nm}{sfx} = ' + (f'out.{nm};' if k == k0 else f'as_type<{k}{w}>(out.{nm});')
        return f'    if (dm_ui{n}) {put("uint", "_ui")} else if (dm_si{n}) {put("int", "_si")} else {put("float", "")}\n'
    members = ''.join(member(ty, nm, at) for ty, nm, at in mem)
    alpha = dmsh_alpha.test(src, 'out')
    logic = None if dmsh_logic.refused(src) else dmsh_logic.parts(members, 'r', 'dm_lo')
    helpers, params, pre, post = '', list(e['params']), '', ''
    if alpha:
        helpers += dmsh_alpha.HELPER; consts += f'constant bool dm_at [[function_constant({dmsh_abi.TYPED_ALPHA_FC})]];\n'
        params.append(f'constant dm_alpha_state& dm_alpha [[buffer({dmsh_abi.ALPHA_BUFFER}), function_constant(dm_at)]]'); pre = f'    if (dm_at) {{ {alpha} }}\n'
    if logic:
        lc, fetch, apply, targets = logic
        helpers += dmsh_logic.HELPER; consts += f'constant bool dm_lo [[function_constant({dmsh_abi.TYPED_LOGIC_FC})]];\n' + dmsh_logic.constants(targets) + lc
        params += fetch + [f'constant dm_logic_state& dm_lop [[buffer({dmsh_abi.LOGIC_BUFFER}), function_constant(dm_lo)]]']; post = f'    {" ".join(apply)}\n'
    struct = 'struct main0_out_u\n{\n' + members + '};\n\n'
    return ('\n' + helpers + consts + '\n' + struct + f'{e["attr"]}fragment main0_out_u main0_u({", ".join(params)})\n{{\n    main0_out out = dm_body_main0({e["args"]});\n'
            + pre + '    main0_out_u r = {};\n' + ''.join(store(nm) for _, nm, _ in mem) + post + '    return r;\n}\n')


def typed_ops(msl):   # what the typed-output entry point of msl holds besides its outputs (nvn_mtl.m: the reflection's entry_u_alpha, entry_u_logic)
    return {'alpha': f'dm_at [[function_constant({dmsh_abi.TYPED_ALPHA_FC})]]' in msl, 'logic': f'dm_lo [[function_constant({dmsh_abi.TYPED_LOGIC_FC})]]' in msl}


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
