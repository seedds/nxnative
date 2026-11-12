// Tessellated draws (NVN primitive type 14, patches) without a Metal tessellator: Metal 4 render encoders have no
// tessellation stage. Included after nvn_geometry.inc, whose vertex capture this reuses:
//   1. the vertex shader's capture entry writes each control point as a 144-word record (an attachment-less pass);
//   2. the hull shader runs as an ordinary kernel (dmsh_tess.py hull): one threadgroup per patch, its control points
//      read from the records through a control-point index list, its outputs in location slots, its factors in
//      SPIRV-Cross's MTLQuadTessellationFactorsHalf;
//   3. the domain shader runs as the vertex function of an ordinary draw (dmsh_tess.py domain) with the game's own
//      fragment shader and render state. Adaptive (default): an expansion kernel (k_tess_msl, after the hull, same
//      compute encoder) reads each patch's factors, skips culled patches (an outer factor <= 0 or NaN), counts the
//      patch's triangles from its levels (quad, fractional_even, clamped to [2, DM_NVN_TESS_MAX], default 64: an inner
//      grid on the inside levels and an outer ring stitched to each edge's own points, as a hardware tessellator) and
//      appends one entry per two triangles to a list whose count feeds an indirect draw; the domain evaluates each
//      vertex at fractional_even positions (dmsh_tess.TESS_MSL dm_tvert: edges subdivided by their own factor, so shared
//      edges match; no folded or overlapping triangles).
//      DM_NVN_TESS_LEVEL=N instead draws every patch as a fixed N x N grid (A/B, diagnostics).
//      The list holds at most DM_NVN_TESS_BUDGET cells per draw (default 2M); patches past it are dropped and counted.
// Instances: each instance's control points are captured as their own records and its patches numbered after the
// previous instance's (the hull and domain shaders of this library use gl_PrimitiveID only to address their outputs).
// Draws this cannot replay (indirect, no translated hull / domain, a mode other than quads) stay skipped
// with a [RED] line, as before.
static id<MTL4ComputeCommandEncoder> dispatch_enc(void);
enum { TESS_CP_SLOTS = 33, TESS_PATCH_SLOTS = 32 };   // dmsh_tess.py CP_SLOTS / PATCH_SLOTS
static uint64_t g_tess_buf, g_tess_info;
static uint32_t g_tess_count;
static uint64_t g_tess_draws, g_tess_patches, g_tess_refused;
static uint64_t g_tess_ind;   // adaptive draw: GPU address of its MTLDrawPrimitivesIndirectArguments (vertex count written by the expansion kernel)
// The expansion kernel. The dm_t* functions are dmsh_tess.TESS_MSL verbatim (the domain shaders evaluate the same layout; generated text, keep in sync).
static const char *k_tess_msl = "#include <metal_stdlib>\n"
"using namespace metal;\n"
"static inline float dm_tl(half h, float lmax) { float l = float(h); return l >= 2.0f ? min(l, lmax) : 2.0f; }\n"
"static inline uint dm_tn(float l) { return uint(ceil(l * 0.5f)) * 2u; }\n"
"static inline float dm_tpos(uint k, uint n, float l) { uint h = n / 2u; bool hi = k > h; uint kk = hi ? n - k : k; float p = kk == h ? 0.5f : float(kk) / l; return hi ? 1.0f - p : p; }\n"
"static inline bool dm_tcull(device const half* L) { return !(float(L[0]) > 0.0f && float(L[1]) > 0.0f && float(L[2]) > 0.0f && float(L[3]) > 0.0f); }\n"
"static inline uint dm_tcount(device const half* L, float lmax) {\n"
"    uint nu = dm_tn(dm_tl(L[4], lmax)), nv = dm_tn(dm_tl(L[5], lmax)), c = (nu - 2u) * (nv - 2u);\n"
"    for (uint s = 0u; s < 4u; s++) c += (dm_tn(dm_tl(L[(s + 1u) & 3u], lmax)) + ((s & 1u) != 0u ? nv : nu) - 1u) / 2u;\n"
"    return c;\n"
"}\n"
"static inline uint dm_tsize(device const half* L, float lmax) { uint m = 2u; for (uint i = 0u; i < 6u; i++) m = max(m, dm_tn(dm_tl(L[i], lmax))); return m; }\n"
"static inline uint dm_tzpos(uint i, uint a, uint b) { uint n = (2u * i + 1u) * b; return i + (n <= a ? 0u : min(b, (n + a - 1u) / (2u * a))); }\n"
"static inline float2 dm_tvert(uint e, uint k, device const half* L, float lmax, bool cw) {\n"
"    float lu = dm_tl(L[4], lmax), lv = dm_tl(L[5], lmax);\n"
"    uint nu = dm_tn(lu), nv = dm_tn(lv), kk = k % 3u, ic = (nu - 2u) * (nv - 2u);\n"
"    if (cw && kk != 0u) kk = 3u - kk;\n"
"    if (e < ic) {\n"
"        uint ci = e % (nu - 2u), cj = e / (nu - 2u);\n"
"        uint2 d = k < 3u ? (kk == 0u ? uint2(0u, 0u) : kk == 1u ? uint2(1u, 0u) : uint2(1u, 1u)) : (kk == 0u ? uint2(0u, 0u) : kk == 1u ? uint2(1u, 1u) : uint2(0u, 1u));\n"
"        return float2(dm_tpos(1u + ci + d.x, nu, lu), dm_tpos(1u + cj + d.y, nv, lv));\n"
"    }\n"
"    uint r = e - ic, s = 0u, a = 2u, n = 2u; float le = 2.0f;\n"
"    for (; s < 4u; s++) {\n"
"        le = dm_tl(L[(s + 1u) & 3u], lmax); a = dm_tn(le); n = (s & 1u) != 0u ? nv : nu;\n"
"        uint c = (a + n - 1u) / 2u;\n"
"        if (r < c) break;\n"
"        r -= c;\n"
"    }\n"
"    s = min(s, 3u);\n"
"    uint b = n - 2u, t = 2u * r + (k < 3u ? 0u : 1u);\n"
"    bool out; uint idx;\n"
"    if (t >= a + b) { out = true; idx = 0u; }\n"
"    else {\n"
"        uint lo = 0u, hi = a;\n"
"        while (lo < hi) { uint m = (lo + hi) / 2u; if (dm_tzpos(m, a, b) < t) lo = m + 1u; else hi = m; }\n"
"        uint i = lo, j = t - i; bool stepA = i < a && dm_tzpos(i, a, b) == t;\n"
"        if (kk == 0u) { out = true; idx = i; }\n"
"        else if (stepA) { out = kk == 1u; idx = kk == 1u ? i + 1u : j; }\n"
"        else { out = false; idx = kk == 1u ? j + 1u : j; }\n"
"    }\n"
"    if (out) {\n"
"        float p = dm_tpos(s < 2u ? idx : a - idx, a, le);\n"
"        return s == 0u ? float2(p, 0.0f) : s == 1u ? float2(1.0f, p) : s == 2u ? float2(p, 1.0f) : float2(0.0f, p);\n"
"    }\n"
"    return s == 0u ? float2(dm_tpos(1u + idx, nu, lu), dm_tpos(1u, nv, lv)) : s == 1u ? float2(dm_tpos(nu - 1u, nu, lu), dm_tpos(1u + idx, nv, lv))\n"
"         : s == 2u ? float2(dm_tpos(nu - 1u - idx, nu, lu), dm_tpos(nv - 1u, nv, lv)) : float2(dm_tpos(1u, nu, lu), dm_tpos(nv - 1u - idx, nv, lv));\n"
"}\n"
"struct dm_targs { atomic_uint vcount; uint icount, vstart, binst; atomic_uint cells; uint cap, np, lmax; };\n"
"kernel void dm_tess_expand(device const half* lev [[buffer(0)]], device uint* list [[buffer(1)]], device dm_targs& a [[buffer(2)]],\n"
"    device atomic_uint* st [[buffer(3)]], uint p [[threadgroup_position_in_grid]], uint t [[thread_index_in_threadgroup]], uint tn [[threads_per_threadgroup]])\n"
"{\n"
"    threadgroup uint base, cnt;\n"
"    if (t == 0) {\n"
"        device const half* L = lev + p * 6u; float lmax = float(a.lmax);\n"
"        uint c = 0, b = 0;\n"
"        if (dm_tcull(L)) atomic_fetch_add_explicit(&st[1], 1u, memory_order_relaxed);\n"
"        else { c = dm_tcount(L, lmax); b = atomic_fetch_add_explicit(&a.cells, c, memory_order_relaxed);\n"
"            if (b + c > a.cap) { atomic_fetch_add_explicit(&st[2], 1u, memory_order_relaxed); c = 0; }\n"
"            else { atomic_fetch_add_explicit(&a.vcount, c * 6u, memory_order_relaxed); atomic_fetch_add_explicit(&st[3], c, memory_order_relaxed);\n"
"                atomic_fetch_add_explicit(&st[4 + min(dm_tsize(L, lmax), 64u) / 2u], 1u, memory_order_relaxed); } }\n"
"        atomic_fetch_add_explicit(&st[0], 1u, memory_order_relaxed);\n"
"        base = b; cnt = c;\n"
"    }\n"
"    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
"    for (uint x = t; x < cnt; x += tn) list[base + x] = (p << 12) | x;\n"
"}\n";
static int tess_max(void) {   // DM_NVN_TESS_MAX: highest level used (even, 2..64; default 64, the OpenGL / Maxwell maximum)
    static int n = 0; if (!n) { const char *e = getenv("DM_NVN_TESS_MAX"); n = e ? atoi(e) : 64; n = n < 2 ? 2 : n > 64 ? 64 : n & ~1; } return n;
}
static uint32_t tess_budget(void) {
    static uint32_t n = 0; if (!n) { const char *e = getenv("DM_NVN_TESS_BUDGET"); long v = e ? atol(e) : 2l << 20; n = (uint32_t)(v < 4096 ? 4096 : v > (16l << 20) ? (16l << 20) : v); } return n;
}
static id<MTLComputePipelineState> g_tess_kpso; static id<MTLBuffer> g_tess_stats; static int g_tess_kfail;
static void tess_stats_log(void) {   // the expansion kernel's running totals (read without synchronisation: approximate, diagnostics only)
    if (!g_tess_stats) return;
    const uint32_t *st = (const uint32_t *)g_tess_stats.contents; char h[512]; int o = 0;
    for (int i = 1; i <= 32 && o < 480; i++) if (st[4 + i]) o += snprintf(h + o, sizeof h - o, " %d:%u", 2 * i, st[4 + i]);
    MLOG("tessellation (adaptive): %u patches, %u culled, %u dropped over budget, %u entries of two triangles (%.1f per drawn patch); size histogram (largest segment count: patches)%s",
        st[0], st[1], st[2], st[3], st[0] > st[1] + st[2] ? (double)st[3] / (st[0] - st[1] - st[2]) : 0.0, h);
}
static int tess_stages(void) {   // DM_NVN_TESS_STAGES=1 / 2: diagnostic, stop after the vertex capture / the hull kernel (the draw is then skipped)
    static int n = 0; if (!n) { const char *e = getenv("DM_NVN_TESS_STAGES"); n = e ? atoi(e) : 3; if (n < 1 || n > 3) n = 3; } return n;
}
static int tess_level(void) {   // 0: adaptive (default); DM_NVN_TESS_LEVEL=N: the fixed N x N grid
    static int n = -1;
    if (n < 0) { const char *e = getenv("DM_NVN_TESS_LEVEL"); n = e ? atoi(e) : 0; n = !e ? 0 : n < 2 ? 2 : n > 64 ? 64 : n & ~1; }
    return n;
}
static void tess_refuse(NvProg *pr, const p_draw *dw, const p_draw_idx *di, const char *why) {
    g_tess_refused++;
    static int n; if (n++ < 8) MLOG("[RED] tessellated draw not replayed (vertex %s hull %s domain %s fragment %s; patch size %d, count %d, instances %d, indexed %d): %s",
        pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[3] ? pr->sh[3]->key : "-", pr->sh[4] ? pr->sh[4]->key : "-", pr->sh[1] ? pr->sh[1]->key : "-",
        S.patch_size, dw ? dw->count : di->count, dw ? dw->instances : di->instances, di != NULL, why);
    PD("Z %llu draw skipped: tessellation not replayed (%s)", g_stat_pass + !g_enc, why);
}
static void tess_draw(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    NvShader *vs = pr->sh[0], *hs = pr->sh[3], *ds = pr->sh[4];
    int count = dw ? dw->count : di->count, instances = dw ? dw->instances : di->instances, ps = S.patch_size;
    if (!vs || !vs->geo_fn) { tess_refuse(pr, dw, di, "no vertex capture entry"); return; }
    if (!hs || !hs->tess_ok || !ds || !ds->tess_ok) { tess_refuse(pr, dw, di, "hull or domain not rewritten for replay (dmsh_tess.py)"); return; }
    static const char *only; static int only_read;   // DM_NVN_TESS_ONLY=<hull key>,...: diagnostic, replay only those programs (to tell their effects apart)
    if (!only_read) { only = getenv("DM_NVN_TESS_ONLY"); only_read = 1; }
    if (only && !strstr(only, [[NSString stringWithUTF8String:hs->key] substringToIndex:MIN((NSUInteger)16, strlen(hs->key))].UTF8String)) { tess_refuse(pr, dw, di, "not in DM_NVN_TESS_ONLY"); return; }
    if (dw ? dw->ind || dw->mdi_cnt : di->ind || di->mdi_cnt) { tess_refuse(pr, dw, di, "indirect"); return; }
    if (instances < 1) { tess_refuse(pr, dw, di, "no instances"); return; }
    if (ps < 1 || ps > 32 || count < ps) { tess_refuse(pr, dw, di, "patch size"); return; }
    uint32_t np1 = (uint32_t)(count / ps), ncp = np1 * (uint32_t)ps, ni = (uint32_t)instances, np = np1 * ni, K = hs->tess_out_cp, N = (uint32_t)tess_level();
    int adapt = N == 0;
    if (adapt && !g_tess_kpso && !g_tess_kfail) {
        NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_tess_msl) options:nil error:&e];
        id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"dm_tess_expand"] : nil;
        g_tess_kpso = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&e] : nil;
        g_tess_stats = [g_dev newBufferWithLength:64 * 4 options:MTLResourceStorageModeShared];
        if (!g_tess_kpso || !g_tess_stats) { g_tess_kfail = 1; MLOG("[RED] tessellation expansion kernel: %s", e.localizedDescription.UTF8String); }
        else { memset(g_tess_stats.contents, 0, 64 * 4); res_add(g_tess_stats); }
    }
    if (adapt && g_tess_kfail) { tess_refuse(pr, dw, di, "expansion kernel"); return; }
    if (adapt && np >= (1u << 20)) { tess_refuse(pr, dw, di, "too many patches for the cell list"); return; }
    uint32_t lcap = 0; if (adapt) { uint64_t mx = (uint64_t)np * (uint64_t)tess_max() * (uint64_t)tess_max(); lcap = (uint32_t)(mx < tess_budget() ? mx : tess_budget()); }
    uint32_t binst = (uint32_t)(dw ? dw->base_instance : di->base_instance);
    if (defer_has_active()) defer_fallback();
    lc_land(); rp_flush(); rp_end(); ubd_land();
    uint32_t *cpi = malloc((size_t)ncp * 4); if (!cpi) { tess_refuse(pr, dw, di, "no memory"); return; }
    uint32_t lo = UINT32_MAX, hi = 0;
    if (dw) { lo = (uint32_t)dw->first; hi = lo + ncp - 1; for (uint32_t i = 0; i < ncp; i++) cpi[i] = i; }
    else {
        int size = 1 << di->itype; uint64_t bytes = (uint64_t)ncp * size;
        if (di->itype < 0 || di->itype > 2) { free(cpi); tess_refuse(pr, dw, di, "index type"); return; }
        if (geo_index_wait(di->ibuf, bytes)) mdi_wait();
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
    id<MTLBuffer> b_cap, b_idx, b_out, b_par;
    size_t o_cap = arena_alloc(512 + cap_bytes, &b_cap), o_idx = arena_alloc((size_t)ncp * ni * 4, &b_idx), o_out = arena_alloc(out_bytes, &b_out), o_par = arena_alloc(64, &b_par);
    uint64_t g_cap = b_cap.gpuAddress + o_cap, g_idx = b_idx.gpuAddress + o_idx, g_out = b_out.gpuAddress + o_out, g_par = b_par.gpuAddress + o_par;
    { uint32_t *dst = (uint32_t *)((uint8_t *)b_idx.contents + o_idx);   // instance i's control point = its own record block (the capture writes (instance - base) * span + vertex - first)
      for (uint32_t i = 0; i < ni; i++) for (uint32_t c = 0; c < ncp; c++) dst[(size_t)i * ncp + c] = i * (uint32_t)span + cpi[c]; }
    free(cpi);
    uint32_t par[16] = {(uint32_t)ps, np, 0, 0, N, K, (uint32_t)patch_off, (uint32_t)ds->tess_cw, (uint32_t)lev_off, (uint32_t)list_off, (uint32_t)adapt, (uint32_t)tess_max(),
        0, 1, 0, binst};   // [0..1]: spvIndirectParams; [4..11]: the domain's dm_tinfo[0..1]; [12..15]: the adaptive draw's indirect arguments (vertex count 0, 1 instance)
    memcpy((uint8_t *)b_par.contents + o_par, par, sizeof par);
    id<MTLBuffer> b_ka = nil; size_t o_ka = 0;
    if (adapt) { o_ka = arena_alloc(32, &b_ka); uint32_t ka[8] = {0, 1, 0, binst, 0, lcap, np, (uint32_t)tess_max()}; memcpy((uint8_t *)b_ka.contents + o_ka, ka, sizeof ka); }
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
        NSError *e = nil; hs->geo_pso = [g_dev newComputePipelineStateWithFunction:hs->fn error:&e];
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
    if (adapt) {   // the expansion kernel: per patch its factors -> cells in the list, the count into the draw's indirect arguments
        [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
        enc_c_pso(ce, g_tess_kpso);
        enc_c_args(ce, g_out + lev_off, 0); enc_c_args(ce, g_out + list_off, 1); enc_c_args(ce, b_ka.gpuAddress + o_ka, 2); enc_c_args(ce, g_tess_stats.gpuAddress, 3);
        enc_c_dispatch(ce, MTLSizeMake(np, 1, 1), MTLSizeMake(64, 1, 1));
        args_dirty_all();
    }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }   // the draw's render pass follows; its full barrier orders it after the kernel
    swrite_note(pr); g_wr_epoch++; g_mdx_epoch++;
    if (tess_stages() < 3) { g_tess_draws++; return; }
    // 3. the domain shader as the vertex stage of an ordinary draw
    NvProg *raster = pr->tess_raster;
    if (!raster || raster->sh[0] != ds || raster->sh[1] != pr->sh[1]) { raster = [NvProg new]; raster->sh[0] = ds; raster->sh[1] = pr->sh[1]; pr->tess_raster = raster; }
    g_geo_program = raster; g_tess_buf = g_out; g_tess_info = g_par + 16; g_tess_count = adapt ? lcap * 6 : np * 6 * N * N;
    g_tess_ind = adapt ? b_ka.gpuAddress + o_ka : 0;
    args_dirty_all();
    p_draw synthetic = {.prim = 4, .first = 0, .count = (int32_t)g_tess_count, .instances = 1, .base_instance = (int32_t)binst};
    draw(&synthetic, NULL);
    g_geo_program = nil; g_tess_buf = g_tess_info = 0; g_tess_count = 0; g_tess_ind = 0; args_dirty_all();
    g_tess_draws++; g_tess_patches += np;
    static int once; if (once++ < 4) MLOG("tessellation: vertex %s hull %s domain %s, %u patches (%u per instance x %u) of %d control points (%u out), %s %u, %s", vs->key, hs->key, ds->key, np, np1, ni, ps, K, adapt ? "adaptive, max level" : "fixed grid", adapt ? (uint32_t)tess_max() : N, di ? "indexed" : "arrays");
    if (adapt && (g_tess_draws == 400 || g_tess_draws == 4000 || g_tess_draws % 40000 == 0)) tess_stats_log();
}
