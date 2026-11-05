// NVN2 task/mesh programs execute in Metal object/mesh stages. The normal raster
// state is shared with vertex draws; guest stages retain separate resource tables.
static int draw_raster_state(id<MTL4RenderCommandEncoder> en) {
    { uint32_t sr = S.st_ref[0];   // enc_r_stencil_ref sends the front value only
      if (!(g_ed.ok & ED_SREF) || g_ed.sref != sr) { enc_r_stencil_ref(en, sr, sr); g_ed.sref = sr; g_ed.ok |= ED_SREF; } }
    // Viewport / scissor (clamped within the target size)
    NSUInteger tw = g_pass.tw, th = g_pass.th;
    { MTLViewport v = S.has_vp ? (MTLViewport){S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3], S.dr[0], S.dr[1]} : (MTLViewport){0, 0, tw, th, 0, 1};
      if (atomic_load_explicit(&g_origin_ll, memory_order_relaxed)) { v.originY += v.height; v.height = -v.height; }   // lower-left origin (see g_origin_ll)
      // Viewport swizzle: NEGATIVE_Y negates clip y, the same as the viewport flipped about its centre; facing is decided after it on the GPU as in Metal, so the winding stays.
      // Other swizzles (axis exchanges, negated x / z / w) are no viewport change and are drawn unswizzled
      { uint32_t sw; memcpy(&sw, S.vp_sw[0], 4);
        if (sw == 0x100) { v.originY += v.height; v.height = -v.height; }
        else if (sw) RED_ONCE(108, "viewport swizzle %d,%d,%d,%d not implemented (drawn unswizzled)", S.vp_sw[0][0], S.vp_sw[0][1] ^ 2, S.vp_sw[0][2] ^ 4, S.vp_sw[0][3] ^ 6); }
      if (!(g_ed.ok & ED_VP) || memcmp(&g_ed.vp, &v, sizeof v)) { enc_r_viewport(en, v); g_ed.vp = v; g_ed.ok |= ED_VP; } } // NOLINT(bugprone-suspicious-memory-comparison): exact Metal state bits, including signed zero, are the cache key.
    MTLScissorRect sc = {0, 0, tw, th};
    if (R.has_sc && !S.rast_discard) {   // raster state cannot suppress vertex storage writes
        long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y <= 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
        if (x1 > (long)tw) x1 = tw; if (y1 > (long)th) y1 = th;
        if (x1 <= x0 || y1 <= y0) { PD("Z %llu draw skipped: empty scissor (%d,%d %dx%d, target %lux%lu)", g_stat_pass, R.sc.x, R.sc.y, R.sc.w, R.sc.h, (unsigned long)tw, (unsigned long)th); return 0; }
        sc = (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
    }
    if (!(g_ed.ok & ED_SC) || memcmp(&g_ed.sc, &sc, sizeof sc)) { enc_r_scissor(en, sc); g_ed.sc = sc; g_ed.ok |= ED_SC; }
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK && !S.rast_discard) { PD("Z %llu draw skipped: both faces culled", g_stat_pass); return 0; }
    { int cu = S.poly.cull == NVN_FACE_FRONT ? MTLCullModeFront : S.poly.cull == NVN_FACE_BACK ? MTLCullModeBack : MTLCullModeNone;
      int wi = (S.poly.front == NVN_FRONT_FACE_CCW) != g_queue_origin_ll ? MTLWindingCounterClockwise : MTLWindingClockwise;   // NVN's raster flip is captured when the queue is initialized, rather than when the device origin changes.
      int fi = S.poly.mode == NVN_POLYGON_MODE_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill, cl = !S.depth_clamp;   // depth clip off = SetDepthClamp
      float bi[3] = {0, 0, 0}; if (S.poly.offset_en & 4) { bi[0] = S.po.units; bi[1] = S.po.factor; bi[2] = S.po.clamp; }
      if (!(g_ed.ok & ED_CULL) || g_ed.cull != cu) { enc_r_cull(en, (MTLCullMode)cu); g_ed.cull = cu; g_ed.ok |= ED_CULL; }
      if (!(g_ed.ok & ED_WIND) || g_ed.wind != wi) { enc_r_winding(en, (MTLWinding)wi); g_ed.wind = wi; g_ed.ok |= ED_WIND; }
      if (!(g_ed.ok & ED_FILL) || g_ed.fill != fi) { enc_r_fill(en, (MTLTriangleFillMode)fi); g_ed.fill = fi; g_ed.ok |= ED_FILL; }
      if (!(g_ed.ok & ED_BIAS) || memcmp(g_ed.bias, bi, sizeof bi)) { enc_r_depth_bias(en, bi[0], bi[1], bi[2]); memcpy(g_ed.bias, bi, sizeof bi); g_ed.ok |= ED_BIAS; } // NOLINT(bugprone-suspicious-memory-comparison): exact Metal state bits, including signed zero, are the cache key.
      if (!(g_ed.ok & ED_CLIP) || g_ed.clip != cl) { enc_r_depth_clip(en, cl); g_ed.clip = cl; g_ed.ok |= ED_CLIP; }
      if (!(g_ed.ok & ED_BLEND) || memcmp(g_ed.blend, S.blend_color, sizeof g_ed.blend)) { enc_r_blend_color(en, S.blend_color[0], S.blend_color[1], S.blend_color[2], S.blend_color[3]); memcpy(g_ed.blend, S.blend_color, sizeof g_ed.blend); g_ed.ok |= ED_BLEND; } } // NOLINT(bugprone-suspicious-memory-comparison): exact Metal state bits, including signed zero, are the cache key.
    return 1;
}

