// nvn_mtl.m, part: merge
// Pass merging and draw rewrites: the readable tile pass merge, deferred passes, fusing, zero-skip, kd / ms draws and feedback snapshots.
// Included by nvn_mtl.m in its original order, as one translation unit: statics are shared with the other parts.
// Residency stats (mtl_res_stats) key each res_add site by its __LINE__, which in a part counts from the part's own top (this header line keeps two sites of other parts, with the same line, apart).
// ---------- Readable tile render pass merge (merge B step 2) ----------
// When a draw of the later pass samples, at the same pixel, a color attachment written by the earlier pass, the two passes are merged into one render pass: what the earlier pass wrote stays in the tile,
// and the later pass reads it directly with the readable tile version (<key>_bf, function-constant specialization), instead of first landing in main memory and being read back just to be sampled.
// Planning (fuse_try, at the start of the earlier pass) does only structural checks; the closed-form proof of same-pixel addressing is done at every draw of the later pass (fz_p2_draw), and if it cannot be proven the merged render pass is ended and
// a normal render pass is reopened with the later pass's original targets (the earlier pass's result is stored as usual, equivalent to not merging).
static void fz_unskip(void) { for (int i = 0; i < g_fz.nskip; i++) for (int j = 0; j < g_hoist_n; j++) if (g_hoist_skip[j] == g_fz.skip[i]) { g_hoist_skip[j] = g_hoist_skip[--g_hoist_n]; break; } }
static int fz_reuse_op(const uint8_t *p) { for (int i = 0; i < g_fz.nreuse; i++) if (g_fz.reuse[i] == p) return 1; return 0; }
// Diagnostic DM_NVN_FUSE_CHECK=1: in-run double rendering. For the later pass's draws that can be proven, keep a state snapshot per draw; after the merged render pass ends, make a copy of the attachments the later pass wrote (the merge path's result),
// then turn off all lookahead and redraw one by one through the normal path with the later pass's original targets and original clears, and compare the two byte by byte (same frame, same input, no dependence on cross-run determinism). The redraw result is left on the attachment (no effect if identical)
static int fz_check_on(void) { static int v = -1; if (v < 0) v = getenv("DM_NVN_FUSE_CHECK") != NULL; return v; }
typedef struct { int idx; p_draw dw; p_draw_idx di; p_scissor sc; int has_sc; void *S_; } fz_snap;
static fz_snap g_fzs[8]; static int g_nfzs, g_fzs_bad, g_fz_checking; static uint64_t g_fzc_same, g_fzc_diff;
static void draw(const p_draw *dw, const p_draw_idx *di);
static void fz_snap_take(const p_draw *dw, const p_draw_idx *di) {
    if (g_nfzs >= 8) { g_fzs_bad = 1; return; }
    fz_snap *z = &g_fzs[g_nfzs++]; z->idx = di != NULL; if (dw) z->dw = *dw; if (di) z->di = *di; z->sc = R.sc; z->has_sc = R.has_sc;
    z->S_ = malloc(sizeof S); memcpy(z->S_, &S, sizeof S);
}
static void fz_check_run(const fuse_t *fz) {
    if (!g_nfzs || g_fzs_bad) { if (g_fzs_bad) MLOG("[RED] in-game double render: the later pass has more than 8 draws, not compared"); goto out; }
    {
    id<MTLTexture> att[8], scr[8]; uint64_t sers[8]; int na = 0;
    // fz_end has already restored R.rt from the physical union to the later pass's logical targets; wrote is still registered by physical slot and must be aligned through p2map.
    for (int j = 0; j < fz->rt2.n && j < 8; j++) if (fz->p2map[j] >= 0 && fz->wrote[fz->p2map[j]] && fz->rt2.color[j]) {
        id<MTLTexture> t = tex_mtl(fz->rt2.color[j]); if (!t) continue;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:t.width height:t.height mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> c = [g_dev newTextureWithDescriptor:d]; if (!c) continue;
        enc_b_copy_texture(blit(), t, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(t.width, t.height, 1), c, 0, 0, MTLOriginMake(0, 0, 0));
        att[na] = t; scr[na] = c; sers[na] = fz->rt2.color[j]->serial; na++; }
    if (!na) { MLOG("[RED] in-game double render: %d draw snapshots kept, but no comparable attachment", g_nfzs); goto out; }
    static typeof(S) Ssave; memcpy(&Ssave, &S, sizeof S); typeof(R) Rsave = R; const uint8_t *en0 = g_exec_next;
    g_fz_checking = 1; g_exec_next = NULL;   // Turn off all lookahead during the redraw (continuation / hoisting / merging / depth-stencil on demand): normal path
    R.rt = fz->rt2; memcpy(R.clear_c, fz->clr2, 8); memcpy(R.cc, fz->cc2, sizeof R.cc); R.clear_d = fz->cd2f; R.clear_s = fz->cs2f; R.cd = fz->cd2; R.cs = fz->cs2;
    for (int i = 0; i < g_nfzs; i++) { fz_snap *z = &g_fzs[i]; memcpy(&S, z->S_, sizeof S); args_dirty_all(); R.sc = z->sc; R.has_sc = z->has_sc; g_pso_dirty = g_dss_dirty = 1; prog_resolve();
        draw(z->idx ? NULL : &z->dw, z->idx ? &z->di : NULL); }
    rp_end();
    memcpy(&S, &Ssave, sizeof S); args_dirty_all(); R = Rsave; g_exec_next = en0; g_fz_checking = 0; g_pso_dirty = g_dss_dirty = 1; prog_resolve();
    for (int a = 0; a < na; a++) {
        id<MTLTexture> t = att[a], c = scr[a]; NSUInteger W = t.width, H = t.height, bpp = (NSUInteger)(att_bytes(t, 0) / (W * H));
        id<MTLBuffer> b1 = [g_dev newBufferWithLength:W * H * bpp options:MTLResourceStorageModeShared], b2 = [g_dev newBufferWithLength:W * H * bpp options:MTLResourceStorageModeShared];
        id<MTL4ComputeCommandEncoder> b = blit();
        enc_b_copy_texture_buffer(b, c, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(W, H, 1), b1, 0, W * bpp, W * H * bpp);
        enc_b_copy_texture_buffer(b, t, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(W, H, 1), b2, 0, W * bpp, W * H * bpp);
        unsigned long long ser = sers[a], pass = g_stat_pass, frame = atomic_load(&g_cur_frame); NSUInteger nb = W * H * bpp;
        [g_commit_blocks addObject:^{
            const uint8_t *x = b1.contents, *y = b2.contents; uint64_t dpx = 0, maxd = 0;
            for (NSUInteger k = 0; k < nb; k += bpp) if (memcmp(x + k, y + k, bpp)) { dpx++; for (NSUInteger q = 0; q < bpp; q++) { uint64_t dd = (uint64_t)abs((int)x[k + q] - (int)y[k + q]); if (dd > maxd) maxd = dd; } }
            if (dpx) { g_fzc_diff++; MLOG("[RED] in-game double render: frame %llu pass %llu attachment ser=%llu merged vs normal: differing pixels %llu / %lu (max per-byte difference %llu)", frame, pass, ser, (unsigned long long)dpx, (unsigned long)(nb / bpp), (unsigned long long)maxd); }
            else { g_fzc_same++; MLOG("in-game double render: frame %llu pass %llu attachment ser=%llu merged vs normal: identical byte for byte (%lu pixels)", frame, pass, ser, (unsigned long)(nb / bpp)); }
            if ((g_fzc_same + g_fzc_diff) % 300 == 0) MLOG("in-game double render totals: identical %llu times, different %llu times", (unsigned long long)g_fzc_same, (unsigned long long)g_fzc_diff);
        }];
    }
    }
out:
    for (int i = 0; i < g_nfzs; i++) free(g_fzs[i].S_);
    g_nfzs = 0; g_fzs_bad = 0;
}
static void fz_end(void) {   // End of a merged render pass (called by rp_end): interrupted before entering the later pass -> retract the skip entries and restore the targets to the earlier pass's; otherwise restore to the later pass's
    if (!g_fz.active) return;
    if (g_fz.phase == 1) { fz_unskip(); R.rt = g_fz.rt1; } else R.rt = g_fz.rt2;
    g_fz.active = 0;
    if (g_fz.phase == 2 && fz_check_on() && !g_fz_checking) { fuse_t fz = g_fz; R.rt = fz.rt2; fz_check_run(&fz); }
    else { for (int i = 0; i < g_nfzs; i++) free(g_fzs[i].S_); g_nfzs = 0; g_fzs_bad = 0; }
}
static int sub_eq(const p_set_rt *a, int ka, const p_set_rt *b, int kb) { uint64_t s1, s2; int l1, l2, x1, x2; rt_sub(a, ka, &s1, &l1, &x1); rt_sub(b, kb, &s2, &l2, &x2); return s1 && s1 == s2 && l1 == l2 && x1 == x2; }
static int prog_samples_ser(const prog_t *pg, uint64_t tex[6][NTEX], uint64_t img[6][32], uint64_t ser) {   // Whether this texture is bound in any texture / image / typed buffer slot declared by the program's reflection
    __unsafe_unretained NvProg *pr = pg ? prog_get(pg) : nil; if (!pr) return 1;
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || sh->stage >= 6) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? img[sh->stage][b->reg] : 0; else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = tex_slot_h(tex[sh->stage], img[sh->stage], b->reg); else continue;
            if (h && h_ser(h) == ser) return 1; } }
    return 0;
}
static int g_defer_env_ab, g_defer_env_off;   // Environment switches: read once at the start of each submit (defer_reset); the hot path does not call getenv
static void defer_env(void) { const char *x = getenv("DM_NVN_DEFER_AB"), *e = getenv("DM_NVN_DEFER"); g_defer_env_ab = x && !strcmp(x, "1"); g_defer_env_off = e && !strcmp(e, "0"); }
static int defer_on(void) {
    if (!g_defer_env_ab) return tbdr_on() && !g_defer_env_off;
    if (g_defer_ab < 0) { g_defer_ab = 0; g_defer_ab_on = 0; }
    return tbdr_on() && (g_defer_ab || g_defer_ab_on); }
