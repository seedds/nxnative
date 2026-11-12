"""Tessellation without a Metal tessellator (Metal 4 render encoders have no tessellation stage).

The host replays a patch draw as: the vertex shader's capture entry (dmsh_geometry.capture, 144-word records) ->
the hull shader as an ordinary compute kernel -> the domain shader as an ordinary vertex function evaluated on a
fixed grid of domain points -> the game's fragment shader in its own render pass (nvn_tess.inc).

SPIRV-Cross writes hull shaders as kernels reading their control points through [[stage_in]] and domain shaders as
post-tessellation vertex functions. Both are rewritten here, from their MSL and the SPIR-V decorations:

* hull: main0_in comes from the capture records (location L = words 16 + 4L, position = words 0..3) through a
  control-point index buffer; the per-control-point output struct and the per-patch output struct are laid out in
  location slots (CP_SLOTS float4 per control point, position in the last; PATCH_SLOTS per patch), so a domain shader
  built on its own finds them by location. Tessellation factors stay in SPIRV-Cross's MTLQuadTessellationFactorsHalf.
* domain: [[patch]] / [[stage_in]] / [[position_in_patch]] are replaced by reads of those slots, gl_TessCoord from
  a host-written cell list (adaptive: each patch subdivided by its own hull factors, see TESS_MSL) or vertex_id on a
  fixed N x N grid (two triangles per cell, in the domain's winding), and the patch's
  culling: a patch whose hull wrote an outer factor <= 0 (or NaN) emits its vertices outside the clip volume.

The tessellation mode is the domain shader's own control section (+0x738 primitive mode, +0x73c spacing, +0x744
vertex order, as OpenGL enums): the translator answers a fixed default (triangles, equal spacing), so the MSL's
[[patch(triangle)]] is not the game's. Only quad domains are rewritten; anything else raises ValueError and the
shader is left as it was (its draws stay skipped). No game IDs or shader keys participate.
"""
import re
import struct

