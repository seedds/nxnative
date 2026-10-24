// nvn_mtl.m, part: draw
// Draw and dispatch, buffer / texture copies, clears a load action cannot express, the cut counters and the replay executor mtl_exec.
// Included by nvn_mtl.m in its original order, as one translation unit: statics are shared with the other parts.
static void draw(const p_draw *dw, const p_draw_idx *di) {
    g_draw_binst = (uint32_t)(dw ? dw->base_instance : di ? di->base_instance : 0);   // c0 + 0x644 of the stages that read the driver segment (encode_args)
    if (!g_geo_program && S.prog) {
        NvProg *geometry_program = prog_get(S.prog);
        if (geometry_program && geometry_program->sh[2] && geometry_program->sh[2]->geo_vertices) {
            if ((dw && dw->mdi_cnt && !mdi_live(dw->mdi_cnt, dw->mdi_i)) || (di && di->mdi_cnt && !mdi_live(di->mdi_cnt, di->mdi_i))) return;
            geo_draw(geometry_program, dw, di); return;
        }
    }
    int mdx_on = (dw && dw->mdi_cnt) || (di && di->mdi_cnt) ? mdx_wanted(dw, di) : 0;   // an indirect-count multi-draw's record masked on the GPU (mdx_run), else read after a wait
    if (!mdx_on && ((di && di->mdi_cnt && !mdi_live(di->mdi_cnt, di->mdi_i)) || (dw && dw->mdi_cnt && !mdi_live(dw->mdi_cnt, dw->mdi_i)))) return;   // past the multi-draw's count
    if (mdx_on && mdx_next(dw, di)) return;   // the next draw of a masked multi-draw: only the draw is encoded
    g_mdxc.next = NULL;
    if (!S.prog) { RED_ONCE(18, "no program bound before the draw"); PD("Z %llu draw skipped: no program bound", g_stat_pass + !g_enc); return; }
    __unsafe_unretained NvProg *pr = g_geo_program ? g_geo_program : prog_get(S.prog);
    draw_gs_check(pr);   // a geometry stage left with no function (see prog_fold_gs)
    if (S.rast_discard) { tf_draw(pr, dw, di); return; }   // the vertex stage only: transform feedback capture, or nothing (tf_draw)
    if (g_nubd && (g_enc || ubd_lazy()) && ubd_conflict(pr, dw, di)) { g_ubd_conflicts++; rp_end(); ubd_land(); }   // land the deferred inline updates before this draw (see ubd_conflict)
    if (zskip_can_draw(pr)) {   // Zero-source skip: the output is always 0, writing 0 over 0, so this draw changes no result (see zskip_can_draw)
        g_zskip_count++; { static int once; if (!once++) MLOG("zero-source skip: first case pass %llu", g_stat_pass + 1); }
        PD("N %llu zero-source draw skipped", g_stat_pass + 1); return; }
    if (g_nlc && !g_enc) lc_take(pr);   // waiting clears: this pass's load actions, or landed before it (see lc_take)
    if (ixc_wanted(dw, di) && !ixc_find(dw, di)) ixc_run(dw, di);   // indirect quads / fans / 8-bit indices: converted on the GPU before the render pass, at their own place in the replay (see ixc_run)
    uint64_t mdx = 0;   // the masked copy of the record (mdx_run); a draw it could not copy waits for the count instead
    if (mdx_on) { uint64_t ind = dw ? dw->ind : di->ind, cnt = dw ? dw->mdi_cnt : di->mdi_cnt; int i = dw ? dw->mdi_i : di->mdi_i;
        if (!mdx_find(ind, cnt, &mdx)) { mdx_run(dw, di); if (!mdx_find(ind, cnt, &mdx)) mdx = 0; }
        if (!mdx && !mdi_live(cnt, i)) return; }
    if (di && wide_wanted(di) && !wide_find(di)) wide_run(di);   // 8-bit indices written earlier in this submission: widened on the GPU before the render pass (see wide_run)
    if (!g_defer_replaying && !g_geo_program) {
        if (g_defer.active && g_defer.in_p && !g_defer.at_q) { if (defer_take(dw, di)) return; }
    }
    // Draws that cannot draw anything (both faces culled, empty scissor) return before the render pass is opened: otherwise the render pass is already open and on TBDR the whole attachment is loaded and stored for nothing
    // (Mac character select page: 3 render passes are opened only because of such draws, all loaded, zero draws)
    // Diagnostic switch for measuring an upper bound DM_NVN_SKIP_FS=<fragment key>[,<fragment key>...] (none by default): drops all draws of these fragment shaders (the picture will be wrong; only used in real-device ABA to measure "how much GPU time this class of draws is worth in total")
    { static char sk[16][20]; static int nsk = -1;
      if (nsk < 0) { nsk = 0; for (const char *q = getenv("DM_NVN_SKIP_FS"); q && *q && nsk < 16; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) { size_t n = strcspn(q, ","); if (n > 16) n = 16; memcpy(sk[nsk], q, n); sk[nsk++][n] = 0; }
                     if (nsk) MLOG("DM_NVN_SKIP_FS: dropping the draws of %d fragment shaders (diagnostic; the picture is wrong)", nsk); }
      if (g_ub_n > 0 && pr->sh[1] && !g_defer_replaying && ub_skip(pr->sh[1]->key)) return;   // In-process alternating measurement: this phase drops this class
      if (nsk && pr->sh[1] && !g_defer_replaying) for (int i = 0; i < nsk; i++) if (!strncmp(pr->sh[1]->key, sk[i], strlen(sk[i]))) return;
      if (ms_draw(pr)) return; }
    // Similar diagnostic DM_NVN_SKIP_SMALL=<width>: drop all draws whose target width (by the attached level) is <= this value -- measures how much "dropping tiny render passes in bulk" is worth under the parallel pipeline (fixed per-pass overhead)
    { static int small = -1; if (small < 0) { const char *e = getenv("DM_NVN_SKIP_SMALL"); small = e ? atoi(e) : 0; if (small) MLOG("DM_NVN_SKIP_SMALL: dropping draws whose target width is ≤ %d (diagnostic; the picture is wrong)", small); }
      if (small > 0 && R.has_rt && !g_defer_replaying) { int k = R.rt.n && R.rt.color[0] ? 0 : R.rt.depth ? 8 : -1;
        if (k >= 0) { uint64_t s_; int lv, sl; rt_sub(&R.rt, k, &s_, &lv, &sl); const tex_t *t = k == 8 ? R.rt.depth : R.rt.color[0]; if ((int)MAX(t->d.width >> lv, 1u) <= small) return; } } }
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK) { PD("Z %llu draw skipped (before the pass begins): both faces culled", g_stat_pass + !g_enc); return; }
    // The continued render pass may have been opened with fewer depth / stencil attachments (open modes 2 / 3 / 4): if the plane this draw needs is not attached, split before it (on reopen, attach per the new lookahead)
    if (g_rt_merged && g_enc_mode != 0 && (((S.ds.dt || S.ds.dw) && !g_pass.hd) || (S.ds.st && !g_pass.hs))) {
        static int once2; if (!once2++) MLOG("same-target continuation: a draw in the continued pass needs a depth / stencil plane that is not attached; split here (first case pass %llu)", g_stat_pass);
        PD("M %llu a draw in the continued pass needs a depth / stencil plane that is not attached; split here", g_stat_pass); rp_end(); }
    if (g_rt_range_active && (!rt_range_pr_ro(pr) || !S.ds.dt || S.ds.dw || S.ds.st)) rp_end();   // The external write visibility at the original encoder boundary is not removed
    if (g_rt_merged && draw_samples_rt(pr)) { static int once; if (!once++) MLOG("same-target continuation: a draw in the continued pass samples an attachment; split here (first case pass %llu)", g_stat_pass);
        PD("M %llu a draw in the continued pass samples an attachment; split here", g_stat_pass); rp_end(); }
    static id<MTLFunction> fz_fn;   // Specialized fragment function for this draw of the later pass in a merged render pass (nil = use the original version)
    if (g_fz.active && g_enc && g_fz.phase == 2 && !fz_p2_draw(pr, dw, di, g_fz.lmap2, &fz_fn)) {   // Cannot be proven: end the merged render pass (the earlier pass's result is stored as usual) and reopen with the later pass's original targets
        static int once3; if (!once3++) MLOG("readable-tile pass merge: a draw of the later pass is not provable; back to separate passes (first case pass %llu)", g_stat_pass);
        g_fz_fallback++; rp_end(); }
    fb_prepare(pr);   // same-pass feedback sampling: copy the snapshot first (see fb_prepare)
    if (R.has_sc && !g_enc) {
        NSUInteger tw0 = 0, th0 = 0; rt_dims(&tw0, &th0);
        long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
        if (x1 > (long)tw0) x1 = tw0; if (y1 > (long)th0) y1 = th0;
        if (x1 <= x0 || y1 <= y0) { PD("Z %llu draw skipped (before the pass begins): empty scissor (%d,%d %dx%d, target %lux%lu)", g_stat_pass + 1, R.sc.x, R.sc.y, R.sc.w, R.sc.h, (unsigned long)tw0, (unsigned long)th0); return; }
    }
#ifdef NVN_RT_RANGE_TEST
    if (!g_enc) rr_begin();