static int defer_handle_sub(uint64_t h, const p_set_rt *r, int k, int cover) {
    int tid = (int)(h & 0xfffff); if (!tid || tid >= g_ntreg || !g_treg[tid].ok) return 0;
    uint64_t ser; int lv, sl; rt_sub(r, k, &ser, &lv, &sl); if (!ser || g_treg[tid].t.serial != ser) return 0;
    if (!g_treg[tid].has_v || !g_treg[tid].v.set_levels) return cover;
    view_t v = g_treg[tid].v; int n = v.nlevels > 0 ? v.nlevels : 1;
    return lv >= v.base_level && lv < v.base_level + n && (!v.set_layers || (sl >= v.min_layer && sl < v.min_layer + (v.nlayers > 0 ? v.nlayers : 1)));
}
static int defer_prog_hits(const prog_t *pg, uint64_t tex[6][NTEX], uint64_t img[6][32], const p_set_rt *r, int color_only, int cover) {
    __unsafe_unretained NvProg *pr = pg ? prog_get(pg) : nil; if (!pr) return 1;
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || sh->stage < 0 || sh->stage >= 6) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h = 0;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? img[sh->stage][b->reg] : 0;
            else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = tex_slot_h(tex[sh->stage], img[sh->stage], b->reg);
            else continue;
            if (!h) continue;
            for (int k = 0; k < (color_only ? r->n : 9); k++) if ((color_only ? r->color[k] : (k < 8 ? r->color[k] : r->depth)) && defer_handle_sub(h, r, k, cover)) return 1;
        }
    }
    return 0;
}
static void defer_collect_reads(defer_t *d, const prog_t *pg, uint64_t tex[6][NTEX], uint64_t img[6][32]) {
    __unsafe_unretained NvProg *pr = pg ? prog_get(pg) : nil; if (!pr) { d->nreads = 513; return; }
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || sh->stage < 0 || sh->stage >= 6) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h = 0;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? img[sh->stage][b->reg] : 0;
            else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = tex_slot_h(tex[sh->stage], img[sh->stage], b->reg);
            else continue;
            if (!h) continue; int seen = 0; for (int j = 0; j < d->nreads; j++) seen |= d->p_reads[j] == h;
            if (!seen && d->nreads < 512) d->p_reads[d->nreads++] = h;
        }
    }
}
static int defer_rt_hits_reads(const p_set_rt *r, int idx) {
    if (idx < 0 || idx >= r->n || !r->color[idx]) return 0;
    for (int i = 0; i < g_defer.nreads; i++) if (defer_handle_sub(g_defer.p_reads[i], r, idx, 1)) return 1;
    return 0;
}
static int defer_q_superset(const p_set_rt *q) {
    memset(g_defer.lmap, 0xff, sizeof g_defer.lmap);
    for (int i = 0; i < g_defer.p_rt.n; i++) if (g_defer.p_rt.color[i]) { int found = 0;
        // The indices must be the same: a normal fragment function's color(n) output is written to physical attachment n, and lmap only changes the write mask / blend and cannot move the output (only the _bf version with output mapping constants can).
        // With different indices, deferral would write P's output into a different attachment of Q.
        if (i < q->n && q->color[i] && sub_eq(&g_defer.p_rt, i, q, i)) { g_defer.lmap[i] = (uint8_t)i; found = 1; }
        if (!found) return 0;
    }
    return 1;
}
static int defer_take(const p_draw *dw, const p_draw_idx *di) {
    if (g_defer.n >= 64) { g_defer.reason = 20; return 0; }
    defer_snap *z = &g_defer.snap[g_defer.n++]; z->idx = di != NULL; if (dw) z->dw = *dw; if (di) z->di = *di;
    z->sc = R.sc; z->has_sc = R.has_sc; z->S_ = malloc(sizeof S); memcpy(z->S_, &S, sizeof S); for (int k = 0; k < 8; k++) z->pm[k] = (uint8_t)k; g_defer_draws++; return 1;
}
static void defer_release(void) { defer_release_one(&g_defer); }
static void defer_env(void);
static void defer_reset(void) { defer_clear_all(); g_defer_replaying = 0; defer_env(); }
static void defer_apply_p_clear(void) {
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
    for (int j = 0; j < g_defer.q_rt.n; j++) { int i = g_defer.lmap[j]; if (i >= 0 && g_defer.clear_c[i]) { R.clear_c[j] = 1; R.cc[j] = g_defer.cc[i]; } }
    if (g_defer.clear_d) { R.clear_d = 1; R.cd = g_defer.cd; } if (g_defer.clear_s) { R.clear_s = 1; R.cs = g_defer.cs; }
}
static int defer_begin_q(const uint8_t *op) {
    int found = 0, first = -1;
    for (int i = 0; i < 4; i++) if (g_defer_slots[i].active && !g_defer_slots[i].at_q && g_defer_slots[i].q_op == op) { found = 1; if (first < 0) first = i; }
    if (!found) return 0;
    rp_flush();
    g_defer_cur = &g_defer_slots[first]; R.rt = g_defer.q_rt; R.has_rt = 1;
    for (int i = 0; i < 4 && !R.rt.depth; i++) { const defer_t *d = &g_defer_slots[i]; if (d->active && !d->at_q && d->q_op == op && d->q_rt.depth) { R.rt.depth = d->q_rt.depth; R.rt.dview = d->q_rt.dview; R.rt.has_dview = d->q_rt.has_dview; } }   // Attach the depth-stencil needed by any group (the depths of groups with the same Q have been checked compatible)
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
    for (int i = 0; i < 4; i++) { defer_t *d = &g_defer_slots[i]; if (!d->active || d->at_q || d->q_op != op) continue;
        for (int j = 0; j < d->q_rt.n; j++) { int k = d->lmap[j]; if (k >= 0 && d->clear_c[k]) { R.clear_c[j] = 1; R.cc[j] = d->cc[k]; } }
        if (d->clear_d) { R.clear_d = 1; R.cd = d->cd; } if (d->clear_s) { R.clear_s = 1; R.cs = d->cs; }
        d->at_q = 1; g_defer_natq++;
    }
    return 1;
}
static void defer_replay(void) {
    if (g_defer_replaying) return;
    int any = 0; for (int i = 0; i < 4; i++) if (g_defer_slots[i].active && g_defer_slots[i].at_q) any = 1;
    if (!any) return;
    typeof(S) save; memcpy(&save, &S, sizeof S); p_scissor sc = R.sc; int has_sc = R.has_sc; int vis = g_vis_counting;
    const uint8_t *en0 = g_exec_next; g_defer_replaying = 1; g_exec_next = NULL;
    for (int gi, after = 0; (gi = defer_next(1, after)) >= 0;) { defer_t *d = &g_defer_slots[gi]; after = d->born;   // In creation order: the groups' P ranges do not overlap and are arranged in sequence, so the group order is the original draw order
        g_defer_cur = d; if (d->reset_between) g_vis_counting = 0;
        { static int once; if (!once++) MLOG("deferred pass merge: first case pass %llu, group %d replays %d draws", g_stat_pass, gi, d->n); }   // With multiple groups, several groups may already have been created before the first replay, so it cannot be judged by g_defer_n == 1
        for (int i = 0; i < d->n; i++) { defer_snap *z = &d->snap[i]; static uint8_t lm[8]; for (int j = 0; j < 8; j++) lm[j] = d->lmap[j] == 0xff || d->lmap[j] >= 8 ? 0xff : z->pm[d->lmap[j]];
            memcpy(&S, z->S_, sizeof S); args_dirty_all(); R.sc = z->sc; R.has_sc = z->has_sc; g_pso_dirty = g_dss_dirty = 1; prog_resolve(); g_pso_lmap = lm; draw(z->idx ? NULL : &z->dw, z->idx ? &z->di : NULL); }
        defer_release_one(d);
    }
    g_vis_counting = vis; memcpy(&S, &save, sizeof S); args_dirty_all(); prog_resolve(); R.sc = sc; R.has_sc = has_sc; g_pso_lmap = NULL; g_pso_fsfn = nil; memset(&g_pso_fz, 0, sizeof g_pso_fz); g_pso_dirty = g_dss_dirty = 1; g_exec_next = en0; g_defer_replaying = 0; g_defer_cur = &g_defer_slots[0];
}
static void defer_fallback(void) {
    if (g_defer_replaying || !defer_has_active()) return;
    g_defer_fallbacks++; MLOG("[RED] deferred pass merge: groups still not replayed at the end of the submission; falling back to each group's original P target");
    if (g_enc) { enc_r_end(g_enc); g_enc = nil; if (g_nbcrt) bcrt_flush(); }
    typeof(S) save; memcpy(&save, &S, sizeof S); p_set_rt rt = R.rt; g_defer_replaying = 1;
    for (int gi, after = 0; (gi = defer_next(0, after)) >= 0;) { defer_t *d = &g_defer_slots[gi]; after = d->born; g_defer_cur = d; R.rt = d->p_rt; R.has_rt = 1; memcpy(R.clear_c, d->clear_c, 8); memcpy(R.cc, d->cc, sizeof R.cc);
        R.clear_d = d->clear_d; R.clear_s = d->clear_s; R.cd = d->cd; R.cs = d->cs;
        for (int i = 0; i < d->n; i++) { defer_snap *z = &d->snap[i]; memcpy(&S, z->S_, sizeof S); args_dirty_all(); R.sc = z->sc; R.has_sc = z->has_sc; g_pso_dirty = g_dss_dirty = 1; g_pso_lmap = z->pm; draw(z->idx ? NULL : &z->dw, z->idx ? &z->di : NULL); }
        rp_end(); defer_release_one(d);
    }
    memcpy(&S, &save, sizeof S); args_dirty_all(); R.rt = rt; g_defer_replaying = 0; g_defer_cur = &g_defer_slots[0];
}
static const uint8_t *defer_cont(int *ci) {   // The next segment after a list ends (within the same submit): first the caller return stack, then the remaining handles of this submit; returns NULL when there are none left
    for (;;) { int k = (*ci)++;
        if (k < g_ret_n) { if (g_ret_n > 16) return NULL; return g_ret[g_ret_n - 1 - k]; }
        k -= g_ret_n; if (k < g_nrest) return (const uint8_t *)g_rest[k];
        return NULL; }
}
// Deferral pre-screen (binary search on the store lookahead index, zero scanning): for every color subresource of P, the first "used as attachment / cleared" event after this draw must be "used as attachment under a different SetRenderTargets,
// whose targets cover all of P's colors"; cleared first / never used as attachment again in this submit -> deferral is impossible, return directly. Read events carry no level; handed to the full lookahead to check by level.
static int defer_pre(void) {
    if (!g_sdx_ok || !g_exec_cur || !g_exec_seq || g_sdx_op[g_exec_seq - 1] != g_exec_cur) return 0;
    uint32_t P = g_exec_seq - 1;
    for (int k = 0; k < R.rt.n; k++) { if (!R.rt.color[k]) continue;
        uint64_t ser; int lv, sl; rt_sub(&R.rt, k, &ser, &lv, &sl);
        int b = sdx_bucket(ser, 0); if (b < 0) return 0;
        uint32_t lo = g_sdx_bst[b], end = g_sdx_bst[b + 1], hi = end;
        while (lo < hi) { uint32_t m = (lo + hi) / 2; if (g_sdx[m].seq <= P) lo = m + 1; else hi = m; }
        int ok = 0, same = 0; uint32_t walk = 0, prt = lo > g_sdx_bst[b] && g_sdx[lo - 1].seq == P ? g_sdx[lo - 1].rtseq : SDX_NONE;   // SetRenderTargets sequence number of P's own render pass (taken from this draw's used-as-attachment event)
        for (uint32_t j = lo; j < end; j++) { const sdx_ev *e = &g_sdx[j];
            if (++walk > 64) return 0;   // Too many events (a heavily sampled texture): not taken as a candidate, conservative and avoids a long walk
            if (g_hoist_n && hoist_skipped(e->op)) continue;
            if (e->kind == SDX_READ || e->kind == SDX_NEED) continue;   // Read events carry no level (reading another level is not a conflict); handed to the full lookahead to check by level
            if (e->plane != 0 || e->lv != lv || e->sl != sl) continue;
            if (e->kind == SDX_CLEAR) return 0;
            if (e->rtseq == SDX_NONE || e->rtseq >= g_sdx_nop) return 0;
            const uint8_t *rop = g_sdx_op[e->rtseq]; const p_set_rt *r = (const p_set_rt *)(rop + 8);
            if (rt_same(r, &R.rt)) { if (e->rtseq != prt) same = 1; continue; }   // Same target: one immediately following is a continuation segment of P, one separated by other render passes is Q; the pre-screen does not distinguish them, and if there is still no other candidate at the end it is handed to the full lookahead
            for (int i = 0; i < R.rt.n; i++) { if (!R.rt.color[i]) continue; int f = 0;
                for (int q = 0; q < r->n && !f; q++) if (r->color[q] && sub_eq(&R.rt, i, r, q)) f = 1;
                if (!f) return 0; }
            ok = 1; break; }
        if (!ok && !same) return 0; }
    return 1;
}
static void defer_try(const p_draw *dw, const p_draw_idx *di) {
    if (!defer_on() || defer_has_in_p() || g_defer_replaying || g_fz.active || g_fz_checking || !g_exec_next || !R.rt.n) return;
    int slot = defer_slot_free(); if (slot < 0) return;
    g_defer_cur = &g_defer_slots[slot];
    int ncol = 0; for (int i = 0; i < R.rt.n; i++) ncol += R.rt.color[i] != NULL; if (!ncol) return;   // Render passes with only depth-stencil attached (shadow maps etc.): Q is defined as covering P's colors, so a P without color can never be deferred -- previously the pre-screen let it through idly and the full lookahead scanned dozens of draws for nothing
    int has_clear = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) has_clear |= R.clear_c[i]; if (!has_clear) return;
    if (!defer_pre()) return;
    memset(&g_defer, 0, sizeof g_defer); g_defer.p_rt = R.rt; memcpy(g_defer.clear_c, R.clear_c, 8); memcpy(g_defer.cc, R.cc, sizeof R.cc);
    g_defer.clear_d = R.clear_d; g_defer.clear_s = R.clear_s; g_defer.cd = R.cd; g_defer.cs = R.cs;
    uint64_t tex[6][NTEX], img[6][32]; memcpy(tex, S.tex, sizeof tex); memcpy(img, S.img, sizeof img); const prog_t *pg = S.prog; ds_st ds = S.ds;
    const uint8_t *p = g_exec_next, *p_end = NULL, *qop = NULL; const p_set_rt *q = NULL; p_set_rt scan_rt = R.rt; int srt_hit = -1, ci = 0, draws = 1, p_has_draw = 1; defer_collect_reads(&g_defer, pg, tex, img);   // The current draw is P's first
    for (int guard = 0; guard < 200000; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        if (g_hoist_n && hoist_skipped(p)) { p += h.size; continue; }
        switch (h.op) {
            case OP_END: { const uint8_t *nx = defer_cont(&ci); if (!nx) { g_defer.reason = 1; goto fail; } p = nx; continue; }
            case OP_CALL: g_defer.reason = 2; goto fail;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_BIND_PROGRAM: pg = ((const p_bind_program *)a)->prog; break;
            case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_IMG: { p_bind_handles t1; const p_bind_handles *b = bind_rec(h.op, a, &t1); bh_fresh(b); int lim = h.op == OP_BIND_IMG ? 32 : NTEX;
                if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) { if (h.op == OP_BIND_IMG) img[b->stage][b->first + k] = b->h[k]; else tex[b->stage][b->first + k] = b->h[k]; } break; }
            case OP_BIND_STATE: { const p_bind_state *b = a; if (b->kind == 5) memcpy(&ds, b->bytes, sizeof ds); break; }
            case OP_DRAW: case OP_DRAW_IDX:
                if (defer_prog_hits(pg, tex, img, &g_defer.p_rt, 1, 1)) { /* Suggestion: here, after identifying that an output mapping is needed but _bf is missing, call bf_want_note. */ g_defer.reason = 7; goto fail; }
                if (p_end) {   // Draws in the intermediate range: must not write textures P reads, must not write P's depth-stencil (and must not test stencil when P writes stencil)
                    if (srt_hit < 0) { srt_hit = 0; for (int k = 0; k < scan_rt.n && !srt_hit; k++) if (scan_rt.color[k] && defer_rt_hits_reads(&scan_rt, k)) srt_hit = 1; }   // Attachments change only at SetRenderTargets: computed once per segment
                    if (srt_hit) { g_defer.reason = 12; goto fail; }
                    if (scan_rt.depth && g_defer.p_rt.depth && scan_rt.depth->serial == g_defer.p_rt.depth->serial && (ds.dw || (g_defer.p_st && ds.st))) { g_defer.reason = 13; goto fail; }
                    break; }
                draws++; if (draws > 64) { g_defer.reason = 20; goto fail; } p_has_draw = 1; defer_collect_reads(&g_defer, pg, tex, img); g_defer.p_dw |= ds.dw; g_defer.p_st |= ds.st && ((S.st_wmask[0] | S.st_wmask[1]) != 0) && (ds.fail_f != NVN_STENCIL_OP_KEEP || ds.zfail_f != NVN_STENCIL_OP_KEEP || ds.zpass_f != NVN_STENCIL_OP_KEEP || ds.fail_b != NVN_STENCIL_OP_KEEP || ds.zfail_b != NVN_STENCIL_OP_KEEP || ds.zpass_b != NVN_STENCIL_OP_KEEP); if (g_defer.p_dw) { g_defer.reason = 8; goto fail; } break;
            case OP_RESET_COUNTER: g_defer.reset_between = 1; break;
            case OP_COPY_T2T: { const p_copy_t2t *c = a;   // Texture copy: allowed only if neither end touches P's attachments and the destination is not a texture P samples
                uint64_t ss = c->src ? c->src->serial : 0, ds_ = c->dst ? c->dst->serial : 0; int bad = !ss || !ds_;
                for (int k = 0; k < g_defer.p_rt.n && !bad; k++) if (g_defer.p_rt.color[k] && (g_defer.p_rt.color[k]->serial == ss || g_defer.p_rt.color[k]->serial == ds_)) bad = 1;
                if (g_defer.p_rt.depth && (g_defer.p_rt.depth->serial == ss || g_defer.p_rt.depth->serial == ds_)) bad = 1;
                for (int k = 0; k < g_defer.nreads && !bad; k++) if (h_ser(g_defer.p_reads[k]) == ds_) bad = 1;
                if (g_defer.nreads > 512) bad = 1;
                if (bad) { g_defer.reason = 3; g_defer.reason_op = h.op; goto fail; } break; }
            case OP_REPORT_COUNTER: case OP_RENDER_COND: case OP_UPDATE_UB: case OP_COPY_B2B: case OP_CLEAR_BUF: case OP_COPY_B2T: case OP_COPY_T2B: case OP_CLEAR_TEX: case OP_DISPATCH: g_defer.reason = 3; g_defer.reason_op = h.op; goto fail;
            case OP_CLEAR_COLOR: { const p_clear_color *c = a; if (!p_end) { g_defer.reason = 16; goto fail; }   // A clear inside P's continuation segment: do not defer
                if (c->index >= 0 && c->index < scan_rt.n && (defer_rt_hits_reads(&scan_rt, c->index) || (scan_rt.color[c->index] && defer_rt_hits_reads(&g_defer.p_rt, c->index)))) { g_defer.reason = 5; goto fail; } break; }
            case OP_CLEAR_DS: if (!p_end) { g_defer.reason = 16; goto fail; } break;
            case OP_SET_RT: { const p_set_rt *r = a;
                if (!p_end && rt_same(r, &g_defer.p_rt)) break;   // The same target set once more (the game often sends it repeatedly): still a continuation segment of P, and its draws are deferred together
                if (!p_end) p_end = p; int shared = 0; for (int i = 0; i < g_defer.p_rt.n; i++) for (int j = 0; j < r->n; j++) if (g_defer.p_rt.color[i] && r->color[j] && sub_eq(&g_defer.p_rt, i, r, j)) shared = 1;
                if (shared) {   // An empty render pass (targets set, then changed again without drawing or clearing) touches no attachment: it is neither taken as Q nor counted as a conflict
                    const uint8_t *e = p + h.size; int ec = ci, empty = 0;
                    for (int g2 = 0; g2 < 200000; g2++) { op_hdr he; memcpy(&he, e, sizeof he);
                        if (he.op == OP_JUMP) { memcpy(&e, e + 8, 8); continue; }
                        if (he.op == OP_END) { const uint8_t *nx = defer_cont(&ec); if (!nx) break; e = nx; continue; }
                        if (he.op == OP_SET_RT) { empty = 1; break; }
                        if (he.op == OP_DRAW || he.op == OP_DRAW_IDX || he.op == OP_CLEAR_COLOR || he.op == OP_CLEAR_DS || he.op == OP_CALL || he.op == OP_DISPATCH || he.op == OP_CLEAR_TEX || he.op == OP_COPY_T2T || he.op == OP_COPY_B2T || he.op == OP_COPY_T2B) break;
                        e += he.size; }
                    if (empty) break; }
                if (!shared) break; if (!p_has_draw || !defer_q_superset(r)) { g_defer.reason = 6; goto fail; } q = r; qop = p; goto q_found; }
            default: break;
        }
        if (h.op == OP_SET_RT) { scan_rt = *(const p_set_rt *)a; srt_hit = -1; }
        p += h.size;
    }
