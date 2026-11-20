// Lines wider than a pixel (SetLineWidth): Metal draws 1-pixel lines only. A line draw (lines, a line loop or a strip) whose width rounds to 2 or more (OpenGL rounds a
// non-antialiased width to an integer) is drawn as a quad per segment, two triangles, as OpenGL draws a non-antialiased wide line (and Maxwell): a parallelogram w pixels
// across the segment's minor axis, its ends on the major axis, so consecutive segments meet (multisampled: OpenGL's rectangle across the segment, wl_encode). It is the game's draw itself, in its render pass, with the vertex shader's wide-line
// entry (dmsh_pull.py <entry>_wl, in the pulling library) in place of its vertex function and 6 vertices a segment in place of its count: each vertex finds its segment's two
// endpoints from its vertex ID and the line block (wl_block: the primitive, the count, the first or base vertex, the index buffer, read on the GPU, and the restart index), runs the
// shader for both, clips the segment in front of the eye (and the near plane with depth clipping on: zclip) and moves its position by half the width. The fragment shader,
// blending, depth and stencil are the draw's own; culling is off (a line has no faces), so a blended line is as heavy as one stroke; the polygon offset is the one enabled for
// lines. A deferred pass (defer_take) copies the draw and the state, and its replay comes back here; a merged pass's attachment map and fragment function reach wl_pso.
// Indirect arguments are read on the CPU, as the geometry and tessellation paths read theirs (geo_indirect_args). A restart index drops the segments it touches: a strip breaks
// there, as OpenGL's; a line list keeps its pairs (OpenGL starts a new pair after it), and an indexed loop with restart on stays 1 pixel wide, its runs' closing segments unknown.
// Drawn 1 pixel wide instead, as before, with a [RED] line naming the reason: a vertex shader without a wide-line entry (no pulling library: no vertex inputs, or a parameter
// the entry cannot pass on), an indexed loop with restart. Programs with a geometry or tessellation stage, transform feedback and adjacency are left to their own paths
// (1 pixel). DM_NVN_LINE_WIDTH=0 draws every line 1 pixel wide. Tested on the GPU by t_wline.m.
typedef struct { uint64_t ib; float vp[2]; float width; uint32_t prim, count; int32_t base; uint32_t isz, restart, ri, zclip, rect, pad; } wl_block;   // dmsh_pull.py dm_wline
_Static_assert(sizeof(wl_block) == 56, "wl_block: dmsh_pull.py dm_wline's layout");
typedef struct { int on, miss, rect; uint64_t block; wl_block *cpu; uint32_t count, instances; } wl_state;   // count: the vertices of the quads; rect: set at encode (wl_encode)
static wl_state g_wl;
static nvn_hmap g_pso_wl = {.klen = sizeof(pso_key)}, g_pso_wl_fail = {.klen = sizeof(pso_key)};   // wide-line pipelines by key, and the keys that failed (submit thread)
static uint64_t g_wl_draws, g_wl_segments;
static int wl_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_LINE_WIDTH"); v = !(e && !strcmp(e, "0")); } return v; }
static int wl_wanted(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    int prim = dw ? dw->prim : di->prim;
    return (prim == 1 || prim == 2 || prim == 3) && pr && pr->sh[0] && !pr->sh[2] && !pr->sh[3] && !pr->sh[4] && roundf(S.line_width) >= 2.0f && !S.rast_discard && !S.tf_on && wl_on();
}
// The draw's pipeline with the vertex shader's wide-line entry and no vertex descriptor (as a pulling pipeline: pso_desc_pull), and the fragment function and attachment map a merged
// pass gives it (g_pso_lmap, g_pso_fsfn, as pso_get); built on first need, then kept
static id<MTLRenderPipelineState> wl_pso(NvProg *pr) {
    NvShader *vs = pr->sh[0], *fs = pr->sh[1];
    if (!vs || !vs->wlfn) return nil;
    pso_key k; pso_key_fill(&k, vs, fs, g_pass.cfmt, g_pass.dfmt, g_pass.sfmt, g_pass.samples, g_pso_lmap != NULL);
    memset(k.attr, 0, sizeof k.attr); memset(k.attr_hi, 0, sizeof k.attr_hi); memset(k.stream, 0, sizeof k.stream);   // the pipeline pulls its streams: no vertex layout in it
    if (g_pso_lmap) { memcpy(k.lmap, g_pso_lmap, 8); k.fsfn = (__bridge void *)g_pso_fsfn; }
    id<MTLRenderPipelineState> h = hm_get_id(&g_pso_wl, &k);
    if (h || hm_get_id(&g_pso_wl_fail, &k)) return h;
    MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, fs, g_pso_fsfn, g_pso_lmap != NULL);
    d.vertexFunction = vs->wlfn; d.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
    NSError *e = nil; id<MTLRenderPipelineState> ps = pso_build(d, !k.border, &e);
    if (!ps) { hm_put_id(&g_pso_wl_fail, &k, NSNull.null); MLOG("[RED] wide-line pipeline (vertex %s fragment %s): %s", vs->key, fs ? fs->key : "-", e.localizedDescription.UTF8String); return nil; }
    hm_put_id(&g_pso_wl, &k, ps);
    return ps;
}
// 1: drawn (or nothing to draw); 0: the caller draws the line 1 pixel wide
static int wl_draw(NvProg *pr, const p_draw *dw0, const p_draw_idx *di0) {
    NvShader *vs = pr->sh[0];
    static int red;
    #define WL_NO(why) do { if (red++ < 6) MLOG("[RED] wide line (width %.2f, vertex %s) drawn 1 pixel wide: %s", S.line_width, vs->key, why); return 0; } while (0)
    (void)pull_fn(vs); if (!vs->wlfn) WL_NO("its vertex shader has no wide-line entry (pulling library)");
    if ((dw0 && dw0->mdi_cnt && !mdi_live(dw0->mdi_cnt, dw0->mdi_i)) || (di0 && di0->mdi_cnt && !mdi_live(di0->mdi_cnt, di0->mdi_i))) return 1;   // past a multi-draw's count
    p_draw dwl = dw0 ? *dw0 : (p_draw){0}; p_draw_idx dil = di0 ? *di0 : (p_draw_idx){0};   // an indirect draw's arguments go into these copies
    const p_draw *dw = dw0 ? &dwl : NULL; const p_draw_idx *di = di0 ? &dil : NULL;
    if (di && (di->itype < 0 || di->itype > 2)) WL_NO("index type");
    if (dw ? dw->ind : di->ind) {
        int drained = 0, r = geo_indirect_args(dw ? &dwl : NULL, di ? &dil : NULL, &drained, "wide line");
        if (r < 0) WL_NO("indirect arguments");
        if (!r) return 1;
    }
    int prim = dw ? dw->prim : di->prim, count = dw ? dw->count : di->count, restart = di && S.restart.on;
    if (prim == 2 && restart) WL_NO("an indexed line loop with primitive restart");
    uint64_t nseg = count < 2 ? 0 : prim == 1 ? (uint64_t)count / 2 : prim == 3 ? (uint64_t)count - 1 : (uint64_t)count;
    if (!nseg || (dw ? dw->instances : di->instances) < 1) return 1;
    if (nseg > (UINT32_MAX - 6) / 6) WL_NO("too many segments");
    uint32_t isz = di ? 1u << di->itype : 0; uint64_t ib = 0;
    if (di) { void *mb; uint64_t mo; if (!mtl_gpu_addr(di->ibuf, (uint64_t)count * isz, &mb, &mo, &ib)) WL_NO("index buffer not in GPU memory"); }
    NSUInteger vw, vh; bare_dims(&vw, &vh);
    float width = fminf(roundf(S.line_width), 64.0f);
    id<MTLBuffer> bb; size_t bo = arena_alloc(sizeof(wl_block), &bb);
    wl_block lb = {ib, {S.has_vp ? fabsf(S.vp[0][2]) * 0.5f : vw * 0.5f, S.has_vp ? fabsf(S.vp[0][3]) * 0.5f : vh * 0.5f}, width, (uint32_t)prim, (uint32_t)count,
                   di ? di->base_vertex : dw->first, isz, (uint32_t)restart, S.restart.index & (isz == 1 ? UINT8_MAX : isz == 2 ? UINT16_MAX : UINT32_MAX), (uint32_t)!S.depth_clamp, 0, 0};
    memcpy((uint8_t *)bb.contents + bo, &lb, sizeof lb);
    g_wl = (wl_state){.on = 1, .block = bb.gpuAddress + bo, .cpu = (wl_block *)((uint8_t *)bb.contents + bo), .count = (uint32_t)(nseg * 6), .instances = (uint32_t)(dw ? dw->instances : di->instances)};
    draw(dw, di);   // the game's draw, with the quads' pipeline and count (draw_: g_wl)
    int miss = g_wl.miss, rect = g_wl.rect; memset(&g_wl, 0, sizeof g_wl);
    if (miss) WL_NO("no wide-line pipeline");
    g_wl_draws++; g_wl_segments += nseg;
    static int once; if (once++ < 3) MLOG("wide lines: width %.0f (vertex %s, fragment %s), %llu segments drawn as %s%s", width, vs->key, pr->sh[1] ? pr->sh[1]->key : "-",
                                          (unsigned long long)nseg, rect ? "rectangles (multisampled)" : "quads", di ? ", indexed" : "");
    if (g_wl_draws == 30 || g_wl_draws == 300 || g_wl_draws % 3000 == 0)
        MLOG("wide lines up to frame %llu: %llu draws, %llu segments drawn as quads", (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)g_wl_draws, (unsigned long long)g_wl_segments);
    return 1;
    #undef WL_NO
}
// At the draw's encode, in its pass: with multisampling on (more than one sample, and the multisample state enables it) a segment is a rectangle across it, as OpenGL draws a wide
// line then, not the parallelogram (dmsh_pull.py dm_wl_offset); then the quads' draw
static void wl_encode(id<MTLRenderCommandEncoder> en_) {
    id<MTL4RenderCommandEncoder> en = (id<MTL4RenderCommandEncoder>)en_;
    g_wl.rect = g_pass.samples > 1 && S.ms.enable; g_wl.cpu->rect = (uint32_t)g_wl.rect;
    enc_r_vbuf(en, 15, g_wl.block); enc_r_draw_instanced(en, MTLPrimitiveTypeTriangle, 0, g_wl.count, g_wl.instances, (uint32_t)g_draw_binst);
}
