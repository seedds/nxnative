// General geometry replay: native VS capture -> GS compute -> ordered index
// compaction -> ordinary rendering. Included after the resource/pipeline helpers.
static NvProg *g_geo_program;
static uint64_t g_geo_indices, g_geo_index_bytes, g_geo_args;
static void draw(const p_draw *dw, const p_draw_idx *di);
static void lc_land(void);

static int geo_cmp_index(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}
enum { GEO_CAPTURE_VERTICES = (256u << 20) / (144 * 4) };
// The capture span is already bounded by its scratch allocation. A bit per
// vertex gives the same ascending unique list without sorting every reference.
// Keep an in-place sort fallback for allocation failure or a wider caller.
// order may be changed; a short destination is never partially written.
static size_t geo_capture_unique(uint32_t *order, size_t count, int restart, uint32_t restart_index,
                                 uint32_t lo, uint64_t span, uint32_t *dst, size_t capacity) {
    if (!span || span > (uint64_t)UINT32_MAX + 1 - lo) return SIZE_MAX;
    for (size_t i = 0; i < count; i++) if (!(restart && order[i] == restart_index) &&
        (order[i] < lo || (uint64_t)order[i] - lo >= span)) return SIZE_MAX;
    size_t nw = (size_t)((span + 63) / 64), n = 0;
    uint64_t *bits = span <= GEO_CAPTURE_VERTICES ? calloc(nw, sizeof *bits) : NULL;
    if (bits) {
        for (size_t i = 0; i < count; i++) if (!(restart && order[i] == restart_index)) {
            uint32_t v = order[i] - lo; bits[v >> 6] |= 1ull << (v & 63);
        }
        for (size_t i = 0; i < nw; i++) n += (size_t)__builtin_popcountll(bits[i]);
        if (n <= capacity) for (size_t i = 0, k = 0; i < nw; i++) {
            uint64_t word = bits[i];
            while (word) { dst[k++] = lo + (uint32_t)(i * 64 + __builtin_ctzll(word)); word &= word - 1; }
        }
        free(bits);
    } else {
        size_t references = 0;
        for (size_t i = 0; i < count; i++) if (!(restart && order[i] == restart_index)) order[references++] = order[i];
        qsort(order, references, sizeof *order, geo_cmp_index);
        for (size_t i = 0; i < references; i++) if (!i || order[i] != order[i - 1]) n++;
        if (n <= capacity) for (size_t i = 0, k = 0; i < references; i++)
            if (!i || order[i] != order[i - 1]) dst[k++] = order[i];
    }
    return n <= capacity ? n : SIZE_MAX;
}
// Only CPU reads need a completed snapshot. Native vertex capture stays on the
// GPU and follows earlier writes through normal encoder ordering. Unknown or
// overflowing write sets always retain the conservative drain.
static int geo_cpu_hazard(uint64_t lo, uint64_t n) {
    if (!n) return 0;
    if (!lo || n > UINT64_MAX - lo || g_sbw_over) return 1;
    return pend_hits(lo, n) || sbw_hits(lo, lo + n) || gw_live(lo, lo + n);
}
static int geo_conversion_hazard(NvShader *vs) {
    for (int v = 0; v < vs->rf.nv; v++) {
        int a = vs->rf.v[v].nvn;
        if (a >= S.nattr || !vconv_wanted(&vs->rf, v, NULL, 0)) continue;
        int st = S.attr[a].stream;
        if (st < 16 && S.vbuf[st].address && geo_cpu_hazard(S.vbuf[st].address, S.vbuf[st].size)) return 1;
    }
    return 0;
}
// GS output -> counts -> offsets/indirect args -> packed indices stay in one
// compute encoder. Each transition needs dispatch ordering and device-memory
// visibility; full queue barriers remain at the encoder's boundaries.
static void geo_pack_encode(id<MTL4ComputeCommandEncoder> ce, id<MTLComputePipelineState> __strong *pack, uint64_t job, uint32_t groups) {
    enc_c_args(ce, job, 0);
    for (int i = 0; i < 3; i++) {
        [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
        enc_c_pso(ce, pack[i]);
        NSUInteger threads = i == 1 ? MIN((NSUInteger)256, pack[i].maxTotalThreadsPerThreadgroup) : 64;
        enc_c_dispatch(ce, i == 1 ? MTLSizeMake(1, 1, 1) : MTLSizeMake((groups + 63) / 64, 1, 1), MTLSizeMake(threads, 1, 1));
    }
}
static id<MTLRenderPipelineState> geo_capture_pso(NvShader *vs) {
    pso_key k; static const uint32_t cf[8]; pso_key_fill(&k, vs, nil, cf, 0, 0, 1, 0); k.fsfn = GEO_MARK;
    id<MTLRenderPipelineState> ps = hm_get_id(&g_pso, &k);
    if (ps || hm_get_id(&g_pso_fail, &k)) return ps;
    NSError *e = nil;
    ps = [g_dev newRenderPipelineStateWithDescriptor:pso_desc(&k, vs, nil, nil, 0) error:&e];
    if (ps) hm_put_id(&g_pso, &k, ps);
    else { hm_put_id(&g_pso_fail, &k, NSNull.null); MLOG("[RED] geometry vertex capture %s: %s", vs->key, e.localizedDescription.UTF8String); }
    return ps;
}
// One diagnostic snapshot through the same native vertex fetch as geometry
// capture. Refuse shaders with writes: observing them must not run their side
// effects twice. Records retain the documented 144-word geometry capture ABI.
static void vertex_dump(NvShader *vs, const p_draw *dw, const p_draw_idx *di) {
    static const char *key, *path; static int configured, captured, limit; static uint64_t frame;
    if (!configured) { key = getenv("DM_NVN_VSDUMP_KEY"); path = getenv("DM_NVN_VSDUMP");
        const char *f = getenv("DM_NVN_VSDUMP_FROM"), *n = getenv("DM_NVN_VSDUMP_MAX");
        frame = f ? strtoull(f,NULL,0) : 0; limit = n ? atoi(n) : key && !strcmp(key,"*") ? 256 : 1; configured = 1; }
    int all = key && !strcmp(key,"*");
    if (captured >= limit || !key || !path || atomic_load(&g_cur_frame) < frame || !vs || (!all && strncmp(vs->key, key, 16)) || !vs->geo_fn ||
        (dw ? dw->ind || dw->mdi_cnt : di->ind || di->mdi_cnt) || S.rast_discard) return;
    for (int i = 0; i < vs->rf.nb; i++) if (vs->rf.b[i].rw) return;
    int count = dw ? dw->count : di->count, instances = dw ? dw->instances : di->instances;
    if (count <= 0 || instances > 1 || (di && (di->itype < 0 || di->itype > 2))) return;
    unsigned number = captured++; lc_land(); rp_flush(); mdi_wait();
    uint32_t lo = dw ? (uint32_t)dw->first : UINT32_MAX, hi = dw ? lo + count - 1 : 0;
    if (di) {
        int size = 1 << di->itype;
        if (!nvn_host_readable(di->ibuf, (uint64_t)count * size)) return;
        for (int i = 0; i < count; i++) { uint32_t n = 0; memcpy(&n, (void *)(uintptr_t)(di->ibuf + (uint64_t)i * size), size);
            if (S.restart.on && n == (S.restart.index & (size == 1 ? 255u : size == 2 ? 65535u : UINT32_MAX))) continue;
            lo = MIN(lo, n); hi = MAX(hi, n); }
        if (lo == UINT32_MAX || (int64_t)lo + di->base_vertex < 0 || (int64_t)hi + di->base_vertex > UINT32_MAX) return;
        lo += di->base_vertex; hi += di->base_vertex;
    }
    uint64_t span = (uint64_t)hi - lo + 1, bytes = span * 144 * 4;
    if (!span || bytes > (64u << 20)) return;
    id<MTLRenderPipelineState> ps = geo_capture_pso(vs); if (!ps) return;
    id<MTLBuffer> output; size_t offset = arena_alloc(bytes + 256, &output);
    uint64_t gpu = output.gpuAddress + offset;
    uint64_t info[4] = {gpu + 256, 0, (uint64_t)lo | span << 32, (uint32_t)(dw ? dw->base_instance : di->base_instance) | 1ull << 32};
    memcpy((uint8_t *)output.contents + offset, info, sizeof info);
    vbuf_prepare();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    MTL4RenderPassDescriptor *rd = [MTL4RenderPassDescriptor new]; rd.renderTargetWidth = rd.renderTargetHeight = rd.defaultRasterSampleCount = 1;
    id<MTL4RenderCommandEncoder> en = enc_r_begin(rd); enc_r_pso(en, ps); bind_vbufs(en);
    p_draw range = {.first = (int32_t)lo, .count = (int32_t)span, .instances = 1, .base_instance = (int32_t)(info[3] & UINT32_MAX)};
    vconv_bind(en, vs, 1, &range, NULL); args_dirty_all(); encode_args(en, vs, 0, 1); enc_r_vbuf(en, 13, gpu);
    enc_r_draw_instanced(en, MTLPrimitiveTypePoint, lo, span, 1, (uint32_t)info[3]); enc_r_end(en);
    mdi_wait();
    char filename[1200]; if (all) snprintf(filename,sizeof filename,"%s_%04u_%s.bin",path,number,vs->key); else strlcpy(filename,path,sizeof filename);
    FILE *f = fopen(filename, "wb"); if (f) { uint32_t header[4] = {lo, (uint32_t)span, all ? 4u : 144u, (uint32_t)info[3]}; fwrite(header, sizeof header, 1, f);
        if (all) for (uint64_t i=0;i<span;i++) fwrite((uint8_t *)output.contents + offset + 256 + i*144*4,1,16,f);
        else fwrite((uint8_t *)output.contents + offset + 256, 1, bytes, f); fclose(f); }
    MLOG("vertex dump %s: first %u, count %llu, %s", vs->key, lo, (unsigned long long)span, filename);
    args_dirty_all(); g_enc_pso = nil; g_enc_dss = nil; g_ed.ok = 0;
}
static void geo_draw(NvProg *pr, const p_draw *dw0, const p_draw_idx *di0);
// A draw whose capture would need more scratch than the bound, drawn as smaller draws in the hardware's order: one instance at a time, then runs of a list topology's primitives
// (points, lines, triangles, lines and triangles with adjacency), each with its own index range. The capture lays the vertices out by vertex number from the lowest index to the highest for
// every instance, and the geometry shader's output takes room per primitive: OCTOPATH TRAVELER II draws 129,032 triangles of one mesh through a cube shadow map's geometry shader in its
// first battle, which asked for more than the bound and was skipped. The index list is read once and cut into runs of at most GEO_RUN_PRIMS primitives whose index range takes at most
// GEO_RUN_SPAN of capture; the runs skip the GPU wait their draw would take (the outer draw waited already, and nothing in between writes index or vertex data). Halving the list at every
// level instead, each part reading its indices again and waiting for the GPU, made that one frame take 805 ms. Strips, fans and primitive restart cannot be cut that way. The draw's
// primitive numbers restart at 0 in each run: its geometry shader (cb17a9453d4789d0, Unreal's one-pass point light shadow) reads only its index map by them, but one that read its
// primitive ID would see other numbers than on the Switch (such draws were skipped before).
// Returns 0 when the draw cannot be split (the caller reports it)
enum { GEO_RUN_PRIMS = 4096 }; static const uint64_t GEO_RUN_SPAN = 64ull << 20;
static int g_geo_split_depth;   // > 0 while geo_split draws its runs: they skip the GPU wait
static int geo_sync_cpu(int *drained) {
    if (*drained) return 1;
    mdi_wait();
    if (g_mdi_ev.signaledValue < g_mdi_v) return 0;
    memset(g_vconv, 0, sizeof g_vconv); *drained = 1;
    return 1;
}
static int geo_split(NvProg *pr, const p_draw *dw, const p_draw_idx *di, int count, int instances, uint32_t first_instance) {
    int prim = dw ? dw->prim : di->prim, per = prim == 0 ? 1 : prim == 1 ? 2 : prim == 4 ? 3 : prim == 10 ? 4 : prim == 12 ? 6 : 0;
    if (!per || (di && S.restart.on) || count / per < 1) return 0;
    static uint64_t nsplit; if (nsplit++ < 4) MLOG("geometry draw above the scratch bound: drawn in parts (vertex %s, geometry %s, %d primitives, %d instances)",
                                                   pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[2] ? pr->sh[2]->key : "-", count / per, instances);
    p_draw a = dw ? *dw : (p_draw){0}; p_draw_idx b = di ? *di : (p_draw_idx){0};
    g_geo_split_depth++;
    if (instances > 1) {
        for (int i = 0; i < instances; i++) {
            if (dw) { a.instances = 1; a.base_instance = (int32_t)(first_instance + i); geo_draw(pr, &a, NULL); }
            else { b.instances = 1; b.base_instance = (int32_t)(first_instance + i); geo_draw(pr, NULL, &b); }
        }
        g_geo_split_depth--; return 1;
    }
    int np = count / per, isz = di ? 1 << di->itype : 0;
    for (int p0 = 0; p0 < np;) {
        int p1 = p0; uint32_t lo = UINT32_MAX, hi = 0;
        while (p1 < np && p1 - p0 < GEO_RUN_PRIMS) {   // extend the run while its index range fits
            uint32_t l = lo, h = hi;
            for (int k = 0; k < per; k++) {
                uint32_t v = (uint32_t)(p1 * per + k);
                if (di) { v = 0; memcpy(&v, (const uint8_t *)(uintptr_t)di->ibuf + (size_t)(p1 * per + k) * isz, isz); }
                l = MIN(l, v); h = MAX(h, v);
            }
            if (p1 > p0 && ((uint64_t)h - l + 1) * 144 * 4 > GEO_RUN_SPAN) break;
            lo = l; hi = h; p1++;
        }
        if (p0 == 0 && p1 == np) {   // one run is the whole draw: its own outputs are what exceed the bound, so halve it
            p1 = np / 2; if (!p1) { g_geo_split_depth--; return 0; }
        }
        if (dw) { a.first = dw->first + p0 * per; a.count = (p1 - p0) * per; geo_draw(pr, &a, NULL); }
        else { b.ibuf = di->ibuf + ((uint64_t)(p0 * per) << di->itype); b.count = (p1 - p0) * per; geo_draw(pr, NULL, &b); }
        p0 = p1;
    }
    g_geo_split_depth--;
    return 1;
}
static void geo_draw(NvProg *pr, const p_draw *dw0, const p_draw_idx *di0) {
    NvShader *vs = pr->sh[0], *base = pr->sh[2];
    if (!vs || !vs->geo_fn || !base || !base->geo_vertices || pr->sh[3] || pr->sh[4]) {
        MLOG("[RED] geometry draw cannot capture this pipeline (vertex %s, geometry %s, tessellation %d, feedback %d)", vs ? vs->key : "-", base ? base->key : "-", !!(pr->sh[3] || pr->sh[4]), S.tf_on); return;
    }
    p_draw dw = dw0 ? *dw0 : (p_draw){0}; p_draw_idx di = di0 ? *di0 : (p_draw_idx){0};
    if (di0 && (di.itype < 0 || di.itype > 2)) { MLOG("[RED] geometry index type %d is invalid", di.itype); return; }
    uint64_t ind = dw0 ? dw.ind : di.ind;
    if (defer_has_active()) defer_fallback();
    lc_land(); rp_flush(); rp_end(); ubd_land();
    int drained = g_geo_split_depth != 0;
    // Decode indirect arguments only after their CPU snapshot is safe. They
    // select the index range checked below; no GPU work is encoded between the
    // two checks, so a successful drain covers both and the converted streams.
    if ((geo_conversion_hazard(vs) || (ind && geo_cpu_hazard(ind, dw0 ? 16 : 20))) && !geo_sync_cpu(&drained)) return;
    if (ind) {
        uint32_t a[5] = {0}; size_t n = dw0 ? 16 : 20;
        if (!nvn_host_readable(ind, n)) { MLOG("[RED] geometry indirect arguments are not readable"); return; }
        memcpy(a, (void *)(uintptr_t)ind, n);
        if (!a[0] || !a[1]) return;
        if (a[0] > INT32_MAX || a[1] > INT32_MAX) { MLOG("[RED] geometry indirect draw exceeds the host count range"); return; }
        if (dw0) { dw.count = a[0]; dw.instances = a[1]; dw.first = a[2]; dw.base_instance = a[3]; dw.ind = dw.mdi_cnt = 0; }
        else { di.count = a[0]; di.instances = a[1]; di.ibuf += (uint64_t)a[2] << di.itype; di.base_vertex = (int32_t)a[3]; di.base_instance = a[4]; di.ind = di.mdi_cnt = 0; }
    }
    int prim = dw0 ? dw.prim : di.prim, topology = nvn_geo_topology(prim);
    int count = dw0 ? dw.count : di.count, instances = dw0 ? dw.instances : di.instances;
    uint32_t first_instance = (uint32_t)(dw0 ? dw.base_instance : di.base_instance);
    g_draw_binst = first_instance;
    if (count <= 0 || instances < 0) return;
    if (!instances) instances = 1;
    if (topology < 0 || count > (1 << 22)) { MLOG("[RED] geometry primitive %d or count %d exceeds the assembly path", prim, count); return; }
    if (di0 && geo_cpu_hazard(di.ibuf, (uint64_t)count << di.itype) && !geo_sync_cpu(&drained)) return;
    NSString *key = [[NSString stringWithUTF8String:base->key] substringToIndex:16];
    NvShader *gs = shader_get([key stringByAppendingFormat:@"_bg%d", topology], 2);
    NvShader *reader = shader_get([key stringByAppendingString:@"_bgv"], 0);
    if (!gs || !reader) return;
    id<MTLRenderPipelineState> capture = geo_capture_pso(vs); if (!capture) return;
    if (!gs->geo_pso) { NSError *e = nil; gs->geo_pso = [g_dev newComputePipelineStateWithFunction:gs->fn error:&e];
        if (!gs->geo_pso) { MLOG("[RED] geometry compute %s: %s", gs->key, e.localizedDescription.UTF8String); return; } }
    if (gs->geo_pso.maxTotalThreadsPerThreadgroup < 32) { MLOG("[RED] geometry kernel cannot run a 32-thread group"); return; }
    static id<MTLComputePipelineState> pack[3];   // geo_pack_count, geo_pack_scan, geo_pack_write (nvn_geometry.h)
    if (!pack[2]) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_nvn_geo_pack_msl) options:nil error:&e];
        const char *fn[3] = {"geo_pack_count", "geo_pack_scan", "geo_pack_write"};
        for (int i = 0; i < 3 && lib; i++) if (!(pack[i] = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@(fn[i])] error:&e])) break;
        if (!pack[2]) { pack[0] = pack[1] = nil; MLOG("[RED] geometry index compaction: %s", e.localizedDescription.UTF8String); return; } }

    uint32_t *order = malloc((size_t)count * 4);
    if (!order) { MLOG("[RED] geometry index allocation failed"); return; }
    uint32_t lo = UINT32_MAX, hi = 0; size_t nu = 0;
    int restart = di0 && S.restart.on;
    int isz = di0 ? 1 << di.itype : 0;
    uint32_t restart_index = S.restart.index & (isz == 1 ? UINT8_MAX : isz == 2 ? UINT16_MAX : UINT32_MAX);
    if (di0 && (di.itype < 0 || di.itype > 2 || !nvn_host_readable(di.ibuf, (uint64_t)count * isz))) {
        free(order); MLOG("[RED] geometry index buffer is not readable"); return;
    }
    for (int i = 0; i < count; i++) {
        uint32_t v = (uint32_t)i;
        if (di0) { v = 0; memcpy(&v, (const uint8_t *)(uintptr_t)di.ibuf + (size_t)i * isz, isz); }
        order[i] = v;
        if (restart && v == restart_index) continue;
        lo = MIN(lo, v); hi = MAX(hi, v); nu++;
    }
    if (!nu) { free(order); return; }
    int64_t first = di0 ? (int64_t)lo + di.base_vertex : dw.first;
    uint64_t span = (uint64_t)hi - lo + 1;
    if (first < 0 || first + span > (uint64_t)UINT32_MAX + 1) { free(order); MLOG("[RED] geometry vertex range is outside 32-bit vertex IDs"); return; }
    // Normalize only after assembly, so a restart value cannot collide with a
    // normalized vertex number. Indexed captures use a sorted unique index list.
    size_t words = nvn_geo_assemble(prim, order, count, restart, restart_index, NULL, 0);
    int width = nvn_geo_width(topology);
    uint64_t np = words == SIZE_MAX ? 0 : words / width;
    if (!np) { free(order); return; }
    uint64_t groups = np * (uint64_t)instances * gs->geo_invocations;
    if (groups > UINT32_MAX || span * (uint64_t)instances > GEO_CAPTURE_VERTICES ||
        (uint64_t)first_instance + instances > (uint64_t)UINT32_MAX + 1) {
        free(order);
        if (!geo_split(pr, dw0 ? &dw : NULL, dw0 ? NULL : &di, count, instances, first_instance)) MLOG("[RED] geometry draw exceeds the capture or invocation bound"); return;
    }
    uint64_t input_size = span * instances * 144 * 4;
    uint64_t output_size = groups * gs->geo_vertices * gs->geo_stride * 4;
    uint64_t raw_size = groups * gs->geo_index_stride * 4;
    uint64_t packed_size = groups * gs->geo_vertices * (gs->geo_output == 7 ? 3 : gs->geo_output == 6 ? 2 : 1) * 4;
    uint64_t total = input_size + output_size + raw_size + packed_size + groups * 4 + words * 4 + nu * 4 + 4096;
    if (groups > UINT32_MAX || total > (256ull << 20) || np > UINT32_MAX) {
        free(order);
        if (!geo_split(pr, dw0 ? &dw : NULL, dw0 ? NULL : &di, count, instances, first_instance)) MLOG("[RED] geometry draw needs %llu MiB of scratch, above the 256 MiB bound", (unsigned long long)(total >> 20)); return;
    }
    id<MTLBuffer> scratch; size_t off = arena_alloc((size_t)total, &scratch);
    uint8_t *cpu = (uint8_t *)scratch.contents + off; uint64_t gpu = scratch.gpuAddress + off;
    size_t used = 0;
    #define GEO_ALLOC(N, G, P) do { used = (used + 255) & ~(size_t)255; (G) = gpu + used; (P) = cpu + used; used += (size_t)(N); } while (0)
    uint64_t input, output, raw, packed, cnt, map, capture_ix, info, capture_info, args, job;
    void *pinput, *poutput, *praw, *ppacked, *pcnt, *pmap, *pcapture_ix, *pinfo, *pcapture_info, *pargs, *pjob;
    GEO_ALLOC(input_size, input, pinput); GEO_ALLOC(output_size, output, poutput); GEO_ALLOC(raw_size, raw, praw); GEO_ALLOC(packed_size, packed, ppacked); GEO_ALLOC(groups * 4, cnt, pcnt);
    GEO_ALLOC(words * 4, map, pmap); GEO_ALLOC(nu * 4, capture_ix, pcapture_ix); GEO_ALLOC(32, info, pinfo); GEO_ALLOC(32, capture_info, pcapture_info);
    GEO_ALLOC(20, args, pargs); GEO_ALLOC(sizeof(nvn_geo_pack_job), job, pjob);
    #undef GEO_ALLOC
    (void)pinput; (void)poutput; (void)praw; (void)ppacked; (void)pcnt;
    if (used > total) MDIE("geometry scratch size accounting");
    nvn_geo_assemble(prim, order, count, restart, restart_index, pmap, words);
    for (size_t i = 0; i < words; i++) ((uint32_t *)pmap)[i] -= lo;
    size_t unique_count = di0 ? geo_capture_unique(order, count, restart, restart_index, lo, span, pcapture_ix, nu) : 0;
    free(order);
    if (unique_count == SIZE_MAX) { MLOG("[RED] geometry capture indices exceed their range or allocation"); return; }
    uint32_t vi[8] = {(uint32_t)span, (uint32_t)instances, 0, 0, (uint32_t)np, 0, 0, 0}; memcpy(pinfo, vi, 32);
    uint64_t ci[2] = {input, 0}; memcpy(pcapture_info, ci, 16);
    uint32_t cc[4] = {(uint32_t)first, (uint32_t)span, first_instance, (uint32_t)instances}; memcpy((uint8_t *)pcapture_info + 16, cc, 16);
    memset(pargs, 0, 20);
    nvn_geo_pack_job pj = {raw, packed, args, (uint32_t)groups, gs->geo_index_stride, gs->geo_output, (uint32_t)(packed_size / 4), cnt};
    memcpy(pjob, &pj, sizeof pj);
    vbuf_prepare();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    MTL4RenderPassDescriptor *rd = [MTL4RenderPassDescriptor new]; rd.renderTargetWidth = rd.renderTargetHeight = rd.defaultRasterSampleCount = 1;
    id<MTL4RenderCommandEncoder> re = enc_r_begin(rd); enc_r_pso(re, capture); bind_vbufs(re);
    p_draw vertex_range = {.first = (int32_t)first, .count = (int32_t)span, .instances = instances, .base_instance = (int32_t)first_instance};
    vconv_bind(re, vs, 1, &vertex_range, NULL);
    args_dirty_all(); encode_args(re, vs, 0, 1); enc_r_vbuf(re, 13, capture_info);
    if (di0) enc_r_draw_indexed(re, MTLPrimitiveTypePoint, unique_count, MTLIndexTypeUInt32, capture_ix, nu * 4, instances, di.base_vertex, first_instance);
    else enc_r_draw_instanced(re, MTLPrimitiveTypePoint, dw.first, dw.count, instances, first_instance);
    enc_r_end(re);
    g_geo_bind[0] = input; g_geo_bind[1] = output; g_geo_bind[2] = raw; g_geo_bind[3] = info; g_geo_bind[4] = map;
    id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL);
    args_dirty_all(); encode_args(ce, gs, 2, 0); enc_c_pso(ce, gs->geo_pso);
    enc_c_dispatch(ce, MTLSizeMake((np + 31) / 32, instances, gs->geo_invocations), MTLSizeMake(32, 1, 1));
    geo_pack_encode(ce, pack, job, (uint32_t)groups); enc_c_end(ce);
    args_dirty_all(); g_enc_pso = nil; g_enc_dss = nil; g_ed.ok = 0;
    swrite_note(pr); udc_note_draw(pr); g_wr_epoch++; g_mdx_epoch++;
    if (S.tf_on) tf_geometry(reader, packed, args, gs->geo_output == 1 ? 1 : gs->geo_output == 6 ? 2 : 3, first_instance);
    if (S.rast_discard) return;
    NvProg *raster = pr->geo_raster;
    if (!raster || raster->sh[0] != reader || raster->sh[1] != pr->sh[1]) {
        raster = [NvProg new]; raster->sh[0] = reader; raster->sh[1] = pr->sh[1]; pr->geo_raster = raster;
    }
    g_geo_program = raster; g_geo_indices = packed; g_geo_index_bytes = packed_size; g_geo_args = args;
    p_draw synthetic = {.prim = gs->geo_output == 1 ? 0 : gs->geo_output == 6 ? 1 : 4, .count = 3, .instances = 1, .base_instance = (int32_t)first_instance};
    draw(&synthetic, NULL);
    g_geo_program = nil; args_dirty_all();
    static int once; if (!once++) MLOG("geometry compute: vertex %s, geometry %s, %llu input primitives, %d instances", vs->key, gs->key, (unsigned long long)np, instances);
}
