"""Tessellation without a Metal tessellator (Metal 4 render encoders have no tessellation stage).

The host replays a patch draw as: the vertex shader's capture entry (dmsh_geometry.capture, 144-word records) ->
the hull shader as an ordinary compute kernel -> an expansion kernel that lists each patch's triangles -> the domain
shader as an ordinary vertex function of one indirect draw -> the game's fragment shader (nvn_tess.inc).

SPIRV-Cross writes hull shaders as kernels reading their control points through [[stage_in]] and domain shaders as
post-tessellation vertex functions. Both are rewritten here, from their MSL and the SPIR-V decorations:

* hull: main0_in comes from the capture records (location L = words 16 + 4L, position = words 0..3) through a
  control-point index buffer; the per-control-point output struct and the per-patch output struct are laid out in
  location slots (CP_SLOTS float4 per control point, position in the last; PATCH_SLOTS per patch), so a domain shader
  built on its own finds them by location. Tessellation factors stay in SPIRV-Cross's MTLQuadTessellationFactorsHalf,
  for either domain.
* domain: [[patch]] / [[stage_in]] / [[position_in_patch]] are replaced by reads of those slots, and gl_TessCoord by
  the point the expansion kernel's list names (TESS_MSL dm_tvert: each patch subdivided by its own hull factors).

The tessellation mode is the domain shader's own control section (+0x738 primitive mode, +0x73c spacing, +0x744
vertex order, as OpenGL enums): the translator answers a fixed default (triangles, equal spacing), so the MSL's
[[patch(triangle)]] is not the game's. Quad and triangle domains are rewritten, in all three spacings; isolines raise
ValueError and the shader is left as it was (its draws stay skipped). No game IDs or shader keys participate.
"""
import re
import struct