#endif
    int defer_q_draw = !g_enc && !g_defer_replaying && g_defer_natq > 0;   // The draw that opens Q's render pass: after the render pass is opened as usual through hoist_try / fuse_try, replay P first and then draw this one
    if (!g_enc && tbdr_on() && !g_geo_program) hoist_try();   // First hoist and execute the pure-clear render passes sandwiched between this render pass and its continuation (before rp_begin_ex, not occupying this render pass)
    // The deferral decision comes after hoist_try: hoisted pure-clear render passes are skipped in the lookahead, and only then can P's same-target continuation segment be recognized
    if (!g_enc && tbdr_on() && !g_geo_program && !defer_has_in_p() && !g_defer_replaying && !g_fz.active && !g_fz_checking && g_exec_next) { defer_try(dw, di); if (g_defer_cur->active && g_defer_cur->in_p) return; }
    if (!g_enc && tbdr_on() && !g_fz_checking && !g_geo_program) fuse_try();   // Readable tile render pass merge is on by default; when the structure / same-pixel proof does not hold it still falls back to a normal render pass, and it is not re-entered during the CHECK redraw
    int need = g_enc || !tbdr_on() ? 3 : pass_ds_need(), mode = defer_q_draw ? 0 : need == 0 ? 2 : need == 2 ? 3 : need == 1 ? 4 : 0;   // 2 attach neither, 3 attach stencil only, 4 attach depth only
    int skip_d = mode != 0;
    if (!g_enc) { g_fd_dw = dw; g_fd_di = di; }
    g_rp_layered = pr->sh[0] && pr->sh[0]->layer_out;
    if (g_enc && g_pass.layered != g_rp_layered) rp_end();   // a layered pass only for layer-writing vertex shaders (see rp_begin_ex)
    id<MTLRenderCommandEncoder> en = rp_begin_ex(mode);
    g_rp_layered = 0;
    g_fd_dw = NULL; g_fd_di = NULL;
    if (!en) { PD("Z %llu draw skipped: the render pass did not begin vs=%.16s fs=%.16s", g_stat_pass, pr && pr->sh[0] ? pr->sh[0]->key : "-", pr && pr->sh[1] ? pr->sh[1]->key : "-"); return; }   // Z lines: why a draw was dropped at these early returns
    if (defer_q_draw) defer_replay();
    if (skip_d) { g_stat_ds_omit++; if (mode == 3) g_stat_s_only++; else if (mode == 4) g_stat_d_only++; }
    // The pipeline depends only on (program, render pass targets, blend / color / write mask / multisample state, vertex layout): if these did not change, reuse the previous one
    static id<MTLRenderPipelineState> cur_ps; static NvProg *cur_pr; static uint64_t cur_ps_pass; static int cur_pull;   // cur_pull: a hybrid (pulling) pipeline, asked for again at the next draw
    if (g_fz.active && !g_defer_replaying) { g_pso_lmap = g_fz.phase == 1 ? g_fz.lmap1 : g_fz.lmap2; g_pso_fsfn = g_fz.phase == 1 ? nil : fz_fn; g_pso_fz = g_fz.phase == 1 ? (pso_fz_desc){0} : g_fz_desc; g_pso_dirty = 1; }   // Readable tile merge: fetch the pipeline by the physical -> logical mapping and the specialization
    if (g_pso_dirty || pr != cur_pr || cur_ps_pass != g_pass.serial) { cur_ps = pso_get(pr, nil); cur_pull = g_pso_pull; cur_pr = pr; cur_ps_pass = g_pass.serial; g_pso_dirty = !cur_ps || cur_pull; }
    if (g_pso_lmap) { g_pso_lmap = NULL; g_pso_fsfn = nil; memset(&g_pso_fz, 0, sizeof g_pso_fz); g_pso_dirty = 1; }   // The next draw fetches again according to its own situation
    __unsafe_unretained id<MTLRenderPipelineState> ps = cur_ps;
    if (!ps) { PD("Z %llu draw skipped: no render pipeline (vertex %s fragment %s)", g_stat_pass, pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[1] ? pr->sh[1]->key : "-"); return; }
    if (ps != g_enc_pso) { enc_r_pso(en, ps); g_enc_pso = ps; }
    static id<MTLDepthStencilState> cur_ds; static uint64_t cur_ds_pass;
    // Diagnostic DM_NVN_DEPTHOVR=fragment shader key prefix,...: those draws compare depth with ALWAYS, which tells "the depth test rejects every fragment" from "nothing rasterizes" (with DM_NVN_HILITE); unset = unchanged
    static char dko[8][24]; static int ndko = -1;
    if (ndko < 0) { ndko = 0; const char *e = getenv("DM_NVN_DEPTHOVR");
        for (const char *q = e; q && *q && ndko < 8; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) { size_t n = strcspn(q, ","); if (n > 23) n = 23; memcpy(dko[ndko], q, n); dko[ndko++][n] = 0; } }
    if (ndko) { int ovr = 0; for (int i = 0; i < ndko && pr->sh[1]; i++) ovr |= !strncmp(pr->sh[1]->key, dko[i], strlen(dko[i]));
        unsigned df0 = (unsigned)S.ds.df; if (ovr) S.ds.df = 8; cur_ds = dss_get(g_pass.hd, g_pass.hs); S.ds.df = df0; cur_ds_pass = g_pass.serial; g_dss_dirty = 0; }
    else if (g_dss_dirty || cur_ds_pass != g_pass.serial) { cur_ds = dss_get(g_pass.hd, g_pass.hs); cur_ds_pass = g_pass.serial; g_dss_dirty = 0; }
    if (cur_ds != g_enc_dss) { enc_r_dss(en, cur_ds); g_enc_dss = cur_ds; }
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
      if (!(g_ed.ok & ED_VP) || memcmp(&g_ed.vp, &v, sizeof v)) { enc_r_viewport(en, v); g_ed.vp = v; g_ed.ok |= ED_VP; } }
    MTLScissorRect sc = {0, 0, tw, th};
    if (R.has_sc) {
        long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y <= 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
        if (x1 > (long)tw) x1 = tw; if (y1 > (long)th) y1 = th;
        if (x1 <= x0 || y1 <= y0) { PD("Z %llu draw skipped: empty scissor (%d,%d %dx%d, target %lux%lu)", g_stat_pass, R.sc.x, R.sc.y, R.sc.w, R.sc.h, (unsigned long)tw, (unsigned long)th); return; }
        sc = (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
    }
    if (!(g_ed.ok & ED_SC) || memcmp(&g_ed.sc, &sc, sizeof sc)) { enc_r_scissor(en, sc); g_ed.sc = sc; g_ed.ok |= ED_SC; }
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK) { PD("Z %llu draw skipped: both faces culled", g_stat_pass); return; }
    { int cu = S.poly.cull == NVN_FACE_FRONT ? MTLCullModeFront : S.poly.cull == NVN_FACE_BACK ? MTLCullModeBack : MTLCullModeNone;
      int wi = (S.poly.front == NVN_FRONT_FACE_CCW) != atomic_load_explicit(&g_origin_ll, memory_order_relaxed) ? MTLWindingCounterClockwise : MTLWindingClockwise;   // the y flip of lower-left mode reverses the winding
      int fi = S.poly.mode == NVN_POLYGON_MODE_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill, cl = !S.depth_clamp;   // depth clip off = SetDepthClamp
      float bi[3] = {0, 0, 0}; if (S.poly.offset_en & 4) { bi[0] = S.po.units; bi[1] = S.po.factor; bi[2] = S.po.clamp; }
      if (!(g_ed.ok & ED_CULL) || g_ed.cull != cu) { enc_r_cull(en, (MTLCullMode)cu); g_ed.cull = cu; g_ed.ok |= ED_CULL; }
      if (!(g_ed.ok & ED_WIND) || g_ed.wind != wi) { enc_r_winding(en, (MTLWinding)wi); g_ed.wind = wi; g_ed.ok |= ED_WIND; }
      if (!(g_ed.ok & ED_FILL) || g_ed.fill != fi) { enc_r_fill(en, (MTLTriangleFillMode)fi); g_ed.fill = fi; g_ed.ok |= ED_FILL; }
      if (!(g_ed.ok & ED_BIAS) || memcmp(g_ed.bias, bi, sizeof bi)) { enc_r_depth_bias(en, bi[0], bi[1], bi[2]); memcpy(g_ed.bias, bi, sizeof bi); g_ed.ok |= ED_BIAS; }
      if (!(g_ed.ok & ED_CLIP) || g_ed.clip != cl) { enc_r_depth_clip(en, cl); g_ed.clip = cl; g_ed.ok |= ED_CLIP; }
      if (!(g_ed.ok & ED_BLEND) || memcmp(g_ed.blend, S.blend_color, sizeof g_ed.blend)) { enc_r_blend_color(en, S.blend_color[0], S.blend_color[1], S.blend_color[2], S.blend_color[3]); memcpy(g_ed.blend, S.blend_color, sizeof g_ed.blend); g_ed.ok |= ED_BLEND; } }
    if (cur_pull) { pull_bind(en); g_hy_draws++; }   // a pulling pipeline reads the streams itself (no conversion either: dm_fetch decodes every format)
    else {
        bind_vbufs(en);
        if (pr->sh[0]) vconv_bind(en, pr->sh[0], 0, dw, di);   // integer inputs fed float-type formats or 8 / 16-bit integers of the other sign (see vconv_ncomp, vconv_int)
    }
    g_kd_force = kd_draw(pr, dw, di);
    if (pr->sh[0]) encode_args(en, pr->sh[0], 0, 1);
    if (pr->sh[1]) encode_args(en, pr->sh[1], 1, 0);
    // Parameter binding captures the actual resources under the registry lock; if the version changed between the eligibility check and the binding, split and retry before any GPU draw.
    if (g_rt_range_active && (g_pass.reg_gen != g_reg_gen || g_pass.ser_gen != g_ser_gen || g_pass.range_dirty != nvn_dirty_now())) { rp_end(); draw(dw, di); return; }
    int prim = dw ? dw->prim : di->prim;
    MTLPrimitiveType mp;
    switch (prim) {
        case 0: mp = MTLPrimitiveTypePoint; break; case 1: mp = MTLPrimitiveTypeLine; break; case 3: mp = MTLPrimitiveTypeLineStrip; break;
        case 4: case 6: case 7: mp = MTLPrimitiveTypeTriangle; break; case 5: mp = MTLPrimitiveTypeTriangleStrip; break;   // 6 triangle fan, 7 quads: Metal has neither, expanded into triangle lists
        default: { static int n; if (n++ < 8) MLOG("[RED] primitive type %d not implemented (vertex %s hull %s domain %s fragment %s; draw skipped)", prim, pr->sh[0] ? pr->sh[0]->key : "-",
                                                    pr->sh[3] ? pr->sh[3]->key : "-", pr->sh[4] ? pr->sh[4]->key : "-", pr->sh[1] ? pr->sh[1]->key : "-"); }
            PD("Z %llu draw skipped: primitive type %d", g_stat_pass, prim); return;
    }
    if (g_vis_counting) {   // Occlusion query counting: a new slot for this render pass (also switch to a new slot after a report / reset)
        if (!g_enc_has_vis) RED_ONCE(34, "occlusion query counting began in a pass without a visibility buffer (the first reset happened mid-pass); that pass's samples are not counted");
        else {
            if (g_vis_cur < 0 || g_vis_cur_pass != g_pass.serial) {
                if (g_vis_n >= VIS_SLOTS) RED_ONCE(35, "visibility slots exhausted");
                else { g_vis_cur = (int)g_vis_n++; g_vis_cur_pass = g_pass.serial;
                    if (g_nvs == g_vscap) { g_vscap = g_vscap ? g_vscap * 2 : 64; g_vs = realloc(g_vs, g_vscap * sizeof *g_vs); }
                    g_vs[g_nvs++] = (vis_slot){(void *)CFBridgingRetain(g_vis), (uint32_t)g_vis_cur}; }
            }
            if (g_vis_cur >= 0 && g_enc_vis != g_vis_cur + 1) { enc_r_visibility(en, MTLVisibilityResultModeCounting, (NSUInteger)g_vis_cur * 8); g_enc_vis = g_vis_cur + 1; }
        }
    }
    g_stat_draw++;
    if (enc_label_on() && !g_el_draws++ && pr->sh[1]) { memcpy(g_el_key, pr->sh[1]->key, 8); g_el_key[8] = 0; }
    if (passdump()) {   // Characteristics of a render pass's first draw (to judge whether the load can be omitted: full screen, no blending, writes all channels, does not depend on depth); with DM_NVN_PASSDUMP_ALL every draw is recorded
        static uint64_t last_pass = ~0ull; static int all = -1; if (all < 0) all = getenv("DM_NVN_PASSDUMP_ALL") != NULL;
        if (all || g_kd_force) PD("d %llu bv=%d bi=%d first=%d vs=%s fs=%s vc=%d inst=%d blend=%#x b0=%#llx dt=%d dw=%d df=%d cull=%d st=%d sf=%d,%d sop=%d,%d,%d/%d,%d,%d smask=%#x,%#x/%#x,%#x sref=%#x,%#x ib=%#llx it=%d cmask=%#x b1=%#llx b2=%#llx b3=%#llx po=%d/%g,%g,%g dc=%d", g_stat_pass, di ? di->base_vertex : 0, di ? di->base_instance : 0, dw ? dw->first : 0, pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[1] ? pr->sh[1]->key : "-",
                    dw ? dw->count : di->count, dw ? dw->instances : di->instances, S.color.blend_en, (unsigned long long)*(uint64_t *)&S.blend[0], (int)S.ds.dt, (int)S.ds.dw, (int)S.ds.df, (int)S.poly.cull,
                    (int)S.ds.st, (int)S.ds.sf_f, (int)S.ds.sf_b, (int)S.ds.fail_f, (int)S.ds.zfail_f, (int)S.ds.zpass_f,
                    (int)S.ds.fail_b, (int)S.ds.zfail_b, (int)S.ds.zpass_b, S.st_vmask[0], S.st_vmask[1], S.st_wmask[0], S.st_wmask[1], S.st_ref[0], S.st_ref[1],
                    di ? (unsigned long long)di->ibuf : 0ull, di ? di->itype : 0, S.chmask.mask, (unsigned long long)*(uint64_t *)&S.blend[1], (unsigned long long)*(uint64_t *)&S.blend[2], (unsigned long long)*(uint64_t *)&S.blend[3],
                    (int)S.poly.offset_en, S.po.factor, S.po.units, S.po.clamp, S.depth_clamp);
        if (all && pr->sh[0]) {   // Content fingerprint of each constant buffer of the vertex stage (to judge whether a draw is completely identical across frames: used by the shadow cache etc.); when unreadable (a write pending execution in this submit) record P
            char ub[400]; size_t o = 0; NvShader *v = pr->sh[0];
            for (int i = 0; i < v->rf.nb && o < sizeof ub - 40; i++) if (v->rf.b[i].kind == NVN_RB_CBUF && v->rf.b[i].reg < 16) {
                const p_bind_buf *u = &S.ubo[0][v->rf.b[i].reg]; void *mb; uint64_t mo; const uint8_t *base; uint64_t h = 1469598103934665603ull;
                if (!u->addr || !u->size) { o += snprintf(ub + o, sizeof ub - o, " r%d=-", v->rf.b[i].reg); continue; }
                if (pend_hits(u->addr, u->size) || !mtl_addr(u->addr, u->size, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) { o += snprintf(ub + o, sizeof ub - o, " r%d=P/%llu", v->rf.b[i].reg, (unsigned long long)u->size); continue; }
                uint64_t h64 = 0; for (uint64_t q = 0; q < u->size; q++) { h = (h ^ base[mo + q]) * 1099511628211ull; if (q == 63) h64 = h; }   // h64: the first 64 bytes (world matrix vec0–3 of the object buffer)
                { static uint64_t hp = ~0ull; static int hn; if (hp != g_stat_pass) { hp = g_stat_pass; hn = 0; }   // For the first 6 draws of each render pass, additionally print buffers of <= 608 bytes in hex (to compare byte by byte which fields changed across frames)
                  if (hn < 12 && u->size <= 608 && v->rf.b[i].reg == 3) { hn++; char hx[1300]; for (uint64_t q = 0; q < u->size; q++) snprintf(hx + 2 * q, 3, "%02x", base[mo + q]); PD("x %llu r%d %s", g_stat_pass, v->rf.b[i].reg, hx); } }
                o += snprintf(ub + o, sizeof ub - o, " r%d=%016llx/%llu/%016llx", v->rf.b[i].reg, (unsigned long long)h, (unsigned long long)u->size, (unsigned long long)h64); }
            PD("u %llu%s", g_stat_pass, ub); }
        if (all) { char vb[1600]; size_t o = 0; o += snprintf(vb + o, sizeof vb - o, "nattr=%d", S.nattr);
            for (int i = 0; i < S.nattr && i < 16 && o < sizeof vb - 100; i++) { int st = S.attr[i].stream; if (!S.attr[i].fmt) continue;   // only attributes with a format
                o += snprintf(vb + o, sizeof vb - o, " a%d{fmt=%#x off=%d s=%d stride=%u div=%u vb=%#llx/%llu}", i, S.attr[i].fmt, S.attr[i].offset, st,
                              st < 16 ? S.stream[st].stride : 0, st < 16 ? S.stream[st].divisor : 0, st < 16 ? (unsigned long long)S.vbuf[st].address : 0ull, st < 16 ? (unsigned long long)S.vbuf[st].size : 0ull); }
            PD("v %llu %s", g_stat_pass, vb);
            // DM_NVN_CBDUMP + DM_NVN_CBDUMP_KEY=vertex shader key: also save that draw's vertex streams and indices (F<frame>_p<pass>_vb<stream>_<address>.bin / _ib_<address>.bin)
            const char *cbd = getenv("DM_NVN_CBDUMP"), *cbk = getenv("DM_NVN_CBDUMP_KEY");
            if (cbd && cbk && pr->sh[0] && !strcmp(pr->sh[0]->key, cbk)) {
                char fp[1024];
                for (int st = 0; st < 16; st++) if (S.vbuf[st].address && S.vbuf[st].size) {
                    snprintf(fp, sizeof fp, "%s/F%llu_p%llu_vb%d_%llx.bin", cbd, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, st, (unsigned long long)S.vbuf[st].address);
                    FILE *f = fopen(fp, "wb"); if (f) { fwrite((const void *)S.vbuf[st].address, 1, S.vbuf[st].size, f); fclose(f); } }
                if (di && di->ibuf) {
                    snprintf(fp, sizeof fp, "%s/F%llu_p%llu_ib%d_%llx.bin", cbd, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, di->itype, (unsigned long long)di->ibuf);
                    FILE *f = fopen(fp, "wb"); if (f) { fwrite((const void *)di->ibuf, 1, (size_t)di->count << (di->itype == 2 ? 2 : di->itype), f); fclose(f); } }
            } }
        if (last_pass != g_stat_pass) {
            last_pass = g_stat_pass;
            PD("D %llu prim=%d vc=%d inst=%d blend=%#x wm=%#x dt=%d dw=%d df=%d st=%d sc=%lu,%lu,%lu,%lu tgt=%lux%lu vp=%.0f,%.0f,%.0f,%.0f dr=%g,%g lo=%u", g_stat_pass, prim,
               dw ? dw->count : di->count, dw ? dw->instances : di->instances, S.color.blend_en, S.chmask.mask, (int)S.ds.dt, (int)S.ds.dw, (int)S.ds.df, (int)S.ds.st,
               (unsigned long)sc.x, (unsigned long)sc.y, (unsigned long)sc.width, (unsigned long)sc.height, (unsigned long)tw, (unsigned long)th,
               S.has_vp ? S.vp[0][0] : 0, S.has_vp ? S.vp[0][1] : 0, S.has_vp ? S.vp[0][2] : (float)tw, S.has_vp ? S.vp[0][3] : (float)th, S.has_vp ? S.dr[0] : 0.f, S.has_vp ? S.dr[1] : 1.f, (unsigned)S.color.logic_op);
        }
    }
    g_kd_force = 0;
    if (rt_range_on() && !rt_range_pr_ro(pr)) g_pass.range_external=1;
    rt_range_note_depth();
#ifdef NVN_RT_RANGE_TEST
    rr_snap(dw,di);
#endif
    udc_note_draw(pr);
    swrite_note(pr);
    if (g_geo_program) {
        enc_r_draw_indexed_indirect(en, mp, MTLIndexTypeUInt32, g_geo_indices, g_geo_index_bytes, g_geo_args);
        return;
    }
    // A draw with 0 primitives (the game does send count=0) draws no pixels, so skip encoding; Metal requires the count to be non-zero, otherwise the validation layer aborts on the spot (measured on the camp / wilderness / combat pages)
    if ((dw && dw->ind) || (di && di->ind)) {   // DrawArraysIndirect / DrawElementsIndirect: NVN's argument records {count, instances, first, base instance} and {count, instances, first index,
        // base vertex, base instance} have the layouts of MTLDrawPrimitivesIndirectArguments and MTLDrawIndexedPrimitivesIndirectArguments; quads, fans and 8-bit indices use the converted ones (ixc_run)
        const ixc_ent *e = ixc_wanted(dw, di) ? ixc_find(dw, di) : NULL; void *mb, *ab; uint64_t mo, ao, ig;
        uint64_t ind = dw ? dw->ind : di->ind;
        if (ixc_wanted(dw, di)) { if (e && e->args) enc_r_draw_indexed_indirect(en, mp, MTLIndexTypeUInt32, e->ixb, e->ixlen, e->args); else RED_ONCE(88, "indirect draw skipped: the arguments for primitive %d were not converted (argument address %#llx is in no memory pool, or the conversion pipeline was not created)", prim, (unsigned long long)ind); }
        else if (!mdx && !mtl_addr(ind, dw ? 16 : 20, &ab, &ao)) RED_ONCE(88, "indirect draw skipped: argument address %#llx is in no memory pool", (unsigned long long)ind);
        else if (dw) { enc_r_draw_indirect(en, mp, mdx ? mdx : ((__bridge id<MTLBuffer>)ab).gpuAddress + ao); if (mdx) mdx_chain(dw->mdi_cnt, dw->mdi_i, mp, MTLIndexTypeUInt32, 0, 0); }
        else if (!mtl_gpu_addr(di->ibuf, 1, &mb, &mo, &ig)) RED_ONCE(22, "index buffer is in no memory pool");
        else { MTLIndexType ty = di->itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32; uint64_t il = ((__bridge id<MTLBuffer>)mb).length - mo;
            enc_r_draw_indexed_indirect(en, mp, ty, ig, il, mdx ? mdx : ((__bridge id<MTLBuffer>)ab).gpuAddress + ao); if (mdx) mdx_chain(di->mdi_cnt, di->mdi_i, mp, ty, ig, il); }
    } else if (dw && prim == 7) {   // Quads: shared indices 0,1,2,0,2,3
        quad_ib_make();
        if (dw->count >= 4) enc_r_draw_indexed(en, mp, (dw->count / 4) * 6, MTLIndexTypeUInt32, g_quad_gpu, g_quad_ib.length, dw->instances > 0 ? dw->instances : 1, dw->first, dw->base_instance);
    } else if (dw && prim == 6) {   // triangle fans (13 Sentinels): shared indices (0, i, i+1), first as the base vertex
        fan_ib_make();
        if (dw->count >= 3) enc_r_draw_indexed(en, mp, (dw->count - 2) * 3, MTLIndexTypeUInt32, g_fan_gpu, g_fan_ib.length, dw->instances > 0 ? dw->instances : 1, dw->first, dw->base_instance);
    } else if (dw) {
        if (dw->count && (dw->instances > 1 || dw->base_instance)) enc_r_draw_instanced(en, mp, dw->first, dw->count, dw->instances > 0 ? dw->instances : 1, dw->base_instance);
        else if (dw->count) enc_r_draw(en, mp, dw->first, dw->count);
    } else {
        if (prim == 6 || prim == 7) {
                                        // indexed triangle fans / quads (13 Sentinels): at replay the indices are read from guest memory and expanded into a 32-bit triangle list in this frame's arena (as for 8-bit indices)
            int isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4;
            uint32_t nt = prim == 7 ? (uint32_t)(di->count / 4) * 2 : di->count >= 3 ? (uint32_t)di->count - 2 : 0;
            if (!nt) return;
            void *mb; uint64_t mo; const uint8_t *src;
            if (!mtl_addr(di->ibuf, (uint64_t)di->count * isz, &mb, &mo) || !(src = [(__bridge id<MTLBuffer>)mb contents])) { RED_ONCE(22, "index buffer is in no memory pool"); return; }
            if (pend_hits(di->ibuf, (uint64_t)di->count * isz)) RED_ONCE(73, "triangle fan / quad index buffer has GPU-side writes in this submission: expanded from the current content");
            src += mo;
            id<MTLBuffer> ab; size_t off = arena_alloc(((size_t)nt * 12 + 15) & ~(size_t)15, &ab);
            uint32_t *d = (uint32_t *)((uint8_t *)ab.contents + off);
            // With primitive restart on, the restart index (compared before the base vertex is added) ends a fan or quad and the next one starts after it: they were joined across it,
            // and the extra triangles cleared parts of Luigi's Mansion 3's cached shadow depth (straight light edges on the garage floor, PR #110). At most nt triangles come out
            uint32_t count = nvn_expand_primitive(d, src, (uint32_t)di->count, (unsigned)isz, (unsigned)prim, S.restart.on, S.restart.index);
            if (count) enc_r_draw_indexed(en, mp, count, MTLIndexTypeUInt32, g_cur_arena->gpu + off, (NSUInteger)count * 4, di->instances ? di->instances : 1, di->base_vertex, di->base_instance);
            if (g_rt_range_pending) { PD("j rt_level_range pass=%llu vc=%u", g_stat_pass, (unsigned)di->count); g_rt_range_pending = 0; }
            return;
        }
        if (di->itype == 0) {
                                // 8-bit indices: Metal only has 16 / 32-bit. At replay they are read from guest memory and widened to 16-bit in this frame's arena (SMBW's full-screen passes and the final copy into the window all use 8-bit indices; without this the whole screen is black)
            if (!di->count) return;
            if (prim == 3 || prim == 5 || prim == 11 || prim == 13) { static int once; if (!once++) MLOG("8-bit indices in a strip (primitive %d, first case pass %llu): a primitive restart index 0xff stays 0x00ff when widened, which Metal does not restart on", prim, g_stat_pass); }
            const wide_ent *w = wide_wanted(di) ? wide_find(di) : NULL;
            if (w && w->gpu) {   // written earlier in this submission: widened on the GPU before this pass (wide_run)
                enc_r_draw_indexed(en, mp, di->count, MTLIndexTypeUInt16, w->gpu, (NSUInteger)di->count * 2, di->instances ? di->instances : 1, di->base_vertex, di->base_instance);
                if (g_rt_range_pending) { PD("j rt_level_range pass=%llu vc=%u", g_stat_pass, (unsigned)di->count); g_rt_range_pending = 0; }
                return;
            }
            void *mb; uint64_t mo; const uint8_t *src;
            if (!mtl_addr(di->ibuf, di->count, &mb, &mo) || !(src = [(__bridge id<MTLBuffer>)mb contents])) { RED_ONCE(22, "index buffer is in no memory pool"); return; }
            if (pend_hits(di->ibuf, di->count)) RED_ONCE(48, "8-bit index buffer has GPU-side writes in this submission and the GPU expansion failed: expanded from the current content");
            id<MTLBuffer> ab; size_t off = arena_alloc(((size_t)di->count * 2 + 15) & ~(size_t)15, &ab);
            uint16_t *d = (uint16_t *)((uint8_t *)ab.contents + off); src += mo;
            for (int32_t i = 0; i < di->count; i++) d[i] = src[i];
            enc_r_draw_indexed(en, mp, di->count, MTLIndexTypeUInt16, g_cur_arena->gpu + off, (NSUInteger)di->count * 2, di->instances ? di->instances : 1, di->base_vertex, di->base_instance);
            if (g_rt_range_pending) { PD("j rt_level_range pass=%llu vc=%u", g_stat_pass, (unsigned)di->count); g_rt_range_pending = 0; }
            return;
        }
        void *mb; uint64_t mo;
        uint64_t gpu;
        if (!mtl_gpu_addr(di->ibuf, 1, &mb, &mo, &gpu)) { RED_ONCE(22, "index buffer is in no memory pool"); return; }
        if (di->count) enc_r_draw_indexed(en, mp, di->count, di->itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32, gpu, ((__bridge id<MTLBuffer>)mb).length - mo,
                di->instances ? di->instances : 1, di->base_vertex, di->base_instance);
    }
    if (g_rt_range_pending) { PD("j rt_level_range pass=%llu vc=%u", g_stat_pass, (unsigned)(dw ? dw->count : di->count)); g_rt_range_pending=0; }
}
static nvn_hmap g_cps = {.klen = sizeof(void *)};
static void warm_claim(void) {
    if (!atomic_exchange_explicit(&g_warm_ready, 0, memory_order_acq_rel)) return;
    os_unfair_lock_lock(&g_warm_lock);
    NSArray<WarmItem *> *items = [g_warm_items copy];
    [g_warm_items removeAllObjects];
    os_unfair_lock_unlock(&g_warm_lock);
    for (WarmItem *item in items) {
        if (item->kind == 0) {
            if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
            if (!g_shcache[item->key]) { g_shcache[item->key] = item->shader; g_warm_claim_shaders++; }
        } else if (item->kind == 1) {
            if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
            NvShader *vs = g_shcache[item->vs_key], *fs = item->fs_key.length ? g_shcache[item->fs_key] : nil;
            if (!vs) { vs = item->vs; g_shcache[item->vs_key] = vs; }
            if (item->fs_key.length && !fs) { fs = item->fs; g_shcache[item->fs_key] = fs; }
            item->pso.vs = (__bridge void *)vs; item->pso.fs = (__bridge void *)fs;
            if (!hm_get_id(&g_pso, &item->pso)) { hm_put_id(&g_pso, &item->pso, item->object); g_warm_claim_rps++; }
        } else {
            NvShader *cs = g_shcache[item->key];
            if (!cs) { cs = item->shader; if (!g_shcache) g_shcache = [NSMutableDictionary dictionary]; g_shcache[item->key] = cs; }
            const void *key = (__bridge const void *)cs;
            if (!hm_get_id(&g_cps, &key)) { hm_put_id(&g_cps, &key, item->object); g_warm_claim_cps++; }
        }
    }
}
// Dispatches share the open copy encoder (a Metal 4 compute encoder) with the copies and dispatches around them, each ordered after everything the encoder already holds by a barrier inside it,
// as a texture copy is (blit). An encoder of their own, with full queue barriers on both sides, cost the GPU a drain per dispatch: Hyrule Warriors: Age of Calamity's battles after the first cutscene
// run about 1,060 small dispatches a frame, and the Mac GPU went from 6.7 to 16 ms a frame at the same draws, the iPhone's from 15 to about 50 (17 fps).
// DM_NVN_DISPATCH_OWN=1 gives each dispatch its own encoder again (so does DM_NVN_PASSGPU, which times each one)
static int dispatch_own(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_NVN_DISPATCH_OWN"); v = (e && !strcmp(e, "1")) || pg_on(); if (v) MLOG("compute dispatches: each in an encoder of its own (DM_NVN_DISPATCH_OWN=1 or DM_NVN_PASSGPU)"); }
    return v;
}
static id<MTL4ComputeCommandEncoder> dispatch_enc(void) {
    id<MTL4ComputeCommandEncoder> e = blit_open(); g_gp_disp++;
    if (g_blit_ops) blit_barrier();
    g_bw_lo[0] = 0; g_bw_hi[0] = ~0ull; g_bw_n = 1; g_br_n = 0; g_blit_ops = 1;   // recorded as "everything written": the next copy or dispatch gets a barrier
    if (!g_blit_ct) { [e setArgumentTable:g_ct]; g_blit_ct = 1; }
    args_dirty_all();
    return e;
}
static void dispatch(const p_dispatch *dp) {
    zskip_clear();
    g_wr_epoch++; g_mdx_epoch++;
    if (!S.cprog) { RED_ONCE(27, "no program bound before the dispatch"); return; }
    // DispatchComputeIndirect: the group counts {x, y, z} at ind (MTLDispatchThreadgroupsIndirectArguments) are usually written by the GPU earlier in the frame; Metal reads them when the dispatch runs
    void *ib = NULL; uint64_t io = 0, ig = 0;
    if (dp->ind && !mtl_gpu_addr(dp->ind, 12, &ib, &io, &ig)) { RED_ONCE(94, "indirect dispatch skipped: argument address %#llx is in no memory pool", (unsigned long long)dp->ind); return; }
    if (dp->ind) { static int once; if (!once++) MLOG("indirect dispatch: first time (the GPU reads the threadgroup counts from %#llx)", (unsigned long long)dp->ind); }
    NvShader *cs = prog_get(S.cprog)->sh[5];
    if (!cs) { RED_ONCE(28, "the dispatched program has no compute shader (or it is not mapped)"); return; }
    const void *k = (__bridge const void *)cs;
    id<MTLComputePipelineState> ps = hm_get_id(&g_cps, &k);
    if (!ps) {
        NSError *e = nil; uint64_t bt = bp_now(); ps = [g_dev newComputePipelineStateWithFunction:cs->fn error:&e];
        g_bp_pso_n++; g_bp_pso_ns += bp_now() - bt;
        if (!ps) { MLOG("[RED] failed to create compute pipeline: %s", e.localizedDescription.UTF8String); return; }
        pso_record("C\t%s\n", cs->key);
        if (cs->key[0]) pso_user_record("C\t%s\n", cs->key);
        hm_put_id(&g_cps, &k, ps);
    }
    if (cs->builtin) {   // Built-in compute kernel: n = uniform buffer 0, src = storage buffer 0, dst = storage buffer 1
        const p_bind_buf *u = &S.ubo[5][0], *a0 = &S.ssbo[5][0], *a1 = &S.ssbo[5][1];
        void *ub, *sb, *db; uint64_t uo, so, dof;
        if (!mtl_addr(u->addr, 4, &ub, &uo) || !mtl_addr(a0->addr, 8, &sb, &so) || !mtl_addr(a1->addr, 8, &db, &dof)) { RED_ONCE(30, "embedded compute kernel %s does not have all its buffers bound", cs->builtin); return; }
        if (!cs->logged++) {
            module_t *m = module_of((void *)dp->caller);
            MLOG("embedded compute kernel %s first dispatch: call site %s+%#llx, threadgroups %dx%dx%d, element count (current CPU-side value) %u", cs->builtin, m ? m->name : "?",
                 m ? (unsigned long long)(dp->caller - (uint64_t)m->base) : (unsigned long long)dp->caller, dp->x, dp->y, dp->z, *(uint32_t *)u->addr);
        }
        rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
        id<MTLComputeCommandEncoder> ce = pg_compute();
        enc_c_pso(ce, ps);
        enc_c_args(ce, ((__bridge id<MTLBuffer>)ub).gpuAddress + uo, 0); enc_c_args(ce, ((__bridge id<MTLBuffer>)sb).gpuAddress + so, 1); enc_c_args(ce, ((__bridge id<MTLBuffer>)db).gpuAddress + dof, 2);   // Built-in compute kernel: cold path, take the addresses directly
        if (dp->ind) enc_c_dispatch_indirect(ce, ig, MTLSizeMake(32, 1, 1)); else enc_c_dispatch(ce, MTLSizeMake(dp->x, dp->y, dp->z), MTLSizeMake(32, 1, 1));
        enc_c_end(ce);
        return;
    }
    const uint16_t *tg = cs->rf.tg;
    if (!tg[0]) { RED_ONCE(29, "compute shader lacks threadgroup size reflection"); return; }
    if (g_nubd && ubd_conflict_cs(cs, dp->ind)) { g_ubd_conflicts++; ubd_land(); }   // see ubd_conflict_cs
    rp_end();
    int own = dispatch_own();
    if (own && g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTLComputeCommandEncoder> ce = own ? pg_compute() : (id<MTLComputeCommandEncoder>)dispatch_enc();
    enc_c_pso(ce, ps);
    g_dispatch_dim[0] = (uint32_t)dp->x; g_dispatch_dim[1] = (uint32_t)dp->y; g_dispatch_dim[2] = (uint32_t)dp->z;   // read back by the driver segment c0 (encode_args_)
    g_c0_dim_buf = nil;
    encode_args(ce, cs, 5, 0);
    if (dp->ind) {   // The shader reads the group counts from c0 + 0x200: copy the GPU-written ones there first (Metal 4 orders nothing inside an encoder without a barrier)
        if (g_c0_dim_buf) { [(id<MTL4ComputeCommandEncoder>)ce copyFromBuffer:(__bridge id<MTLBuffer>)ib sourceOffset:io toBuffer:g_c0_dim_buf destinationOffset:g_c0_dim_off size:12];
            [(id<MTL4ComputeCommandEncoder>)ce barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice]; }
        enc_c_dispatch_indirect(ce, ig, MTLSizeMake(tg[0], tg[1], tg[2]));
    } else enc_c_dispatch(ce, MTLSizeMake(dp->x, dp->y, dp->z), MTLSizeMake(tg[0], tg[1], tg[2]));
    if (own) enc_c_end(ce);
}
// GPU: texture -> buffer (row stride taken from SetCopyRowStride, layer stride from SetCopyImageStride, 0 = tightly packed)
static void copy_t2b(const p_copy_b2t *c) {
    rp_flush();   // Materialize pending clears before reading, matching copy_t2t.
    g_wr_epoch++; g_mdx_epoch++;
    if (c->tex) xv_check(c->tex->serial, c->has_view && c->view.set_levels ? c->view.base_level : 0, c->has_view && c->view.set_layers ? c->view.min_layer : 0, "texture-to-buffer copy");
    id<MTLTexture> t = tex_mtl(c->tex);
    if (!t) return;
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    unsigned level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((c->r.width + bw - 1) / bw) * bpb;
    size_t rows = (c->r.height + bh - 1) / bh, img = S.copy_img_stride ? (size_t)S.copy_img_stride : row * rows;   // per layer: SetCopyImageStride, else tightly packed
    int is3d = c->tex->d.target == 2, nz = c->r.depth ? c->r.depth : 1;
    void *mb; uint64_t mo;
    if (!mtl_addr(c->src, img * nz, &mb, &mo)) { RED_ONCE(31, "texture→buffer copy destination is in no memory pool"); return; }
    sbw_note(c->src, img * nz, SBW_GPU);
    id<MTL4ComputeCommandEncoder> b = blit();
    if (is3d)
        enc_b_copy_texture_buffer(b, t, 0, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, c->r.zoffset), MTLSizeMake(c->r.width, c->r.height, nz), (__bridge id<MTLBuffer>)mb, mo, row, img);
    else
        for (int z = 0; z < nz; z++)
            enc_b_copy_texture_buffer(b, t, layer0 + c->r.zoffset + z, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, 0), MTLSizeMake(c->r.width, c->r.height, 1), (__bridge id<MTLBuffer>)mb, mo + z * img, row, img);
}
// CPU texture read (ReadTexels): for packaged textures the pool memory is the ground truth, de-layout and read directly; the rest are read back from the MTLTexture by blit (the caller has already waited for the GPU per NVN rules)
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    xv_check(t->serial, level, layer0, "CPU readback");
    PD("U cpu_read ser=%llu lv%u sl%u xyz=%d,%d,%d size=%d,%d,%d dst=%p row=%lu img=%lu", t->serial, level, layer0, r->xoffset, r->yoffset, r->zoffset, r->width, r->height, r->depth, dst, (unsigned long)row, (unsigned long)img);
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (r->height + bh - 1) / bh, x0 = (size_t)(r->xoffset / bw) * bpb, y0 = r->yoffset / bh;
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    // A texture the CPU filled has its content in guest memory: read it there, on whatever thread asks, without resolving (and maybe uploading) the host texture.
    // That includes a tex_dirty_upload texture the CPU wrote since registration but the submit thread has not resolved yet: Tears of the Kingdom fills 1024x1024 BC4 material
    // textures from the CPU, reads them back with ReadTexels on its own thread before any draw uses them, and copies the blocks into its 5120x5120 roughness atlas. Read from the
    // never-uploaded host texture they were zeros, the atlas stayed empty, and the opening cave's stone blocks got roughness 0 and no torch light
    int nvn_ft_written(const tex_t *);
    if ((t->has_layout || tex_cpu_written(t->serial) || (DM_GAME_TEX_DIRTY_UPLOAD && nvn_ft_written(t))) && t->mem) {
        uint32_t lw = MAX(t->d.width >> level, 1u), lh = MAX(t->d.height >> level, 1u), lz = is3d ? MAX(t->d.depth >> level, 1u) : 1;
        size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh;
        uint8_t *lin = malloc(lp * lr * lz);
        for (int z = 0; z < nz; z++) {
            unsigned layer = is3d ? 0 : layer0 + r->zoffset + z;
            if ((is3d && z == 0) || !is3d) if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) { RED_ONCE(32, "ReadTexels deswizzle failed"); free(lin); return; }
            const uint8_t *src = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0);
            for (size_t y = 0; y < hb; y++) memcpy(dst + z * img + y * row, src + (y0 + y) * lp + x0, wb);
        }
        free(lin);
        return;
    }
    id<MTLTexture> mt = tex_mtl(t);
    if (!mt) { memset(dst, 0, img * nz); return; }
    mtl_finish_gpu();
    id<MTLBuffer> buf = [g_dev newBufferWithLength:wb * hb * nz options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = enc_b_begin(cb);
    for (int z = 0; z < nz; z++)
        [b copyFromTexture:mt sourceSlice:is3d ? 0 : layer0 + r->zoffset + z sourceLevel:level sourceOrigin:MTLOriginMake(r->xoffset, r->yoffset, is3d ? r->zoffset + z : 0)
                sourceSize:MTLSizeMake(r->width, r->height, 1) toBuffer:buf destinationOffset:z * wb * hb destinationBytesPerRow:wb destinationBytesPerImage:wb * hb];
    enc_b_end(b); [cb commit]; [cb waitUntilCompleted];
    for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memcpy(dst + z * img + y * row, (uint8_t *)buf.contents + z * wb * hb + y * wb, wb);
}
static void copy_b2t(const p_copy_b2t *c) {
    if (c->tex) zskip_del(c->tex->serial);
    id<MTLTexture> t = tex_mtl(c->tex);
    if (!t) { PD("C ser=%llu (no host texture)", c->tex->serial); return; }
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    unsigned level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((c->r.width + bw - 1) / bw) * bpb;
    size_t rows = (c->r.height + bh - 1) / bh, img = S.copy_img_stride ? (size_t)S.copy_img_stride : row * rows;   // per layer: SetCopyImageStride, else tightly packed
    int is3d = c->tex->d.target == 2, nz = c->r.depth ? c->r.depth : 1;
    PD("C ser=%llu target %d %ux%ux%u format %#x lv%u sl%u region %d,%d,%d %dx%dx%d from %#llx row %zu image %zu", c->tex->serial, c->tex->d.target, c->tex->d.width, c->tex->d.height, c->tex->d.depth,
       c->tex->d.format, level, layer0, c->r.xoffset, c->r.yoffset, c->r.zoffset, c->r.width, c->r.height, c->r.depth, (unsigned long long)c->src, row, img);
    void *mb; uint64_t mo;
    if (!mtl_addr(c->src, img * nz, &mb, &mo)) { RED_ONCE(23, "buffer→texture copy source is in no memory pool"); return; }
    g_cut_copy = 1;
    if (is3d && passdump()) { const char *dd = getenv("DM_NVN_CBDUMP"); if (dd) {   // the source bytes of a 3D upload, beside the uniform dumps (F<frame>_c3d_<serial>.bin)
        char p[1024]; snprintf(p, sizeof p, "%s/F%llu_c3d_%llu.bin", dd, (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)c->tex->serial);
        FILE *f = fopen(p, "wb"); if (f) { fwrite((const uint8_t *)((__bridge id<MTLBuffer>)mb).contents + mo, 1, img * nz, f); fclose(f); } } }
    id<MTL4ComputeCommandEncoder> b = blit();
    if (is3d)
        enc_b_copy_buffer_texture(b, (__bridge id<MTLBuffer>)mb, mo, row, img, MTLSizeMake(c->r.width, c->r.height, nz), t, 0, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, c->r.zoffset));
    else
        for (int z = 0; z < nz; z++)
            enc_b_copy_buffer_texture(b, (__bridge id<MTLBuffer>)mb, mo + z * img, row, img, MTLSizeMake(c->r.width, c->r.height, 1), t, layer0 + c->r.zoffset + z, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, 0));
}