CP_SLOTS = 33       # float4 per output control point: locations 0..31, position last
PATCH_SLOTS = 32    # float4 per patch: patch locations 0..31
CAPTURE_WORDS = 144
# Adaptive quad / fractional_even subdivision, shared with the host's expansion kernel (nvn_tess.inc k_tess_msl: keep
# the two texts identical). L = the hull's MTLQuadTessellationFactorsHalf: edges 0 (u=0), 1 (v=0), 2 (u=1), 3 (v=1),
# inside 0 (u), 1 (v). A level is clamped to [2, lmax] (NaN -> 2); n = its even segment count. fractional_even: n-2
# segments of 1/l and two equal shorter ones in the middle (placement is implementation-defined; symmetric here so a
# shared edge evaluated from either side gives the same values).
# Layout as a hardware tessellator's (a single grid snapped to the edge points folds triangles where an inside level is
# below an edge level): an inner grid of (nu-2) x (nv-2) cells on the inside levels' interior lines (positions 1 .. n-1), and an
# outer ring of four strips, one per edge, each stitching that edge's own points (its own level, corner to corner) to the
# inner grid's border points; the strips run counter-clockwise (v=0, u=1, v=1, u=0) and share the corner diagonals.
# A strip of a outer and b inner segments has a + b triangles in a zipper order fixed by the indices alone (step i of the
# outer chain before step j of the inner one when (2i+1)b <= (2j+1)a). Every triangle lies between two parallel monotone
# chains or inside the grid, so nothing folds or overlaps, and neighbours sharing an edge generate exactly the same edge
# vertices (no cracks). Entries: a patch's inner cells, then each strip's triangles two per entry (6 vertices; a missing
# second triangle collapses to a point); at most lmax * lmax entries per patch, numbered 0 .. dm_tcount - 1.
TESS_MSL = """static inline float dm_tl(half h, float lmax) { float l = float(h); return l >= 2.0f ? min(l, lmax) : 2.0f; }
static inline uint dm_tn(float l) { return uint(ceil(l * 0.5f)) * 2u; }
static inline float dm_tpos(uint k, uint n, float l) { uint h = n / 2u; bool hi = k > h; uint kk = hi ? n - k : k; float p = kk == h ? 0.5f : float(kk) / l; return hi ? 1.0f - p : p; }
static inline bool dm_tcull(device const half* L) { return !(float(L[0]) > 0.0f && float(L[1]) > 0.0f && float(L[2]) > 0.0f && float(L[3]) > 0.0f); }
static inline uint dm_tcount(device const half* L, float lmax) {
    uint nu = dm_tn(dm_tl(L[4], lmax)), nv = dm_tn(dm_tl(L[5], lmax)), c = (nu - 2u) * (nv - 2u);
    for (uint s = 0u; s < 4u; s++) c += (dm_tn(dm_tl(L[(s + 1u) & 3u], lmax)) + ((s & 1u) != 0u ? nv : nu) - 1u) / 2u;
    return c;
}
static inline uint dm_tsize(device const half* L, float lmax) { uint m = 2u; for (uint i = 0u; i < 6u; i++) m = max(m, dm_tn(dm_tl(L[i], lmax))); return m; }
static inline uint dm_tzpos(uint i, uint a, uint b) { uint n = (2u * i + 1u) * b; return i + (n <= a ? 0u : min(b, (n + a - 1u) / (2u * a))); }
static inline float2 dm_tvert(uint e, uint k, device const half* L, float lmax, bool cw) {
    float lu = dm_tl(L[4], lmax), lv = dm_tl(L[5], lmax);
    uint nu = dm_tn(lu), nv = dm_tn(lv), kk = k % 3u, ic = (nu - 2u) * (nv - 2u);
    if (cw && kk != 0u) kk = 3u - kk;
    if (e < ic) {
        uint ci = e % (nu - 2u), cj = e / (nu - 2u);
        uint2 d = k < 3u ? (kk == 0u ? uint2(0u, 0u) : kk == 1u ? uint2(1u, 0u) : uint2(1u, 1u)) : (kk == 0u ? uint2(0u, 0u) : kk == 1u ? uint2(1u, 1u) : uint2(0u, 1u));
        return float2(dm_tpos(1u + ci + d.x, nu, lu), dm_tpos(1u + cj + d.y, nv, lv));
    }
    uint r = e - ic, s = 0u, a = 2u, n = 2u; float le = 2.0f;
    for (; s < 4u; s++) {
        le = dm_tl(L[(s + 1u) & 3u], lmax); a = dm_tn(le); n = (s & 1u) != 0u ? nv : nu;
        uint c = (a + n - 1u) / 2u;
        if (r < c) break;
        r -= c;
    }
    s = min(s, 3u);
    uint b = n - 2u, t = 2u * r + (k < 3u ? 0u : 1u);
    bool out; uint idx;
    if (t >= a + b) { out = true; idx = 0u; }
    else {
        uint lo = 0u, hi = a;
        while (lo < hi) { uint m = (lo + hi) / 2u; if (dm_tzpos(m, a, b) < t) lo = m + 1u; else hi = m; }
        uint i = lo, j = t - i; bool stepA = i < a && dm_tzpos(i, a, b) == t;
        if (kk == 0u) { out = true; idx = i; }
        else if (stepA) { out = kk == 1u; idx = kk == 1u ? i + 1u : j; }
        else { out = false; idx = kk == 1u ? j + 1u : j; }
    }
    if (out) {
        float p = dm_tpos(s < 2u ? idx : a - idx, a, le);
        return s == 0u ? float2(p, 0.0f) : s == 1u ? float2(1.0f, p) : s == 2u ? float2(p, 1.0f) : float2(0.0f, p);
    }
    return s == 0u ? float2(dm_tpos(1u + idx, nu, lu), dm_tpos(1u, nv, lv)) : s == 1u ? float2(dm_tpos(nu - 1u, nu, lu), dm_tpos(1u + idx, nv, lv))
         : s == 2u ? float2(dm_tpos(nu - 1u - idx, nu, lu), dm_tpos(nv - 1u, nv, lv)) : float2(dm_tpos(1u, nu, lu), dm_tpos(nv - 1u - idx, nv, lv));
}
"""
CP_BUFFER, INFO_BUFFER = 14, 15                  # domain (vertex stage) buffer indices, beside nvn_mtl.m's 1-13 / 16-30
INDEX_BUFFER, CAPTURE_BUFFER = 24, 25            # hull (compute) buffer indices, beside SPIRV-Cross's 26-29