fail:
    if (g_defer.reason && passdump()) PD("G %llu rejected reason=%d op=%d", g_stat_pass + 1, g_defer.reason, g_defer.reason_op); memset(&g_defer, 0, sizeof g_defer); return;
q_found:
    p = qop; op_hdr qh; memcpy(&qh, p, sizeof qh); p += qh.size; int q_body = 0, q_clears = 0;   // Binding tracking continues from the main scan's state (does not go back to S at the start of P)
    for (int guard = 0; guard < 200000; guard++) { op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8; if (h.op == OP_JUMP) { memcpy(&p, a, 8); continue; }
        // The whole Q segment (up to the next SetRenderTargets / end of list): no draw may sample P's color attachments through a view covering P's level -- after deferral P's result is still in the tile and has not landed in main memory, so sampling would read old content
        if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) { if (defer_prog_hits(pg, tex, img, &g_defer.p_rt, 1, 1)) { g_defer.reason = 14; goto fail; } q_body = 1; p += h.size; continue; }
        if (q_body && (h.op == OP_SET_RT || h.op == OP_END)) break;
        if (!q_body && h.op == OP_END) { const uint8_t *nx = defer_cont(&ci); if (!nx) { g_defer.reason = 9; goto fail; } p = nx; continue; }
        if (h.op == OP_CALL) { g_defer.reason = 9; g_defer.reason_op = h.op; goto fail; }
        // After Q's SetRenderTargets and before its first draw and any clear, another SetRenderTargets arrives (the game sets targets repeatedly, the previous one being an empty render pass): if the new targets still cover P take it as Q, otherwise do not defer
        if (!q_body && h.op == OP_SET_RT && !q_clears) { const p_set_rt *r2 = a; if (!defer_q_superset(r2)) { g_defer.reason = 15; goto fail; } q = r2; qop = p; p += h.size; continue; }
        if (h.op == OP_CLEAR_COLOR || h.op == OP_CLEAR_DS) q_clears = 1;
        if (!q_body && (h.op == OP_END || h.op == OP_SET_RT || h.op == OP_UPDATE_UB || h.op == OP_COPY_B2B || h.op == OP_CLEAR_BUF || h.op == OP_DISPATCH)) { g_defer.reason = 9; g_defer.reason_op = h.op; goto fail; }
        if (h.op == OP_BIND_PROGRAM) pg = ((const p_bind_program *)a)->prog; else if (h.op == OP_BIND_TEX || h.op == OP_BIND_TEX1 || h.op == OP_BIND_IMG) { p_bind_handles t1; const p_bind_handles *b = bind_rec(h.op, a, &t1); bh_fresh(b); int lim = h.op == OP_BIND_IMG ? 32 : NTEX; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) { if (h.op == OP_BIND_IMG) img[b->stage][b->first + k] = b->h[k]; else tex[b->stage][b->first + k] = b->h[k]; } }
        else if (h.op == OP_CLEAR_COLOR) { const p_clear_color *c = a; if (c->index >= 0 && c->index < q->n && q->color[c->index]) for (int i = 0; i < g_defer.p_rt.n; i++) if (g_defer.p_rt.color[i] && sub_eq(q, c->index, &g_defer.p_rt, i)) { g_defer.reason = 10; goto fail; } }
        p += h.size;
    }
    if (!q || !defer_q_superset(q) || g_defer.n >= 64) { g_defer.reason = 11; goto fail; }
    for (int i = 0; i < 4; i++) if (&g_defer_slots[i] != &g_defer && g_defer_slots[i].active && g_defer_slots[i].q_op == qop && !defer_same_q_depth(&g_defer_slots[i], q)) { g_defer.reason = 17; goto fail; }
    g_defer.q_rt = *q; if (!q->depth && g_defer.p_rt.depth) { g_defer.q_rt.depth = g_defer.p_rt.depth; g_defer.q_rt.dview = g_defer.p_rt.dview; g_defer.q_rt.has_dview = g_defer.p_rt.has_dview; } { int na = 0; for (int i = 0; i < 4; i++) { const defer_t *d = &g_defer_slots[i]; if (d != &g_defer && d->active && d->at_q) na += d->n; }
      if (na + 1 > 64) { g_defer.reason = 20; goto fail; } }
    g_defer.q_op = qop; g_defer.p_end_op = p_end; g_defer.in_p = 1; g_defer.active = 1; g_defer_nact++; g_defer_pend = p_end; g_defer_n++; g_defer.born = g_defer_n;
    // Chained: this render pass is exactly the Q of upstream groups (they are at_q, waiting to be replayed when this render pass opens), and this render pass is itself deferred as a whole -- the upstream groups are merged into the new group in creation order:
    // the target space of an upstream group = this render pass's targets = the new group's P targets, and the snapshot's mapping is composed as pm' = upstream lmap o pm; the upstream clears are already in R and are carried along with the new group below
    for (int gi, after = 0; (gi = defer_next(1, after)) >= 0;) { defer_t *d = &g_defer_slots[gi]; after = d->born; if (d == &g_defer) continue;
        for (int k = 0; k < 8; k++) if (d->lmap[k] != 0xff && d->lmap[k] != k) { g_defer.reason = 18; goto fail; }   // Only merge in upstream groups with an identity mapping (see defer_q_superset)
        for (int i = 0; i < d->n; i++) { defer_snap z = d->snap[i]; uint8_t pm[8]; for (int k = 0; k < 8; k++) pm[k] = d->lmap[k] == 0xff || d->lmap[k] >= 8 ? 0xff : z.pm[d->lmap[k]];
            memcpy(z.pm, pm, 8); g_defer.snap[g_defer.n++] = z; }
        g_defer.reset_between |= d->reset_between; d->n = 0; defer_release_one(d); } if (!defer_take(dw, di)) { defer_release_one(&g_defer); return; }
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0; PD("G %llu accepted group %d P→Q draws=%d", g_stat_pass + 1, slot, draws);
}
// Pre-screen of the necessary condition for merging (binary search on the submit index, zero scanning on failure): merging requires that "the first draw of the later pass (the next SET_RT with a different target that has not been hoisted)
// samples some color attachment of the earlier pass", i.e. there is an SDX_READ event for that attachment at that draw's sequence number. If not satisfied there is certainly no merge, saving the per-operation scan below;
// only if satisfied does it go through the original planning (the decision is exactly the same as without the pre-screen). When the index is unavailable, plan as before.
static int fz_prefilter(void) {
    if (!g_sdx_ok || !g_exec_seq || g_sdx_op[g_exec_seq - 1] != g_exec_cur) return 1;
    uint32_t P = g_exec_seq - 1, lo = 0, hi = g_rtx_n;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (g_rtx_seq[m] <= P) lo = m + 1; else hi = m; }
    for (; lo < g_rtx_n; lo++) { const uint8_t *op = g_sdx_op[g_rtx_seq[lo]]; if (g_hoist_n && hoist_skipped(op)) continue; if (!rt_same(&R.rt, (const p_set_rt *)(op + 8))) break; }
    if (lo >= g_rtx_n) return 0;
    uint32_t r2 = g_rtx_seq[lo], a = 0, b = g_drx_n;
    while (a < b) { uint32_t m = (a + b) / 2; if (g_drx_seq[m] <= r2) a = m + 1; else b = m; }
    if (a >= g_drx_n) return 0;
    uint32_t fd = g_drx_seq[a];
    for (uint32_t k = lo + 1; k < g_rtx_n && g_rtx_seq[k] < fd; k++) if (!(g_hoist_n && hoist_skipped(g_sdx_op[g_rtx_seq[k]]))) return 0;   // The target changes again before the first draw: step two of the original planning gives up as well
    for (int j = 0; j < R.rt.n && j < 8; j++) { if (!R.rt.color[j]) continue; int bk = sdx_bucket(R.rt.color[j]->serial, 0); if (bk < 0) continue;
        uint32_t e0 = g_sdx_bst[bk], e1 = g_sdx_bst[bk + 1];
        while (e0 < e1) { uint32_t m = (e0 + e1) / 2; if (g_sdx[m].seq < fd) e0 = m + 1; else e1 = m; }
        for (; e0 < g_sdx_bst[bk + 1] && g_sdx[e0].seq == fd; e0++) if (g_sdx[e0].kind == SDX_READ) return 1; }
    return 0;
}
static void fuse_try(void) {
    if (g_fz.active || !g_exec_next || g_hoist_n + 12 > HOIST_MAX || !R.rt.n) return;
    if (!fz_prefilter()) return;
    // One: from the earlier pass to the next SetRenderTargets, only draws and state are allowed
    const uint8_t *p = NULL, *r2op = NULL, *reuse[8]; int nreuse = 0; const p_set_rt *r2 = NULL; p_scissor sc = R.sc; int has_sc = R.has_sc;
    fz_cursor cur = {.p = g_exec_next}; op_hdr h; const void *a;
    for (int guard = 0; guard < 200000 && !r2; guard++) {
        if (!fz_next(&cur, &p, &h, &a)) return;
        switch (h.op) {
            case OP_COPY_T2T: case OP_COPY_B2T: case OP_COPY_T2B: case OP_CLEAR_TEX: case OP_DISPATCH: case OP_RENDER_COND:
            case OP_CLEAR_COLOR: case OP_CLEAR_DS: case OP_UPDATE_UB: case OP_COPY_B2B: case OP_CLEAR_BUF: return;
            case OP_SCISSOR: sc = *(const p_scissor *)a; has_sc = 1; break;
            case OP_SET_RT: {
                int pend = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
                if (rt_same(&R.rt, a)) {
                    if (g_enc_mode == 1 || pend || !R.has_rt || !tbdr_on() || nreuse >= 8) return;
                    reuse[nreuse++] = p; continue;
                }
                r2 = a; r2op = p; continue;
            }
            default: break;
        }
    }
    if (!r2 || rt_same(&R.rt, r2)) return;
    // Two: from the start of the later pass to its first draw, only clears (not clearing shared attachments) and state are allowed
    const uint8_t *cl[8]; int ncl = 0; uint8_t clr[8] = {0}; MTLClearColor cc[8]; int cd_f = 0, cs_f = 0; double cdv = 0; uint32_t csv = 0;
    if (!fz_next(&cur, &p, &h, &a)) return;   // Continue from the cursor of step one (already past the later pass's SET_RT, keeping the cross-handle / return stack position)
    for (int guard = 0; guard < 200000; guard++) {
        if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) break;
        switch (h.op) {
            case OP_COPY_T2T: case OP_COPY_B2T: case OP_COPY_T2B: case OP_CLEAR_TEX: case OP_DISPATCH: case OP_SET_RT: case OP_UPDATE_UB: case OP_COPY_B2B: case OP_CLEAR_BUF: case OP_RENDER_COND: return;
            case OP_SCISSOR: sc = *(const p_scissor *)a; has_sc = 1; break;
            case OP_CLEAR_COLOR: { const p_clear_color *c = a; if (ncl >= 8 || c->index < 0 || c->index >= r2->n || !r2->color[c->index] || c->mask != 0xf) return;
                const tex_t *t = r2->color[c->index];
                if (has_sc && !(sc.x <= 0 && sc.y <= 0 && sc.x + sc.w >= (int)t->d.width && sc.y + sc.h >= (int)t->d.height)) return;
                clr[c->index] = 1; cc[c->index] = MTLClearColorMake(c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]); cl[ncl++] = p; break; }
            case OP_CLEAR_DS: { const p_clear_ds *c = a; if (ncl >= 8 || !r2->depth) return;
                if (c->depth_mask) { cd_f = 1; cdv = c->depth; } if (c->stencil_mask) { cs_f = 1; csv = (uint32_t)c->stencil; } cl[ncl++] = p; break; }
            default: break;
        }
        if (!fz_next(&cur, &p, &h, &a)) return;
    }
    // Three: union of attachments (the earlier pass's attachments keep their original indices, those unique to the later pass are appended), consistent sizes, compatible depth
    p_set_rt U = R.rt; int n = R.rt.n; int8_t p2map[8]; memset(p2map, -1, sizeof p2map);
    for (int k = 0; k < r2->n && k < 8; k++) { if (!r2->color[k]) continue;
        int j = -1; for (int m = 0; m < R.rt.n; m++) if (R.rt.color[m] && sub_eq(&R.rt, m, r2, k)) { j = m; break; }
        if (j < 0) { if (n >= 8) return; j = n++; U.color[j] = r2->color[k]; U.cview[j] = r2->cview[k]; U.has_cview[j] = r2->has_cview[k]; }
        else if (clr[k]) return;   // The start of the later pass wants to clear an attachment the earlier pass is drawing to: after merging this clear has nowhere to go
        p2map[k] = (int8_t)j; }
    U.n = n;
    { uint64_t s0; int l0, x0; rt_sub(&U, 0, &s0, &l0, &x0); if (!U.color[0]) return;
      uint32_t w0 = MAX(U.color[0]->d.width >> l0, 1u), h0 = MAX(U.color[0]->d.height >> l0, 1u);
      for (int j = 1; j < n; j++) { uint64_t s_; int l_, x_; rt_sub(&U, j, &s_, &l_, &x_); if (!U.color[j] || MAX(U.color[j]->d.width >> l_, 1u) != w0 || MAX(U.color[j]->d.height >> l_, 1u) != h0) return; } }
    int depth_from_r2 = 0;
    if (R.rt.depth && r2->depth && !sub_eq(&R.rt, 8, r2, 8)) return;
    if (!R.rt.depth && r2->depth) { U.depth = r2->depth; U.dview = r2->dview; U.has_dview = r2->has_dview; depth_from_r2 = 1; }
    if ((cd_f || cs_f) && !depth_from_r2) return;   // The depth is already in use in the earlier pass: the depth clear at the start of the later pass has nowhere to go
    // Four: draws of the earlier pass must not sample attachments brought in by the later pass; when the depth comes from the later pass, depth-stencil testing must not be enabled
    static uint64_t tex[6][NTEX], img[6][32]; memcpy(tex, S.tex, sizeof tex); memcpy(img, S.img, sizeof img);
    const prog_t *pg = S.prog; ds_st ds = S.ds;
    cur = (fz_cursor){.p = g_exec_next};
    for (;;) {   // The first loop iteration checks the draw that opens the render pass (current state), then checks the draws in the range one by one
        if (depth_from_r2 && (ds.dt || ds.dw || ds.st)) return;
        for (int j = R.rt.n; j < n; j++) if (prog_samples_ser(pg, tex, img, U.color[j]->serial)) return;
        if (depth_from_r2 && prog_samples_ser(pg, tex, img, U.depth->serial)) return;
        for (;;) {
            const uint8_t *q; op_hdr hq; const void *aq;
            if (!fz_next(&cur, &q, &hq, &aq)) return;
            if (q == r2op) goto planned;
            if (hq.op == OP_BIND_PROGRAM) pg = ((const p_bind_program *)aq)->prog;
            else if (hq.op == OP_BIND_STATE) { const p_bind_state *b = aq; if (b->kind == 5) memcpy(&ds, b->bytes, sizeof ds); }
            else if (hq.op == OP_BIND_TEX || hq.op == OP_BIND_TEX1 || hq.op == OP_BIND_IMG) { p_bind_handles t1; const p_bind_handles *b = bind_rec(hq.op, aq, &t1); bh_fresh(b); int lim = hq.op == OP_BIND_IMG ? 32 : NTEX;
                if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) { if (hq.op == OP_BIND_IMG) img[b->stage][b->first + k] = b->h[k]; else tex[b->stage][b->first + k] = b->h[k]; } }
            else if (hq.op == OP_DRAW || hq.op == OP_DRAW_IDX) break;
        }
    }