// Texture copies a blit cannot do -- a format change, a scaled region or a mirrored one (MIRROR_X / MIRROR_Y) (Link's Awakening converts its 1408x792 RG11B10F scene to RGBA16F every frame; Super Mario 3D World scales 1920x1080 to 1280x720
// when leaving the title): one render pass per layer that draws the source region into the destination region, nearest or (LINEAR_FILTER) bilinear. Float, unorm and sRGB formats only, 2D / 2D array,
// destinations that can be render targets; anything else returns 0 and keeps the red line. Depth (Depth32Float / Depth32Float_Stencil8 into the same format) is drawn by writing depth from the fragment shader,
// always nearest, and stencil is written from the fragment shader too ([[stencil]] export, read through an X32_Stencil8 view of the source). Link's Awakening halves its depth-stencil buffer twice
// (1920x1080 → 960x540 → 480x270) and its depth-of-field mask pass only writes where stencil bit 1 is clear: with the stencil left as the new texture's undefined content, the mask kept its
// clear value 1.0 (full blur) everywhere and the whole picture was out of focus. Without a stencil view the stencil is cleared to 0 instead
static int fmt_int_or_ds(MTLPixelFormat f) {
    switch (f) {
        case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint: case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint: case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint:
        case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint:
        case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint:
        case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: case MTLPixelFormatDepth16Unorm: case MTLPixelFormatDepth32Float: case MTLPixelFormatStencil8:
        case MTLPixelFormatDepth32Float_Stencil8: case MTLPixelFormatX32_Stencil8: return 1;
        default: return 0;
    }
}
static int copy_t2t_draw(id<MTLTexture> s, id<MTLTexture> d, unsigned sl, unsigned ss, unsigned dl, unsigned ds, const NVNcopyRegion *sr, const NVNcopyRegion *dr, int linear, int mirror) {
    static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *pso; static id<MTLFunction> vf, ff, fd, fds; static id<MTLSamplerState> samp[2]; static id<MTL4ArgumentTable> at; static id<MTLDepthStencilState> dss, dss_s; static int failed;
    if (failed || sr->width <= 0 || sr->height <= 0 || dr->width <= 0 || dr->height <= 0) return 0;
    int depth = (s.pixelFormat == MTLPixelFormatDepth32Float || s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) && d.pixelFormat == s.pixelFormat, stencil = depth && d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
    if (!depth && (fmt_int_or_ds(s.pixelFormat) || fmt_int_or_ds(d.pixelFormat))) return 0;
    if (mtl_fmt_block(d.pixelFormat) || !(d.usage & MTLTextureUsageRenderTarget) || s.sampleCount > 1 || d.sampleCount > 1) return 0;
    if (!pso) {
        static const char *src =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; };\n"
            "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
            // p[0] = source origin (texels) + source texels per destination texel, p[1] = destination origin + source level size
            "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 *p [[buffer(0)]]) {\n"
            "  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; return t.sample(s, st / p[1].zw, level(0)); }\n"
            "struct D { float d [[depth(any)]]; };\n"
            "fragment D fsd(V in [[stage_in]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 *p [[buffer(0)]]) {\n"
            "  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; D o; o.d = t.read(uint2(st)); return o; }\n"
            "struct DS { float d [[depth(any)]]; uint s [[stencil]]; };\n"
            "fragment DS fsds(V in [[stage_in]], depth2d<float> t [[texture(0)]], texture2d<uint> ts [[texture(1)]], constant float4 *p [[buffer(0)]]) {\n"
            "  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; DS o; o.d = t.read(uint2(st)); o.s = ts.read(uint2(st)).r; return o; }\n";
        NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src) options:nil error:&e];
        vf = [lib newFunctionWithName:@"vs"]; ff = [lib newFunctionWithName:@"fs"]; fd = [lib newFunctionWithName:@"fsd"]; fds = [lib newFunctionWithName:@"fsds"];
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new]; dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = YES; dss = [g_dev newDepthStencilStateWithDescriptor:dd];
        MTLStencilDescriptor *sd_ = [MTLStencilDescriptor new]; sd_.stencilCompareFunction = MTLCompareFunctionAlways; sd_.depthStencilPassOperation = MTLStencilOperationReplace;   // Replace writes the [[stencil]] output
        dd.frontFaceStencil = dd.backFaceStencil = sd_; dss_s = [g_dev newDepthStencilStateWithDescriptor:dd];
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 2; ad.maxSamplerStateBindCount = 1; ad.maxBufferBindCount = 1; at = rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]);
        for (int i = 0; i < 2; i++) { MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = i ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge; sd.supportArgumentBuffers = YES; samp[i] = [g_dev newSamplerStateWithDescriptor:sd]; }
        if (!vf || !ff || !fd || !fds || !dss || !dss_s || !at || !samp[0] || !samp[1]) { failed = 1; MLOG("[RED] cannot create the pipelines for format-converting / scaling texture copies: %s", e.localizedDescription.UTF8String); return 0; }
        pso = [NSMutableDictionary dictionary];
    }
    // Stencil: read through a stencil-only view of the source (one per layer below); without one it is cleared to 0
    id<MTLTexture> stv0 = stencil && s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? [s newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8 textureType:MTLTextureType2D levels:NSMakeRange(sl, 1) slices:NSMakeRange(ss + sr->zoffset, 1)] : nil;
    int scopy = stv0 != nil;
    NSNumber *pk = @(d.pixelFormat * 2 + scopy);
    id<MTLRenderPipelineState> ps = pso[pk];
    if (!ps) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = vf; pd.fragmentFunction = depth ? (scopy ? fds : fd) : ff;
        if (depth) { pd.depthAttachmentPixelFormat = d.pixelFormat; if (stencil) pd.stencilAttachmentPixelFormat = d.pixelFormat; } else pd.colorAttachments[0].pixelFormat = d.pixelFormat;
        NSError *e = nil; ps = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!ps) { RED_ONCE(83, "format-converting / scaling texture copy: cannot create the pipeline for destination format %lu: %s", (unsigned long)d.pixelFormat, e.localizedDescription.UTF8String); return 0; }
        pso[pk] = ps;
    }
    unsigned sw = MAX(s.width >> sl, 1ul), sh = MAX(s.height >> sl, 1ul), dw = MAX(d.width >> dl, 1ul), dh = MAX(d.height >> dl, 1ul);
    if ((unsigned)(dr->xoffset + dr->width) > dw || (unsigned)(dr->yoffset + dr->height) > dh) return 0;
    int nz = sr->depth ? sr->depth : 1;
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    for (int z = 0; z < nz; z++) {
        id<MTLTexture> sv = [s newTextureViewWithPixelFormat:s.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(sl, 1) slices:NSMakeRange(ss + sr->zoffset + z, 1)];
        id<MTLBuffer> pb; size_t po = arena_alloc(32, &pb);   // parameters in the frame arena (resident already): a fresh 32-byte buffer per copy was a residency add, a commit and a release (Link's Awakening, about 11 a frame)
        if (!sv || !pb) { RED_ONCE(84, "format-converting / scaling texture copy: cannot create the source view / argument block"); return z > 0; }
        float *p = (float *)((uint8_t *)pb.contents + po); p[0] = sr->xoffset; p[1] = sr->yoffset; p[2] = (float)sr->width / dr->width; p[3] = (float)sr->height / dr->height;
        if (mirror & NVN_COPY_FLAGS_MIRROR_X_BIT) { p[0] += sr->width; p[2] = -p[2]; }   // mirrored: walk the source region from its far edge
        if (mirror & NVN_COPY_FLAGS_MIRROR_Y_BIT) { p[1] += sr->height; p[3] = -p[3]; }
        p[4] = dr->xoffset; p[5] = dr->yoffset; p[6] = sw; p[7] = sh;
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        MTLRenderPassAttachmentDescriptor *a = depth ? (MTLRenderPassAttachmentDescriptor *)rp.depthAttachment : (MTLRenderPassAttachmentDescriptor *)rp.colorAttachments[0];
        int whole = !dr->xoffset && !dr->yoffset && (unsigned)dr->width == dw && (unsigned)dr->height == dh;   // the copy draws every pixel of the level (no blending, no discard): its old content is not loaded (Link's Awakening copies its scene this way every frame)
        a.texture = d; a.level = dl; a.slice = ds + dr->zoffset + z; a.loadAction = whole ? MTLLoadActionDontCare : MTLLoadActionLoad; a.storeAction = MTLStoreActionStore;
        id<MTLTexture> stv = scopy ? (z ? [s newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8 textureType:MTLTextureType2D levels:NSMakeRange(sl, 1) slices:NSMakeRange(ss + sr->zoffset + z, 1)] : stv0) : nil;
        if (stencil) { MTLRenderPassStencilAttachmentDescriptor *st = rp.stencilAttachment; st.texture = d; st.level = dl; st.slice = a.slice; st.loadAction = scopy ? (whole ? MTLLoadActionDontCare : MTLLoadActionLoad) : MTLLoadActionClear; st.clearStencil = 0; st.storeAction = MTLStoreActionStore; }
        ser_gate(); id<MTL4RenderCommandEncoder> en = enc_r_begin(rp);
        if (depth) [en setDepthStencilState:scopy ? dss_s : dss];
        [en setArgumentTable:at atStages:MTLRenderStageFragment];
        [at setTexture:sv.gpuResourceID atIndex:0]; if (stv) [at setTexture:stv.gpuResourceID atIndex:1]; [at setSamplerState:samp[!!linear && !depth].gpuResourceID atIndex:0]; [at setAddress:pb.gpuAddress + po atIndex:0];
        [en setViewport:(MTLViewport){dr->xoffset, dr->yoffset, dr->width, dr->height, 0, 1}];
        [en setScissorRect:(MTLScissorRect){(NSUInteger)dr->xoffset, (NSUInteger)dr->yoffset, (NSUInteger)dr->width, (NSUInteger)dr->height}];
        [en setRenderPipelineState:ps]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; enc_r_end(en);
        defer_obj_unref(sv); if (stv) defer_obj_unref(stv);   // released after this submission has run
    }
    args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
    { static int once[2]; if (!once[depth]++) MLOG("format-converting / scaling texture copy: first case %dx%d format %lu → %dx%d format %lu%s%s", sr->width, sr->height, (unsigned long)s.pixelFormat, dr->width, dr->height, (unsigned long)d.pixelFormat,
                                                    linear && !depth ? " (bilinear)" : "", stencil ? (scopy ? " (depth + stencil)" : " (no stencil view possible, cleared to 0)") : ""); }
    return 1;
}
// GPU: texture -> texture (previously mislisted as a no-op, 3 times per frame on the character select page). Same size, not mirrored, same format -> blit layer by layer; the other semantics (scaling with filtering, mirroring, format change) cannot be expressed by blit: report a failure, to be implemented
static void copy_t2t(const p_copy_t2t *c) {
    if (c->src) xv_check(c->src->serial, c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, "texture copy source");
    rp_flush();   // Land the pending pure-clear render pass first (zero-source candidates only enter the set when the render pass ends); the blit() below would end the render pass anyway
    if (c->dst) zskip_del(c->dst->serial);
    if (c->src && c->dst && c->sr.xoffset == 0 && c->sr.yoffset == 0 && c->dr.xoffset == 0 && c->dr.yoffset == 0
        && c->sr.width == (int)c->src->d.width && c->sr.height == (int)c->src->d.height && c->dr.width == (int)c->dst->d.width && c->dr.height == (int)c->dst->d.height) {
        int sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, dl = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
        int ss = c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, ds = c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0;
        if (zskip_has(c->src->serial, sl, ss)) zskip_add(c->dst->serial, dl, ds);
    }
    id<MTLTexture> s = tex_mtl(c->src), d = tex_mtl(c->dst);
    PD("T src=%llu dst=%llu %dx%dx%d→%dx%dx%d flags=%#x", c->src->serial, c->dst->serial, c->sr.width, c->sr.height, c->sr.depth, c->dr.width, c->dr.height, c->dr.depth, c->flags);
    if (!s || !d) return;
    unsigned sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, dl = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
    unsigned sz0 = c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, dz0 = c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0;
    if (c->flags & DM_COPY_RESOLVE) {   // Downsample: multisample resolve into the single-sample destination (depth takes sample 0); with equal sample counts it is a plain whole copy
        if (s.sampleCount == d.sampleCount && s.pixelFormat == d.pixelFormat && s.width == d.width && s.height == d.height) {
            enc_b_copy_texture(blit(), s, sz0, sl, MTLOriginMake(0, 0, 0), MTLSizeMake(s.width, s.height, 1), d, dz0, dl, MTLOriginMake(0, 0, 0));
            return;
        }
        if (s.sampleCount <= 1 || d.sampleCount != 1 || s.pixelFormat != d.pixelFormat || s.width != d.width || s.height != d.height) {
            RED_ONCE(72, "Downsample case not implemented: %lux%lu %lu samples format %lu → %lux%lu %lu samples format %lu", (unsigned long)s.width, (unsigned long)s.height, (unsigned long)s.sampleCount,
                     (unsigned long)s.pixelFormat, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.sampleCount, (unsigned long)d.pixelFormat);
            return;
        }
        rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        MTLPixelFormat pf = s.pixelFormat;
        int depth = pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth16Unorm || pf == MTLPixelFormatDepth32Float_Stencil8;
        MTLRenderPassAttachmentDescriptor *a = depth ? rp.depthAttachment : rp.colorAttachments[0];
        a.texture = s; a.level = sl; a.slice = sz0; a.resolveTexture = d; a.resolveLevel = dl; a.resolveSlice = dz0;
        a.loadAction = MTLLoadActionLoad; a.storeAction = MTLStoreActionStoreAndMultisampleResolve;
        if (depth) rp.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
        ser_gate(); enc_r_end(enc_r_begin(rp));
        return;
    }
    PD("t src=%llu lv%u sl%u xyz=%d,%d,%d dst=%llu lv%u sl%u xyz=%d,%d,%d size=%d,%d,%d", c->src->serial, sl, sz0, c->sr.xoffset, c->sr.yoffset, c->sr.zoffset,
       c->dst->serial, dl, dz0, c->dr.xoffset, c->dr.yoffset, c->dr.zoffset, c->sr.width, c->sr.height, c->sr.depth);
    int s3 = c->src->d.target == 2, d3 = c->dst->d.target == 2, nz = c->sr.depth ? c->sr.depth : 1;
    // The draw path renders into the destination: one made without render-target use (packaged) gets it first, as a whole-level clear does (Paper Mario: The Thousand-Year Door copies a
    // 128x128 RGBA8 render target into an RG8 texture, mirrored in Y)
    if ((c->sr.width != c->dr.width || c->sr.height != c->dr.height || s.pixelFormat != d.pixelFormat || (c->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT))) && !s3 && !d3
        && !(d.usage & MTLTextureUsageRenderTarget) && !mtl_fmt_block(d.pixelFormat) && d.sampleCount == 1 && !fmt_int_or_ds(s.pixelFormat) && !fmt_int_or_ds(d.pixelFormat))
        d = tex_upgrade(c->dst, d, MTLTextureUsageRenderTarget, d.pixelFormat);
    if ((c->sr.width != c->dr.width || c->sr.height != c->dr.height || s.pixelFormat != d.pixelFormat || (c->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT))) && !s3 && !d3
        && (c->sr.depth ? c->sr.depth : 1) == (c->dr.depth ? c->dr.depth : 1)
        && !(c->flags & ~(NVN_COPY_FLAGS_ENGINE_2D_BIT | NVN_COPY_FLAGS_LINEAR_FILTER_BIT | NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT))
        && copy_t2t_draw(s, d, sl, sz0, dl, dz0, &c->sr, &c->dr, !!(c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT), c->flags)) return;   // format change / scaling / mirroring: draw it (see copy_t2t_draw)
    if (c->sr.width != c->dr.width || c->sr.height != c->dr.height || (c->sr.depth ? c->sr.depth : 1) != (c->dr.depth ? c->dr.depth : 1) || (c->flags & ~NVN_COPY_FLAGS_ENGINE_2D_BIT & ~NVN_COPY_FLAGS_LINEAR_FILTER_BIT)
        || s.pixelFormat != d.pixelFormat || s3 != d3) {
        RED_ONCE(33, "texture→texture copy case not implemented: %dx%dx%d→%dx%dx%d flags %#x format %lu→%lu 3D %d→%d", c->sr.width, c->sr.height, c->sr.depth, c->dr.width, c->dr.height, c->dr.depth,
                 c->flags, (unsigned long)s.pixelFormat, (unsigned long)d.pixelFormat, s3, d3);
        return;
    }
    id<MTL4ComputeCommandEncoder> b = blit();
    if (s3)
        enc_b_copy_texture(b, s, 0, sl, MTLOriginMake(c->sr.xoffset, c->sr.yoffset, c->sr.zoffset), MTLSizeMake(c->sr.width, c->sr.height, nz), d, 0, dl, MTLOriginMake(c->dr.xoffset, c->dr.yoffset, c->dr.zoffset));
    else
        for (int z = 0; z < nz; z++)
            enc_b_copy_texture(b, s, sz0 + c->sr.zoffset + z, sl, MTLOriginMake(c->sr.xoffset, c->sr.yoffset, 0), MTLSizeMake(c->sr.width, c->sr.height, 1), d, dz0 + c->dr.zoffset + z, dl, MTLOriginMake(c->dr.xoffset, c->dr.yoffset, 0));
    // Diagnostic DM_NVN_T2T_TRACE=WxH:file: after each copy into an RGBA16F texture of that size, copy the result once more into a trace buffer on the GPU timeline (frame numbers kept on the CPU side); after 240 copies write the file
    { static int on = -1; static uint32_t tw, th; static const char *tf; static id<MTLBuffer> tb; static int tn; static uint64_t tfr[240], from;
      if (on < 0) { const char *e = getenv("DM_NVN_T2T_TRACE"), *fe = getenv("DM_NVN_T2T_FROM"); on = e && sscanf(e, "%ux%u", &tw, &th) == 2 && (tf = strchr(e, ':')) && *++tf; from = fe ? strtoull(fe, NULL, 10) : 0; }
      if (on > 0 && tn < 240 && atomic_load(&g_cur_frame) >= from && d.width == tw && d.height == th && d.pixelFormat == MTLPixelFormatRGBA16Float && !s3) {
          if (!tb) { tb = [g_dev newBufferWithLength:(size_t)240 * tw * th * 8 options:MTLResourceStorageModeShared]; res_add(tb); }
          enc_b_copy_texture_buffer(blit(), d, dz0, dl, MTLOriginMake(0, 0, 0), MTLSizeMake(tw, th, 1), tb, (size_t)tn * tw * th * 8, (size_t)tw * 8, (size_t)tw * th * 8);
          tfr[tn++] = atomic_load(&g_cur_frame);
          if (tn == 240) {
              rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
              mtl_finish_gpu();
              FILE *f = fopen(tf, "wb");
              if (f) { fprintf(f, "%u %u %d\n", tw, th, tn); fwrite(tfr, 8, 240, f); fwrite(tb.contents, 1, (size_t)240 * tw * th * 8, f); fclose(f); }
              MLOG("[t2t-trace] %d copies → %s (frames %llu–%llu)", tn, tf, (unsigned long long)tfr[0], (unsigned long long)tfr[239]);
          }
      }
    }
}

