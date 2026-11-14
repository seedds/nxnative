// Tessellated draws (NVN primitive type 14, patches) without a Metal tessellator: Metal 4 render encoders have no
// tessellation stage. Included after nvn_geometry.inc, whose vertex capture this reuses:
//   1. the vertex shader's capture entry writes each control point as a 144-word record (an attachment-less pass);
//   2. the hull shader runs as an ordinary kernel (dmsh_tess.py hull): one threadgroup per patch, its control points
//      read from the records through a control-point index list, its outputs in location slots, its factors in
//      SPIRV-Cross's MTLQuadTessellationFactorsHalf (for either domain);
//   3. an expansion kernel (k_tess_msl, after the hull, same compute encoder) reads each patch's factors, skips culled
//      patches (an outer factor <= 0 or NaN), counts the patch's triangles in the domain's mode (quads or triangles,
//      equal / fractional_odd / fractional_even, levels up to DM_NVN_TESS_MAX, default 64: an inner part on the inside
//      levels and an outer ring stitched to each edge's own points, as a hardware tessellator) and appends one entry per
//      two triangles to a list whose count feeds an indirect draw;
//   4. the domain shader is the vertex function of that draw (dmsh_tess.py domain), with the game's own fragment shader
//      and render state: it evaluates each vertex at the point the list names (dmsh_tess.TESS_MSL dm_tvert: edges
//      subdivided by their own factor, so shared edges match; no folded or overlapping triangles).
//   DM_NVN_TESS_LEVEL=N gives every patch level N instead of its hull's (A/B, diagnostics).
//   The list holds at most DM_NVN_TESS_BUDGET entries per draw (default 2M); patches past it are dropped and counted.
// Instances: each instance's control points are captured as their own records and its patches numbered after the
// previous instance's (the hull and domain shaders of this library use gl_PrimitiveID only to address their outputs).
// An indirect draw's arguments are read on the CPU once the GPU work that writes them is done (geo_indirect_args), as
// the geometry path reads its draws: the control-point indices are read on the CPU too.
// A geometry shader after the domain takes the domain's triangles through the geometry path, their count written on the GPU (nvn_geometry.inc geo_prep_gpu).
// Draws this cannot replay (no translated hull / domain, an isoline domain) stay skipped with a [RED] line.
static id<MTL4ComputeCommandEncoder> dispatch_enc(void);
enum { TESS_CP_SLOTS = 33, TESS_PATCH_SLOTS = 32 };   // dmsh_tess.py CP_SLOTS / PATCH_SLOTS
static uint64_t g_tess_buf, g_tess_info;
static uint32_t g_tess_count;   // the draw's vertex bound (6 per list entry); the count drawn is the expansion kernel's
static uint64_t g_tess_draws, g_tess_patches, g_tess_refused;
static uint64_t g_tess_ind;   // the draw's MTLDrawPrimitivesIndirectArguments (vertex count written by the expansion kernel)
// The expansion kernel. The dm_t* functions are dmsh_tess.TESS_MSL verbatim (the domain shaders evaluate the same layout; generated text, keep in sync).
static const char *k_tess_msl = "#include <metal_stdlib>\n"
"using namespace metal;\n"
"static inline uint dm_tsp(uint md) { return (md >> 1) & 3u; }\n"
"static inline float dm_tl(half h, float lmax, uint sp) { float l = float(h), lo = sp == 2u ? 2.0f : 1.0f, hi = sp == 1u ? lmax - 1.0f : lmax; return l >= lo ? min(l, hi) : lo; }\n"
"static inline uint dm_tn(float l, uint sp) { return sp == 0u ? uint(ceil(l)) : sp == 1u ? uint(ceil((l - 1.0f) * 0.5f)) * 2u + 1u : uint(ceil(l * 0.5f)) * 2u; }\n"
"static inline uint dm_tni(float l, uint sp) { uint n = dm_tn(l, sp); return n != 1u ? n : sp == 1u ? 3u : 2u; }\n"
"static inline float dm_tli(float l, uint sp) { return dm_tn(l, sp) != 1u ? l : float(dm_tni(l, sp)); }\n"
"static inline float dm_tpos(uint k, uint n, float l, uint sp) {\n"
"    uint h = n / 2u; bool hi = 2u * k > n; uint kk = hi ? n - k : k; float p;\n"
"    if (kk == 0u) p = 0.0f;\n"
"    else if (sp == 0u) p = float(kk) / float(n);\n"
"    else if (sp == 2u) p = kk == h ? 0.5f : float(kk) / l;\n"
"    else p = kk < h ? float(kk) / l : float(h - 1u) / l + (1.0f - float(n - 2u) / l) * 0.5f;\n"
"    return hi ? 1.0f - p : p;\n"
"}\n"
"static inline bool dm_ttri(uint md) { return (md & 1u) != 0u; }\n"
"static inline bool dm_tcull(device const half* L, uint md) { return !(float(L[0]) > 0.0f && float(L[1]) > 0.0f && float(L[2]) > 0.0f && (dm_ttri(md) || float(L[3]) > 0.0f)); }\n"
"static inline bool dm_tones(device const half* L, float lmax, uint md) {\n"
"    uint sp = dm_tsp(md), m = dm_ttri(md) ? 0x17u : 0x3fu;\n"
"    for (uint i = 0u; i < 6u; i++) if ((m >> i & 1u) != 0u && dm_tn(dm_tl(L[i], lmax, sp), sp) != 1u) return false;\n"
"    return true;\n"
"}\n"
"static inline uint dm_tedge(device const half* L, uint s, float lmax, uint sp, thread float& le) {\n"
"    le = dm_tl(L[s], lmax, sp); return dm_tn(le, sp);\n"
"}\n"
"static inline uint dm_tcount(device const half* L, float lmax, uint md) {\n"
"    uint sp = dm_tsp(md); float le;\n"
"    if (dm_tones(L, lmax, md)) return 1u;\n"
"    if (dm_ttri(md)) {\n"
"        uint n = dm_tni(dm_tl(L[4], lmax, sp), sp), c = n & 1u;\n"
"        for (uint s = 0u; s < 3u; s++) c += (dm_tedge(L, (s + 1u) % 3u, lmax, sp, le) + n - 1u) / 2u;\n"
"        for (uint r = 1u; 2u * r + 2u <= n; r++) c += 3u * (n - 2u * r - 1u);\n"
"        return c;\n"
"    }\n"
"    uint nu = dm_tni(dm_tl(L[4], lmax, sp), sp), nv = dm_tni(dm_tl(L[5], lmax, sp), sp), c = (nu - 2u) * (nv - 2u);\n"
"    for (uint s = 0u; s < 4u; s++) c += (dm_tedge(L, (s + 1u) & 3u, lmax, sp, le) + ((s & 1u) != 0u ? nv : nu) - 1u) / 2u;\n"
"    return c;\n"
"}\n"
"static inline uint dm_tsize(device const half* L, float lmax, uint md) { uint sp = dm_tsp(md), m = 1u; for (uint i = 0u; i < 6u; i++) if (!dm_ttri(md) || (i != 3u && i != 5u)) m = max(m, dm_tn(dm_tl(L[i], lmax, sp), sp)); return m; }\n"
"static inline uint dm_tzpos(uint i, uint a, uint b) { uint n = (2u * i + 1u) * b; return i + (n <= a ? 0u : min(b, (n + a - 1u) / (2u * a))); }\n"
"// Corner kk of triangle t of a strip of a outer and b inner segments: out (a point of the outer chain) and its index\n"
"static inline void dm_tzip(uint t, uint kk, uint a, uint b, thread bool& out, thread uint& idx) {\n"
"    if (t >= a + b) { out = true; idx = 0u; return; }\n"
"    uint lo = 0u, hi = a;\n"
"    while (lo < hi) { uint m = (lo + hi) / 2u; if (dm_tzpos(m, a, b) < t) lo = m + 1u; else hi = m; }\n"
"    uint i = lo, j = t - i; bool stepA = i < a && dm_tzpos(i, a, b) == t;\n"
"    if (kk == 0u) { out = true; idx = i; }\n"
"    else if (stepA) { out = kk == 1u; idx = kk == 1u ? i + 1u : j; }\n"
"    else { out = false; idx = kk == 1u ? j + 1u : j; }\n"
"}\n"
"// A triangle domain's point on the side of corner x towards corner y (z the third) of ring r, at the inside level's point k\n"
"// (r <= k <= n - r): weights (x, y, z) = (1 - p_k - p_r / 3, p_k - p_r / 3, 2 p_r / 3); a ring's corners by one expression\n"
"static inline float3 dm_tring(uint x, uint r, uint k, uint n, float l, uint sp) {\n"
"    float3 w;\n"
"    if (2u * r == n) return float3(1.0f / 3.0f);\n"
"    float pr = dm_tpos(r, n, l, sp), q = 2.0f * pr / 3.0f;\n"
"    if (k == r || k == n - r) { uint c = k == r ? x : (x + 1u) % 3u; w = float3(q); w[c] = 1.0f - 2.0f * q; return w; }\n"
"    float pk = dm_tpos(k, n, l, sp);\n"
"    w[x] = 1.0f - pk - pr / 3.0f; w[(x + 1u) % 3u] = pk - pr / 3.0f; w[(x + 2u) % 3u] = q;\n"
"    return w;\n"
"}\n"
"static inline float3 dm_tvert(uint e, uint k, device const half* L, float lmax, uint md) {\n"
"    uint sp = dm_tsp(md), kk = k % 3u;\n"
"    if ((md & 8u) != 0u && kk != 0u) kk = 3u - kk;\n"
"    if (dm_tones(L, lmax, md)) {\n"
"        if (dm_ttri(md)) { uint c = k < 3u ? (kk + 2u) % 3u : 2u; float3 w = float3(0.0f); w[c] = 1.0f; return w; }\n"
"        uint2 d = k < 3u ? (kk == 0u ? uint2(0u, 0u) : kk == 1u ? uint2(1u, 0u) : uint2(1u, 1u)) : (kk == 0u ? uint2(0u, 0u) : kk == 1u ? uint2(1u, 1u) : uint2(0u, 1u));\n"
"        return float3(float2(d), 0.0f);\n"
"    }\n"
"    bool out; uint idx;\n"
"    if (dm_ttri(md)) {\n"
"        // corners in (u, v): u (1, 0), v (0, 1), w (0, 0); counter-clockwise w -> u -> v. Side s runs from corner x = (s + 2) % 3 to (s + 3) % 3:\n"
"        // s 0: w -> u (v = 0, L[1]), 1: u -> v (w = 0, L[2]), 2: v -> w (u = 0, L[0])\n"
"        float li = dm_tl(L[4], lmax, sp), le; uint n = dm_tni(li, sp);\n"
"        li = dm_tli(li, sp); uint r = e, s = 0u, ring = 0u, a = 0u, b = 0u;\n"
"        for (; s < 3u; s++) { a = dm_tedge(L, (s + 1u) % 3u, lmax, sp, le); b = n - 2u; uint c = (a + b + 1u) / 2u; if (r < c) break; r -= c; }\n"
"        if (s == 3u) {\n"
"            for (ring = 1u; 2u * ring + 2u <= n; ring++) { uint c = n - 2u * ring - 1u; if (r < 3u * c) { s = r / c; r -= s * c; a = n - 2u * ring; b = a - 2u; break; } r -= 3u * c; }\n"
"            if (2u * ring + 2u > n) {   // the middle triangle (odd n): ring (n - 1) / 2's corners\n"
"                if (k >= 3u) return dm_tring(2u, ring, ring, n, li, sp);\n"
"                return dm_tring((kk + 2u) % 3u, ring, ring, n, li, sp);\n"
"            }\n"
"        }\n"
"        uint x = (s + 2u) % 3u;\n"
"        dm_tzip(2u * r + (k < 3u ? 0u : 1u), kk, a, b, out, idx);\n"
"        if (out && ring == 0u) { float t = dm_tpos(idx, a, le, sp); float3 w = float3(0.0f); w[x] = 1.0f - t; w[(x + 1u) % 3u] = t; return w; }\n"
"        return out ? dm_tring(x, ring, ring + idx, n, li, sp) : dm_tring(x, ring + 1u, ring + 1u + idx, n, li, sp);\n"
"    }\n"
"    float lu = dm_tl(L[4], lmax, sp), lv = dm_tl(L[5], lmax, sp);\n"
"    uint nu = dm_tni(lu, sp), nv = dm_tni(lv, sp), ic = (nu - 2u) * (nv - 2u);\n"
"    lu = dm_tli(lu, sp); lv = dm_tli(lv, sp);\n"
"    if (e < ic) {\n"
"        uint ci = e % (nu - 2u), cj = e / (nu - 2u);\n"
"        uint2 d = k < 3u ? (kk == 0u ? uint2(0u, 0u) : kk == 1u ? uint2(1u, 0u) : uint2(1u, 1u)) : (kk == 0u ? uint2(0u, 0u) : kk == 1u ? uint2(1u, 1u) : uint2(0u, 1u));\n"
"        return float3(dm_tpos(1u + ci + d.x, nu, lu, sp), dm_tpos(1u + cj + d.y, nv, lv, sp), 0.0f);\n"
"    }\n"
"    uint r = e - ic, s = 0u, a = 2u, n = 2u; float le = 2.0f;\n"
"    for (; s < 4u; s++) {\n"
"        a = dm_tedge(L, (s + 1u) & 3u, lmax, sp, le); n = (s & 1u) != 0u ? nv : nu;\n"
"        uint c = (a + n - 1u) / 2u;\n"
"        if (r < c) break;\n"
"        r -= c;\n"
"    }\n"
"    s = min(s, 3u);\n"
"    dm_tzip(2u * r + (k < 3u ? 0u : 1u), kk, a, n - 2u, out, idx);\n"
"    if (out) {\n"
"        float p = dm_tpos(s < 2u ? idx : a - idx, a, le, sp);\n"
"        return s == 0u ? float3(p, 0.0f, 0.0f) : s == 1u ? float3(1.0f, p, 0.0f) : s == 2u ? float3(p, 1.0f, 0.0f) : float3(0.0f, p, 0.0f);\n"
"    }\n"
"    return s == 0u ? float3(dm_tpos(1u + idx, nu, lu, sp), dm_tpos(1u, nv, lv, sp), 0.0f) : s == 1u ? float3(dm_tpos(nu - 1u, nu, lu, sp), dm_tpos(1u + idx, nv, lv, sp), 0.0f)\n"
"         : s == 2u ? float3(dm_tpos(nu - 1u - idx, nu, lu, sp), dm_tpos(nv - 1u, nv, lv, sp), 0.0f) : float3(dm_tpos(1u, nu, lu, sp), dm_tpos(nv - 1u - idx, nv, lv, sp), 0.0f);\n"
"}\n"
"struct dm_targs { atomic_uint vcount; uint icount, vstart, binst; atomic_uint cells; uint cap, np, lmax, md, fix, ginv; };\n"
"kernel void dm_tess_expand(device half* lev [[buffer(0)]], device uint* list [[buffer(1)]], device dm_targs& a [[buffer(2)]],\n"
"    device atomic_uint* st [[buffer(3)]], uint p [[threadgroup_position_in_grid]], uint t [[thread_index_in_threadgroup]], uint tn [[threads_per_threadgroup]])\n"
"{\n"
"    threadgroup uint base, cnt;\n"
"    if (t == 0) {\n"
"        device half* L = lev + p * 6u; float lmax = float(a.lmax);\n"
"        uint c = 0, b = 0;\n"
"        if (dm_tcull(L, a.md)) atomic_fetch_add_explicit(&st[1], 1u, memory_order_relaxed);\n"
"        else {\n"
"            if (a.fix != 0u) for (uint i = 0u; i < 6u; i++) L[i] = half(float(a.fix));\n"
"            c = dm_tcount(L, lmax, a.md); b = atomic_fetch_add_explicit(&a.cells, c, memory_order_relaxed);\n"
"            if (b + c > a.cap) { atomic_fetch_add_explicit(&st[2], 1u, memory_order_relaxed); c = 0; }\n"
"            else { atomic_fetch_add_explicit(&a.vcount, c * 6u, memory_order_relaxed); atomic_fetch_add_explicit(&st[3], c, memory_order_relaxed);\n"
"                atomic_fetch_add_explicit(&st[4 + min(dm_tsize(L, lmax, a.md), 64u) / 2u], 1u, memory_order_relaxed); } }\n"
"        atomic_fetch_add_explicit(&st[0], 1u, memory_order_relaxed);\n"
"        base = b; cnt = c;\n"
"    }\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    for (uint x = t; x < cnt; x += tn) list[base + x] = (p << 12) | x;\n"
"}\n"
"// A geometry shader after the domain (nvn_geometry.inc geo_prep_gpu): its primitive count (the info block's GeometryCounts.x), the compaction's group count\n"
"// (the pack job's groups, word 6) and both dispatches' threadgroup counts, from the list's vertex count: 3 vertices a triangle\n"
"kernel void dm_tess_geo(device dm_targs& a [[buffer(0)]], device uint* info [[buffer(1)]], device uint* job [[buffer(2)]], device uint* disp [[buffer(3)]])\n"
"{\n"
"    uint np = atomic_load_explicit(&a.vcount, memory_order_relaxed) / 3u, groups = np * a.ginv;\n"
"    info[4] = np; job[6] = groups;\n"
"    disp[0] = (np + 31u) / 32u; disp[1] = 1u; disp[2] = a.ginv;\n"
"    disp[4] = (groups + 63u) / 64u; disp[5] = 1u; disp[6] = 1u;\n"
"}\n";
static int tess_max(void) {   // DM_NVN_TESS_MAX: highest level used (even, 2..64; default 64, the OpenGL / Maxwell maximum)
    static int n = 0; if (!n) { const char *e = getenv("DM_NVN_TESS_MAX"); n = e ? atoi(e) : 64; n = n < 2 ? 2 : n > 64 ? 64 : n & ~1; } return n;
}
static uint32_t tess_budget(void) {
    static uint32_t n = 0; if (!n) { const char *e = getenv("DM_NVN_TESS_BUDGET"); long v = e ? atol(e) : 2l << 20; n = (uint32_t)(v < 4096 ? 4096 : v > (16l << 20) ? (16l << 20) : v); } return n;
}
static id<MTLComputePipelineState> g_tess_kpso, g_tess_gpso; static id<MTLBuffer> g_tess_stats; static int g_tess_kfail;   // gpso: dm_tess_geo
static void tess_stats_log(void) {   // the expansion kernel's running totals (read without synchronisation: approximate, diagnostics only)
    if (!g_tess_stats) return;
    const uint32_t *st = (const uint32_t *)g_tess_stats.contents; char h[512]; int o = 0;
    for (int i = 0; i <= 32 && o < 480; i++) if (st[4 + i]) o += snprintf(h + o, sizeof h - o, " %d:%u", 2 * i, st[4 + i]);
    MLOG("tessellation: %u patches, %u culled, %u dropped over budget, %u entries of two triangles (%.1f per drawn patch); size histogram (largest segment count, rounded down to even: patches)%s",
        st[0], st[1], st[2], st[3], st[0] > st[1] + st[2] ? (double)st[3] / (st[0] - st[1] - st[2]) : 0.0, h);
}
static int tess_stages(void) {   // DM_NVN_TESS_STAGES=1 / 2: diagnostic, stop after the vertex capture / the hull kernel (the draw is then skipped)
    static int n = 0; if (!n) { const char *e = getenv("DM_NVN_TESS_STAGES"); n = e ? atoi(e) : 3; if (n < 1 || n > 3) n = 3; } return n;
}
static int tess_level(void) {   // 0: each patch's own levels (default); DM_NVN_TESS_LEVEL=N: level N for every edge and inside of every patch drawn
    static int n = -1;
    if (n < 0) { const char *e = getenv("DM_NVN_TESS_LEVEL"); n = e ? atoi(e) : 0; n = !e ? 0 : n < 1 ? 1 : n > 64 ? 64 : n; }
    return n;
}
static void tess_refuse(NvProg *pr, const p_draw *dw, const p_draw_idx *di, const char *why) {
    g_tess_refused++;
    static int n; if (n++ < 8) MLOG("[RED] tessellated draw not replayed (vertex %s hull %s domain %s fragment %s; patch size %d, count %d, instances %d, indexed %d): %s",
        pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[3] ? pr->sh[3]->key : "-", pr->sh[4] ? pr->sh[4]->key : "-", pr->sh[1] ? pr->sh[1]->key : "-",
        S.patch_size, dw ? dw->count : di->count, dw ? dw->instances : di->instances, di != NULL, why);
    PD("Z %llu draw skipped: tessellation not replayed (%s)", g_stat_pass + !g_enc, why);
}
static void tess_draw(NvProg *pr, const p_draw *dw0, const p_draw_idx *di0) {
    NvShader *vs = pr->sh[0], *hs = pr->sh[3], *ds = pr->sh[4];
    p_draw dwl = dw0 ? *dw0 : (p_draw){0}; p_draw_idx dil = di0 ? *di0 : (p_draw_idx){0};   // an indirect draw's arguments go into these copies
    const p_draw *dw = dw0 ? &dwl : NULL; const p_draw_idx *di = di0 ? &dil : NULL;
    if (!vs || !vs->geo_fn) { tess_refuse(pr, dw, di, "no vertex capture entry"); return; }
    if (!hs || !hs->tess_ok || !ds || !ds->tess_ok) { tess_refuse(pr, dw, di, "hull or domain not rewritten for replay (dmsh_tess.py)"); return; }
    static const char *only; static int only_read;   // DM_NVN_TESS_ONLY=<hull key>,...: diagnostic, replay only those programs (to tell their effects apart)
    if (!only_read) { only = getenv("DM_NVN_TESS_ONLY"); only_read = 1; }
    if (only && !strstr(only, [[NSString stringWithUTF8String:hs->key] substringToIndex:MIN((NSUInteger)16, strlen(hs->key))].UTF8String)) { tess_refuse(pr, dw, di, "not in DM_NVN_TESS_ONLY"); return; }
    if (di && (di->itype < 0 || di->itype > 2)) { tess_refuse(pr, dw, di, "index type"); return; }
    int drained = 0;
    if (dw ? dw->ind : di->ind) {   // the GPU may have written the arguments: read them once that work is done (a multi-draw's live draw was checked by draw_)
        int r = geo_indirect_args(dw ? &dwl : NULL, di ? &dil : NULL, &drained, "tessellated");
        if (r < 0) { tess_refuse(pr, dw, di, "indirect arguments"); return; }
        if (!r) return;
    }
    int count = dw ? dw->count : di->count, instances = dw ? dw->instances : di->instances, ps = S.patch_size;
    if (instances < 1) instances = 1;   // instances 0 counts as 1 (p_draw)
    if (ps < 1 || ps > 32 || count < ps) { tess_refuse(pr, dw, di, "patch size"); return; }
    uint32_t np1 = (uint32_t)(count / ps), ncp = np1 * (uint32_t)ps, ni = (uint32_t)instances, np = np1 * ni, K = hs->tess_out_cp;
    if (!g_tess_kpso && !g_tess_kfail) {
        NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_tess_msl) options:nil error:&e];
        id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"dm_tess_expand"] : nil;
        g_tess_kpso = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&e] : nil;
        id<MTLFunction> gfn = lib ? [lib newFunctionWithName:@"dm_tess_geo"] : nil;
        g_tess_gpso = gfn ? [g_dev newComputePipelineStateWithFunction:gfn error:&e] : nil;
        g_tess_stats = [g_dev newBufferWithLength:64 * 4 options:MTLResourceStorageModeShared];
        if (!g_tess_kpso || !g_tess_gpso || !g_tess_stats) { g_tess_kfail = 1; MLOG("[RED] tessellation expansion kernel: %s", e.localizedDescription.UTF8String); }
        else { memset(g_tess_stats.contents, 0, 64 * 4); res_add(g_tess_stats); }
    }
    if (g_tess_kfail) { tess_refuse(pr, dw, di, "expansion kernel"); return; }
    if (np >= (1u << 20)) { tess_refuse(pr, dw, di, "too many patches for the cell list"); return; }
    uint64_t mx = (uint64_t)np * 4096u; uint32_t lcap = (uint32_t)(mx < tess_budget() ? mx : tess_budget());   // 4096: the most entries a patch has (TESS_MSL)
    // A geometry shader after the domain (nvn_geometry.inc geo_prep_gpu): its triangles, two an entry, must fit the geometry path's 256 MiB scratch
    NvShader *ggs = nil, *greader = nil; id<MTLComputePipelineState> __strong *gpack = NULL;
    if (pr->sh[2]) {
        uint64_t per = geo_gpu_kernels(pr, &ggs, &greader);
        if (!per || !(gpack = geo_pack_psos())) { tess_refuse(pr, dw, di, "geometry shader after the domain: no triangle kernel"); return; }
        if (!ds->geo_fn) { tess_refuse(pr, dw, di, "geometry shader after the domain: the domain has no capture entry"); return; }
        uint64_t gcap = ((256ull << 20) - (64u << 10)) / per / 2;
        if (gcap < lcap) lcap = (uint32_t)gcap;
        if (!lcap) { tess_refuse(pr, dw, di, "geometry shader after the domain: scratch"); return; }
    }
    uint32_t binst = (uint32_t)(dw ? dw->base_instance : di->base_instance);
    if (defer_has_active()) defer_fallback();
    lc_land(); rp_flush(); rp_end(); ubd_land();
    uint32_t *cpi = malloc((size_t)ncp * 4); if (!cpi) { tess_refuse(pr, dw, di, "no memory"); return; }
    uint32_t lo = UINT32_MAX, hi = 0;
    if (dw) { lo = (uint32_t)dw->first; hi = lo + ncp - 1; for (uint32_t i = 0; i < ncp; i++) cpi[i] = i; }
    else {
        int size = 1 << di->itype; uint64_t bytes = (uint64_t)ncp * size;
        if (!drained && geo_index_wait(di->ibuf, bytes)) mdi_wait();
        if (!nvn_host_readable(di->ibuf, bytes)) { free(cpi); tess_refuse(pr, dw, di, "index buffer not readable"); return; }
        for (uint32_t i = 0; i < ncp; i++) { uint32_t n = 0; memcpy(&n, (void *)(uintptr_t)(di->ibuf + (uint64_t)i * size), size); cpi[i] = n;
            if (n < lo) lo = n; if (n > hi) hi = n; }
        if ((int64_t)lo + di->base_vertex < 0 || (int64_t)hi + di->base_vertex > UINT32_MAX) { free(cpi); tess_refuse(pr, dw, di, "base vertex"); return; }
        for (uint32_t i = 0; i < ncp; i++) cpi[i] -= lo;
        lo += di->base_vertex; hi += di->base_vertex;
    }
    uint64_t span = (uint64_t)hi - lo + 1, cap_bytes = span * ni * GEO_CAPTURE_WORDS * 4;
    uint64_t cp_bytes = (uint64_t)np * K * TESS_CP_SLOTS * 16, patch_off = cp_bytes, lev_off = patch_off + (uint64_t)np * TESS_PATCH_SLOTS * 16;
    uint64_t list_off = (lev_off + (uint64_t)np * 12 + 15) & ~15ull, out_bytes = list_off + (uint64_t)lcap * 4;
    if (!span || cap_bytes > (256u << 20) || out_bytes > (256u << 20) || (uint64_t)ncp * ni > (64u << 20) || !K || K > 32) { free(cpi); tess_refuse(pr, dw, di, "too large"); return; }
    id<MTLBuffer> b_cap, b_idx, b_out, b_par, b_ka;
    size_t o_cap = arena_alloc(512 + cap_bytes, &b_cap), o_idx = arena_alloc((size_t)ncp * ni * 4, &b_idx), o_out = arena_alloc(out_bytes, &b_out), o_par = arena_alloc(64, &b_par), o_ka = arena_alloc(48, &b_ka);
    uint64_t g_cap = b_cap.gpuAddress + o_cap, g_idx = b_idx.gpuAddress + o_idx, g_out = b_out.gpuAddress + o_out, g_par = b_par.gpuAddress + o_par, g_ka = b_ka.gpuAddress + o_ka;
    { uint32_t *dst = (uint32_t *)((uint8_t *)b_idx.contents + o_idx);   // instance i's control point = its own record block (the capture writes (instance - base) * span + vertex - first)
      for (uint32_t i = 0; i < ni; i++) for (uint32_t c = 0; c < ncp; c++) dst[(size_t)i * ncp + c] = i * (uint32_t)span + cpi[c]; }
    free(cpi);
    uint32_t par[16] = {(uint32_t)ps, np, 0, 0, 0, K, (uint32_t)patch_off, 0, (uint32_t)lev_off, (uint32_t)list_off, 0, (uint32_t)tess_max()};   // [0..1]: spvIndirectParams; [4..11]: the domain's dm_tinfo[0..1]
    memcpy((uint8_t *)b_par.contents + o_par, par, sizeof par);
    uint32_t ka[12] = {0, 1, 0, binst, 0, lcap, np, (uint32_t)tess_max(), ds->tess_md, (uint32_t)tess_level(), ggs ? MAX(ggs->geo_invocations, 1u) : 0};   // dm_targs: the draw's indirect arguments (vertex count 0, 1 instance), then the expansion's
    memcpy((uint8_t *)b_ka.contents + o_ka, ka, sizeof ka);
    geo_job gj; if (ggs && !geo_prep_gpu(pr, ds, ggs, greader, lcap * 2, binst, &gj)) { tess_refuse(pr, dw, di, "geometry shader after the domain: its job"); return; }
    nvn_geo_capture gc = {g_cap + 512, 0, {lo, (uint32_t)span, binst, ni}, GEO_CAPTURE_WORDS, {0}};
    for (int k = 0; k < GEO_CAPTURE_WORDS; k++) gc.slot[k] = (int16_t)k;
    memcpy((uint8_t *)b_cap.contents + o_cap, &gc, sizeof gc);
    // 1. control points through the vertex shader's capture entry
    id<MTLRenderPipelineState> cap = geo_capture_pso(vs); if (!cap) { tess_refuse(pr, dw, di, "vertex capture pipeline"); return; }
    p_draw range = {.first = (int32_t)lo, .count = (int32_t)span, .instances = (int32_t)ni, .base_instance = (int32_t)binst};
    vbuf_prepare(&vs->rf, &range, NULL);
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    g_draw_binst = binst;
    MTL4RenderPassDescriptor *rd = [MTL4RenderPassDescriptor new]; rd.renderTargetWidth = rd.renderTargetHeight = rd.defaultRasterSampleCount = 1;
    id<MTL4RenderCommandEncoder> re = enc_r_begin(rd); enc_r_pso(re, cap); bind_vbufs(re, &vs->rf);
    vconv_bind(re, vs, 1, &range, NULL); args_dirty_all(); encode_args(re, vs, 0, 1); enc_r_vbuf(re, 13, g_cap);
    enc_r_draw_instanced(re, MTLPrimitiveTypePoint, lo, span, ni, binst); enc_r_end(re);
    if (tess_stages() < 2) { g_tess_draws++; return; }
    // 2. the hull shader, one threadgroup per patch
    if (!hs->geo_pso && !hs->geo_pso_failed) {
        NSError *e = nil; hs->geo_pso = [g_dev newComputePipelineStateWithFunction:fn_dev(hs->fn) error:&e];   // fn_dev: a hull that samples has the border corrections' constant (as geometry kernels, nvn_geometry.inc)
        if (!hs->geo_pso) { hs->geo_pso_failed = 1; MLOG("[RED] hull compute %s: %s", hs->key, e.localizedDescription.UTF8String); }
    }
    uint32_t threads = (uint32_t)(ps > (int)K ? ps : (int)K);
    if (!hs->geo_pso || hs->geo_pso.maxTotalThreadsPerThreadgroup < threads) { tess_refuse(pr, dw, di, "hull pipeline"); return; }
    // the shared compute / copy encoder, as dispatch(): a texture upload or upgrade encode_args needs lands in this same open encoder
    id<MTL4ComputeCommandEncoder> ce = (id<MTL4ComputeCommandEncoder>)dispatch_enc();
    enc_c_pso(ce, hs->geo_pso); args_dirty_all(); g_args_hull = 1; encode_args(ce, hs, 3, 0); g_args_hull = 0;
    enc_c_args(ce, g_idx, 24); enc_c_args(ce, g_cap + 512, 25); enc_c_args(ce, g_out + lev_off, 26); enc_c_args(ce, g_out + patch_off, 27); enc_c_args(ce, g_out, 28); enc_c_args(ce, g_par, 29);
    [ce setThreadgroupMemoryLength:((NSUInteger)ps * hs->tess_in_bytes + 15) & ~(NSUInteger)15 atIndex:0];
    enc_c_dispatch(ce, MTLSizeMake(np, 1, 1), MTLSizeMake(threads, 1, 1));
    // 3. the expansion kernel: per patch its factors -> entries in the list, the count into the draw's indirect arguments
    [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
    enc_c_pso(ce, g_tess_kpso);
    enc_c_args(ce, g_out + lev_off, 0); enc_c_args(ce, g_out + list_off, 1); enc_c_args(ce, g_ka, 2); enc_c_args(ce, g_tess_stats.gpuAddress, 3);
    enc_c_dispatch(ce, MTLSizeMake(np, 1, 1), MTLSizeMake(64, 1, 1));
    if (ggs) {   // the geometry job's counts and threadgroup counts from the list's vertex count
        [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
        enc_c_pso(ce, g_tess_gpso); enc_c_args(ce, g_ka, 0); enc_c_args(ce, gj.info, 1); enc_c_args(ce, gj.job, 2); enc_c_args(ce, gj.gs_dispatch, 3);
        enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1));
    }
    args_dirty_all();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }   // the draw's render pass follows; its full barrier orders it after the kernels
    swrite_note(pr); g_wr_epoch++; g_mdx_epoch++;
    if (tess_stages() < 3) { g_tess_draws++; return; }
    if (ggs) {   // 4. a geometry shader after the domain: the domain's capture of every listed vertex, the geometry kernel and compaction, the geometry path's draw
        MTL4RenderPassDescriptor *gd = [MTL4RenderPassDescriptor new]; gd.renderTargetWidth = gd.renderTargetHeight = gd.defaultRasterSampleCount = 1;
        id<MTL4RenderCommandEncoder> ge = enc_r_begin(gd); enc_r_pso(ge, gj.capture);
        g_draw_binst = binst; args_dirty_all(); encode_args(ge, ds, 4, 1);
        enc_r_vbuf(ge, 13, gj.capture_info); enc_r_vbuf(ge, 14, g_out); enc_r_vbuf(ge, 15, g_par + 16);
        enc_r_draw_indirect(ge, MTLPrimitiveTypePoint, g_ka); enc_r_end(ge);
        id<MTL4ComputeCommandEncoder> gc2 = enc_c_begin(NULL); geo_gs_gpu(gc2, &gj, gpack); enc_c_end(gc2);
        args_dirty_all(); g_enc_pso = nil; g_enc_dss = nil; g_ed.ok = 0;
        geo_raster(&gj, 1);
        g_tess_draws++; g_tess_patches += np;
        static int gonce; if (gonce++ < 4) MLOG("tessellation (frame %lu) into geometry %s: vertex %s hull %s domain %s fragment %s, %u patches of %d control points, at most %u entries (%u triangles), %s %s%s",
            (unsigned long)atomic_load(&g_cur_frame), pr->sh[2]->key, vs->key, hs->key, ds->key, pr->sh[1] ? pr->sh[1]->key : "-", np, ps, lcap, lcap * 2, ds->tess_md & 1 ? "triangles" : "quads",
            (const char *[]){"equal", "fractional_odd", "fractional_even", "?"}[ds->tess_md >> 1 & 3], di ? ", indexed" : "");
        if (g_tess_draws == 400 || g_tess_draws == 4000 || g_tess_draws % 40000 == 0) tess_stats_log();
        return;
    }
    // 4. the domain shader as the vertex stage of an ordinary draw
    NvProg *raster = pr->tess_raster;
    if (!raster || raster->sh[0] != ds || raster->sh[1] != pr->sh[1]) { raster = [NvProg new]; raster->sh[0] = ds; raster->sh[1] = pr->sh[1]; pr->tess_raster = raster; }
    g_geo_program = raster; g_tess_buf = g_out; g_tess_info = g_par + 16; g_tess_count = lcap * 6; g_tess_ind = g_ka;
    args_dirty_all();
    p_draw synthetic = {.prim = 4, .first = 0, .count = (int32_t)g_tess_count, .instances = 1, .base_instance = (int32_t)binst};
    draw(&synthetic, NULL);
    g_geo_program = nil; g_tess_buf = g_tess_info = 0; g_tess_count = 0; g_tess_ind = 0; args_dirty_all();
    g_tess_draws++; g_tess_patches += np;
    static int once; if (once++ < 4) MLOG("tessellation (frame %lu): vertex %s hull %s domain %s fragment %s, %u patches (%u per instance x %u) of %d control points (%u out), %s %s%s, max level %d%s, %s",
        (unsigned long)atomic_load(&g_cur_frame), vs->key, hs->key, ds->key, pr->sh[1] ? pr->sh[1]->key : "-", np, np1, ni, ps, K, ds->tess_md & 1 ? "triangles" : "quads", (const char *[]){"equal", "fractional_odd", "fractional_even", "?"}[ds->tess_md >> 1 & 3],
        ds->tess_md & 8 ? " cw" : "", tess_max(), tess_level() ? ", every level fixed" : "", di ? "indexed" : "arrays");
    if (g_tess_draws == 400 || g_tess_draws == 4000 || g_tess_draws % 40000 == 0) tess_stats_log();
}