GL_MODES = {0x0004: 'triangles', 0x0007: 'quads', 0x8E7A: 'isolines'}
GL_SPACING = {0x0202: 'equal', 0x8E7B: 'fractional_odd', 0x8E7C: 'fractional_even'}
GL_ORDER = {0x0900: 'cw', 0x0901: 'ccw'}


def control_mode(ctl):
    """Domain control section -> {'mode', 'spacing', 'winding'} or None (not a domain control section)."""
    if len(ctl) < 0x748:
        return None
    mode, spacing = struct.unpack_from('<2I', ctl, 0x738)
    order = struct.unpack_from('<I', ctl, 0x744)[0]
    if mode not in GL_MODES or spacing not in GL_SPACING or order not in GL_ORDER:
        return None
    return {'mode': GL_MODES[mode], 'spacing': GL_SPACING[spacing], 'winding': GL_ORDER[order]}


def spirv_io(path):
    """{variable id: {'storage': 'in'|'out', 'location': L or None, 'patch': bool, 'builtin': B or None}}"""
    with open(path, 'rb') as f:
        data = f.read()
    w = struct.unpack('<%dI' % (len(data) // 4), data)
    deco, var = {}, {}
    i = 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xffff
        if not n or i + n > len(w):
            raise ValueError('tessellation: invalid SPIR-V')
        if op == 71 and n >= 3:           # OpDecorate target decoration [literal]
            d = deco.setdefault(w[i + 1], {})
            if w[i + 2] == 30 and n >= 4: d['location'] = w[i + 3]
            elif w[i + 2] == 15: d['patch'] = True
            elif w[i + 2] == 11 and n >= 4: d['builtin'] = w[i + 3]
        elif op == 59 and n >= 4:         # OpVariable type id storage
            if w[i + 3] in (1, 3):
                var[w[i + 2]] = 'in' if w[i + 3] == 1 else 'out'
        i += n
    return {v: {'storage': s, 'location': deco.get(v, {}).get('location'), 'patch': deco.get(v, {}).get('patch', False),
                'builtin': deco.get(v, {}).get('builtin')} for v, s in var.items()}


_MEMBER = re.compile(r'^\s*(float|int|uint)(\d?) (\w+)(?: \[\[([^\]]*)\]\])?;\s*$')


def _struct(src, name):
    m = re.search(r'struct ' + name + r'\s*\{(.*?)\};', src, re.S)
    if not m:
        return None, []
    return m, [ln for ln in m.group(1).splitlines() if ln.strip()]


def _location(io, member, storage, patch):
    """A struct member's location from its SPIR-V variable (SPIRV-Cross names unnamed variables m_<id>)."""
    m = re.fullmatch(r'm_(\d+)', member)
    if not m:
        raise ValueError(f'tessellation: member {member} is not a SPIR-V variable name')
    v = io.get(int(m.group(1)))
    if not v or v['storage'] != storage or v['location'] is None or v['patch'] != patch or not 0 <= v['location'] < 32:
        raise ValueError(f'tessellation: no {"patch " if patch else ""}{storage}put location for {member}')
    return v['location']


def _slot_struct(name, members, slots, position_slot):
    """members: [(type, n, member, slot)] -> a struct of float4-sized slots keeping the member names."""
    by = {}
    for ty, n, mem, slot in members:
        if n not in ('4',) or slot in by:
            raise ValueError(f'tessellation: {name}.{mem} is not one 4-component slot')
        by[slot] = (f'{ty}4', mem)
    lines = []
    for s in range(slots):
        ty, mem = by.get(s, ('float4', f'dm_p{s}'))
        lines.append(f'    {ty} {mem};')
    return f'struct {name}\n{{\n' + '\n'.join(lines) + '\n};'


def _words(ty, n, base):
    k = int(n or 1)
    ws = ', '.join(f'dm_r[{base + c}]' for c in range(k))
    vec = 'uint' + (str(k) if k > 1 else '')
    t = ty + (str(k) if k > 1 else '')
    return f'as_type<{t}>({vec}({ws}))' if ty != 'uint' else f'{vec}({ws})'


def hull(src, spv):
    """Rewrite a SPIRV-Cross tessellation-control kernel. -> (source, facts)"""
    io = spirv_io(spv)
    if not re.search(r'\bkernel void main0\(main0_in in \[\[stage_in\]\], ', src):
        raise ValueError('tessellation: hull kernel without a stage_in control point')
    out_cp = re.search(r'device main0_out\* gl_out = &spvOut\[gl_PrimitiveID \* (\d+)\];', src)
    if not out_cp:
        raise ValueError('tessellation: hull output control point count not found')
    # inputs: the capture record of each input control point
    m, lines = _struct(src, 'main0_in')
    if not m:
        raise ValueError('tessellation: hull without main0_in')
    loads, plain, in_bytes = [], [], 0
    for ln in lines:
        v = _MEMBER.match(ln)
        if not v or not v.group(4) or not v.group(4).startswith('attribute('):
            raise ValueError('tessellation: unknown hull input ' + ln.strip())
        ty, n, mem = v.group(1), v.group(2), v.group(3)
        if mem == 'gl_Position':
            base = 0
        else:
            loc = _location(io, mem, 'in', False)
            if int(v.group(4)[10:-1]) != loc:
                raise ValueError(f'tessellation: hull input {mem} attribute is not its location')
            base = 16 + 4 * loc
        loads.append(f'in.{mem} = {_words(ty, n, base)};')
        plain.append(f'    {ty}{n} {mem};')
        in_bytes += 16
    src = src[:m.start()] + 'struct main0_in\n{\n' + '\n'.join(plain) + '\n};\n\n' + (
           '// Maxwell reads an input control point past the patch (the translator\'s index 0x80000000 for one) as zero\n'
           'static inline main0_in dm_gin(threadgroup main0_in* p, int i, uint n) { return uint(i) < n ? p[i] : main0_in{}; }') + src[m.end():]
    # outputs: location slots
    for name, slots, patch in (('main0_out', CP_SLOTS, False), ('main0_patchOut', PATCH_SLOTS, True)):
        m, lines = _struct(src, name)
        if not m:
            if name == 'main0_out':
                raise ValueError('tessellation: hull without main0_out')
            continue
        members = []
        for ln in lines:
            v = _MEMBER.match(ln)
            if not v or v.group(4):
                raise ValueError(f'tessellation: unknown hull output {ln.strip()}')
            ty, n, mem = v.group(1), v.group(2) or '1', v.group(3)
            slot = CP_SLOTS - 1 if mem == 'gl_Position' and not patch else _location(io, mem, 'out', patch)
            members.append((ty, n, mem, slot))
        src = src[:m.start()] + _slot_struct(name, members, slots, CP_SLOTS - 1) + src[m.end():]
    src = src.replace('kernel void main0(main0_in in [[stage_in]], ', 'kernel void main0(', 1)
    src = re.sub(r'(threadgroup main0_in\* gl_in \[\[threadgroup\(0\)\]\])\)',
                 rf'\1, device const uint* dm_tidx [[buffer({INDEX_BUFFER})]], device const uint* dm_tcap [[buffer({CAPTURE_BUFFER})]])', src, count=1)
    if 'dm_tcap' not in src:
        raise ValueError('tessellation: hull kernel parameters not recognized')
    body = re.search(r'kernel void main0\([^{]*\)\s*\{\n', src)
    if not body:
        raise ValueError('tessellation: hull kernel body not found')
    pro = ('    main0_in in = {};\n'
           '    if (gl_InvocationID < spvIndirectParams[0])\n    {\n'
           '        device const uint* dm_r = dm_tcap + ulong(dm_tidx[gl_PrimitiveID * spvIndirectParams[0] + gl_InvocationID]) * '
           f'{CAPTURE_WORDS}u;\n' + ''.join(f'        {x}\n' for x in loads) + '    }\n')
    src = src[:body.end()] + pro + src[body.end():]
    src, n_reads = re.subn(r'\bgl_in\[(_\d+)\]', r'dm_gin(gl_in, \1, spvIndirectParams[0])', src)
    if 'gl_in[gl_InvocationID] = in;' not in src:
        raise ValueError('tessellation: hull without its control point store')
    return src, {'out_cp': int(out_cp.group(1)), 'in_bytes': in_bytes, 'cp_slots': CP_SLOTS, 'patch_slots': PATCH_SLOTS}


def domain(src, spv, mode):
    """Rewrite a SPIRV-Cross post-tessellation vertex function into a plain vertex function. -> (source, facts)"""
    if not mode or mode['mode'] != 'quads':
        raise ValueError(f'tessellation: domain mode {mode and mode["mode"]} is not implemented (quads only)')
    io = spirv_io(spv)
    entry = re.search(r'\[\[ patch\(\w+, \d+\) \]\] vertex main0_out main0\((.*?)\)\s*\{\n', src, re.S)
    if not entry:
        raise ValueError('tessellation: domain without a patch vertex entry')
    params = [p.strip() for p in entry.group(1).split(', ')]
    keep = []
    for p in params:
        if p == 'main0_patchIn patchIn [[stage_in]]' or p == 'float3 gl_TessCoord [[position_in_patch]]':
            continue
        if re.fullmatch(r'uint gl_PrimitiveID \[\[patch_id\]\]', p):
            continue
        if not p.startswith('constant spvDescriptorSetBuffer0& '):
            raise ValueError('tessellation: unknown domain parameter ' + p)
        keep.append(p)
    pid_param = any(re.fullmatch(r'uint gl_PrimitiveID \[\[patch_id\]\]', p) for p in params)
    keep += ['uint dm_vid [[vertex_id]]', f'device const float4* dm_tbuf [[buffer({CP_BUFFER})]]', f'constant uint4* dm_tinfo [[buffer({INFO_BUFFER})]]']
    # control point inputs
    m, lines = _struct(src, 'main0_in')
    cp_loads = []
    if m:
        plain = []
        for ln in lines:
            v = _MEMBER.match(ln)
            if not v or not v.group(4) or not v.group(4).startswith('attribute('):
                raise ValueError('tessellation: unknown domain input ' + ln.strip())
            ty, n, mem = v.group(1), v.group(2) or '1', v.group(3)
            if n != '4':
                raise ValueError('tessellation: domain input ' + mem + ' is not a 4-component slot')
            slot = CP_SLOTS - 1 if mem == 'gl_Position' else _location(io, mem, 'in', False)
            cp_loads.append((mem, ty, slot))
            plain.append(f'    {ty}4 {mem};')
        src = src[:m.start()] + 'struct main0_in\n{\n' + '\n'.join(plain) + '\n};' + src[m.end():]
    m, lines = _struct(src, 'main0_patchIn')
    if not m:
        raise ValueError('tessellation: domain without main0_patchIn')
    patch_loads, plain = [], []
    for ln in lines:
        if ln.strip() == 'patch_control_point<main0_in> gl_in;':
            plain.append('    dm_cps<main0_in> gl_in;')
            continue
        v = _MEMBER.match(ln)
        if not v or not v.group(4) or not v.group(4).startswith('attribute('):
            raise ValueError('tessellation: unknown domain patch input ' + ln.strip())
        ty, n, mem = v.group(1), v.group(2) or '1', v.group(3)
        if n != '4':
            raise ValueError('tessellation: domain patch input ' + mem + ' is not a 4-component slot')
        patch_loads.append((mem, ty, _location(io, mem, 'in', True)))
        plain.append(f'    {ty}4 {mem};')
    # Maxwell reads an attribute of a control point past the patch (the translator's index 0x80000000 for one) as zero
    helper = ('template<typename T> struct dm_cps\n{\n    T e[32];\n    uint n;\n'
              '    T operator[](uint i) const thread { return i < n && i < 32u ? e[i] : T{}; }\n'
              '    uint size() const thread { return n; }\n};\n\n' + TESS_MSL + '\n')

    src = src[:m.start()] + helper + 'struct main0_patchIn\n{\n' + '\n'.join(plain) + '\n};' + src[m.end():]
    entry = re.search(r'\[\[ patch\(\w+, \d+\) \]\] vertex main0_out main0\((.*?)\)\s*\{\n', src, re.S)
    o = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    pos = re.search(r'float4 (\w+) \[\[position(?:, invariant)?\]\];', o.group(1)) if o else None
    if not pos:
        raise ValueError('tessellation: domain output without a position')
    if not entry:
        raise ValueError('tessellation: domain entry point not found')
    ld = lambda v, ty, at: f'as_type<{ty}4>({at})' if ty != 'float' else at
    # dm_tinfo[0] = {fixed grid N, control points, patch-output offset, cw}; dm_tinfo[1] = {factor offset, cell-list
    # offset, adaptive, lmax}. Adaptive: vertex v is corner v % 6 of the list entry v / 6 (patch << 12 | entry, see
    # TESS_MSL dm_tvert), written by the host's expansion kernel; fixed: N x N cells per patch in vertex_id order
    # (DM_NVN_TESS_LEVEL).
    pro = ['    uint dm_n = dm_tinfo[0].x, dm_ncp = dm_tinfo[0].y, dm_cells = dm_n * dm_n;',
           '    bool dm_adapt = dm_tinfo[1].z != 0u;',
           '    uint gl_PrimitiveID, dm_k, dm_e = 0u;',
           '    uint2 dm_cell = uint2(0u);',
           '    if (dm_adapt)',
           '    {',
           '        dm_e = ((device const uint*)((device const uchar*)dm_tbuf + dm_tinfo[1].y))[dm_vid / 6u];',
           '        gl_PrimitiveID = dm_e >> 12; dm_k = dm_vid % 6u;',
           '    }',
           '    else',
           '    {',
           '        gl_PrimitiveID = dm_vid / (dm_cells * 6u); uint dm_r = dm_vid % (dm_cells * 6u), dm_ci = dm_r / 6u;',
           '        dm_k = dm_r % 6u; dm_cell = uint2(dm_ci % dm_n, dm_ci / dm_n);',
           '    }',
           '    const uint2 dm_ccw[6] = { uint2(0, 0), uint2(1, 0), uint2(1, 1), uint2(0, 0), uint2(1, 1), uint2(0, 1) };',
           '    const uint2 dm_cw[6] = { uint2(0, 0), uint2(1, 1), uint2(1, 0), uint2(0, 0), uint2(0, 1), uint2(1, 1) };',
           '    uint2 dm_c = (dm_tinfo[0].w ? dm_cw[dm_k] : dm_ccw[dm_k]) + dm_cell;',
           '    device const half* dm_lev = (device const half*)((device const uchar*)dm_tbuf + dm_tinfo[1].x) + ulong(gl_PrimitiveID) * 6u;',
           '    bool dm_cull = dm_tcull(dm_lev);',
           '    float3 gl_TessCoord = float3(dm_adapt ? dm_tvert(dm_e & 4095u, dm_k, dm_lev, float(dm_tinfo[1].w), dm_tinfo[0].w != 0u) : float2(dm_c) / float(dm_n), 0.0);',
           '    device const float4* dm_cp = dm_tbuf + ulong(gl_PrimitiveID) * dm_ncp * ' + str(CP_SLOTS) + 'u;',
           '    device const float4* dm_pp = dm_tbuf + dm_tinfo[0].z / 16u + ulong(gl_PrimitiveID) * ' + str(PATCH_SLOTS) + 'u;',
           '    main0_patchIn patchIn;',
           '    patchIn.gl_in.n = dm_ncp;',
           '    for (uint dm_i = 0; dm_i < dm_ncp && dm_i < 32u; dm_i++)',
           '    {']
    pro += [f'        patchIn.gl_in.e[dm_i].{mem} = {ld(mem, ty, f"dm_cp[dm_i * {CP_SLOTS}u + {slot}u]")};' for mem, ty, slot in cp_loads]
    pro += ['    }']
    pro += [f'    patchIn.{mem} = {ld(mem, ty, f"dm_pp[{slot}u]")};' for mem, ty, slot in patch_loads]
    body_start = entry.end()
    end, depth = body_start, 1
    while depth and end < len(src):
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    body = src[body_start:end - 1]
    if 'return out;' not in body:
        raise ValueError('tessellation: domain without return')
    body = body.replace('return out;', f'{{ if (dm_cull) out.{pos.group(1)} = float4(2.0, 0.0, 0.0, 1.0); return out; }}')
    head = 'vertex main0_out main0(' + ', '.join(keep) + ')\n{\n'
    src = src[:entry.start()] + head + '\n'.join(pro) + '\n' + body + src[end - 1:]
    if pid_param and 'gl_PrimitiveID' not in body:
        pass
    return src, {'mode': mode['mode'], 'spacing': mode['spacing'], 'winding': mode['winding'], 'cp_slots': CP_SLOTS, 'patch_slots': PATCH_SLOTS}