// The colour channels a Metal format has, as a channel write mask: a clear whose mask covers them all writes every texel whole (FINAL FANTASY TACTICS - The Ivalice Chronicles clears an
// R8 render target with mask 0x1)
static unsigned mtl_pf_chmask(MTLPixelFormat pf) {
    switch ((int)pf) {
        case 1: return 0x8;                                                                                      // A8
        case 10: case 11: case 12: case 13: case 14: case 20: case 22: case 23: case 24: case 25: case 53: case 54: case 55: return 0x1;   // R8 / R16 / R32
        case 30: case 31: case 32: case 33: case 34: case 60: case 62: case 63: case 64: case 65: case 103: case 104: case 105: return 0x3;   // RG8 / RG16 / RG32
        case 40: case 92: case 93: return 0x7;                                                                   // B5G6R5 / RG11B10F / RGB9E5
    }
    return 0xf;
}
// GPU: clear a texture region with the given value (previously mislisted as a no-op, called 5 times at initialization; the initial value of a Metal private texture is undefined, and the game relies on the cleared initial value).
// Whole level and whole face, all four channels written -> open one clear-only render pass per layer / depth slice (the hardware converts the clear value by the texture format, including integer and depth);
// view with changed format: clear on a view of that format; partial region, partial channels, compressed / packaged textures (no render target usage): report a failure, to be implemented
static void clear_tex(const p_clear_tex *c) {
    if (c->tex) zskip_del(c->tex->serial);
    id<MTLTexture> t = tex_mtl(c->tex);
    PD("X ser=%llu view format %#x region %d,%d,%d %dx%dx%d mask %#x %s %g,%g,%g,%g", c->tex->serial, c->has_view && c->view.set_format ? c->view.format : -1, c->r.xoffset, c->r.yoffset, c->r.zoffset, c->r.width, c->r.height, c->r.depth, c->mask,
       c->is_uint ? "ui" : "f", c->is_uint ? (double)c->c.u[0] : c->c.f[0], c->is_uint ? (double)c->c.u[1] : c->c.f[1], c->is_uint ? (double)c->c.u[2] : c->c.f[2], c->is_uint ? (double)c->c.u[3] : c->c.f[3]);
    if (!t) return;
    unsigned lv = c->has_view && c->view.set_levels ? c->view.base_level : 0, l0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    NSUInteger w = MAX(t.width >> lv, 1u), h = MAX(t.height >> lv, 1u);
    int is3d = t.textureType == MTLTextureType3D, nz = c->r.depth ? c->r.depth : 1;
    if (!(t.usage & MTLTextureUsageRenderTarget) && !mtl_fmt_block(t.pixelFormat) && t.sampleCount == 1 && !c->r.xoffset && !c->r.yoffset && (NSUInteger)c->r.width == w && (NSUInteger)c->r.height == h && (c->mask & mtl_pf_chmask(t.pixelFormat)) == mtl_pf_chmask(t.pixelFormat))
        t = tex_upgrade(c->tex, t, MTLTextureUsageRenderTarget, t.pixelFormat);   // a whole-level clear of a texture made without render-target use (packaged): give it that use
    if (c->has_view && c->view.set_format) {   // View with changed format: the clear value is interpreted in the view format (all 5 of the game's calls are the ui version with an integer view), and the clear is done on a view of the same format
        uint32_t vpf = 0; uint8_t swz[4];
        if (!nvn_fmt_to_mtl(c->view.format, &vpf, swz) || !vpf) { RED_ONCE(42, "texture clear view format %#x has no equivalent", c->view.format); return; }
        if (tex_need_pfv(t.pixelFormat, (MTLPixelFormat)vpf) && !(t.usage & MTLTextureUsagePixelFormatView)) t = tex_upgrade_pfv(c->tex, t, (MTLPixelFormat)vpf);
        if (tex_need_pfv(t.pixelFormat, (MTLPixelFormat)vpf)) {
            id<MTLTexture> v = [t newTextureViewWithPixelFormat:(MTLPixelFormat)vpf];
            if (!v) { RED_ONCE(43, "texture clear: cannot create the view: format %lu → %u", (unsigned long)t.pixelFormat, vpf); return; }
            t = v;
        }
    }
    MTLPixelFormat pf = t.pixelFormat;
    int depth = pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth16Unorm || pf == MTLPixelFormatDepth32Float_Stencil8, stencil = pf == MTLPixelFormatStencil8 || pf == MTLPixelFormatDepth32Float_Stencil8;
    if (c->r.xoffset || c->r.yoffset || (NSUInteger)c->r.width != w || (NSUInteger)c->r.height != h || (c->mask & mtl_pf_chmask(pf)) != mtl_pf_chmask(pf)
        || !(t.usage & MTLTextureUsageRenderTarget)) {
        RED_ONCE(44, "texture clear case not implemented: region %d,%d %dx%d (level size %lux%lu) mask %#x render-target capable %d (format %#x flags %#x)", c->r.xoffset, c->r.yoffset, c->r.width, c->r.height,
                 (unsigned long)w, (unsigned long)h, c->mask, !!(t.usage & MTLTextureUsageRenderTarget), c->tex->d.format, c->tex->d.flags);
        return;
    }
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    for (int z = 0; z < nz; z++) {
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        MTLRenderPassAttachmentDescriptor *a = depth ? rp.depthAttachment : stencil ? rp.stencilAttachment : rp.colorAttachments[0];
        a.texture = t; a.level = lv; a.loadAction = MTLLoadActionClear; a.storeAction = MTLStoreActionStore;
        if (is3d) a.depthPlane = c->r.zoffset + z; else a.slice = l0 + c->r.zoffset + z;
        if (depth) { rp.depthAttachment.clearDepth = c->c.f[0];
            if (stencil) { rp.stencilAttachment.texture = t; rp.stencilAttachment.level = lv; rp.stencilAttachment.slice = a.slice; rp.stencilAttachment.loadAction = MTLLoadActionClear;
                           rp.stencilAttachment.storeAction = MTLStoreActionStore; rp.stencilAttachment.clearStencil = c->c.u[1]; } }
        else if (stencil) rp.stencilAttachment.clearStencil = c->c.u[0];
        else rp.colorAttachments[0].clearColor = c->is_uint ? MTLClearColorMake(c->c.u[0], c->c.u[1], c->c.u[2], c->c.u[3]) : MTLClearColorMake(c->c.f[0], c->c.f[1], c->c.f[2], c->c.f[3]);
        ser_gate(); enc_r_end(enc_r_begin(rp));
    }
}