CP_SLOTS = 33       # float4 per output control point: locations 0..31, position last
PATCH_SLOTS = 32    # float4 per patch: patch locations 0..31
CAPTURE_WORDS = 144
# The subdivision, shared with the host's expansion kernel (nvn_tess.inc k_tess_msl: keep the two texts identical; test_dmsh_tess.py
# checks it). md, the domain's mode: bit 0 a triangle domain (else quads), bits 1-2 the spacing (0 equal, 1 fractional_odd,
# 2 fractional_even), bit 3 clockwise. L = the hull's six factors as SPIRV-Cross writes them for either domain
# (MTLQuadTessellationFactorsHalf): quads: edges 0 (u=0), 1 (v=0), 2 (u=1), 3 (v=1), inside 0 (u), 1 (v); triangles: edges 0 (u=0),
# 1 (v=0), 2 (w=0), inside L[4].
# A level is clamped as its spacing does (equal [1, lmax], fractional_odd [1, lmax - 1], fractional_even [2, lmax]; NaN -> the
# minimum) and rounded up to n segments (any, odd, even). equal: n equal segments; fractional: n - 2 segments of 1/l and two equal
# shorter ones, symmetric about the middle (OpenGL leaves the placement to the implementation; symmetric, so a shared edge
# evaluated from either side gives the same values): beside the middle point (even) or beside the middle segment (odd). An inside
# level of one is taken as 1 + epsilon (two or three segments, placed as for that level: the short segments of 1 + epsilon would be
# empty and fold), unless every level is one: then the patch is not subdivided (one entry: two triangles of a quad, or the triangle).
# Layout as a hardware tessellator's (a single grid snapped to the edge points folds triangles where an inside level is below an
# edge level): an inner part on the inside levels, and an outer ring of strips, one per edge, each stitching that edge's own
# points (its own level, corner to corner) to the inner part's border; the strips run counter-clockwise in (u, v) and share the
# corner diagonals.
#   quads: the inner part is a grid of (nu-2) x (nv-2) cells on the inside levels' interior lines (positions 1 .. n-1).
#   triangles: the inner part is OpenGL's concentric rings: ring r (1 .. n/2) has its corner at the corner's barycentric weight
#   1 - 4p/3 (p = the inside level's position r: where the perpendiculars to the two edges through their points r meet), its edges
#   cut where the perpendiculars through the points r .. n - r meet them; strips stitch ring r to ring r + 1, and an odd n leaves
#   one triangle in the middle, an even n one point (exactly 1/3, 1/3, 1/3).
# A strip of a outer and b inner segments has a + b triangles in a zipper order fixed by the indices alone (step i of the outer
# chain before step j of the inner one when (2i+1)b <= (2j+1)a). Every triangle lies between two parallel monotone chains or inside
# the grid, so nothing folds or overlaps, and neighbours sharing an edge generate exactly the same edge vertices (no cracks). A
# vertex two strips share is computed by one expression. Entries: two triangles each (6 vertices; a missing second triangle
# collapses to a point), at most 4096 a patch (quads at level 64: 3844 cells and 252 strip entries), numbered 0 .. dm_tcount - 1:
# quads: the inner cells, then the strips; triangles: the outer strips, the strips of rings 1 .. n/2 - 1, then the middle triangle.
TESS_MSL = """static inline uint dm_tsp(uint md) { return (md >> 1) & 3u; }
static inline float dm_tl(half h, float lmax, uint sp) { float l = float(h), lo = sp == 2u ? 2.0f : 1.0f, hi = sp == 1u ? lmax - 1.0f : lmax; return l >= lo ? min(l, hi) : lo; }
static inline uint dm_tn(float l, uint sp) { return sp == 0u ? uint(ceil(l)) : sp == 1u ? uint(ceil((l - 1.0f) * 0.5f)) * 2u + 1u : uint(ceil(l * 0.5f)) * 2u; }
static inline uint dm_tni(float l, uint sp) { uint n = dm_tn(l, sp); return n != 1u ? n : sp == 1u ? 3u : 2u; }
static inline float dm_tli(float l, uint sp) { return dm_tn(l, sp) != 1u ? l : float(dm_tni(l, sp)); }
static inline float dm_tpos(uint k, uint n, float l, uint sp) {
    uint h = n / 2u; bool hi = 2u * k > n; uint kk = hi ? n - k : k; float p;
    if (kk == 0u) p = 0.0f;
    else if (sp == 0u) p = float(kk) / float(n);
    else if (sp == 2u) p = kk == h ? 0.5f : float(kk) / l;
    else p = kk < h ? float(kk) / l : float(h - 1u) / l + (1.0f - float(n - 2u) / l) * 0.5f;
    return hi ? 1.0f - p : p;
}
static inline bool dm_ttri(uint md) { return (md & 1u) != 0u; }
static inline bool dm_tcull(device const half* L, uint md) { return !(float(L[0]) > 0.0f && float(L[1]) > 0.0f && float(L[2]) > 0.0f && (dm_ttri(md) || float(L[3]) > 0.0f)); }
static inline bool dm_tones(device const half* L, float lmax, uint md) {
    uint sp = dm_tsp(md), m = dm_ttri(md) ? 0x17u : 0x3fu;
    for (uint i = 0u; i < 6u; i++) if ((m >> i & 1u) != 0u && dm_tn(dm_tl(L[i], lmax, sp), sp) != 1u) return false;
    return true;
}
static inline uint dm_tedge(device const half* L, uint s, float lmax, uint sp, thread float& le) {
    le = dm_tl(L[s], lmax, sp); return dm_tn(le, sp);
}
static inline uint dm_tcount(device const half* L, float lmax, uint md) {
    uint sp = dm_tsp(md); float le;
    if (dm_tones(L, lmax, md)) return 1u;
    if (dm_ttri(md)) {
        uint n = dm_tni(dm_tl(L[4], lmax, sp), sp), c = n & 1u;
        for (uint s = 0u; s < 3u; s++) c += (dm_tedge(L, (s + 1u) % 3u, lmax, sp, le) + n - 1u) / 2u;
        for (uint r = 1u; 2u * r + 2u <= n; r++) c += 3u * (n - 2u * r - 1u);
        return c;
    }
    uint nu = dm_tni(dm_tl(L[4], lmax, sp), sp), nv = dm_tni(dm_tl(L[5], lmax, sp), sp), c = (nu - 2u) * (nv - 2u);
    for (uint s = 0u; s < 4u; s++) c += (dm_tedge(L, (s + 1u) & 3u, lmax, sp, le) + ((s & 1u) != 0u ? nv : nu) - 1u) / 2u;
    return c;
}
static inline uint dm_tsize(device const half* L, float lmax, uint md) { uint sp = dm_tsp(md), m = 1u; for (uint i = 0u; i < 6u; i++) if (!dm_ttri(md) || (i != 3u && i != 5u)) m = max(m, dm_tn(dm_tl(L[i], lmax, sp), sp)); return m; }
static inline uint dm_tzpos(uint i, uint a, uint b) { uint n = (2u * i + 1u) * b; return i + (n <= a ? 0u : min(b, (n + a - 1u) / (2u * a))); }
// Corner kk of triangle t of a strip of a outer and b inner segments: out (a point of the outer chain) and its index
static inline void dm_tzip(uint t, uint kk, uint a, uint b, thread bool& out, thread uint& idx) {
    if (t >= a + b) { out = true; idx = 0u; return; }
    uint lo = 0u, hi = a;
    while (lo < hi) { uint m = (lo + hi) / 2u; if (dm_tzpos(m, a, b) < t) lo = m + 1u; else hi = m; }
    uint i = lo, j = t - i; bool stepA = i < a && dm_tzpos(i, a, b) == t;
    if (kk == 0u) { out = true; idx = i; }
    else if (stepA) { out = kk == 1u; idx = kk == 1u ? i + 1u : j; }
    else { out = false; idx = kk == 1u ? j + 1u : j; }
}
// A triangle domain's point on the side of corner x towards corner y (z the third) of ring r, at the inside level's point k
// (r <= k <= n - r): weights (x, y, z) = (1 - p_k - p_r / 3, p_k - p_r / 3, 2 p_r / 3); a ring's corners by one expression
static inline float3 dm_tring(uint x, uint r, uint k, uint n, float l, uint sp) {
    float3 w;
    if (2u * r == n) return float3(1.0f / 3.0f);
    float pr = dm_tpos(r, n, l, sp), q = 2.0f * pr / 3.0f;
    if (k == r || k == n - r) { uint c = k == r ? x : (x + 1u) % 3u; w = float3(q); w[c] = 1.0f - 2.0f * q; return w; }
    float pk = dm_tpos(k, n, l, sp);
    w[x] = 1.0f - pk - pr / 3.0f; w[(x + 1u) % 3u] = pk - pr / 3.0f; w[(x + 2u) % 3u] = q;
    return w;
}
static inline float3 dm_tvert(uint e, uint k, device const half* L, float lmax, uint md) {
    uint sp = dm_tsp(md), kk = k % 3u;
    if ((md & 8u) != 0u && kk != 0u) kk = 3u - kk;
    if (dm_tones(L, lmax, md)) {
        if (dm_ttri(md)) { uint c = k < 3u ? (kk + 2u) % 3u : 2u; float3 w = float3(0.0f); w[c] = 1.0f; return w; }
        uint2 d = k < 3u ? (kk == 0u ? uint2(0u, 0u) : kk == 1u ? uint2(1u, 0u) : uint2(1u, 1u)) : (kk == 0u ? uint2(0u, 0u) : kk == 1u ? uint2(1u, 1u) : uint2(0u, 1u));
        return float3(float2(d), 0.0f);
    }
    bool out; uint idx;
    if (dm_ttri(md)) {
        // corners in (u, v): u (1, 0), v (0, 1), w (0, 0); counter-clockwise w -> u -> v. Side s runs from corner x = (s + 2) % 3 to (s + 3) % 3:
        // s 0: w -> u (v = 0, L[1]), 1: u -> v (w = 0, L[2]), 2: v -> w (u = 0, L[0])
        float li = dm_tl(L[4], lmax, sp), le; uint n = dm_tni(li, sp);
        li = dm_tli(li, sp); uint r = e, s = 0u, ring = 0u, a = 0u, b = 0u;
        for (; s < 3u; s++) { a = dm_tedge(L, (s + 1u) % 3u, lmax, sp, le); b = n - 2u; uint c = (a + b + 1u) / 2u; if (r < c) break; r -= c; }
        if (s == 3u) {
            for (ring = 1u; 2u * ring + 2u <= n; ring++) { uint c = n - 2u * ring - 1u; if (r < 3u * c) { s = r / c; r -= s * c; a = n - 2u * ring; b = a - 2u; break; } r -= 3u * c; }
            if (2u * ring + 2u > n) {   // the middle triangle (odd n): ring (n - 1) / 2's corners
                if (k >= 3u) return dm_tring(2u, ring, ring, n, li, sp);
                return dm_tring((kk + 2u) % 3u, ring, ring, n, li, sp);
            }
        }
        uint x = (s + 2u) % 3u;
        dm_tzip(2u * r + (k < 3u ? 0u : 1u), kk, a, b, out, idx);
        if (out && ring == 0u) { float t = dm_tpos(idx, a, le, sp); float3 w = float3(0.0f); w[x] = 1.0f - t; w[(x + 1u) % 3u] = t; return w; }
        return out ? dm_tring(x, ring, ring + idx, n, li, sp) : dm_tring(x, ring + 1u, ring + 1u + idx, n, li, sp);
    }
    float lu = dm_tl(L[4], lmax, sp), lv = dm_tl(L[5], lmax, sp);
    uint nu = dm_tni(lu, sp), nv = dm_tni(lv, sp), ic = (nu - 2u) * (nv - 2u);
    lu = dm_tli(lu, sp); lv = dm_tli(lv, sp);
    if (e < ic) {
        uint ci = e % (nu - 2u), cj = e / (nu - 2u);
        uint2 d = k < 3u ? (kk == 0u ? uint2(0u, 0u) : kk == 1u ? uint2(1u, 0u) : uint2(1u, 1u)) : (kk == 0u ? uint2(0u, 0u) : kk == 1u ? uint2(1u, 1u) : uint2(0u, 1u));
        return float3(dm_tpos(1u + ci + d.x, nu, lu, sp), dm_tpos(1u + cj + d.y, nv, lv, sp), 0.0f);
    }
    uint r = e - ic, s = 0u, a = 2u, n = 2u; float le = 2.0f;
    for (; s < 4u; s++) {
        a = dm_tedge(L, (s + 1u) & 3u, lmax, sp, le); n = (s & 1u) != 0u ? nv : nu;
        uint c = (a + n - 1u) / 2u;
        if (r < c) break;
        r -= c;
    }
    s = min(s, 3u);
    dm_tzip(2u * r + (k < 3u ? 0u : 1u), kk, a, n - 2u, out, idx);
    if (out) {
        float p = dm_tpos(s < 2u ? idx : a - idx, a, le, sp);
        return s == 0u ? float3(p, 0.0f, 0.0f) : s == 1u ? float3(1.0f, p, 0.0f) : s == 2u ? float3(p, 1.0f, 0.0f) : float3(0.0f, p, 0.0f);
    }
    return s == 0u ? float3(dm_tpos(1u + idx, nu, lu, sp), dm_tpos(1u, nv, lv, sp), 0.0f) : s == 1u ? float3(dm_tpos(nu - 1u, nu, lu, sp), dm_tpos(1u + idx, nv, lv, sp), 0.0f)
         : s == 2u ? float3(dm_tpos(nu - 1u - idx, nu, lu, sp), dm_tpos(nv - 1u, nv, lv, sp), 0.0f) : float3(dm_tpos(1u, nu, lu, sp), dm_tpos(nv - 1u - idx, nv, lv, sp), 0.0f);
}
"""
CP_BUFFER, INFO_BUFFER = 14, 15                  # domain (vertex stage) buffer indices, beside nvn_mtl.m's 1-13 / 16-30
INDEX_BUFFER, CAPTURE_BUFFER = 24, 25            # hull (compute) buffer indices, beside SPIRV-Cross's 26-29