typedef struct { pso_key raster; void *object_fn, *mesh_fn; } taskmesh_pso_key;
static nvn_hmap g_taskmesh_pso = {.klen = sizeof(taskmesh_pso_key)};
static id<MTLRenderPipelineState> taskmesh_pipeline(NvProg *pr) {
    NvShader *ms = pr->sh[NVN_SHADER_STAGE_MESH], *ts = pr->sh[NVN_SHADER_STAGE_TASK], *fs = pr->sh[1];
    id<MTLFunction> mf = ts ? ms->fn : ms->mesh_direct_fn;
    if (!mf || (ts && !ts->fn)) return nil;
    taskmesh_pso_key k = {0};
    pso_key_fill(&k.raster, ms, fs, g_pass.cfmt, g_pass.dfmt, g_pass.sfmt, g_pass.samples, 0);
    if (S.rast_discard) k.raster.fsfn = DISCARD_MARK;
    memset(k.raster.attr, 0, sizeof k.raster.attr); memset(k.raster.stream, 0, sizeof k.raster.stream);
    k.object_fn = ts ? (__bridge void *)ts->fn : NULL; k.mesh_fn = (__bridge void *)mf;
    id have = hm_get_id(&g_taskmesh_pso, &k);
    if (have) return have == NSNull.null ? nil : have;
    MTLRenderPipelineDescriptor *d = pso_desc(&k.raster, ms, S.rast_discard ? nil : fs, nil, 0);
    MTLMeshRenderPipelineDescriptor *m = [MTLMeshRenderPipelineDescriptor new];
    m.objectFunction = ts ? ts->fn : nil; m.meshFunction = mf; m.fragmentFunction = d.fragmentFunction;
    if (ts) m.maxTotalThreadsPerObjectThreadgroup = ts->mesh_threads;
    m.maxTotalThreadsPerMeshThreadgroup = ms->mesh_threads;
    m.payloadMemoryLength = ts ? ts->mesh_payload : 0;
    m.rasterSampleCount = d.rasterSampleCount; m.alphaToCoverageEnabled = d.alphaToCoverageEnabled;
    m.rasterizationEnabled = d.rasterizationEnabled;
    for (int i = 0; i < 8; i++) m.colorAttachments[i] = d.colorAttachments[i];
    m.depthAttachmentPixelFormat = d.depthAttachmentPixelFormat; m.stencilAttachmentPixelFormat = d.stencilAttachmentPixelFormat;
    NSError *e = nil;
    id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithMeshDescriptor:m options:MTLPipelineOptionNone reflection:nil error:&e];
    hm_put_id(&g_taskmesh_pso, &k, ps ?: NSNull.null);
    MLOG("%stask/mesh pipeline: task %s mesh %s fragment %s%s%s", ps ? "" : "[RED] ", ts ? ts->key : "-", ms->key, fs ? fs->key : "-", ps ? "" : ": ", ps ? "" : e.localizedDescription.UTF8String);
    return ps;
}