// Context description of one submit (mtl_sub_commit takes one per submit and prints it into the failure line when the command buffer errors): for the GPU memory fault on the first frame entering Travincal, the failure line had only the sequence number and error code, and it was impossible to tell what was in that submit
static const char *cctx_snap(void) {
    static char b[320]; static uint64_t p_pass, p_up, p_reup, p_ser; uint64_t up = 0, reup = 0; mtl_pkg_stats(&up, &reup);
    pthread_mutex_lock(&g_deferred_mu); unsigned long nd = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    snprintf(b, sizeof b, "frame %llu; since the last submission: passes +%llu, first texture uploads +%llu, re-uploads +%llu, serial table changes +%llu; deferred-release queue %lu",
             (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)(g_stat_pass - p_pass), (unsigned long long)(up - p_up), (unsigned long long)(reup - p_reup),
             (unsigned long long)(g_ser_gen - p_ser), nd);
    p_pass = g_stat_pass; p_up = up; p_reup = reup; p_ser = g_ser_gen; return b;
}
// When the game itself asks for 30 fps (pacing by the game's present interval, interval 2, hle_nvn.c) the display link runs at 30 Hz too: games whose logic counts frames, such as Engage, are not pulled up to 60 fps by the display link on iOS
__attribute__((weak)) int nvn_game_fps(void) { return 60; }   // hle_nvn.c provides the strong definition
int mtl_fps_target(void) { static int env = -2; if (env == -2) { const char *e = getenv("DM_FPS"); env = e ? atoi(e) : -1; if (env != 30 && env != 60) env = -1; else MLOG("DM_FPS=%d: frame rate pinned by the test switch", env); }
    return env > 0 ? env : nvn_game_fps() == 30 ? 30 : 60; }