planned:
    {   // Five: pre-screen the first draw of the later pass (static conditions), to avoid attaching the union of attachments for nothing: track the program and texture bindings up to that draw
        const prog_t *pg2 = S.prog; static uint64_t t2[6][NTEX], i2[6][32]; memcpy(t2, S.tex, sizeof t2); memcpy(i2, S.img, sizeof i2);
        cur = (fz_cursor){.p = g_exec_next};
        for (;;) { const uint8_t *q; op_hdr hq; const void *aq; if (!fz_next(&cur, &q, &hq, &aq)) return; if (q == p) break;
            if (hq.op == OP_BIND_PROGRAM) pg2 = ((const p_bind_program *)aq)->prog;
            else if (hq.op == OP_BIND_TEX || hq.op == OP_BIND_TEX1 || hq.op == OP_BIND_IMG) { p_bind_handles t1; const p_bind_handles *b = bind_rec(hq.op, aq, &t1); bh_fresh(b); int lim = hq.op == OP_BIND_IMG ? 32 : NTEX;
                if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) { if (hq.op == OP_BIND_IMG) i2[b->stage][b->first + k] = b->h[k]; else t2[b->stage][b->first + k] = b->h[k]; } }
        }
        NvProg *pr2 = pg2 ? prog_get(pg2) : nil; NvShader *vs2 = pr2 ? pr2->sh[0] : nil, *fs2 = pr2 ? pr2->sh[1] : nil;
        if (!vs2 || !fs2) return;
        int touches = 0;
        for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr2->sh[st]; if (!sh || sh->stage >= 6) continue;
            for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *bb = &sh->rf.b[i]; uint64_t h;
                if (bb->kind == NVN_RB_IMG) h = bb->reg < 32 ? i2[sh->stage][bb->reg] : 0; else if (bb->kind == NVN_RB_TEX || bb->kind == NVN_RB_TBUF) h = tex_slot_h(t2[sh->stage], i2[sh->stage], bb->reg); else continue;
                uint64_t ser = h ? h_ser(h) : 0; if (!ser) continue;
                int j = -1; for (int m = 0; m < R.rt.n; m++) if (R.rt.color[m] && R.rt.color[m]->serial == ser) { j = m; break; }
                if (j < 0) continue;
                if (sh != fs2 || bb->kind != NVN_RB_TEX || (fs2->rf.fc_mixed[bb->reg >> 6] >> (bb->reg & 63) & 1)) return;
                int any = 0;
                for (int c = 0; c < fs2->rf.nfc; c++) if (fs2->rf.fc_reg[c] == bb->reg) { int l = fs2->rf.fc_locn[c];
                    if (l >= 32 || (vs2->rf.uv_attr[l] < 0 && vs2->rf.uv_aff_attr[l] < 0)) return; any = 1; }
                if (!any) return;
                touches = 1; } }
        if (!touches) return;   // The later pass does not read the earlier pass's attachments: merging has no benefit
        int ident = 1; for (int c = 0; c < 8; c++) if (fs2->rf.color_mask >> c & 1) { if (p2map[c] < 0) return; if (p2map[c] != c) ident = 0; }
        if (!ident && !fs2->rf.nfo) return;
    }
    memset(&g_fz, 0, sizeof g_fz);
    g_fz.active = 1; g_fz.phase = 1; g_fz.n1 = R.rt.n; g_fz.nreuse = nreuse; g_fz.rt1 = R.rt; g_fz.rt2 = *r2; g_fz.rt2_op = r2op; memcpy(g_fz.reuse, reuse, sizeof reuse); memcpy(g_fz.p2map, p2map, 8);
    for (int j = 0; j < 8; j++) g_fz.lmap1[j] = j < R.rt.n ? (uint8_t)j : 0xff;
    fz_probe_cov2(r2, p, p2map, clr, g_fz.n1, n);
    g_fz.skip[g_fz.nskip++] = r2op; for (int i = 0; i < ncl; i++) g_fz.skip[g_fz.nskip++] = cl[i];
    for (int i = 0; i < g_fz.nskip; i++) g_hoist_skip[g_hoist_n++] = g_fz.skip[i];
    R.rt = U;
    memcpy(g_fz.clr2, clr, 8); memcpy(g_fz.cc2, cc, sizeof cc); g_fz.cd2f = (uint8_t)cd_f; g_fz.cs2f = (uint8_t)cs_f; g_fz.cd2 = cdv; g_fz.cs2 = csv;
    for (int k = 0; k < 8; k++) if (clr[k]) { R.clear_c[p2map[k]] = 1; R.cc[p2map[k]] = cc[k]; }
    if (cd_f) { R.clear_d = 1; R.cd = cdv; } if (cs_f) { R.clear_s = 1; R.cs = csv; }
    if (!g_fz_n++) MLOG("readable-tile pass merge: first case pass %llu (%d physical attachments)", g_stat_pass + 1, n);
    PD("F %llu merge with the next pass (readable tile): physical attachments %d, previous pass %d", g_stat_pass + 1, n, R.rt.n == n ? g_fz.n1 : g_fz.n1);
}
static id<MTLFunction> bf_fn(NvShader *fs, const int *fx, uint8_t fbmask, const uint8_t *lmap) {   // The readable tile version is specialized by constants (cached)
    os_unfair_lock_lock(&g_fz_fn_lock);
    if (!g_bf_fn_cache) { g_bf_fn_cache = [NSMutableDictionary dictionary]; g_bf_lib_cache = [NSMutableDictionary dictionary]; }
    size_t kl = strlen(fs->key); NSString *base = [NSString stringWithFormat:@"%.*s", (int)(kl > 2 ? kl - 2 : kl), fs->key];
    id<MTLLibrary> lib = g_bf_lib_cache[base];
    if (!lib) {
        NSError *e = nil; NSString *path = [NSString stringWithFormat:@"%@/%@_bf.metallib", g_msl_dir, base];
        lib = lib_from_file(path, &e);
        if (!lib) MLOG("[RED] failed to load the readable-tile version %s: %s", path.UTF8String, e.localizedDescription.UTF8String);
        else g_bf_lib_cache[base] = lib;
    }
    if (!lib) { os_unfair_lock_unlock(&g_fz_fn_lock); return nil; }
    NSMutableString *k = [NSMutableString stringWithFormat:@"%02x", fbmask];
    for (int i = 0; i < fs->rf.nfc; i++) [k appendFormat:@",%d", fx[i]];
    for (int j = 0; j < 8; j++) [k appendFormat:@"/%02x", lmap[j]];
    NSString *cacheKey = [NSString stringWithFormat:@"%@|%@", base, k];
    id<MTLFunction> f = g_bf_fn_cache[cacheKey];
    if (f) { os_unfair_lock_unlock(&g_fz_fn_lock); return f; }
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
    for (int i = 0; i < fs->rf.nfc; i++) { int v = fx[i]; [cv setConstantValue:&v type:MTLDataTypeInt atIndex:(NSUInteger)i]; }
    for (int q = 0; q < 8; q++) { bool b = fbmask >> q & 1; [cv setConstantValue:&b type:MTLDataTypeBool atIndex:(NSUInteger)(32 + q)]; }
    for (int j = 0; j < fs->rf.nfo; j++) for (int q = 0; q < 8; q++) { bool b = lmap[q] == fs->rf.fo_color[j]; [cv setConstantValue:&b type:MTLDataTypeBool atIndex:(NSUInteger)(64 + 8 * j + q)]; }
    NSError *e = nil; f = fn4_note([lib newFunctionWithName:@"main0" constantValues:cv error:&e], lib, @"main0", cv);
    if (!f) { MLOG("[RED] readable-tile version specialization failed %s (%s): %s", fs->key, k.UTF8String, e.localizedDescription.UTF8String); os_unfair_lock_unlock(&g_fz_fn_lock); return nil; }
    g_bf_fn_cache[cacheKey] = f; os_unfair_lock_unlock(&g_fz_fn_lock); return f;
}
static int ubo_float(int slot, uint16_t ref, float *out) {   // Component cmp of vector vec of the uniform slot (read on the CPU side; not read if it falls within a buffer write pending execution in this submit)
    int sl = ref >> 8, vec = ref >> 2 & 63, cmp = ref & 3;
    if (slot < 0 || slot >= 6 || sl < 0 || sl >= 16) return 0;
    const p_bind_buf *u = &S.ubo[slot][sl]; uint64_t off = (uint64_t)vec * 16 + (uint64_t)cmp * 4;
    if (!u->addr || off + 4 > u->size || pend_hits(u->addr + off, 4)) return 0;
    void *mb; uint64_t mo; const uint8_t *base;
    if (!mtl_addr(u->addr + off, 4, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) return 0;
    memcpy(out, base + mo, 4); return 1;
}
static int zskip_fact_for(const char *key, zskip_fact *out) {
    if (!key) return 0;
    for (int i = 0; i < g_zskip_nf; i++) if (!strncmp(key, g_zskip_facts[i].key, strlen(g_zskip_facts[i].key))) { *out = g_zskip_facts[i]; return 1; }   // Runtime keys carry the _b suffix
    return 0;
}
static int zskip_view_zero(int tid) {
    if (!tid || tid >= g_ntreg || !g_treg[tid].ok) return 0;
    treg_t *r = &g_treg[tid]; const tex_t *t = &r->t; int lv0 = r->has_v && r->v.set_levels ? r->v.base_level : 0;
    int nl = r->has_v && r->v.set_levels && r->v.nlevels > 0 ? r->v.nlevels : 1;
    int sl0 = r->has_v && r->v.set_layers ? r->v.min_layer : 0;
    int ns = r->has_v && r->v.set_layers && r->v.nlayers > 0 ? r->v.nlayers : 1;
    for (int l = 0; l < nl; l++) for (int s = 0; s < ns; s++) if (!zskip_has(t->serial, lv0 + l, sl0 + s)) return 0;
    return 1;
}
// While an occlusion query is counting, a skipped draw should have contributed passed samples: it only does not matter if "there is no report before the next reset".
// Looking forward in replay order from after this draw: sample count reset encountered first -> does not matter (return 1); sample count report encountered first, or the end cannot be seen (submit ends) -> matters (return 0)
static int zskip_vis_dead(void) {
    if (!g_vis_counting) return 1;
    const uint8_t *p = g_exec_next; int ci = 0; if (!p) return 0;
    for (int guard = 0; guard < 200000; guard++) { op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        if (h.op == OP_JUMP) { memcpy(&p, a, 8); continue; }
        if (h.op == OP_END) { p = defer_cont(&ci); if (!p) return 0; continue; }
        if (h.op == OP_CALL) return 0;
        if (h.op == OP_RESET_COUNTER || h.op == OP_REPORT_COUNTER) { uint64_t t; memcpy(&t, a, 8); if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) return h.op == OP_RESET_COUNTER; }
        p += h.size; }
    return 0;
}
static int zskip_can_draw(NvProg *pr) {
    // Skip conditions (all must hold): the fragment key is in the fact table, the view bound to every texture slot declared by its reflection covers only subresources known to be all zero, the coefficient is finite,
    // no depth write and no stencil, not counting an occlusion query, and every color attachment to be written is already 0 (a candidate cleared entirely to 0 in this render pass, or known all zero) -- writing 0 over 0.
    // Reasons for not holding (the N line of PASSDUMP): 1 depth / stencil 2 occlusion query counting with a report before the reset 3 coefficient unreadable or non-finite 4 image slot 5 sampled view not known all zero 6 no target 7 target is not 0
    if (!zskip_on() || !pr) return 0;
    NvShader *fs = nil; for (int st = 0; st < 5; st++) if (pr->sh[st] && pr->sh[st]->stage == 1) fs = pr->sh[st];
    zskip_fact f; float coeff; int why = 0;
    if (!fs || !zskip_fact_for(fs->key, &f)) return 0;
    if (S.ds.dw || S.ds.st) why = 1;
    else if (!zskip_vis_dead()) why = 2;
    else if (f.ubo_slot >= 0 && (!ubo_float(1, (uint16_t)((f.ubo_slot << 8) | (f.vec << 2) | f.comp), &coeff) || !isfinite(coeff))) why = 3;   // ubo_slot < 0: this shader has no coefficient
    for (int i = 0; !why && i < fs->rf.nb; i++) { nvn_rbind *b = &fs->rf.b[i];
        if (b->kind == NVN_RB_IMG) { why = 4; break; }
        if (b->kind != NVN_RB_TEX && b->kind != NVN_RB_TBUF) continue;
        int tid = (int)(tex_slot_h(S.tex[1], S.img[1], b->reg) & 0xfffff); if (!zskip_view_zero(tid)) why = 5; }
    if (!why && (!R.has_rt || !R.rt.n)) why = 6;
    for (int i = 0; !why && i < R.rt.n && i < 8; i++) { if (!R.rt.color[i]) continue;
        int pending = g_enc ? g_zskip_cand_ok[i] : zskip_zero_clear(i);   // Render pass already open: look at the candidates noted when the render pass was opened (not yet drawn to); not open: look at the pending clears
        if (!pending && (g_enc || R.clear_c[i] || !zskip_target_zero(R.rt.color[i], &R.rt.cview[i], R.rt.has_cview[i]))) why = 7; }
    if (why) PD("N %llu zero-source draw not skipped reason=%d set %d", g_stat_pass + 1, why, g_zskip_n);
    return !why;
}
static int uv_proof(NvShader *vs, int locn, const p_draw *dw, const p_draw_idx *di) {   // The interpolated output locn at every pixel center is exactly this pixel's texel center: u = (x + 1) / 2, v = (1 - y) / 2, w = 1, viewport and scissor cover the whole block
    if (locn < 0 || locn >= 32 || vs->rf.pos_attr < 0) return 0;
    int direct = vs->rf.uv_attr[locn] >= 0, aff = vs->rf.uv_aff_attr[locn] >= 0;
    if (!direct && !aff) return 0;
    float sc[4] = {1, 0, 1, 0};   // Pass-through as-is = scale 1, offset 0
    if (!direct) for (int q = 0; q < 4; q++) if (!ubo_float(0, vs->rf.uv_aff[locn][q], &sc[q])) return 0;
    NSUInteger W = g_pass.tw, H = g_pass.th;
    if (S.has_vp && !(S.vp[0][0] == 0 && S.vp[0][1] == 0 && S.vp[0][2] == (float)W && S.vp[0][3] == (float)H)) return 0;
    if (R.has_sc && !(R.sc.x <= 0 && R.sc.y <= 0 && R.sc.x + R.sc.w >= (int)W && R.sc.y + R.sc.h >= (int)H)) return 0;
    float P[64][4], Q[64][4]; int n = dw ? dw->count : di->count;
    if (attr_read(vs->rf.pos_attr, dw, di, P) || attr_read(direct ? vs->rf.uv_attr[locn] : vs->rf.uv_aff_attr[locn], dw, di, Q)) return 0;
    for (int k = 0; k < n; k++) { float w = vs->rf.pos_w1 ? 1.0f : P[k][3];
        float u = direct ? Q[k][0] : fmaf(Q[k][0], sc[0], sc[1]), v = direct ? Q[k][1] : fmaf(Q[k][1], sc[2], sc[3]);   // The same fma as the shader
        if (w != 1.0f || u != (P[k][0] + 1.0f) * 0.5f || v != (1.0f - P[k][1]) * 0.5f) { PD("F %llu uv proof: vertex %d position %g,%g uv %g,%g (scale / offset %g,%g,%g,%g)", g_stat_pass, k, P[k][0], P[k][1], u, v, sc[0], sc[1], sc[2], sc[3]); return 0; } }
    return 1;
}
// One draw of the later pass: verify and produce the specialization. Returns 0 = cannot be proven (the caller ends the merged render pass and reopens with the original targets)
static int fz_p2_draw(NvProg *pr, const p_draw *dw, const p_draw_idx *di, uint8_t lmap[8], __strong id<MTLFunction> *fn) {
    NvShader *vs = pr->sh[0], *fs = pr->sh[1]; *fn = nil;
    if (!vs || !fs) { PD("F %llu not provable: no vertex / fragment shader", g_stat_pass); return 0; }
    if (!g_fz.rt2.depth && R.rt.depth && (S.ds.dt || S.ds.dw || S.ds.st)) { PD("F %llu not provable: depth comes from the previous pass and this draw has depth / stencil testing on", g_stat_pass); return 0; }
    int fx[32]; for (int i = 0; i < 32; i++) fx[i] = -1; uint8_t fbmask = 0;
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || sh->stage >= 6) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? S.img[sh->stage][b->reg] : 0; else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = tex_slot_h(S.tex[sh->stage], S.img[sh->stage], b->reg); else continue;
            uint64_t ser = h ? h_ser(h) : 0; if (!ser) continue;
            int j = -1; for (int m = 0; m < g_fz.n1; m++) if (R.rt.color[m] && R.rt.color[m]->serial == ser) { j = m; break; }
            if (j < 0) continue;   // What is sampled is not an attachment of the earlier pass
            if (sh != fs || b->kind != NVN_RB_TEX) { PD("F %llu not provable: a non-fragment stage or a non-texture slot reads the previous pass's attachment", g_stat_pass); return 0; }
            if (fs->rf.fc_mixed[b->reg >> 6] >> (b->reg & 63) & 1) { PD("F %llu not provable: slot %d has mixed use", g_stat_pass, b->reg); return 0; }
            uint64_t s_; int lv, sl; rt_sub(&R.rt, j, &s_, &lv, &sl);
            if (lv || sl || MAX(R.rt.color[j]->d.width, 1u) != g_pass.tw || MAX(R.rt.color[j]->d.height, 1u) != g_pass.th) { PD("F %llu not provable: the attachment is not level 0 layer 0, or its size differs", g_stat_pass); return 0; }
            {   // The sampled view must also be exactly the attachment itself (previously only the attachment being level 0 layer 0 was checked, not the view bound by the shader -- when the same texture is sampled through a view of level k / layer k / a changed format,
                // the proof would still hold, the sampling would be changed to read on-tile level 0 / the original format, and the picture would be wrong). The view's starting level / layer must be 0 and its format must equal the attachment's actual format, otherwise do not merge
                int tid_ = (int)(h & 0xfffff); void *tv_ = NULL; if (tid_) tex_fast_rid(tid_, &tv_); __unsafe_unretained id<MTLTexture> tv = (__bridge id<MTLTexture>)tv_;
                uint32_t apf_ = tv.parentTexture ? (uint32_t)tv.parentTexture.pixelFormat : (uint32_t)tv.pixelFormat; uint8_t swz_[4];
                if (R.rt.has_cview[j] && R.rt.cview[j].set_format && !(nvn_fmt_to_mtl(R.rt.cview[j].format, &apf_, swz_) && apf_)) apf_ = 0;   // The attachment is attached through a format-changing view: take that format; if unrecognized, do not merge
                MTLPixelFormat apf = (MTLPixelFormat)apf_; MTLTextureSwizzleChannels tsw = tv.swizzle;   // a read of the tile takes no channel mapping either (alpha 1 of an RGBX attachment, tex_by_id)
                int tsw_id = tsw.red == MTLTextureSwizzleRed && tsw.green == MTLTextureSwizzleGreen && tsw.blue == MTLTextureSwizzleBlue && tsw.alpha == MTLTextureSwizzleAlpha;
                if (!tv || (tv.parentTexture && (tv.parentRelativeLevel || tv.parentRelativeSlice)) || tv.pixelFormat != apf || !tsw_id) {
                    static int lg; if (lg++ < 8) LOG("[nvn-mtl] on-chip read merge: slot %d sampled view (base level %lu layer %lu format %lu%s) does not match the attachment (level 0 layer 0 format %lu); not merged (frame %llu pass %llu)", b->reg,
                        tv && tv.parentTexture ? (unsigned long)tv.parentRelativeLevel : 0ul, tv && tv.parentTexture ? (unsigned long)tv.parentRelativeSlice : 0ul, (unsigned long)tv.pixelFormat, tsw_id ? "" : ", swizzled", (unsigned long)apf,
                        (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass);
                    PD("F %llu not provable: slot %d sampled view is not the attachment itself (level / layer / format / channel mapping differ)", g_stat_pass, b->reg); return 0; } }
            int any = 0;
            for (int c = 0; c < fs->rf.nfc; c++) if (fs->rf.fc_reg[c] == b->reg) {
                if (!uv_proof(vs, fs->rf.fc_locn[c], dw, di)) { PD("F %llu not provable: slot %d sample coordinate is not this pixel (interpolated output %d)", g_stat_pass, b->reg, fs->rf.fc_locn[c]); return 0; }
                fx[c] = j; fbmask |= (uint8_t)(1u << j); any = 1; }
            if (!any) { PD("F %llu not provable: slot %d sample is not in the rewritable table", g_stat_pass, b->reg); return 0; } } }
    memset(lmap, 0xff, 8);
    for (int c = 0; c < 8; c++) if (fs->rf.color_mask >> c & 1) { if (g_fz.p2map[c] < 0) { PD("F %llu not provable: output color %d has no matching attachment", g_stat_pass, c); return 0; } lmap[g_fz.p2map[c]] = (uint8_t)c; }
    int ident = !fbmask; for (int c = 0; c < 8; c++) if ((fs->rf.color_mask >> c & 1) && g_fz.p2map[c] != c) ident = 0;
    if (ident) { for (int q = 0; q < 8; q++) if (lmap[q] != 0xff) g_fz.wrote[q] = 1; if (fz_check_on()) fz_snap_take(dw, di); return 1; }   // Does not read the tile and the output is not shifted: use the original version (the write mask still follows the mapping)
    if (!fs->rf.nfo) { bf_want_note(fs->key); PD("F %llu not provable: no readable-tile version (outputs would need moving)", g_stat_pass); return 0; }
    *fn = bf_fn(fs, fx, fbmask, lmap);
    if (!*fn) return 0;
    memset(&g_fz_desc, 0, sizeof g_fz_desc); g_fz_desc.kind = 1; g_fz_desc.fbmask = fbmask; for (int i = 0; i < 32; i++) g_fz_desc.fx[i] = (int8_t)fx[i];
    for (int q = 0; q < 8; q++) if (lmap[q] != 0xff) g_fz.wrote[q] = 1;
    if (fz_check_on()) fz_snap_take(dw, di);
    g_fz_draws++; return 1;
}
// Diagnostic DM_NVN_KEYDUMP=<fragment shader key prefix>[:<count, default 6>[:<frame interval>]]: watch only this class of draws (for real devices; no frame number to guess). The first N matching draws force PASSDUMP's per-binding lines on (K / B / d:
// textures with format / view / channel swizzle, samplers with LOD clamps), plus [kd] lines: vertex attribute formats, each vertex stream (first 4 vertices of per-vertex streams, all instances of per-instance streams) and every value of each uniform buffer of both stages.
// Run the same switch once on the Mac and once on the device and compare item by item to see which input differs on the device (Super Mario 3D World + Bowser's Fury's magenta squares in 1-1: particle 5e4de7561c670fee, iPhone only, not reproducible on the Mac)
static void kd_floats(const char *what, int reg, uint64_t addr, uint64_t size, uint64_t cap) {
    void *mb; uint64_t mo; const uint8_t *base;
    if (!addr || !size) { fprintf(stderr, "[kd]   %s r%d unbound\n", what, reg); return; }
    if (!mtl_addr(addr, 1, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) { fprintf(stderr, "[kd]   %s r%d addr=%#llx not in a memory pool\n", what, reg, (unsigned long long)addr); return; }
    uint64_t n = MIN(size, cap) / 4; const float *v = (const float *)(base + mo);
    fprintf(stderr, "[kd]   %s r%d addr=%#llx size=%llu%s\n", what, reg, (unsigned long long)addr, (unsigned long long)size, pend_hits(addr, size) ? " (pending writes in this submission)" : "");
    for (uint64_t i = 0; i < n; i += 16) {
        char line[1024]; size_t o = 0;
        for (uint64_t k = i; k < n && k < i + 16 && o < sizeof line - 24; k++) o += snprintf(line + o, sizeof line - o, " %.6g", v[k]);
        fprintf(stderr, "[kd]     +%#06llx%s\n", (unsigned long long)(i * 4), line);
    }
}
static int kd_draw(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    static char key[24]; static int left = -1, every; static uint64_t last = ~0ull;
    if (left < 0) { left = 0; const char *e = getenv("DM_NVN_KEYDUMP");
        if (e && *e) { size_t n = strcspn(e, ":"); if (n > 23) n = 23; memcpy(key, e, n); key[n] = 0; const char *c = strchr(e, ':'); left = c ? atoi(c + 1) : 6; every = c && strchr(c + 1, ':') ? atoi(strchr(c + 1, ':') + 1) : 0;
            MLOG("DM_NVN_KEYDUMP: fragment shader %s, %d draws logged in detail%s ([kd] / [pass] lines)", key, left, every ? ", at most once every that many frames" : ""); } }
    if (left <= 0 || !pr->sh[1] || strncmp(pr->sh[1]->key, key, strlen(key))) return 0;
    uint64_t fr = atomic_load(&g_cur_frame);
    if (every > 0 && last != ~0ull && fr < last + (uint64_t)every) return 0;   // at most once every `every` frames: samples spread over the session, no need to guess the frame of the scene
    last = fr; left--;
    int count = dw ? dw->count : di->count, inst = dw ? dw->instances : di->instances, first = dw ? dw->first : 0, binst = dw ? dw->base_instance : di->base_instance;
    fprintf(stderr, "[kd] F%llu pass %llu vs=%s fs=%s prim %d vertices %d instances %d first %d base_vertex %d base_instance %d target %s\n", (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass,
            pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[1]->key, dw ? dw->prim : di->prim, count, inst, first, di ? di->base_vertex : 0, binst, R.rt.n && R.rt.color[0] ? "yes" : "no");
    for (int i = 0; i < S.nattr && i < 16; i++) fprintf(stderr, "[kd]   attr %d format %#x offset %u stream %u\n", i, (unsigned)S.attr[i].fmt, (unsigned)S.attr[i].offset, (unsigned)S.attr[i].stream);
    for (int s_ = 0; s_ < S.nstream && s_ < 16; s_++) {
        uint32_t st = S.stream[s_].stride, dv = S.stream[s_].divisor;
        fprintf(stderr, "[kd]   stream %d stride %u divisor %u buffer %#llx size %llu\n", s_, st, dv, (unsigned long long)S.vbuf[s_].address, (unsigned long long)S.vbuf[s_].size);
        if (!S.vbuf[s_].address || !st) continue;
        uint64_t from = (uint64_t)(dv ? binst : (di ? di->base_vertex : first)) * st, n = (uint64_t)st * (dv ? (uint64_t)MIN(inst, 8) : 4);
        if (from < S.vbuf[s_].size) kd_floats("stream", s_, S.vbuf[s_].address + from, MIN(n, S.vbuf[s_].size - from), 512);
    }
    for (int stg = 0; stg < 2; stg++) { NvShader *sh = pr->sh[stg]; if (!sh) continue;
        for (int i = 0; i < sh->rf.nb; i++) if (sh->rf.b[i].kind == NVN_RB_CBUF && sh->rf.b[i].reg < 16) {
            const p_bind_buf *u = &S.ubo[stg][sh->rf.b[i].reg]; kd_floats(stg ? "fragment uniform" : "vertex uniform", sh->rf.b[i].reg, u->addr, u->size, 4096); } }
    return 1;
}
// Diagnostic DM_NVN_MAGSCAN=<fragment key>:<scan every N frames, default 30>:<frames per phase, default 600>: on-device self-check for the magenta squares (Super Mario 3D World + Bowser's Fury 1-1, iPhone only, not reproducible on the Mac).
// In a scan frame, as soon as each render pass ends, a compute kernel reads its color attachments (read goes through the same format decode as the game's shader samples) and gathers per 16x16 block: the magenta pixel count (R and B both >= 0.8 of the largest channel, G <= 0.1 of it),
// the non-finite pixel count and each channel's min / max. An attachment with a block holding one single magenta value throughout (exactly what the squares in the iPhone screenshot are, aligned to the 16-pixel grid) or with non-finite values gets a [mag] line (index of the pass within the frame, distance from the pass holding this particle, attachment serial / format / size, block position and value).
// Four phases rotate: phase 0 draws as usual, phase 1 drops every draw of this fragment shader, phases 2 / 3 keep the geometry and replace the fragment output with a constant (grey / bright magenta). Comparing the phases tells whether this draw causes it
// and whether its fragment output (texture reads, value range) matters; the first pass where the magenta blocks show tells which step goes wrong
static char g_ms_key[24]; static int g_ms_every, g_ms_plen, g_ms_init;
static int ms_on(void);
static const char *const k_ms_phase[4] = {"normal", "shader dropped", "shader outputs constant gray", "shader outputs constant bright magenta"};
static int ms_phase_now(void) { return (int)(atomic_load(&g_cur_frame) / (uint64_t)g_ms_plen % 4); }
// Phases 2 / 3: this shader's draws keep their geometry and the fragment stage is replaced by a constant output (grey 0.5 / bright magenta 1500,2,1500). The pipeline cache key carries the replacement function, kept apart from the original pipeline; these pipelines are not recorded in the prewarm list
static id<MTLFunction> ms_fs_override(NvShader *fs) {
    if (!fs || !ms_on() || strncmp(fs->key, g_ms_key, strlen(g_ms_key))) return nil;
    int ph = ms_phase_now(); if (ph < 2) return nil;
    static id<MTLFunction> fn[2]; static int failed;
    if (!fn[0] && !failed) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
            "fragment float4 msg() { return float4(0.5, 0.5, 0.5, 1.0); }\nfragment float4 msm() { return float4(1500.0, 2.0, 1500.0, 0.6); }\n" options:nil error:&e];
        fn[0] = [lib newFunctionWithName:@"msg"]; fn[1] = [lib newFunctionWithName:@"msm"];
        if (!fn[0] || !fn[1]) { failed = 1; MLOG("[RED] DM_NVN_MAGSCAN: cannot compile the constant fragment shaders %s", e.localizedDescription.UTF8String); } }
    return failed ? nil : fn[ph - 2];
}
static int ms_on(void) {
    if (!g_ms_init) { g_ms_init = 1; const char *e = getenv("DM_NVN_MAGSCAN");
        if (e && *e) { size_t n = strcspn(e, ":"); if (n > 23) n = 23; memcpy(g_ms_key, e, n); g_ms_key[n] = 0;
            const char *c = strchr(e, ':'); g_ms_every = c ? atoi(c + 1) : 30; c = c ? strchr(c + 1, ':') : NULL; g_ms_plen = c ? atoi(c + 1) : 600;
            if (g_ms_every < 1) g_ms_every = 30; if (g_ms_plen < g_ms_every) g_ms_plen = 600;
            MLOG("DM_NVN_MAGSCAN: fragment shader %s; every %d frames, scans each pass's color attachments for magenta blocks; every %d frames, switches phase (0 normal / 1 drop this shader's draws / 2 it outputs constant gray / 3 it outputs constant bright magenta) ([mag] lines)", g_ms_key, g_ms_every, g_ms_plen); } }
    return g_ms_key[0] != 0;
}
static uint64_t g_ms_ppass = ~0ull, g_ms_pframe = ~0ull, g_ms_drawn, g_ms_dropped;
// returns 1 = this phase drops the draw
static int ms_draw(NvProg *pr) {
    if (!ms_on() || !pr->sh[1] || strncmp(pr->sh[1]->key, g_ms_key, strlen(g_ms_key))) return 0;   // a deferred draw returns before this when recorded and only arrives here when replayed: every draw passes exactly once
    uint64_t f = atomic_load(&g_cur_frame);
    if (f / (uint64_t)g_ms_plen % 4 == 1) { g_ms_dropped++; return 1; }
    g_ms_drawn++; if (g_ms_pframe != f) { g_ms_pframe = f; g_ms_ppass = g_stat_pass; }   // the pass where this particle first draws this frame
    return 0;
}
static const char k_ms_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"kernel void ms(texture2d<float, access::read> t [[texture(0)]], device uint *o [[buffer(0)]], uint2 g [[thread_position_in_grid]], uint2 tg [[threadgroup_position_in_grid]],\n"
"               uint li [[thread_index_in_threadgroup]], uint si [[simdgroup_index_in_threadgroup]], uint sl [[thread_index_in_simdgroup]]) {\n"
"  threadgroup uint sh[8][8]; bool in = g.x < o[0] && g.y < o[1]; float4 c = in ? t.read(g) : float4(0); float m = max(c.r, max(c.g, c.b));\n"
"  bool bad = in && (isnan(c.r) || isnan(c.g) || isnan(c.b) || isinf(c.r) || isinf(c.g) || isinf(c.b));\n"
"  bool mag = in && !bad && m > 0.0 && c.r >= 0.8 * m && c.b >= 0.8 * m && c.g <= 0.1 * m;\n"
"  uint r = as_type<uint>(max(c.r, 0.0)), gg = as_type<uint>(max(c.g, 0.0)), b = as_type<uint>(max(c.b, 0.0));\n"
"  uint v[8] = {simd_sum(mag ? 1u : 0u), simd_sum(bad ? 1u : 0u), simd_min(in ? r : ~0u), simd_max(in ? r : 0u), simd_min(in ? gg : ~0u), simd_max(in ? gg : 0u), simd_min(in ? b : ~0u), simd_max(in ? b : 0u)};\n"
"  if (sl == 0) for (int k = 0; k < 8; k++) sh[si][k] = v[k];\n"
"  threadgroup_barrier(mem_flags::mem_threadgroup); if (li) return;\n"
"  for (int s = 1; s < 8; s++) { v[0] += sh[s][0]; v[1] += sh[s][1]; for (int k = 2; k < 8; k += 2) { v[k] = min(v[k], sh[s][k]); v[k + 1] = max(v[k + 1], sh[s][k + 1]); } }\n"
"  device uint *q = o + 4 + 4 * (tg.y * o[2] + tg.x); q[0] = v[0] | v[1] << 16 | (v[2] == v[3] && v[4] == v[5] && v[6] == v[7] ? 1u << 31 : 0u); q[1] = v[3]; q[2] = v[5]; q[3] = v[7]; }\n";
#define MS_BUF (16u << 20)
#define MS_MAXE 256
typedef struct { uint64_t pass_idx, ser; int att, pdist, fmt; uint32_t w, h, off; } ms_ent;
typedef struct { __strong id<MTLBuffer> b; uint64_t frame, fence; uint32_t used; int n, busy, full; ms_ent e[MS_MAXE]; } ms_scan;
static ms_scan g_ms_s[2]; static int g_ms_cur = -1;   // which buffer this scan frame writes (-1 = not scanning this frame)
static uint64_t g_ms_curf = ~0ull, g_ms_first;
static __strong id<MTLTexture> g_ms_att[8]; static NSUInteger g_ms_lv[8], g_ms_sl[8]; static uint64_t g_ms_aser[8]; static int g_ms_na; static uint64_t g_ms_apass;
static int g_ms_seq = -1, g_ms_sc, g_ms_hitf, g_ms_maxblk, g_ms_pscan; static uint64_t g_ms_f0, g_ms_f1, g_ms_pmag;   // the phase being tallied (frame / frames per phase) and its totals
static void ms_poll(void) {
    for (int k = 0; k < 2; k++) { ms_scan *s = &g_ms_s[k]; if (!s->busy || g_ms_cur == k || s->fence > g_done) continue;
        int seq = (int)(s->frame / (uint64_t)g_ms_plen);
        if (seq != g_ms_seq) {
            if (g_ms_seq >= 0) MLOG("[mag] phase %d (%s) frames %llu–%llu: scanned %d frames, frames with uniform magenta blocks (a whole 16×16 block of one magenta value) %d, at most %d blocks in one frame; magenta pixels in the attachments of the shader's pass %.1f per frame (%d frames); this shader drawn %llu times, dropped %llu times so far",
                                    g_ms_seq % 4, k_ms_phase[g_ms_seq % 4], (unsigned long long)g_ms_f0, (unsigned long long)g_ms_f1, g_ms_sc, g_ms_hitf, g_ms_maxblk, g_ms_pscan ? (double)g_ms_pmag / g_ms_pscan : 0.0, g_ms_pscan,
                                    (unsigned long long)g_ms_drawn, (unsigned long long)g_ms_dropped);
            g_ms_seq = seq; g_ms_sc = g_ms_hitf = g_ms_maxblk = g_ms_pscan = 0; g_ms_pmag = 0; g_ms_f0 = s->frame; }
        g_ms_sc++; g_ms_f1 = s->frame; int fblk = 0, pseen = 0;
        for (int i = 0; i < s->n; i++) { const ms_ent *e = &s->e[i]; const uint32_t *o = (const uint32_t *)((const uint8_t *)s->b.contents + e->off);
            uint32_t tw = (e->w + 15) / 16, th = (e->h + 15) / 16; uint64_t bad = 0, mag = 0; int ublk = 0, mblk = 0, nl = 0; char line[1400]; size_t lo = 0;
            for (uint32_t t = 0; t < tw * th; t++) { const uint32_t *q = o + 4 + 4 * t; uint32_t nmag = q[0] & 0xffff, nbad = q[0] >> 16 & 0x7fff; bad += nbad; mag += nmag;
                uint32_t npx = MIN(16u, e->w - t % tw * 16) * MIN(16u, e->h - t / tw * 16);
                int uni = q[0] >> 31;   // the whole block holds one value
                int allm = nmag == npx;
                if (allm) mblk++; if (allm && uni && npx >= 64) ublk++;
                if (((allm && uni && npx >= 64) || nbad) && nl < 8 && lo < sizeof line - 120) { float r, g, b; memcpy(&r, &q[1], 4); memcpy(&g, &q[2], 4); memcpy(&b, &q[3], 4); nl++;
                    lo += snprintf(line + lo, sizeof line - lo, " [%u,%u%s max %g %g %g non-finite %u]", t % tw * 16, t / tw * 16, uni ? " uniform" : "", r, g, b, nbad); } }
            fblk += ublk; if (e->pdist == 0 && e->att == 0) { g_ms_pmag += mag; pseen = 1; }
            if (ublk || bad) MLOG("[mag] F%llu phase %llu pass %llu (shader's pass %+d) attachment %d ser=%llu format %d %ux%u: uniform magenta blocks %d (all-magenta blocks %d), non-finite pixels %llu%s", (unsigned long long)s->frame, (unsigned long long)(s->frame / (uint64_t)g_ms_plen % 4),
                                  (unsigned long long)e->pass_idx, e->pdist, e->att, (unsigned long long)e->ser, e->fmt, e->w, e->h, ublk, mblk, (unsigned long long)bad, line);
        }
        if (fblk) g_ms_hitf++; if (fblk > g_ms_maxblk) g_ms_maxblk = fblk; g_ms_pscan += pseen;
        if (s->full) MLOG("[mag] F%llu scan buffer full; the remaining passes of this frame were not scanned", (unsigned long long)s->frame);
        s->busy = 0; s->n = 0; s->used = 0; s->full = 0; }
}
void ms_note(MTL4RenderPassDescriptor *pd, NSUInteger nc) {
    g_ms_na = 0; if (!ms_on()) return;
    uint64_t f = atomic_load(&g_cur_frame);
    if (f != g_ms_curf) {   // a new frame: the previous scan frame's counts are processed once its submission completes
        g_ms_curf = f; g_ms_first = g_stat_pass + 1; if (g_ms_cur >= 0) g_ms_cur = -1;
        if (f % (uint64_t)g_ms_every == 0) { for (int k = 0; k < 2; k++) if (!g_ms_s[k].busy) { g_ms_cur = k; break; }
            if (g_ms_cur >= 0) { ms_scan *s = &g_ms_s[g_ms_cur];
                if (!s->b) { s->b = [g_dev newBufferWithLength:MS_BUF options:MTLResourceStorageModeShared]; if (!s->b) { g_ms_cur = -1; return; } res_add(s->b); }
                s->busy = 1; s->frame = f; s->n = 0; s->used = 0; s->full = 0; s->fence = 0; } } }
    ms_poll();
    if (g_ms_cur < 0) return;
    g_ms_apass = g_stat_pass + 1;
    for (NSUInteger i = 0; i < nc && i < 8; i++) { id<MTLTexture> t = pd.colorAttachments[i].texture; if (!t) continue;
        g_ms_att[g_ms_na] = t; g_ms_lv[g_ms_na] = pd.colorAttachments[i].level; g_ms_sl[g_ms_na] = pd.colorAttachments[i].slice; g_ms_aser[g_ms_na] = R.rt.color[i] ? R.rt.color[i]->serial : 0; g_ms_na++; }
}
void ms_after(void) {
    if (!g_ms_na) return;
    int na = g_ms_na; g_ms_na = 0;
    if (g_ms_cur < 0 || g_stat_pass != g_ms_apass) { for (int i = 0; i < na; i++) g_ms_att[i] = nil; return; }
    static int failed; static id<MTLComputePipelineState> ps; static id<MTL4ArgumentTable> at; static int logged_att;
    if (failed) return;
    if (!ps) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_ms_msl) options:nil error:&e]; id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"ms"] : nil;
        ps = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&e] : nil;
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 1; ad.maxBufferBindCount = 1; at = rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]);
        if (!ps || !at) { failed = 1; MLOG("[RED] DM_NVN_MAGSCAN: cannot create the compute pipeline %s", e.localizedDescription.UTF8String); return; } }
    ms_scan *s = &g_ms_s[g_ms_cur]; int pdist = g_ms_pframe == s->frame && g_ms_ppass != ~0ull ? (int)((int64_t)g_stat_pass - (int64_t)g_ms_ppass) : 9999;
    for (int i = 0; i < na; i++) { id<MTLTexture> t = g_ms_att[i]; g_ms_att[i] = nil;
        MTLPixelFormat pf = t.pixelFormat;
        int ok = pf == MTLPixelFormatRGBA8Unorm || pf == MTLPixelFormatRGBA8Unorm_sRGB || pf == MTLPixelFormatBGRA8Unorm || pf == MTLPixelFormatBGRA8Unorm_sRGB || pf == MTLPixelFormatRGB10A2Unorm
              || pf == MTLPixelFormatRG11B10Float || pf == MTLPixelFormatRGBA16Float || pf == MTLPixelFormatRGBA16Unorm || pf == MTLPixelFormatRGBA32Float;
        if (!ok || !(t.usage & MTLTextureUsageShaderRead) || t.sampleCount > 1 || (t.width >> g_ms_lv[i]) < 64 || t.storageMode == MTLStorageModeMemoryless || (t.textureType != MTLTextureType2D && t.textureType != MTLTextureType2DArray)) continue;
        if (pdist == 0 && !logged_att++) MLOG("[mag] attachment %d of the shader's pass: ser=%llu format %lu %lux%lu level %lu layer %lu storage %lu usage %#lx compression type %ld allowGPUOptimizedContents %d", i, (unsigned long long)g_ms_aser[i], (unsigned long)pf,
                                             (unsigned long)t.width, (unsigned long)t.height, (unsigned long)g_ms_lv[i], (unsigned long)g_ms_sl[i], (unsigned long)t.storageMode, (unsigned long)t.usage, (long)t.compressionType, (int)t.allowGPUOptimizedContents);
        uint32_t w = (uint32_t)MAX(t.width >> g_ms_lv[i], 1ul), h = (uint32_t)MAX(t.height >> g_ms_lv[i], 1ul), tw = (w + 15) / 16, th = (h + 15) / 16, need = 16 + tw * th * 16;
        if (s->n >= MS_MAXE || s->used + need > MS_BUF) { s->full = 1; continue; }
        id<MTLTexture> v = t;
        if (t.textureType != MTLTextureType2D || g_ms_lv[i] || g_ms_sl[i]) v = [t newTextureViewWithPixelFormat:pf textureType:MTLTextureType2D levels:NSMakeRange(g_ms_lv[i], 1) slices:NSMakeRange(g_ms_sl[i], 1)];
        if (!v) continue;
        uint32_t *o = (uint32_t *)((uint8_t *)s->b.contents + s->used); o[0] = w; o[1] = h; o[2] = tw; o[3] = 0;
        ms_ent *e = &s->e[s->n++]; e->pass_idx = g_stat_pass - g_ms_first; e->ser = g_ms_aser[i]; e->att = i; e->pdist = pdist; e->fmt = (int)pf; e->w = w; e->h = h; e->off = s->used; s->used += need;
        if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
        id<MTL4ComputeCommandEncoder> ce = (id<MTL4ComputeCommandEncoder>)enc_c_begin(NULL); [ce setArgumentTable:at];
        [at setTexture:v.gpuResourceID atIndex:0]; [at setAddress:s->b.gpuAddress + e->off atIndex:0];
        enc_c_pso(ce, ps); enc_c_dispatch(ce, MTLSizeMake(tw, th, 1), MTLSizeMake(16, 16, 1)); enc_c_end(ce);
        args_dirty_all(); s->fence = g_fence + 1;
        if (v != t) defer_obj_unref(v);
    }
}
// Same-pass feedback sampling: a texture view this draw's fragment stage samples is exactly the level and layer of a color attachment of the current render pass (Super Mario 3D World + Bowser's Fury:
// 66a71a3081b91b9a writes the tone-mapped 1920x1080 RGB10A2 image as an attachment while sampling it as a texture). On the Switch (Maxwell, immediate-mode) the read returns the content from before this draw;
// on Metal, writing and sampling the same attachment within one render pass is undefined: the Mac happens to read correctly, while the iPhone (A20 Pro) now and then reads whole 16x16 blocks as (1,0,1) and FXAA carries them into the picture - the magenta squares in 1-1
// (measured with DM_NVN_MAGSCAN: the squares first appear on this attachment, and still appear with the particle dropped).
// The fix: before this draw opens its render pass, end the current render pass (the attachment lands in memory), copy that level and layer into a snapshot texture in the replay stream, make this draw sample the snapshot; later draws reopen the render pass as usual (loading the attachment).
// Only single-level 2D views that are exactly the attachment's level and layer are handled; other shapes are reported once and left as they were. Done only for the fragment shaders the game profile's feedback_snap lists (see fb_want)
// Which fragment shaders get it: the game profile's feedback_snap (key prefixes, space-separated); DM_NVN_FB_SNAP=0 none, all every shader, <keys,...> another list. The cost of doing all:
// Breath of the Wild has over a thousand draws a frame sampling the G-buffer of their own pass (frame time 16.6 -> 36.6 ms) and Mario Wonder's submit time went 1.4 -> 4.0 ms, so only shaders seen to break are listed
static char g_fb_keys[16][20]; static int g_fb_nk = -1, g_fb_all;
static int fb_want(const char *key) {
    if (g_fb_nk < 0) { g_fb_nk = 0; const char *e = getenv("DM_NVN_FB_SNAP"), *src = e && *e ? e : DM_GAME_FEEDBACK_SNAP;
        if (!strcmp(src, "all")) g_fb_all = 1;
        else if (strcmp(src, "0")) for (const char *q = src; *q && g_fb_nk < 16;) { size_t n = strcspn(q, ", "); if (n) { if (n > 16) n = 16; memcpy(g_fb_keys[g_fb_nk], q, n); g_fb_keys[g_fb_nk++][n] = 0; } q += n; q += strspn(q, ", "); }
        if (g_fb_all || g_fb_nk) MLOG("same-pass feedback sampling snapshot: %s%s", g_fb_all ? "all fragment shaders" : src, e && *e ? " (DM_NVN_FB_SNAP)" : " (profile key feedback_snap)"); }
    if (g_fb_all) return 1;
    for (int i = 0; i < g_fb_nk; i++) if (!strncmp(key, g_fb_keys[i], strlen(g_fb_keys[i]))) return 1;
    return 0;
}
static id<MTLTexture> fb_snap_tex(MTLPixelFormat pf, NSUInteger w, NSUInteger h) {   // one per (format, size); full queue barriers separate consecutive snapshots, so reuse within a frame does not conflict
    static __strong id<MTLTexture> pool[4]; static int next;
    for (int i = 0; i < 4; i++) if (pool[i] && pool[i].pixelFormat == pf && pool[i].width == w && pool[i].height == h) return pool[i];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d]; if (!t) return nil; res_add(t);
    if (pool[next]) defer_obj_release(pool[next]); pool[next] = t; next = (next + 1) & 3;
    return t;
}
// Called before the draw opens its render pass: if a snapshot is needed, end the current render pass, copy the snapshot and register the substitution (used by this draw's argument encoding)
static void fb_prepare(NvProg *pr) {
    g_draw_seq++; g_fb_n = 0;
    if (g_fb_rebind) { args_dirty_all(); g_fb_rebind = 0; }   // the previous draw had a substituted binding: re-encode with the original bindings this time
    if (!pr || !pr->sh[1] || !R.rt.n || g_fz.active || g_defer_replaying || !fb_want(pr->sh[1]->key)) return;
    NvShader *fs = pr->sh[1];
    for (int i = 0; i < fs->rf.nb && g_fb_n < 4; i++) { const nvn_rbind *b = &fs->rf.b[i]; if (b->kind != NVN_RB_TEX || b->reg >= 128) continue;
        int tid = (int)(tex_slot_h(S.tex[1], S.img[1], b->reg) & 0xfffff); if (!tid) continue;
        pthread_mutex_lock(&g_rmu); uint64_t ser = tid < g_ntreg && g_treg[tid].ok ? g_treg[tid].t.serial : 0; pthread_mutex_unlock(&g_rmu);
        if (!ser) continue;
        for (int k = 0; k < R.rt.n && k < 8 && g_fb_n < 4; k++) { if (!R.rt.color[k] || R.rt.color[k]->serial != ser) continue;
            uint64_t aser; int lv, sl; rt_sub(&R.rt, k, &aser, &lv, &sl);
            pthread_mutex_lock(&g_rmu); void *tv = NULL; tex_fast_rid(tid, &tv); id<MTLTexture> t = (__bridge id<MTLTexture>)tv; pthread_mutex_unlock(&g_rmu);
            if (!t) continue;
            int dup = 0; for (int q = 0; q < g_fb_n; q++) dup |= g_fb_src[q] == t; if (dup) continue;
            id<MTLTexture> base = t.parentTexture ? t.parentTexture : t; NSUInteger tl = t.parentTexture ? t.parentRelativeLevel : 0, ts = t.parentTexture ? t.parentRelativeSlice : 0;
            if (tl + t.mipmapLevelCount <= (NSUInteger)lv || tl > (NSUInteger)lv) continue;   // the sampled levels do not include the attachment's level (mip generation etc.): not feedback
            if (t.textureType != MTLTextureType2D || t.mipmapLevelCount != 1 || t.sampleCount != 1 || ts != (NSUInteger)sl || b->dtt != MTLTextureType2D || base.parentTexture) {
                RED_ONCE(80, "same-pass feedback sampling: the sampled view is not a single-level 2D view of the attachment's level and layer (type %lu levels %lu+%lu layer %lu, attachment level %d layer %d, shader %s); left as is", (unsigned long)t.textureType, (unsigned long)tl, (unsigned long)t.mipmapLevelCount, (unsigned long)ts, lv, sl, fs->key);
                continue; }
            NSUInteger w = MAX(base.width >> lv, 1ul), h = MAX(base.height >> lv, 1ul);
            id<MTLTexture> snap = fb_snap_tex(base.pixelFormat, w, h); if (!snap) continue;
            if (g_enc) rp_end();
            id<MTL4ComputeCommandEncoder> e = blit(); enc_b_copy_texture(e, base, (NSUInteger)sl, (NSUInteger)lv, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, 1), snap, 0, 0, MTLOriginMake(0, 0, 0));
            MTLTextureSwizzleChannels sw = t.swizzle;
            int same = t.pixelFormat == snap.pixelFormat && sw.red == MTLTextureSwizzleRed && sw.green == MTLTextureSwizzleGreen && sw.blue == MTLTextureSwizzleBlue && sw.alpha == MTLTextureSwizzleAlpha;
            id<MTLTexture> use = same ? snap : [snap newTextureViewWithPixelFormat:t.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:sw];
            if (!use) { RED_ONCE(81, "same-pass feedback sampling: cannot create the snapshot view (format %lu); left as is", (unsigned long)t.pixelFormat); continue; }
            if (g_fb_use[g_fb_n] && g_fb_use[g_fb_n] != use) defer_obj_unref(g_fb_use[g_fb_n]);
            g_fb_src[g_fb_n] = t; g_fb_use[g_fb_n] = use; g_fb_rid[g_fb_n] = use.gpuResourceID._impl; g_fb_n++;
            g_fb_count++;
            if (g_fb_count == 1 || g_fb_count == 10 || g_fb_count == 1000 || g_fb_count % 100000 == 0)
                MLOG("same-pass feedback sampling: snapshot #%llu (frame %llu pass %llu, shader %s slot %d samples attachment %d ser=%llu %lux%lu format %lu)", (unsigned long long)g_fb_count, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass + !g_enc,
                     fs->key, b->reg, k, (unsigned long long)ser, (unsigned long)w, (unsigned long)h, (unsigned long)base.pixelFormat);
            PD("N %llu same-pass feedback sampling: attachment %d ser=%llu level %d layer %d copied into a snapshot; this draw samples the snapshot", g_stat_pass + !g_enc, k, (unsigned long long)ser, lv, sl);
        } }
    if (g_fb_n) { g_fb_seq = g_draw_seq; args_dirty_all(); g_fb_rebind = 1; }
}