static void taskmesh_draw(const p_draw_mesh *d) {
    g_draw_binst = 0;   // task/mesh draws have no base instance for c0 + 0x644
    g_mdxc.next = NULL;
    if (!d->x || !d->y || !d->z) return;
    if (d->ind) { RED_ONCE(160, "indirect task/mesh draw has no decoder"); return; }
    NvProg *pr = S.prog ? prog_get(S.prog) : nil;
    NvShader *ms = pr ? pr->sh[NVN_SHADER_STAGE_MESH] : nil, *ts = pr ? pr->sh[NVN_SHADER_STAGE_TASK] : nil;
    if (!ms) { RED_ONCE(161, "task/mesh draw has no mesh program"); return; }
    // The vertex-only pass optimizations do not prove mesh work's coverage or
    // dependencies. Return to the full attachment pass at this command boundary.
    if (g_enc && (g_enc_mode || g_rt_merged || g_pass.layered || g_fz.active || g_sm_pass || g_rt_range_active)) rp_end();
    if (g_enc && g_rp_aw && (!R.has_sc || (long)R.sc.x + R.sc.w > (long)g_rp_aw || (long)R.sc.y + R.sc.h > (long)g_rp_ah)) rp_end();
    if (g_nubd) { rp_end(); ubd_land(); }
    image_prepare(pr, (1u << NVN_SHADER_STAGE_TASK) | (1u << NVN_SHADER_STAGE_MESH) | (1u << 1));
    order_draw(pr, 1);
    if (g_nlc && !g_enc) lc_take(pr);
    fb_prepare(pr);
    if (g_enc && g_vis_counting && g_vis_n >= VIS_SLOTS) rp_end();
    id<MTL4RenderCommandEncoder> en = (id<MTL4RenderCommandEncoder>)rp_begin_ex(0);
    if (!en) return;
    id<MTLRenderPipelineState> ps = taskmesh_pipeline(pr);
    if (!ps) return;
    enc_r_pso(en, ps); g_enc_pso = ps;
    id<MTLDepthStencilState> ds = dss_get(g_pass.hd, g_pass.hs);
    enc_r_dss(en, ds); g_enc_dss = ds;
    if (!draw_raster_state(en)) return;
    args_sync_dirty();
    [en setArgumentTable:g_mt atStages:MTLRenderStageMesh];
    if (ts) [en setArgumentTable:g_ot atStages:MTLRenderStageObject];
    if (ts) encode_args(en, ts, NVN_SHADER_STAGE_TASK, 0);
    encode_args(en, ms, NVN_SHADER_STAGE_MESH, 0);
    if (pr->sh[1] && !S.rast_discard) encode_args(en, pr->sh[1], 1, 0);
    if (g_enc != en) {
        // A texture view upgrade ends its old encoder; it is now materialized.
        static int again;
        if (again) { RED_ONCE(162, "task/mesh resource binding ended its pass twice"); return; }
        again = 1; taskmesh_draw(d); again = 0; return;
    }
    id<MTLBuffer> ab; size_t off = arena_alloc(16, &ab);
    uint32_t first[4] = {d->first, 0, 0, 0}; memcpy((uint8_t *)ab.contents + off, first, sizeof first);
    id<MTL4ArgumentTable> table = ts ? g_ot : g_mt;
    if (rec_on()) rec_table_op(RO_T_ADDR, table, ab.gpuAddress + off, 14); else [table setAddress:ab.gpuAddress + off atIndex:14];
    if (g_vis_counting && g_enc_has_vis) {
        if (g_vis_cur < 0 || g_vis_cur_pass != g_pass.serial) {
            g_vis_cur = (int)g_vis_n++; g_vis_cur_pass = g_pass.serial;
            if (g_nvs == g_vscap) { g_vscap = g_vscap ? g_vscap * 2 : 64; vis_slot *next = realloc(g_vs, g_vscap * sizeof *g_vs); if (!next) MDIE("visibility slot allocation failed"); g_vs = next; }
            g_vs[g_nvs++] = (vis_slot){(void *)CFBridgingRetain(g_vis), (uint32_t)g_vis_cur};
        }
        enc_r_visibility(en, MTLVisibilityResultModeCounting, (NSUInteger)g_vis_cur * 8); g_enc_vis = g_vis_cur + 1;
    }
    udc_note_draw(pr, draw_color_writes(pr)); swrite_note(pr); order_note(pr); rt_range_note_depth();
    [en drawMeshThreadgroups:MTLSizeMake(d->x, d->y, d->z) threadsPerObjectThreadgroup:MTLSizeMake(ts ? ts->mesh_threads : 1, 1, 1) threadsPerMeshThreadgroup:MTLSizeMake(ms->mesh_threads, 1, 1)];
    g_stat_draw++;
    if (passdump()) PD("mesh %llu first=%u groups=%u,%u,%u task=%s mesh=%s", g_stat_pass, d->first, d->x, d->y, d->z, ts ? ts->key : "-", ms->key);
}