// Hand a submission to the GPU in segments (DM_NVN_CUT=draw count, 0 = off; default 64): when the game holds back the next frame's submit until this frame's GPU work is done (The Legend of Zelda: Breath of the Wild: the main thread's nvnSyncWait on the fence of the frame just submitted),
// the GPU sits idle for the milliseconds the host spends translating inside the submit (in the shrine each frame is GPU 13 ms + translation 7 ms back to back). Each time this many draws have accumulated, the command buffer encoded so far is committed between two passes: the GPU runs the earlier passes while the CPU translates the later ones.
// Cut only when no encoder is open and no pass is deferred / merged / hoisted; commit this batch's uploads first (later draws use them). The fence signal and completion work stay on the last segment only: the queue executes in commit order and every encoder starts with a queue barrier
// A buffer → texture copy is cut after as well, at the next pass boundary whatever the draw count (g_cut_copy, set by copy_b2t): on the Switch the GPU runs a submission's copies as soon as the
// submit returns, and a game may reuse the copy's source right after. Catherine: Full Body's resource thread writes the next texture into the staging block 64 ms after the main thread
// recorded a copy from it (main+0x305e64, frame start) and before that frame's submit had even returned: the host was building pipelines inside the submit for 300 ms, and committed the
// copy only at its end, so the subtitle band's gradient (BC3, 32x512) came out as the next texture's blocks. Committed at the first pass boundary, the copies run while the host encodes the rest
static int cut_draws(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_CUT"); v = e ? atoi(e) : 64; MLOG("segmented submission: a segment is submitted between passes every %d draws (DM_NVN_CUT, 0 = off)", v); } return v; }
static uint64_t g_cut_draw0, g_cut_sub, g_cuts, g_copy_cuts;
static void sub_cut(void) {
    int n = cut_draws();
    if (!g_cb || g_enc) return;
    if (g_cut_sub != g_sub_serial) { g_cut_sub = g_sub_serial; g_cut_draw0 = g_stat_draw; }   // count from the start of each submission
    if ((!g_cut_copy && (n <= 0 || g_stat_draw - g_cut_draw0 < (uint64_t)n)) || g_hoist_n || g_defer_nact || g_defer_pend || g_fz.active || g_rt_merged) return;
    if (g_cut_copy) g_copy_cuts++;
    g_cut_copy = 0;
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    up_flush();
    uint64_t f = atomic_load(&g_cur_frame); gpu_tag_begin(f);
    uint32_t k = g_seg++; seg_clk *sc = NULL;   // reports encoded so far keep this segment's GPU times (seg_ts)
    if (seg_ts()) { if (!g_sclk) { g_sclk = calloc(1, sizeof *g_sclk); atomic_store(&g_sclk->refs, 1); } sc = g_sclk; atomic_fetch_add(&sc->refs, 1); }
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> cb) {
        gpu_tag_done(f, cb.GPUStartTime, cb.GPUEndTime);
        if (sc) { if (k < SEG_CLK_N) { atomic_store(&sc->t[k][0], gpu_s_guest_ns(cb.GPUStartTime)); atomic_store(&sc->t[k][1], gpu_s_guest_ns(cb.GPUEndTime)); } seg_clk_put(sc); }
        if (cb.error) { atomic_store(&g_last_commit_error, 1); static int k; if (k++ < 8) MLOG("[RED] segment command buffer execution error: %s", cb.error.description.UTF8String); }
    }];
    res_commit();
    [g_cb endCommandBuffer];
    rec_commit(g_cb, o);
    g_cb = rec_cb_new();
    [g_cb beginCommandBufferWithAllocator:g_alloc4[g_alloc_cur]];   // one allocator encodes several command buffers in sequence; it is reset only once its fence (the last segment) has passed, when a later submission starts
    g_cut_draw0 = g_stat_draw;
    if (!g_cuts++) MLOG("segmented submission: first segment submitted at frame %llu pass %llu", (unsigned long long)f, (unsigned long long)g_stat_pass);
}
uint64_t mtl_cut_count(void) { return g_cuts; }
uint64_t mtl_copy_cut_count(void) { return g_copy_cuts; }