GL_MODES = {0x0004: 'triangles', 0x0007: 'quads', 0x8E7A: 'isolines'}
GL_SPACING = {0x0202: 'equal', 0x8E7B: 'fractional_odd', 0x8E7C: 'fractional_even'}
GL_ORDER = {0x0900: 'cw', 0x0901: 'ccw'}


def mode_bits(mode):
    """{'mode', 'spacing', 'winding'} -> TESS_MSL's md: bit 0 triangles, bits 1-2 the spacing (0 equal, 1 fractional_odd, 2 fractional_even), bit 3 clockwise."""
    return (mode['mode'] == 'triangles') | {'equal': 0, 'fractional_odd': 1, 'fractional_even': 2}[mode['spacing']] << 1 | (mode['winding'] == 'cw') << 3


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
    if not mode or mode['mode'] not in ('quads', 'triangles'):
        raise ValueError(f'tessellation: domain mode {mode and mode["mode"]} is not implemented (quads and triangles)')
    md = mode_bits(mode)
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
    # dm_tinfo[0] = {0, control points, patch-output offset, 0}; dm_tinfo[1] = {factor offset, cell-list offset, 0, lmax}. Vertex v is
    # corner v % 6 of the list entry v / 6 (patch << 12 | entry, see TESS_MSL dm_tvert), written by the host's expansion kernel, which
    # lists no entry for a culled patch. The mode is this shader's own (md), compiled in.
    pro = ['    uint dm_ncp = dm_tinfo[0].y;',
           '    uint dm_e = ((device const uint*)((device const uchar*)dm_tbuf + dm_tinfo[1].y))[dm_vid / 6u];',
           '    uint gl_PrimitiveID = dm_e >> 12, dm_k = dm_vid % 6u;',
           '    device const half* dm_lev = (device const half*)((device const uchar*)dm_tbuf + dm_tinfo[1].x) + ulong(gl_PrimitiveID) * 6u;',
           f'    float3 gl_TessCoord = dm_tvert(dm_e & 4095u, dm_k, dm_lev, float(dm_tinfo[1].w), {md}u);',
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
    head = 'vertex main0_out main0(' + ', '.join(keep) + ')\n{\n'
    src = src[:entry.start()] + head + '\n'.join(pro) + '\n' + body + src[end - 1:]
    return src, {'mode': mode['mode'], 'spacing': mode['spacing'], 'winding': mode['winding'], 'md': md, 'cp_slots': CP_SLOTS, 'patch_slots': PATCH_SLOTS}