// ---------- Clears a load action cannot express ----------
// A colour clear issued inside an open render pass, with a partial channel mask or with a scissor smaller than the target, and a depth / stencil clear with a scissor inside an
// open pass (Tears of the Kingdom issues all of them) are drawn as one triangle over the whole pass in the render encoder. It writes the clear value into the masked channels of
// that attachment, or the depth / the stencil bits of the clear's write mask, inside the scissor, with the depth test ALWAYS and no visibility counting. The value is read from
// the frame arena through the triangle's own argument tables (the draws' g_vt / g_ft are put back after it), so the pipelines depend only on the pass formats, the slot and the
// mask. Clears the load action expresses keep it. Adapted from MacNativeGames' clear_quad / clear_ds_quad (github.com/ly05010419/MacNativeGames, MIT).
static int pf_int_kind(MTLPixelFormat f) {   // the fragment output type a colour attachment takes: 0 float (also normalized), 1 uint, 2 int
    switch (f) {
        case MTLPixelFormatR8Uint: case MTLPixelFormatR16Uint: case MTLPixelFormatR32Uint: case MTLPixelFormatRG8Uint: case MTLPixelFormatRG16Uint: case MTLPixelFormatRG32Uint:
        case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA32Uint: return 1;
        case MTLPixelFormatR8Sint: case MTLPixelFormatR16Sint: case MTLPixelFormatR32Sint: case MTLPixelFormatRG8Sint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRG32Sint:
        case MTLPixelFormatRGBA8Sint: case MTLPixelFormatRGBA16Sint: case MTLPixelFormatRGBA32Sint: return 2;
        default: return 0;
    }
}
static id<MTLLibrary> clr_lib(int slot, int kind) {   // vertex clr_vs (the triangle, depth from buffer 0) and fragment clr_fs (colour slot `slot` of type `kind` from buffer 0)
    static id<MTLLibrary> libs[8][3]; static uint8_t failed[8][3];
    if (libs[slot][kind] || failed[slot][kind]) return libs[slot][kind];
    static const char *ty[3] = {"float", "uint", "int"}; char src[720];
    snprintf(src, sizeof src, "#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n"
             "vertex V clr_vs(uint i [[vertex_id]], device const float *z [[buffer(0)]]) { V o; o.p = float4(float2(float((i << 1) & 2), float(i & 2)) * 2.0 - 1.0, z[0], 1.0); return o; }\n"
             "struct O { %s4 c [[color(%d)]]; };\nfragment O clr_fs(device const %s4 *v [[buffer(0)]]) { O o; o.c = v[0]; return o; }\n", ty[kind], slot, ty[kind]);
    NSError *e = nil; libs[slot][kind] = [g_dev newLibraryWithSource:@(src) options:nil error:&e];
    if (!libs[slot][kind]) { failed[slot][kind] = 1; MLOG("[RED] clear-triangle shader compile failed (slot %d kind %d): %s", slot, kind, e.localizedDescription.UTF8String); }
    return libs[slot][kind];
}
typedef struct { int32_t slot, kind, mask; uint32_t cfmt[8], dfmt, sfmt, samples; } clr_key;   // slot -1: a depth / stencil clear (no fragment function)
static id<MTLRenderPipelineState> clr_pso(const clr_key *k) {
    static NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *cache; if (!cache) cache = [NSMutableDictionary dictionary];
    NSData *kd = [NSData dataWithBytes:k length:sizeof *k]; id<MTLRenderPipelineState> ps = cache[kd]; if (ps) return ps;
    id<MTLLibrary> lib = clr_lib(k->slot < 0 ? 0 : k->slot, k->slot < 0 ? 0 : k->kind); if (!lib) return nil;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"clr_vs"]; if (k->slot >= 0) d.fragmentFunction = [lib newFunctionWithName:@"clr_fs"];
    for (int i = 0; i < 8; i++) if (k->cfmt[i]) { d.colorAttachments[i].pixelFormat = (MTLPixelFormat)k->cfmt[i]; d.colorAttachments[i].writeMask = MTLColorWriteMaskNone; }   // the other attachments stay as they are
    if (k->slot >= 0) d.colorAttachments[k->slot].writeMask = (MTLColorWriteMask)((k->mask & 1 ? MTLColorWriteMaskRed : 0) | (k->mask & 2 ? MTLColorWriteMaskGreen : 0) | (k->mask & 4 ? MTLColorWriteMaskBlue : 0) | (k->mask & 8 ? MTLColorWriteMaskAlpha : 0));
    d.depthAttachmentPixelFormat = (MTLPixelFormat)k->dfmt; d.stencilAttachmentPixelFormat = (MTLPixelFormat)k->sfmt; d.rasterSampleCount = k->samples ? k->samples : 1;
    NSError *e = nil; ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!ps) { static int n; if (n++ < 4) MLOG("[RED] cannot create the clear-triangle pipeline (slot %d color format %u depth %u stencil %u samples %u): %s; this clear is skipped", k->slot, k->slot >= 0 ? k->cfmt[k->slot] : 0, k->dfmt, k->sfmt, k->samples, e.localizedDescription.UTF8String); return nil; }
    cache[kd] = ps; return ps;
}
static id<MTLDepthStencilState> clr_dss(int dw, unsigned sm) {   // depth ALWAYS, written when dw; stencil ALWAYS → REPLACE through write mask sm (no stencil test when 0)
    static id<MTLDepthStencilState> c[2][256];
    if (c[dw][sm]) return c[dw][sm];
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new]; d.depthCompareFunction = MTLCompareFunctionAlways; d.depthWriteEnabled = dw != 0;
    if (sm) { MTLStencilDescriptor *st = [MTLStencilDescriptor new]; st.stencilCompareFunction = MTLCompareFunctionAlways; st.stencilFailureOperation = st.depthFailureOperation = MTLStencilOperationKeep;
        st.depthStencilPassOperation = MTLStencilOperationReplace; st.readMask = 0xff; st.writeMask = sm; d.frontFaceStencil = d.backFaceStencil = st; }
    return c[dw][sm] = [g_dev newDepthStencilStateWithDescriptor:d];
}
static int clr_scissor(MTLScissorRect *sc) {   // the clear's scissor in the open pass, clamped as for draws; 0 = empty
    NSUInteger tw = g_pass.tw, th = g_pass.th; *sc = (MTLScissorRect){0, 0, tw, th};
    if (!R.has_sc) return tw && th;
    long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
    if (x1 > (long)tw) x1 = (long)tw; if (y1 > (long)th) y1 = (long)th;
    if (x1 <= x0 || y1 <= y0) return 0;
    *sc = (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)}; return 1;
}
static void clr_tri(id<MTL4RenderCommandEncoder> en, id<MTLRenderPipelineState> ps, id<MTLDepthStencilState> ds, MTLScissorRect sc, const uint32_t val[4], float z, uint32_t sref) {
    static id<MTL4ArgumentTable> vt, ft;
    if (!ft) { NSError *e = nil; MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1;
        vt = rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]); ft = vt ? rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]) : nil;
        if (!ft) { vt = nil; static int n; if (!n++) MLOG("[RED] cannot create the clear-triangle argument table: %s; this clear is skipped", e.localizedDescription.UTF8String); return; } }
    id<MTLBuffer> b; size_t o = arena_alloc(32, &b); uint8_t *p = (uint8_t *)b.contents + o; memcpy(p, val, 16); memcpy(p + 16, &z, 4);   // colour at 0, depth at 16
    [ft setAddress:b.gpuAddress + o atIndex:0]; [vt setAddress:b.gpuAddress + o + 16 atIndex:0];
    [en setArgumentTable:vt atStages:MTLRenderStageVertex]; [en setArgumentTable:ft atStages:MTLRenderStageFragment];
    enc_r_pso(en, ps); g_enc_pso = nil; enc_r_dss(en, ds); g_enc_dss = nil;   // the next draw sets its own again
    if (g_enc_vis) { enc_r_visibility(en, MTLVisibilityResultModeDisabled, 0); g_enc_vis = 0; }   // not counted by an occlusion query; the next counted draw turns counting back on
    enc_r_viewport(en, (MTLViewport){0, 0, (double)g_pass.tw, (double)g_pass.th, 0, 1}); enc_r_scissor(en, sc);
    enc_r_cull(en, MTLCullModeNone); enc_r_fill(en, MTLTriangleFillModeFill); enc_r_depth_bias(en, 0, 0, 0); enc_r_depth_clip(en, YES); enc_r_stencil_ref(en, sref, sref); g_ed.ok = 0;
    enc_r_draw(en, MTLPrimitiveTypeTriangle, 0, 3);
    [en setArgumentTable:g_vt atStages:MTLRenderStageVertex]; [en setArgumentTable:g_ft atStages:MTLRenderStageFragment];
}
static void clr_note(int slot, int depth, int stencil) {   // as udc_note_draw for a draw: the attachment was written (zero-source skip, unwritten-attachment store)
    if (slot >= 0 && R.rt.color[slot]) { zskip_del(R.rt.color[slot]->serial); g_zskip_cand_ok[slot] = 0; }
    for (int i = 0; i < g_nsd; i++) { sd_att *t = &g_sd[i];
        if ((t->depth == 0 && slot >= 0 && t->idx == slot) || (t->depth == 1 && depth) || (t->depth == 2 && stencil)) { t->wrote = 1; t->wwhy = 2; } }
}
static uint64_t g_clr_tri[2];   // clears drawn as a triangle: colour, depth / stencil
static void clear_color_tri(const p_clear_color *c) {
    int slot = c->index, mask = c->mask & 0xf;
    if (!mask || slot < 0 || slot >= 8 || slot >= R.rt.n || !R.rt.color[slot]) return;
    id<MTL4RenderCommandEncoder> en = rp_begin();   // a clear outside a pass opens it as a draw would (pending clears land first)
    if (en && !g_pass.cfmt[slot] && g_enc_mode) { rp_end(); en = rp_begin(); }   // opened without this attachment (clear-only / TBDR modes): reopen with all of them
    if (!en) return;
    int at = slot;
    if (!g_pass.cfmt[at] && rt_dup_slot(&R.rt, slot))   // a slot attached twice is attached only once (rt_dup_slot): clear the first one
        for (int j = 0; j < slot; j++) { const tex_t *a = R.rt.color[j], *b = R.rt.color[slot]; if (!a || a->serial != b->serial || !g_pass.cfmt[j]) continue;
            const view_t *va = &R.rt.cview[j], *vb = &R.rt.cview[slot]; int ha = R.rt.has_cview[j], hb = R.rt.has_cview[slot];
            if ((ha && va->set_levels ? va->base_level : 0) == (hb && vb->set_levels ? vb->base_level : 0) && (ha && va->set_layers ? va->min_layer : 0) == (hb && vb->set_layers ? vb->min_layer : 0)) { at = j; break; } }
    if (!g_pass.cfmt[at]) { PD("c %llu color-clear triangle skipped: slot %d is not in this pass", g_stat_pass, slot); return; }
    MTLScissorRect sc; if (!clr_scissor(&sc)) { PD("c %llu color-clear triangle skipped: empty scissor slot %d", g_stat_pass, slot); return; }
    int kind = pf_int_kind((MTLPixelFormat)g_pass.cfmt[at]);
    clr_key k; memset(&k, 0, sizeof k); k.slot = at; k.kind = kind; k.mask = mask; memcpy(k.cfmt, g_pass.cfmt, sizeof k.cfmt); k.dfmt = g_pass.dfmt; k.sfmt = g_pass.sfmt; k.samples = g_pass.samples;
    id<MTLRenderPipelineState> ps = clr_pso(&k); id<MTLDepthStencilState> ds = clr_dss(0, 0); if (!ps || !ds) return;
    uint32_t v[4];
    for (int i = 0; i < 4; i++) { float f = c->rgba[i]; int32_t si;   // integer attachments take the value converted, as a load-action clear does
        if (kind == 1) v[i] = f > 0 ? (f < 4294967040.f ? (uint32_t)f : 0xffffffffu) : 0;
        else if (kind == 2) { si = !(f == f) ? 0 : f >= 2147483520.f ? INT32_MAX : f <= -2147483648.f ? INT32_MIN : (int32_t)f; memcpy(&v[i], &si, 4); }
        else memcpy(&v[i], &f, 4); }
    clr_tri(en, ps, ds, sc, v, 0, 0); clr_note(at, 0, 0);
    if (!g_clr_tri[0]++) MLOG("mid-pass / partial-mask / partial-scissor color clear: drawn as a full-pass triangle (first case pass %llu slot %d mask %#x scissor %lu,%lu %lux%lu)", g_stat_pass, at, mask,
                              (unsigned long)sc.x, (unsigned long)sc.y, (unsigned long)sc.width, (unsigned long)sc.height);
    PD("c %llu color clear drawn as a triangle slot%d ser=%llu mask=%#x scissor=%lu,%lu,%lu,%lu color=%g,%g,%g,%g", g_stat_pass, at, (unsigned long long)R.rt.color[at]->serial, mask,
       (unsigned long)sc.x, (unsigned long)sc.y, (unsigned long)sc.width, (unsigned long)sc.height, c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]);
}
static void clear_ds_tri(const p_clear_ds *c) {
    unsigned want_s = (unsigned)c->stencil_mask & 0xff;
    if (!c->depth_mask && !want_s) return;
    id<MTL4RenderCommandEncoder> en = rp_begin();
    if (en && g_enc_mode && ((c->depth_mask && !g_pass.hd) || (want_s && !g_pass.hs))) { rp_end(); en = rp_begin(); }   // opened without the depth or stencil plane (TBDR lookahead): reopen with all attachments
    if (!en) return;
    int dw = c->depth_mask && g_pass.hd; unsigned sm = g_pass.hs ? want_s : 0;
    if (!dw && !sm) return;
    MTLScissorRect sc; if (!clr_scissor(&sc)) { PD("c %llu depth-stencil clear triangle skipped: empty scissor", g_stat_pass); return; }
    clr_key k; memset(&k, 0, sizeof k); k.slot = -1; memcpy(k.cfmt, g_pass.cfmt, sizeof k.cfmt); k.dfmt = g_pass.dfmt; k.sfmt = g_pass.sfmt; k.samples = g_pass.samples;
    id<MTLRenderPipelineState> ps = clr_pso(&k); id<MTLDepthStencilState> ds = clr_dss(dw, sm); if (!ps || !ds) return;
    float z = c->depth > 0 ? (c->depth < 1 ? c->depth : 1) : 0;   // as clearDepth: within [0, 1] (NaN → 0)
    const uint32_t v[4] = {0};
    clr_tri(en, ps, ds, sc, v, z, (uint32_t)c->stencil & 0xff); clr_note(-1, dw, sm != 0);
    if (!g_clr_tri[1]++) MLOG("mid-pass scissored depth-stencil clear: drawn as a full-pass triangle (first case pass %llu depth %d stencil mask %#x scissor %lu,%lu %lux%lu)", g_stat_pass, dw, sm,
                              (unsigned long)sc.x, (unsigned long)sc.y, (unsigned long)sc.width, (unsigned long)sc.height);
    PD("c %llu depth-stencil clear drawn as a triangle ser=%llu depth=%g(%d) stencil=%d mask=%#x scissor=%lu,%lu,%lu,%lu", g_stat_pass, R.rt.depth ? (unsigned long long)R.rt.depth->serial : 0ull, z, dw, c->stencil, sm,
       (unsigned long)sc.x, (unsigned long)sc.y, (unsigned long)sc.width, (unsigned long)sc.height);
}

void mtl_exec(const uint8_t *p) {
    static int inited; if (!inited) { inited = 1; state_reset(); }
    static int dump_all = -1; if (dump_all < 0) dump_all = getenv("DM_NVN_PASSDUMP_ALL") != NULL;
    int trace = dump_all && passdump();   // Normal mode does not check the frame range per operation; the full command position is only used for the designated diagnostic frames
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        g_exec_cur = p;   // Scan start point of the store lookahead: the operation that ends the render pass (inclusive)
        if (p == g_defer_pend) { for (int gi = 0; gi < 4; gi++) if (g_defer_slots[gi].active && g_defer_slots[gi].p_end_op == p) g_defer_slots[gi].in_p = 0; g_defer_pend = NULL; }
        sdx_seq(p);
        if (trace) PD("o cmd=%p pass=%llu op=%u size=%u", p, g_stat_pass, (unsigned)h.op, (unsigned)h.size);
        // Diagnostic DM_NVN_ALLOCPROF=1: growth of the GPU allocated size is charged to the previous operation's type (rough: allocations on other threads are counted too); the memory report line appends the top 4
        if (g_allocprof < 0) g_allocprof = getenv("DM_NVN_ALLOCPROF") != NULL;
        if (g_allocprof) { static uint64_t last; static int prev = -1; uint64_t now = g_dev.currentAllocatedSize; if (prev >= 0 && prev < 64 && now > last) g_ap_bytes[prev] += now - last; last = now; prev = h.op; }
        if (g_hoist_n && hoist_take(p)) { if (g_fz.active && p == g_fz.rt2_op) { g_fz.phase = 2; g_rt_merged = 0; PD("F %llu merged pass enters the later pass's draws", g_stat_pass); } p += h.size; continue; }   // A pure-clear render pass already executed early / the start of a later pass already merged in   // Entering the later pass: the same-target continuation of the earlier pass ends here; the later pass reading the earlier pass's attachment is proven to be same-pixel by fz_p2_draw, and is no longer split by the continuation rule
        const void *a = p + 8;
        if (g_nlc && !lc_keeps(h.op) && !(h.op == OP_END && g_ret_n)) lc_land();   // see lc_keeps (a sub-list's end returns to its caller)
        switch (h.op) {
            case OP_END: g_exec_cur = NULL;
                if (g_hoist_n && !g_ret_n && !g_nrest) {
                    if (g_fz.active) fz_unskip();
                    if (g_hoist_n) { MLOG("[RED] pure-clear pass hoisting: %d hoisted operations were not reached by the end of the submission; cleared (should not happen)", g_hoist_n); g_hoist_n = 0; }
                }
                return;   // An end-of-pass triggered outside a list (present / CPU readback etc.) must not scan expired command memory: a null start point means store everything
            case OP_BEGIN: break;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8);
                if (g_ret_n < 16) g_ret[g_ret_n] = p + h.size; g_ret_n++;   // Return stack for lookahead: after a sub-list ends, continue looking at the caller
                mtl_exec((const uint8_t *)t); g_ret_n--; break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, (const uint8_t *)a + 8, 4); memcpy(&v, (const uint8_t *)a + 12, 4);
                PD("Y signal_event cmd=%p dst=%p mode=%u value=%u", p, val, m, v); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, (const uint8_t *)a + 8, 8); g_cnt_report[t & 31]++;
                PD("Q report type=%llu cmd=%p dst=%#llx", (unsigned long long)t, p, (unsigned long long)ad);
                if (DM_GAME_RENDER_COND) rc_note(ad, t == 0 || t == 0x10 ? 1 : 2);
                if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) { post((post_op){3, NULL, 0, 0, ad, (uint32_t)t, vis_snapshot()}); g_vis_cur = -1; }
                else mtl_sub_counter(ad, (uint32_t)t);
                break; }
            case OP_RENDER_COND: { const p_render_cond *rc = a;   // recorded only with the profile key render_cond (hle_nvn.c)
                if (rc->mode >= 2) { g_rc_skip = rc->mode == 3; PD("E %llu render enable %d", g_stat_pass, rc->mode == 2); }
                else { uint64_t x = 0, y = 0; int known = rc_value(rc->addr, &x) & rc_value(rc->addr + 16, &y);
                    g_rc_skip = known && (rc->mode ? x == y : x != y);
                    PD("E %llu conditional render %s addr=%#llx values %llu / %llu%s%s", g_stat_pass, rc->mode ? "draw if not equal" : "draw if equal", (unsigned long long)rc->addr, (unsigned long long)x, (unsigned long long)y, known ? "" : " (unknown, drawn)", g_rc_skip ? " → not drawn" : ""); }
                break; }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = g_fence + 1; s->fenced = 1;   // the fence value this submission (segment) commits with
                PD("Y fence_sync cmd=%p sync=%p fence=%llu", p, (void *)s, (unsigned long long)s->fence); break; }
            case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); g_cnt_reset[t & 31]++; PD("Q reset type=%llu cmd=%p", (unsigned long long)t, p);
                if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
#ifdef NVN_RT_RANGE_TEST
                    rr_reset();
#endif
                    g_vis_on = 1; g_vis_counting = 1; vis_clear();
                    // The first reset can come inside a render pass that was begun without a visibility buffer (Octopath Traveler 0: Unreal's occlusion queries, once at boot): split the pass
                    // here, so the continuation is begun with the buffer and its samples are counted (they used not to be: RED "occlusion query counting began in a pass without a visibility buffer")
                    if (g_enc && !g_enc_has_vis) { PD("M %llu occlusion query counting starts: render pass split here", g_stat_pass); rp_end(); } }
                break; }
            case OP_CLEAR_BUF: { const p_clear_buf *cb = a; mtl_sub_fill(cb->addr, cb->size, cb->value); break; }
            case OP_COPY_B2B: { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, (const uint8_t *)a + 8, 8); memcpy(&n, (const uint8_t *)a + 16, 8);
                PD("Y copy_b2b cmd=%p src=%#llx dst=%#llx bytes=%llu", p, (unsigned long long)s_, (unsigned long long)d, (unsigned long long)n); mtl_sub_copy(s_, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8);
                if (p == g_mdx_stop) { g_mdx_epoch++; g_mdx_stop = NULL; }   // it writes a record or count that masked copies were made of (mdx_run)
                PD("Y update_ub cmd=%p src=%#llx dst=%#llx bytes=%llu f=%g,%g,%g,%g", p, (unsigned long long)((const uint8_t *)a + 16), (unsigned long long)d, (unsigned long long)n,
                   n >= 4 ? ((const float *)((const uint8_t *)a + 16))[0] : 0.f, n >= 8 ? ((const float *)((const uint8_t *)a + 16))[1] : 0.f, n >= 12 ? ((const float *)((const uint8_t *)a + 16))[2] : 0.f, n >= 16 ? ((const float *)((const uint8_t *)a + 16))[3] : 0.f); mtl_sub_update((const uint8_t *)a + 16, d, n); break; }
            case OP_SET_RT: {
                const p_set_rt *r = a;
                if (defer_begin_q(p)) break;
                int pend = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
                if (g_enc && !pend && R.has_rt && tbdr_on() && rt_range_same(&R.rt,r)) {
                    g_rt_merged = g_rt_range_active = g_rt_range_pending = 1; R.rt = *r; break; }
                if (g_enc && g_fz.active && g_fz.phase == 1 && fz_reuse_op(p) && !pend && g_enc_mode != 1 && R.has_rt && tbdr_on() && rt_same(&g_fz.rt1, r)) {
                    g_rt_merged = 1; if (!g_rt_merge_n++) MLOG("same-target continuation: first case pass %llu", g_stat_pass); PD("M %llu same-target continuation pass", g_stat_pass); break; }
                // Do not continue a render pass opened only for a clear; one with fewer depth / stencil attachments can be continued (check the planes before drawing)
                if (g_enc && g_enc_mode != 1 && !pend && R.has_rt && tbdr_on() && rt_same(&R.rt, r)) { g_rt_merged = 1; if (!g_rt_merge_n++) MLOG("same-target continuation: first case pass %llu", g_stat_pass); PD("M %llu same-target continuation pass", g_stat_pass); R.rt = *r; break; }
                if (pend && !g_enc && g_cb && lc_on()) lc_push(); else rp_flush();   // clears with no draw wait for the next pass that attaches their targets (lc_push)
                sub_cut(); R.rt = *r; R.has_rt = 1; break;
            }
            case OP_CLEAR_COLOR: {
                const p_clear_color *c = a;
                if (g_rc_skip) { PD("c %llu color clear skipped by conditional rendering slot%d", g_stat_pass, c->index); break; }
                // A scissored clear to 0 outside a pass with all channels (SMBW clears pyramid levels as sub-rectangles of 480×272 and other targets): copy from the zero buffer into the scissor rectangle in the replay stream
                if (!g_enc && c->mask == 0xf && !c->rgba[0] && !c->rgba[1] && !c->rgba[2] && !c->rgba[3] && R.has_sc && c->index >= 0 && c->index < R.rt.n && R.rt.color[c->index]
                    && !scissor_full(R.rt.color[c->index], &R.rt.cview[c->index], R.rt.has_cview[c->index]) && clear_rect_zero(c->index)) break;
                if (g_rt_merged) rp_end();   // A clear arrived in a continued render pass: end it here, and the clear is attached to the start of the next render pass as usual
                if (c->index < 0 || c->index >= R.rt.n || !R.rt.color[c->index]) { RED_ONCE(5, "color clear target %d invalid", c->index); break; }
                if (g_enc || c->mask != 0xf || !scissor_full(R.rt.color[c->index], &R.rt.cview[c->index], R.rt.has_cview[c->index])) {
                    // Inside a pass, with a partial mask or a partial scissor: drawn as a triangle (clear_color_tri)
                    static uint64_t n; static int clog = -1; if (clog < 0) clog = getenv("DM_NVN_CLEARLOG") != NULL; if (clog && n++ < 40) MLOG("color clear drawn as a triangle frame %llu pass %llu target %d ser=%llu %ux%u mid=%d mask=%#x scissor=%d,%d,%d,%d color=%g,%g,%g,%g", (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, c->index,
                        (unsigned long long)R.rt.color[c->index]->serial, R.rt.color[c->index]->d.width, R.rt.color[c->index]->d.height, g_enc != nil, c->mask, R.sc.x, R.sc.y, R.sc.w, R.sc.h, c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]);
                    clear_color_tri(c); break; }
                R.clear_c[c->index] = 1; R.cc[c->index] = MTLClearColorMake(c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]);
                PD("c %llu color clear recorded slot%d ser=%llu mask=%#x scissor=%d:%d,%d,%d,%d", g_stat_pass, c->index, (unsigned long long)R.rt.color[c->index]->serial, c->mask, R.has_sc, R.sc.x, R.sc.y, R.sc.w, R.sc.h);
                break;
            }
            case OP_CLEAR_DS: {
                const p_clear_ds *c = a;
                if (g_rc_skip) { PD("c %llu depth-stencil clear skipped by conditional rendering", g_stat_pass); break; }
                if (g_rt_merged) rp_end();
                // A load-action clear covers the whole attachment and every stencil bit. A clear whose scissor does not cover the depth target, or whose stencil mask is partial, is drawn as a triangle
                // (clear_ds_tri), in or before a pass: before the first draw and for a partial stencil mask in a pass it used to clear the whole target (PR #108)
                unsigned sm = (unsigned)c->stencil_mask & 0xff;
                if (R.rt.depth && (!scissor_full(R.rt.depth, &R.rt.dview, R.rt.has_dview) || (sm && sm != 0xff))) { clear_ds_tri(c); break; }
                // A full clear in the middle of a pass: end the pass and attach the clear to the start of the next one (color attachments load as usual, as a continued pass does above when a clear arrives).
                // It used to be skipped, and later draws depth-tested against stale depth
                if (g_enc) {
                    { static int once; if (!once++) MLOG("mid-pass depth-stencil clear: ending this pass; the clear moves to the start of the next pass (first case pass %llu)", g_stat_pass); }
                    rp_end();
                }
                if (c->depth_mask) { R.clear_d = 1; R.cd = c->depth; }
                if (c->stencil_mask) { R.clear_s = 1; R.cs = (uint32_t)c->stencil; }
                PD("c %llu depth-stencil clear recorded ser=%llu depth=%g depth_mask=%d stencil=%d stencil_mask=%#x", g_stat_pass, R.rt.depth ? (unsigned long long)R.rt.depth->serial : 0ull, c->depth, c->depth_mask, c->stencil, c->stencil_mask);
                break;
            }
            case OP_DRAW: { if (g_rc_skip) break; p_draw t; g_exec_cur = p; g_exec_next = p + h.size; draw(draw_rec(a, h.size, &t), NULL); g_exec_next = NULL; break; }   // Lookahead start point
            case OP_DRAW_IDX: { if (g_rc_skip) break; p_draw_idx t; g_exec_cur = p; g_exec_next = p + h.size; draw(NULL, draw_idx_rec(a, h.size, &t)); g_exec_next = NULL; break; }
            // Do not mark dirty if the state did not change (the game often re-sends the same program / state block / vertex format before each draw; the dirty flag only decides whether to re-fetch the pipeline / depth-stencil object, and equal values mean the same object)
            case OP_SCISSOR: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
            case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL:
            case OP_POLY_OFFSET: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_DEPTH_CLAMP: case OP_VP_SWIZZLE: case OP_BLEND_COLOR: case OP_COPY_STRIDE:
                state_op(h.op, a); break;
            case OP_TF_BIND: { const p_tf_bind *b = a; PD("Y tf bind %d addr=%#llx size=%llu", b->index, (unsigned long long)b->addr, (unsigned long long)b->size); state_op(h.op, a); break; }
            case OP_TF_STATE: { const p_tf_state *b = a; static const char *const w[4] = {"begin", "end", "pause", "resume"}; PD("Y tf %s ctl=%#llx written %llu/%llu/%llu/%llu", w[b->what & 3], (unsigned long long)b->ctl,
                (unsigned long long)S.tf_off[0], (unsigned long long)S.tf_off[1], (unsigned long long)S.tf_off[2], (unsigned long long)S.tf_off[3]); state_op(h.op, a); break; }
            case OP_PRIM_RESTART: state_op(h.op, a); break;
            case OP_RAST_DISCARD: PD("Y rasterizer discard %d", ((const p_rast_discard *)a)->on); state_op(h.op, a); break;
            case OP_COPY_B2T: copy_b2t(a); break;
            case OP_COPY_T2B: copy_t2b(a); break;
            case OP_COPY_T2T: copy_t2t(a); break;
            case OP_CLEAR_TEX: clear_tex(a); break;
            case OP_DISPATCH: dispatch(a); break;
            case OP_BARRIER: { int32_t bits; memcpy(&bits, a, 4);   // ordered at the next draw pass (barrier_game); inside a pass with shader writes, the pass ends first (swrite_note)
                int split = g_enc && g_pass_swrite && (bits & (NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT));
                PD("I %llu barrier bits %#x%s", g_stat_pass, bits, split ? ": ends the pass (shader writes)" : g_enc ? " (render pass open)" : "");
                if (split) { rp_end(); static int once; if (!once++) MLOG("a game barrier (bits %#x) after shader writes in a draw pass: the pass ends there (first case pass %llu)", bits, (unsigned long long)g_stat_pass); }
                if (bits) g_game_barrier = 1; break; }
            case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: state_op(h.op, a); break;
            default: break;
        }
        p += h.size;
    }
}

