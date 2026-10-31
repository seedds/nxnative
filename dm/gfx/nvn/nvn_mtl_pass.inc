// nvn_mtl.m, part: pass
// Render passes: opening and closing, zero-source skip, pass dumps and per-pass GPU time, the store lookahead and the cross-submission validity ledger, attachment views and rp_begin.
// Included by nvn_mtl.m in its original order, as one translation unit: statics are shared with the other parts.
// ---------- Render passes: SetRenderTargets opens a new pass; clears before the first command in the pass are folded into the load action ----------
static struct {
    p_set_rt rt; int has_rt;
    uint8_t clear_c[8]; MTLClearColor cc[8];
    uint8_t clear_d, clear_s; double cd; uint32_t cs;
    p_scissor sc; int has_sc;
} R;

// ---------- Zero-source skip ----------
// Shader fact table (statically proven keys): 'the textures it samples are all 0 and the coefficient is finite ⇒ the output is all 0'. Coefficient = component comp of vector vec of fragment-stage constant buffer register ubo_slot.
//   No shader is in the table yet: a proof comes from reading the shader's translation line by line (dmsh_b/<chain version>/<key>_b.metal); unit tests add their own (zskip_test_add).
typedef struct { char key[17]; int ubo_slot, vec, comp; } zskip_fact;
static zskip_fact g_zskip_facts[16];
static int g_zskip_nf;
void zskip_test_add(const char *key, int ubo_slot, int vec, int comp) {
    if (!key || g_zskip_nf >= 16) return;
    snprintf(g_zskip_facts[g_zskip_nf].key, sizeof g_zskip_facts[0].key, "%s", key);
    g_zskip_facts[g_zskip_nf].ubo_slot = ubo_slot; g_zskip_facts[g_zskip_nf].vec = vec; g_zskip_facts[g_zskip_nf].comp = comp; g_zskip_nf++;
}
int zskip_test_count(void) { return g_zskip_count; }
static int g_zskip_env = -1;   // Cache of DM_NVN_ZSKIP (−1 = not read). This function is called on every draw, and getenv once took about 8% of the submit path; unit tests set it back to −1 after toggling the switch mid-run
static int zskip_on(void) { if (g_zskip_env < 0) { const char *e = getenv("DM_NVN_ZSKIP"); g_zskip_env = !e || strcmp(e, "0"); } return g_zskip_env && tbdr_on(); }
static void zskip_clear(void) { g_zskip_n = 0; memset(g_zskip_cand_ok, 0, sizeof g_zskip_cand_ok); }
static void zskip_del(uint64_t ser) { for (int i = 0; i < g_zskip_n;) if (g_zskip_zero[i].ser == ser) g_zskip_zero[i] = g_zskip_zero[--g_zskip_n]; else i++; }
static int zskip_has(uint64_t ser, int lv, int sl) { for (int i = 0; i < g_zskip_n; i++) if (g_zskip_zero[i].ser == ser && g_zskip_zero[i].level == lv && g_zskip_zero[i].slice == sl) return 1; return 0; }
static void zskip_add(uint64_t ser, int lv, int sl) { if (!ser || zskip_has(ser, lv, sl) || g_zskip_n >= 4096) return; g_zskip_zero[g_zskip_n++] = (zskip_zero){ser, (uint16_t)lv, (uint16_t)sl}; }
static int zskip_zero_clear(int i) { return R.clear_c[i] && R.cc[i].red == 0 && R.cc[i].green == 0 && R.cc[i].blue == 0 && R.cc[i].alpha == 0; }
static void zskip_commit_clears(void) { for (int i = 0; i < 8; i++) if (g_zskip_cand_ok[i]) zskip_add(g_zskip_cand[i].ser, g_zskip_cand[i].level, g_zskip_cand[i].slice); }
static int zskip_target_zero(const tex_t *t, const view_t *v, int hv) { int lv = hv && v->set_levels ? v->base_level : 0, sl = hv && v->set_layers ? v->min_layer : 0; return t && zskip_has(t->serial, lv, sl); }
#ifdef NVN_RT_RANGE_TEST
static int g_rr_checking;
static void rr_begin(void), rr_finish(int ranged);
static void rr_snap(const p_draw *, const p_draw_idx *);
static void rr_reset(void);
#endif
static id<MTL4RenderCommandEncoder> g_enc;
static int enc_open(void) { return g_enc != nil; }
// Target info of the current render pass: determined once from the attachments at rp_begin and shared by all draws in the pass (pipeline key, depth-stencil, viewport / scissor all depend only on it)
static struct { uint32_t cfmt[8], dfmt, sfmt, samples; int hd, hs; NSUInteger tw, th; uint64_t serial, reg_gen, ser_gen, range_dirty; int range_external, range_depth_nonro, layered; __unsafe_unretained id<MTLTexture> range_color, range_depth; } g_pass;
static int g_rp_layered;   // the pass about to open is for a vertex shader that writes the layer (draw → rp_begin_ex)
// Slice-merged passes (DM_NVN_SLICE_MERGE, on by default; 0 off): consecutive passes that each draw into another layer of one level of the same array or cube texture,
// with nothing between them but state, uniform updates and draws (no game barrier, clear, copy or dispatch: sm_scan), run as one layered pass. Each draw reaches its layer through a vertex
// amplification view mapping (count 1, renderTargetArrayIndexOffset), which needs pipelines built with maxVertexAmplificationCount 2 (sm_amp_get; a pipeline with the default 1 ignores
// the mapping and every draw lands in the first layer). On Apple GPUs a render pass costs about 35-40 us whatever it draws (M2 Max): Breath of the Wild filters its environment cube map
// every tenth frame in 132 one-draw passes, 22 levels of 6 faces, and that frame's GPU time grew by about 5 ms
static struct { int on, lo, hi; } g_sm_plan;   // set by draw() for the pass rp_begin_ex is about to open
static int g_sm_pass, g_sm_lo, g_sm_hi, g_sm_cur, g_sm_applied;   // the open pass is slice-merged: its attachment holds layers lo..hi; cur: the layer the game's target names now; applied: the offset set on the encoder (-1 none)
static uint64_t g_sm_npass, g_sm_ncont, g_sm_nwait;   // slice-merged passes, layer switches taken into them, draws that left one because their pipeline was not built yet
static int g_pso_amp;   // pso_get: the pipeline of a slice-merged pass (sm_amp_get)
void mtl_sm_stats(uint64_t *np, uint64_t *nc, uint64_t *nw) { *np = g_sm_npass; *nc = g_sm_ncont; *nw = g_sm_nwait; }
static id<MTLRenderPipelineState> g_enc_pso; static id<MTLDepthStencilState> g_enc_dss;   // Already set on this encoder (not set again if identical)
// Dynamic state already sent on the open render encoder: each draw used to send all nine (stencil reference, viewport, scissor, cull, winding, fill, depth bias, depth clip, blend colour);
// now a value is sent only when it differs. g_ed.ok holds one bit per item and is cleared for every new encoder and after a clear triangle (which sets its own)
static struct { unsigned ok; uint32_t sref; MTLViewport vp; MTLScissorRect sc; int cull, wind, fill, clip; float bias[3], blend[4]; } g_ed;
enum { ED_SREF = 1, ED_VP = 2, ED_SC = 4, ED_CULL = 8, ED_WIND = 16, ED_FILL = 32, ED_BIAS = 64, ED_CLIP = 128, ED_BLEND = 256 };
static int g_enc_vis, g_enc_has_vis;   // This encoder's current visibility count mode (0 off / slot number + 1), and whether a visibility buffer is attached
static int g_pso_dirty = 1, g_dss_dirty = 1;   // Pipeline / depth-stencil related state was changed during replay
static uint64_t g_stat_args_enc, g_stat_args_reuse;   // Argument buffer: new-encode / reuse counts
static uint64_t g_stat_draw, g_stat_pass, g_bw_load, g_bw_store, g_pass_full_rw, g_udc_saved_bytes;   // Bandwidth ledger: attachment load / store bytes, number of full load-store passes
// DM_NVN_PASSDUMP: records pass attachments / samples / presents one by one (textures by serial) for offline data-flow analysis; the pass numbers of P / D / R / S lines agree (= g_stat_pass after the pass is opened)
// DM_NVN_PASSDUMP=frame number, or start-end (inclusive at both ends)
static int g_kd_force;   // the draw DM_NVN_KEYDUMP matched: force the per-binding records on (see kd_draw)
static _Atomic long g_pd_a0 = -2, g_pd_a1;   // the frames dumped (inclusive); -2 = DM_NVN_PASSDUMP not read yet, -3 = DM_NVN_PASSDUMP=every:<n>
static long g_pd_every;
static inline int passdump(void) {
    if (g_kd_force) return 1;
#ifdef NVN_RT_RANGE_TEST
    if (g_rr_checking) return 0;
#endif
    if (__builtin_expect(g_pd_a0 == -1, 1)) return 0;   // off (the usual case): answered first, it is asked for every binding of every draw
    if (g_pd_a0 == -2) { const char *e = getenv("DM_NVN_PASSDUMP");
        if (e && !strncmp(e, "every:", 6)) { g_pd_every = atol(e + 6); g_pd_a1 = -1; g_pd_a0 = g_pd_every > 0 ? -3 : -1; }   // one frame in every n, halfway between two DM_NVN_GPUPROF=every:n frames
        else { g_pd_a1 = e && strchr(e, '-') ? atol(strchr(e, '-') + 1) : e ? atol(e) : -1; g_pd_a0 = e ? atol(e) : -1; } }
    long f = (long)atomic_load(&g_cur_frame), a0 = g_pd_a0;
    if (a0 == -3) return f > 0 && f % g_pd_every == g_pd_every / 2;
    return a0 >= 0 && f >= a0 && f <= g_pd_a1;
}
// The next n frames' passes, on request (the Mac's DM_CTL "passdump n", hle_hid.c): for a scene that a fixed DM_NVN_PASSDUMP frame range cannot be timed to
void dm_gfx_passdump_next(long n) { long f = (long)atomic_load(&g_cur_frame); g_pd_a1 = f + n; g_pd_a0 = f + 1; }
#define PD(...) do { if (passdump()) { flockfile(stderr); fprintf(stderr, "[pass] F%llu ", (unsigned long long)atomic_load(&g_cur_frame)); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); funlockfile(stderr); } } while (0)
static void blit_note(void) { if (g_enc) PD("E %llu copy / buffer update ends the render pass", g_stat_pass); }
__attribute__((weak)) void dm_guest_frame_tick(void) {}   // hle_os.c provides the strong definition (DM_DETCLOCK deterministic guest clock); a no-op when graphics unit tests do not link the host   // diag.c provides the strong definition (DM_TICK_HOLD=E:SHOT unfreeze); a no-op when graphics unit tests do not link the host
__attribute__((weak)) void dm_cheats_present(void) {}   // hle_cheats.c provides the strong definition (the Atmosphère cheat pass per presented frame); a no-op when graphics unit tests do not link the host
__attribute__((weak)) int dm_detclock_enabled(void) { return 0; }   // Same as above: the host's hle_os.c provides the strong definition; when graphics unit tests do not link the host, the deterministic clock is treated as off
__attribute__((weak)) uint64_t guest_ns(void) { return 0; }          // Same as above: read only when the deterministic clock is on; never reached in graphics unit tests
static uint64_t att_bytes(id<MTLTexture> t, NSUInteger level) {
    if (!t) return 0;
    NSUInteger w = MAX(t.width >> level, 1), h = MAX(t.height >> level, 1);
    NSUInteger bpp = t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? 5 : t.pixelFormat == MTLPixelFormatRGBA16Float || t.pixelFormat == MTLPixelFormatRG32Float ? 8 :
                     t.pixelFormat == MTLPixelFormatRGBA32Float ? 16 : t.pixelFormat == MTLPixelFormatR8Unorm || t.pixelFormat == MTLPixelFormatStencil8 ? 1 :
                     t.pixelFormat == MTLPixelFormatRG8Unorm || t.pixelFormat == MTLPixelFormatR16Float ? 2 : 4;
    return (uint64_t)w * h * bpp * t.sampleCount;
}
// The render attachment selects a particular mip, while MTLTexture.width/height still describe the base level. Empty attachments do not take part in the bounds.
static NSUInteger mip_extent(NSUInteger size, NSUInteger level) {
    return level < sizeof(size) * 8 ? MAX(size >> level, 1) : 1;
}
static void pass_dims(MTL4RenderPassDescriptor *pd, NSUInteger nc, NSUInteger *w, NSUInteger *h) {
    *w = *h = 0;
    for (NSUInteger i = 0; i < nc + 2; i++) {
        MTLRenderPassAttachmentDescriptor *a = i < nc ? pd.colorAttachments[i] : i == nc ? pd.depthAttachment : pd.stencilAttachment;
        if (!a.texture) continue;
        NSUInteger aw = mip_extent(a.texture.width, a.level), ah = mip_extent(a.texture.height, a.level);
        *w = *w ? MIN(*w, aw) : aw; *h = *h ? MIN(*h, ah) : ah;
    }
    if (!*w) { *w = MAX(pd.renderTargetWidth, 1); *h = MAX(pd.renderTargetHeight, 1); }   // attachment-less pass (rp_begin_ex_): its explicit size
}

// ---------- DM_NVN_PASSGPU=<file>: per-pass GPU time (stage-boundary timestamp counters) ----------
// Each render pass samples 4 points (vertex start/end, fragment start/end), a compute pass 2 points; resolved after the command buffer completes and accumulated by 'index of the pass within the frame'.
// Each line: frame idx kind gpu_ms inc_ms w h fmt0 dfmt load draws t0_ns t1_ns (kind: R render / C compute; load: L load / C clear for each color attachment + depth)
// On TBDR passes overlap in the pipeline (the later pass's vertex stage is earlier than the end of the earlier pass's fragment stage): gpu_ms = this pass's start-to-end span (including queueing) and cannot be summed;
// attribution uses the timeline increment inc_ms = t1 − max(t0, latest t1 of the passes completed so far); the sum of the increments of all passes = total time the GPU is busy (command buffers complete in submission order)
typedef struct { uint64_t frame, pass; int idx, kind, w, h, draws, slot; uint32_t cfmt0, dfmt; char load[12]; } pg_pass;   // pass: global pass number (same as the pass number in the DM_NVN_PASSDUMP lines, for joining the two records)
#define PG_SLOTS 4096   // Counter sample buffer limit 32768 bytes = 4096 timestamps (measured from the error on this machine)
#define PG_RING 8
static int g_pg = -1; static FILE *g_pg_f;
static id<MTLCounterSampleBuffer> g_pg_buf[PG_RING]; static int g_pg_b; static uint32_t g_pg_used;
static pg_pass *g_pg_pass; static int g_pg_np, g_pg_cap, g_pg_open = -1;
static uint64_t g_pg_frame = ~0ull; static int g_pg_idx;
static pthread_mutex_t g_pg_mu = PTHREAD_MUTEX_INITIALIZER;
static inline void args_dirty_all(void);
typedef struct { double sum, last; uint64_t n; pg_pass meta; } pg_stat;
static pg_stat g_pg_st[256]; static uint64_t g_pg_rep_frame;
static int pg_on(void) {
    if (g_pg >= 0) return g_pg;
    const char *e = getenv("DM_NVN_PASSGPU"); g_pg = 0;
    if (!e) return 0;
    if (![g_dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) { MLOG("[RED] DM_NVN_PASSGPU: this device does not support counter sampling at stage boundaries"); return 0; }
    id<MTLCounterSet> ts = nil; for (id<MTLCounterSet> c in g_dev.counterSets) if ([c.name isEqualToString:MTLCommonCounterSetTimestamp]) ts = c;
    if (!ts) { MLOG("[RED] DM_NVN_PASSGPU: no timestamp counter set"); return 0; }
    MTLCounterSampleBufferDescriptor *d = [MTLCounterSampleBufferDescriptor new];
    d.counterSet = ts; d.storageMode = MTLStorageModeShared; d.sampleCount = PG_SLOTS;
    for (int i = 0; i < PG_RING; i++) { NSError *er = nil; g_pg_buf[i] = [g_dev newCounterSampleBufferWithDescriptor:d error:&er]; if (!g_pg_buf[i]) { MLOG("[RED] cannot create the counter sample buffer: %s", er.localizedDescription.UTF8String); return 0; } }
    if (!(g_pg_f = fopen(e, "w"))) { MLOG("[RED] DM_NVN_PASSGPU cannot open %s", e); return 0; }
    fprintf(g_pg_f, "frame\tidx\tkind\tgpu_ms\tinc_ms\tw\th\tfmt0\tdfmt\tload\tdraws\tt0_ns\tt1_ns\tpass\n");
    MLOG("[passgpu] per-pass GPU timing → %s", e);
    return g_pg = 1;
}
static void pg_sub_begin(uint64_t fence) {   // This submission uses block fence % PG_RING of the ring; first wait for the submission that used it PG_RING submissions ago to complete
    if (!pg_on()) return;
    if (fence > PG_RING) mtl_wait_at(fence - PG_RING, UINT64_MAX, WS_PG);
    g_pg_b = (int)(fence % PG_RING); g_pg_used = 0; g_pg_np = 0; g_pg_open = -1;
}
static pg_pass *pg_new(int kind, int nslot) {
    if (!pg_on() || g_pg_used + nslot > PG_SLOTS) return NULL;
    uint64_t f = atomic_load(&g_cur_frame); if (f != g_pg_frame) { g_pg_frame = f; g_pg_idx = 0; }
    if (g_pg_np == g_pg_cap) { g_pg_cap = g_pg_cap ? g_pg_cap * 2 : 128; g_pg_pass = realloc(g_pg_pass, g_pg_cap * sizeof *g_pg_pass); }
    pg_pass *p = &g_pg_pass[g_pg_np++]; memset(p, 0, sizeof *p);
    p->frame = f; p->pass = g_stat_pass; p->idx = g_pg_idx++; p->kind = kind; p->slot = (int)g_pg_used; p->draws = -(int)g_stat_draw;
    g_pg_used += nslot;
    return p;
}
static void pg_render(MTL4RenderPassDescriptor *pd, NSUInteger nc) {
    (void)pd; (void)nc;
    return;
#if 0
    pg_pass *p = pg_new(0, 4); if (!p) return;
    MTLRenderPassSampleBufferAttachmentDescriptor *a = pd.sampleBufferAttachments[0];
    a.sampleBuffer = g_pg_buf[g_pg_b];
    a.startOfVertexSampleIndex = p->slot; a.endOfVertexSampleIndex = p->slot + 1; a.startOfFragmentSampleIndex = p->slot + 2; a.endOfFragmentSampleIndex = p->slot + 3;
    id<MTLTexture> t0 = nil; int k = 0;
    for (NSUInteger i = 0; i < nc && i < 8; i++) { id<MTLTexture> t = pd.colorAttachments[i].texture; if (!t) continue;
        if (!t0) t0 = t; if (k < 10) p->load[k++] = pd.colorAttachments[i].loadAction == MTLLoadActionClear ? 'C' : 'L'; }
    id<MTLTexture> dt = pd.depthAttachment.texture;
    if (dt) { if (k < 10) { p->load[k++] = '|'; p->load[k++] = pd.depthAttachment.loadAction == MTLLoadActionClear ? 'C' : 'L'; } p->dfmt = (uint32_t)dt.pixelFormat; }
    if (!t0) t0 = dt;
    NSUInteger pw, ph; pass_dims(pd, nc, &pw, &ph);
    p->w = (int)pw; p->h = (int)ph; p->cfmt0 = t0 && t0 != dt ? (uint32_t)t0.pixelFormat : 0;
    g_pg_open = g_pg_np - 1;
#endif
}
static id<MTLComputeCommandEncoder> pg_compute(void) {
    pg_pass *p = pg_new(1, 2);
    ser_gate();
    if (!p) { args_dirty_all(); return enc_c_begin(NULL); }
    MTLComputePassDescriptor *cd = [MTLComputePassDescriptor computePassDescriptor];
    cd.sampleBufferAttachments[0].sampleBuffer = g_pg_buf[g_pg_b];
    cd.sampleBufferAttachments[0].startOfEncoderSampleIndex = p->slot; cd.sampleBufferAttachments[0].endOfEncoderSampleIndex = p->slot + 1;
    p->draws = 0;
    args_dirty_all();
    return enc_c_begin(cd);
}
static void pg_end_render(void) { if (g_pg_open >= 0) { g_pg_pass[g_pg_open].draws += (int)g_stat_draw; g_pg_open = -1; } }
static void pg_commit(id<MTLCommandBuffer> cb) {   // Hand this submission's pass table to the completion callback for resolving
    if (!g_pg || !g_pg_np) return;
    int n = g_pg_np; pg_pass *ps = malloc(n * sizeof *ps); memcpy(ps, g_pg_pass, n * sizeof *ps);
    id<MTLCounterSampleBuffer> buf = g_pg_buf[g_pg_b]; uint32_t used = g_pg_used;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        (void)c;
        NSData *d = [buf resolveCounterRange:NSMakeRange(0, used)];
        const MTLCounterResultTimestamp *ts = d.bytes;
        pthread_mutex_lock(&g_pg_mu);
        for (int i = 0; d && i < n; i++) {
            pg_pass *p = &ps[i]; uint64_t a, b;
            #define OK(x) ((x) != MTLCounterErrorValue && (x) != 0)
            if (p->kind == 0) { a = OK(ts[p->slot].timestamp) ? ts[p->slot].timestamp : ts[p->slot + 2].timestamp; b = OK(ts[p->slot + 3].timestamp) ? ts[p->slot + 3].timestamp : ts[p->slot + 1].timestamp; }
            else { a = ts[p->slot].timestamp; b = ts[p->slot + 1].timestamp; }
            if (!OK(a) || !OK(b) || b < a) continue;
            #undef OK
            static uint64_t prev_end;   // Latest end time of the passes completed so far (completion callbacks are serialized in submission order)
            double ms = (b - a) / 1e6;   // Timestamps are in ns, same as the CPU (measured 1:1 via sampleTimestamps on this machine)
            double inc = b > prev_end ? (b - (a > prev_end ? a : prev_end)) / 1e6 : 0;
            if (b > prev_end) prev_end = b;
            fprintf(g_pg_f, "%llu\t%d\t%c\t%.4f\t%.4f\t%d\t%d\t%u\t%u\t%s\t%d\t%llu\t%llu\t%llu\n", (unsigned long long)p->frame, p->idx, p->kind ? 'C' : 'R', ms, inc,
                    p->w, p->h, p->cfmt0, p->dfmt, p->load, p->draws, (unsigned long long)a, (unsigned long long)b, (unsigned long long)p->pass);
            if (p->idx < 256 && p->frame > 300) { pg_stat *st = &g_pg_st[p->idx]; st->sum += inc; st->n++; st->last = inc; st->meta = *p; }
            if (p->frame >= g_pg_rep_frame + 300 && p->frame > 300) {   // Report the top five every 300 frames (accumulated from frame 300)
                g_pg_rep_frame = p->frame; double tot = 0; int top[5] = {-1, -1, -1, -1, -1};
                for (int k = 0; k < 256; k++) if (g_pg_st[k].n) { double m = g_pg_st[k].sum / g_pg_st[k].n; tot += m;
                    for (int j = 0; j < 5; j++) if (top[j] < 0 || m > g_pg_st[top[j]].sum / g_pg_st[top[j]].n) { for (int q = 4; q > j; q--) top[q] = top[q - 1]; top[j] = k; break; } }
                char line[1024]; size_t o = 0;
                for (int j = 0; j < 5 && top[j] >= 0; j++) { pg_stat *t = &g_pg_st[top[j]];
                    o += snprintf(line + o, sizeof line - o, " #%d %.2f ms %dx%d fmt%u d%u %s %d draws;", top[j], t->sum / t->n, t->meta.w, t->meta.h, t->meta.cfmt0, t->meta.dfmt, t->meta.load, t->meta.draws); }
                MLOG("[passgpu] up to frame %llu: sum of per-pass means per frame %.2f ms; top five:%s", (unsigned long long)p->frame, tot, line);
                fflush(g_pg_f);
            }
        }
        pthread_mutex_unlock(&g_pg_mu);
        free(ps);
    }];
}
void ts_after(void);
void ms_after(void);
// ---------- Store lookahead: at pass end, decide from the later commands of the same submission whether each attachment's write-back is redundant ----------
// On TBDR every attachment is written back to memory in full when the pass ends; Switch (Maxwell immediate-mode rendering) has no such cost. If an attachment is cleared in full before being read, the write-back is redundant (DontCare).
// For the decision see mtl_sub_index / sdx_decide; only what can be proven within the same submission is accepted, and anything still undecided at the end of the submission is always written back.
typedef struct { uint64_t ser, bytes; uint32_t lv, sl; int idx; uint8_t depth, state, ridx, drawn, why, load, wrote, wwhy; const char *wkey; int wslot; } sd_att;   // depth: 0 color 1 depth 2 stencil (the two parts of a combined depth-stencil texture are decided separately)   // state 0 undecided 1 needed 2 redundant; idx: color attachment index, -1 for depth
static sd_att g_sd[9]; static int g_nsd;
static int g_udc_ab = -1, g_udc_ab_on;
// Attachments this pass did not write (depth / stencil test only, color write mask 0, etc.) get their write-back changed to DontCare: the content is already in memory, saving one full write-back.
// On by default: on Mac, same-process on / off screenshots follow the same distribution as the null control; on A19 Pro, private Depth32F_S8 / RGBA8 read back after DontCare differ in 0 places.
// DM_NVN_UNWRITTEN_DC=0 turns it off (for equivalence comparison)
static int udc_on(void) {
    if (g_udc_ab < 0) { const char *e = getenv("DM_NVN_UNWRITTEN_DC"); g_udc_ab = !(e && !strcmp(e, "0"));
        const char *a = getenv("DM_NVN_UDC_AB"); if (a && !strcmp(a, "1")) { g_udc_ab = 0; g_udc_ab_on = 0; } }
    return g_udc_ab || g_udc_ab_on;
}
// DM_NVN_TBDR=0 disables lookahead and pass merging. The independent unwritten-attachment proof remains active; DM_NVN_UNWRITTEN_DC=0 disables that too.
// Only for equivalence comparison (the same replay run on / off, comparing the picture byte by byte), not a product configuration
// Mario: off by default (only DM_NVN_TBDR=1 turns it on). These optimizations infer read / write relations from "which textures are bound in texture slots", while 38% of this game's shaders use bindless textures (handles in uniform buffers, NVN_RB_TEXU),
//   so the inference would miss readers - e.g. deciding an attachment is never read afterwards and not storing it. Turn them back on once they recognize bindless readers.
static int tbdr_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_TBDR"); v = e && !strcmp(e, "1"); if (!v) MLOG("TBDR optimizations off by default (this game uses bindless textures; DM_NVN_TBDR=1 turns them on)"); } return v; }
static int rt_range_on(void) { static int v = -1; if (v < 0) { const char *e=getenv("DM_NVN_RT_RANGE"); v=e && !strcmp(e,"1"); } return v; }   // Narrow candidate, off by default
// ---------- In-process A/B phases (DM_AB, measurement) ----------
// DM_AB=<frames> splits the run into phases of that many frames (frame f is in phase f / frames) and alternates the switches whose value is ab (sw_mode; DM_NVN_BARRIER=ab so far):
// ab = on in every second phase (phase 0 with all of them off), ab:<k>[,<k>...]/<n> = on in the phases whose number modulo n is one of the k (four configurations of two switches: one
// ab:1,3/4, the other ab:2,3/4). Both configurations then run in the same process, scene and thermal state, which separate runs on a phone cannot give. A switch reads its mode once
// (sw_mode) and asks sw_on at each use; one that must stay the same over a pass latches it when the pass begins
static _Atomic long g_ab_phase; static long g_ab_len = -1;
static long ab_len(void) { if (g_ab_len < 0) { const char *e = getenv("DM_AB"); g_ab_len = e ? atol(e) : 0; if (g_ab_len < 0) g_ab_len = 0; if (g_ab_len) MLOG("DM_AB: switches set to ab alternate every %ld frames (phase 0 with all of them off)", g_ab_len); } return g_ab_len; }
enum { SW_MAX = 8 }; static const char *g_sw_name[SW_MAX]; static int g_sw_mode[SW_MAX], g_sw_n;
static inline int sw_on_at(int m, long ph) { if (m < 2) return m; int n = (m - 2) & 15, mask = (m - 2) >> 4; return mask >> (ph % n) & 1; }
// The mode of switch <name>: 0 off, 1 on, 2 + n + 16 * mask = on in the phases whose number modulo n is in mask; ab without DM_AB is off
static int sw_mode(const char *name, int dflt) {
    const char *e = getenv(name); int m = dflt, mask = 2, n = 2;
    if (e && !strncmp(e, "ab", 2)) {
        if (e[2] == ':') { const char *sl = strchr(e, '/'); int nn = sl ? atoi(sl + 1) : 0, mk = 0;
            for (const char *q = e + 3; sl && q < sl; ) { int k = atoi(q); if (k >= 0 && k < 15) mk |= 1 << k; while (q < sl && *q != ',') q++; if (q < sl) q++; }
            if (nn >= 2 && nn <= 15 && mk && mk < (1 << nn)) { mask = mk; n = nn; } }
        m = ab_len() ? 2 + n + 16 * mask : 0;
        if (!ab_len()) MLOG("[RED] %s=%s needs DM_AB=<frames>: the switch is off", name, e); }
    else if (e) m = atoi(e) ? 1 : 0;
    if (m >= 2) MLOG("%s: on in the DM_AB phases whose number modulo %d is in the set %#x", name, n, mask);
    if (g_sw_n < SW_MAX) { g_sw_name[g_sw_n] = name; g_sw_mode[g_sw_n++] = m; }
    return m;
}
static inline int sw_on(int m) { return sw_on_at(m, atomic_load_explicit(&g_ab_phase, memory_order_relaxed)); }
// Each phase is reported once its frames' GPU times have arrived (8 frames late) as an [ab] line: the switches on in it, GPU time and whole frame time (p50 / p90 / mean over the phase
// without its first quarter, while the GPU clock and the frame step settle), and attachment traffic and render passes per frame (the bandwidth ledger)
enum { AB_MAXF = 4096 };
static float g_ab_gpu[AB_MAXF], g_ab_fr[AB_MAXF]; static int g_ab_nf; static long g_ab_ph = -1; static uint64_t g_ab_bw0[2], g_ab_pass0, g_ab_f0;
static uint64_t g_ab_bw1[2], g_ab_pass1, g_ab_f1;   // the counters at the switch to the current phase (taken in ab_frame), copied to *0 when the previous phase is reported
static int flt_cmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
static void ab_frame(uint64_t f) {   // frame f begins (mtl_set_frame): the phase follows the frame number
    long ph = (long)(f / (uint64_t)g_ab_len);
    if (atomic_load(&g_ab_phase) != ph || f % (uint64_t)g_ab_len == 0) { g_ab_bw1[0] = g_bw_load; g_ab_bw1[1] = g_bw_store; g_ab_pass1 = g_stat_pass; g_ab_f1 = f; }
    atomic_store(&g_ab_phase, ph);
}
static void ab_report(long ph) {
    int n = g_ab_nf, skip = n / 4, m = n - skip; if (m < 10) return;
    float *g = malloc(sizeof(float) * (size_t)m), *fr = malloc(sizeof(float) * (size_t)m); double gs = 0, fs = 0;
    for (int i = 0; i < m; i++) { g[i] = g_ab_gpu[skip + i]; fr[i] = g_ab_fr[skip + i]; gs += g[i]; fs += fr[i]; }
    qsort(g, (size_t)m, sizeof *g, flt_cmp); qsort(fr, (size_t)m, sizeof *fr, flt_cmp);
    double nf = g_ab_f1 > g_ab_f0 ? (double)(g_ab_f1 - g_ab_f0) : 1;
    char on[160] = ""; size_t o = 0;
    for (int i = 0; i < g_sw_n && o < sizeof on; i++) if (g_sw_mode[i] >= 2 && sw_on_at(g_sw_mode[i], ph)) {
        const char *nm = g_sw_name[i]; if (!strncmp(nm, "DM_NVN_", 7)) nm += 7; else if (!strncmp(nm, "DM_", 3)) nm += 3;
        o += (size_t)snprintf(on + o, sizeof on - o, "%s%s", o ? "," : "", nm); }
    MLOG("[ab] phase %ld %s: %d frames, GPU p50 %.2f / p90 %.2f / mean %.2f ms, frame p50 %.2f / p90 %.2f / mean %.2f ms; per frame: attachments load %.0f MB store %.0f MB, passes %.1f",
         ph, o ? on : "base", m, g[m / 2], g[m * 9 / 10], gs / m, fr[m / 2], fr[m * 9 / 10], fs / m,
         (g_ab_bw1[0] - g_ab_bw0[0]) / nf / 1048576.0, (g_ab_bw1[1] - g_ab_bw0[1]) / nf / 1048576.0, (g_ab_pass1 - g_ab_pass0) / nf);
    free(g); free(fr);
}
// Once per frame with that frame's GPU time and whole frame time (hle_nvn.c, 8 frames late); the phase is the one frame f ran in
void mtl_ab_gpu(uint64_t f, double gpu_ms, double frame_ms) {
    if (!ab_len()) return;
    long ph = (long)(f / (uint64_t)g_ab_len);
    if (ph != g_ab_ph) {
        if (g_ab_ph >= 0) ab_report(g_ab_ph);
        g_ab_ph = ph; g_ab_nf = 0;
        memcpy(g_ab_bw0, g_ab_bw1, sizeof g_ab_bw0); g_ab_pass0 = g_ab_pass1; g_ab_f0 = g_ab_f1;
    }
    if (g_ab_nf < AB_MAXF) { g_ab_gpu[g_ab_nf] = (float)gpu_ms; g_ab_fr[g_ab_nf] = (float)frame_ms; g_ab_nf++; }
}

static uint64_t g_sd_dead_bytes, g_sd_n_dead, g_sd_n_all;
// ---------- Cross-submission attachment validity ledger ----------
// Entry = subresource (texture serial, level, layer) + state, surviving across submissions:
//   XV_INVALID       content was not written back / not generated and must not be read. The next touch must be a full Clear, or use as an attachment with load action DontCare / Clear; otherwise it is a violation.
//   XV_PENDING_CLEAR reserved for 'clear-only pending across submissions' (not implemented): accepts only full, unscissored clears with the write mask fully open; clearing again while pending = replace the clear value without landing it;
//                    as an attachment with load DontCare / Clear → the pending clear is voided, with load Load → changed to a Clear load; depth-stencil is recorded per plane.
// Violation = report a failure + that stable key (format / width / height / level) permanently disabled (in memory + persisted to DM_XV_FILE, read at the next launch) + all entries of the same texture deleted. The fallback can only detect, not undo (in that frame the attachment holds old content).
// Its first use: color attachments 'scanned to the end undecided' at the end of a submission are not written back (sd_finish) - conditions: its first touch in this submission is a full clear / DontCare load (reset every frame before use), it is not a window texture, and the key is not disabled.
// Switch DM_NVN_XV=0 turns it off; has no effect when tbdr_on()==0.
// Trial period (XV_PROBE): for a key's first XV_TRIAL times it is only 'shadow-accounted' - written back as usual, but its next touch is tracked all the same; only after XV_TRIAL consecutive compliant touches does the write-back really start being skipped.
//   On a read / Load during the trial period: only disable that key (within this process, not persisted, no failure), the picture is intact - in practice there are readers as early as the launch / menu screens (upscale target 36), and not every fresh install should get one wrong frame.
enum { XV_INVALID = 1, XV_PENDING_CLEAR = 2, XV_PROBE = 3 };
#define XV_TRIAL 3
typedef struct { uint64_t ser, tag; uint32_t lv, sl; uint8_t state; } xv_ent;
static xv_ent g_xv[64]; static int g_xv_n, g_xv_block, g_xv_init;
static uint64_t g_xv_disabled[128]; static int g_xv_ndis;
static struct { uint64_t tag; int good; } g_xv_trial[128]; static int g_xv_ntrial;   // Number of consecutive compliant touches for each key
static int xv_trial_get(uint64_t tag, int add) { for (int i = 0; i < g_xv_ntrial; i++) if (g_xv_trial[i].tag == tag) return g_xv_trial[i].good += add; if (g_xv_ntrial < 128) { g_xv_trial[g_xv_ntrial].tag = tag; return g_xv_trial[g_xv_ntrial++].good = add; } return 0; }
static uint64_t g_xv_present[128]; static int g_xv_npresent;
static uint64_t g_xv_dc, g_xv_viol;
typedef struct { uint64_t ser; uint32_t lv, sl; uint8_t good; } xv_touch;
static xv_touch g_xv_touch[128]; static int g_xv_ntouch;
static uint64_t xv_tag(const tex_t *t, uint32_t lv) { return ((uint64_t)t->d.format << 40) ^ ((uint64_t)t->d.width << 24) ^ ((uint64_t)t->d.height << 8) ^ lv; }
// Stable key (unchanged across launches, does not use the texture serial): format / width / height / level, then mixed with the formats of the color attachments of the pass where the write-back is skipped and this attachment's index - textures of the same format and size but different purpose (e.g. transmittance 27 and upscale target 36) do not share a key
static uint64_t xv_tag_pass(const tex_t *t, uint32_t lv, int idx, const p_set_rt *rt) { uint64_t h = xv_tag(t, lv) ^ ((uint64_t)(idx + 1) << 60); for (int i = 0; i < rt->n && i < 8; i++) h = (h ^ (rt->color[i] ? (uint64_t)rt->color[i]->d.format + 1 : 0)) * 0x100000001b3ull; return h; }
static void xv_init(void) {
    if (g_xv_init) return; g_xv_init = 1;
    const char *p = getenv("DM_XV_FILE"); if (!p || !*p) return;
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[256], why[160]; unsigned long long tag; int ch, n = 0;
    while (fgets(line, sizeof line, f) && g_xv_ndis < 128) if (sscanf(line, "%llx\t%159[^\t]\t%d", &tag, why, &ch) >= 1) g_xv_disabled[g_xv_ndis++] = tag, n++;
    fclose(f); MLOG("read %d disable records", n);
}
static int xv_disabled(uint64_t tag) { xv_init(); for (int i = 0; i < g_xv_ndis; i++) if (g_xv_disabled[i] == tag) return 1; return 0; }
static xv_ent *xv_find(uint64_t ser, uint32_t lv, uint32_t sl) { for (int i = 0; i < g_xv_n; i++) if (g_xv[i].ser == ser && g_xv[i].lv == lv && g_xv[i].sl == sl) return &g_xv[i]; return NULL; }
static void xv_drop(uint64_t ser, uint32_t lv, uint32_t sl) { for (int i = 0; i < g_xv_n; i++) if (g_xv[i].ser == ser && g_xv[i].lv == lv && g_xv[i].sl == sl) { g_xv[i] = g_xv[--g_xv_n]; return; } }
static void xv_drop_all_of(uint64_t ser) { for (int i = 0; i < g_xv_n;) if (g_xv[i].ser == ser) g_xv[i] = g_xv[--g_xv_n]; else i++; }
static void xv_set(uint64_t ser, uint32_t lv, uint32_t sl, uint8_t state, uint64_t tag) {
    xv_init(); if (g_xv_block || xv_disabled(tag)) return;
    xv_ent *e = xv_find(ser, lv, sl); if (e) { e->state = state; e->tag = tag; return; }
    if (g_xv_n < 64) g_xv[g_xv_n++] = (xv_ent){ser, tag, lv, sl, state};
}
static void xv_violation(const xv_ent *e, const char *why) {
    xv_init();
    if (e->state == XV_PROBE) {   // Trial period: the content is still there and the picture is intact - only disable the key (this process)
        if (!xv_disabled(e->tag) && g_xv_ndis < 128) g_xv_disabled[g_xv_ndis++] = e->tag;
        PD("V xv reader seen during probation: key=%016llx texture=%llu level=%u reason=%s → key disabled", (unsigned long long)e->tag, (unsigned long long)e->ser, e->lv, why);
        xv_drop_all_of(e->ser); return; }
    g_xv_viol++;
    int seen = xv_disabled(e->tag); if (!seen && g_xv_ndis < 128) g_xv_disabled[g_xv_ndis++] = e->tag;
    MLOG("[RED] xv tag=%016llx texture=%llu level=%u layer=%u reason=%s pass=%llu", (unsigned long long)e->tag, (unsigned long long)e->ser, e->lv, e->sl, why, (unsigned long long)g_stat_pass);
    const char *p = getenv("DM_XV_FILE"); if (p && *p) { FILE *f = fopen(p, "a"); if (f) { fprintf(f, "%016llx\t%s\t%llu\n", (unsigned long long)e->tag, why, (unsigned long long)g_stat_pass); fclose(f); } }
    xv_drop_all_of(e->ser);
}
static void xv_check(uint64_t ser, uint32_t lv, uint32_t sl, const char *why) { xv_ent *e = xv_find(ser, lv, sl); if (e && (e->state == XV_INVALID || e->state == XV_PROBE)) { xv_ent copy = *e; xv_violation(&copy, why); } }
static void xv_good(xv_ent *e) { if (e->state == XV_PROBE) xv_trial_get(e->tag, 1); xv_drop(e->ser, e->lv, e->sl); }   // Compliant touch: counts once toward the trial period, and the entry returns to valid
static void xv_touch_add(uint64_t ser, uint32_t lv, uint32_t sl, int good) { for (int i = 0; i < g_xv_ntouch; i++) if (g_xv_touch[i].ser == ser && g_xv_touch[i].lv == lv && g_xv_touch[i].sl == sl) return; if (g_xv_ntouch < 128) g_xv_touch[g_xv_ntouch++] = (xv_touch){ser, lv, sl, (uint8_t)good}; }
static int xv_first_good(uint64_t ser, uint32_t lv, uint32_t sl) { for (int i = 0; i < g_xv_ntouch; i++) if (g_xv_touch[i].ser == ser && g_xv_touch[i].lv == lv && g_xv_touch[i].sl == sl) return g_xv_touch[i].good; return 0; }
static int xv_present(uint64_t ser) { for (int i = 0; i < g_xv_npresent; i++) if (g_xv_present[i] == ser) return 1; return 0; }
static void xv_forward(void);
static int tbdr_on(void);
static int xv_on(void) { static int off = -1; if (off < 0) { const char *e = getenv("DM_NVN_XV"); off = e && !strcmp(e, "0"); } return !off && tbdr_on(); }
void mtl_xv_window_tex(uint64_t serial) { if (!xv_present(serial) && g_xv_npresent < 128) g_xv_present[g_xv_npresent++] = serial; }   // Window (swapchain) texture: never enters the ledger
void xv_test_counts(int *dc, int *viol) { if (dc) *dc = (int)g_xv_dc; if (viol) *viol = (int)g_xv_viol; }
static void sd_finish(void);
// Load elision (fully covering first draw): if the draw that opens the pass can be proven to cover a color attachment in full (first_draw_covers), that attachment's load becomes DontCare -
// the first draw does not blend and writes all channels, every covered pixel is rewritten by it, and the original content is not visible (Switch immediate-mode rendering has no such cost; on TBDR load = reading the whole thing from memory into the tile)
static const p_draw *g_fd_dw; static const p_draw_idx *g_fd_di;   // The draw that opens the pass (one of non-indexed / indexed); non-null only while draw() is opening a pass
static uint64_t g_fd_n;         // Cumulative number of attachments judged elidable
static int g_fd_why;            // Reason for the most recent failure (Q line of PASSDUMP: 1 no program 2 shader 3 attachment 4 blend/mask/raster 5 depth-stencil test 6 vertex count 7 position attribute 8 format/stream 9 out of bounds 10 address 11 geometry)
static int first_draw_covers(int i, id<MTLTexture> t, NSUInteger level);
static int pf_is_float(MTLPixelFormat f) {   // Float color formats that can store NaN (sentinel choice for the load-elision probe; RGB9E5 cannot store NaN and does not count)
    switch (f) { case MTLPixelFormatR16Float: case MTLPixelFormatRG16Float: case MTLPixelFormatRGBA16Float: case MTLPixelFormatR32Float:
        case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA32Float: case MTLPixelFormatRG11B10Float: return 1; default: return 0; }
}
static int fz_load_covers(int i, id<MTLTexture> t, NSUInteger level);
static NSMutableArray *g_lp;    // DM_NVN_LOADPROBE=1 / 2 / 3 (two constant sentinel values / NaN - under float formats normal shader output cannot reach NaN, ruling out numeric coincidence): attachments judged elidable are cleared to the sentinel color instead, and at pass end the remaining sentinel pixels are read back and counted (diagnostic: should be 0)
static void lp_finish(void);
// Same-target continuation: when the game calls SetRenderTargets again on the same set of targets (with no copy / dispatch / clear in between) the pass is not ended - on TBDR this saves one full write-back + load.
// Basis for equivalence: NVN's Barrier between two passes requires the latter to see all writes of the former. Within one Metal render encoder, later draws' reads and writes of the attachment itself (blend, depth / stencil test)
// are ordered with earlier draws in submission order; but 'sampling' the attachment texture is not covered (reading and writing the same attachment within one pass is undefined). So continuation happens only when later draws do not sample these attachments:
// if during continuation a draw needs to sample them, or a clear arrives, the pass is ended before it, returning to the separate semantics. Copies / dispatches end the render encoder anyway
static int g_enc_mode;          // How the current encoder was opened (rp_begin_ex argument: 0 all attachments 1 only for clears 2 depth omitted)
// Only the synthetic raster draw of geo_draw may open a pass without these
// colors. geo_draw ends that pass before returning to ordinary command replay.
static int g_geo_omit_colors, g_enc_geo_nocolor;
static int g_rt_range_active, g_rt_range_pending;
static int g_rt_merged;         // The current pass has already been continued (at least one same-target SetRenderTargets was merged in)
static uint64_t g_rt_merge_n;   // Cumulative continuation count
static int g_defer_replaying;
static void fz_end(void);   // Wrap-up of the readable-tile pass merge (defined before draw)
static void defer_fallback(void);
// DM_NVN_ENC_LABEL=1: label every render encoder with 'p<index of the render pass within the frame> <width>x<height> <first 8 digits of the first draw's fragment key> d<draw count>'; in Instruments (xctrace Game Performance / Metal System Trace)
// the encoder track and the GPU interval table carry this name directly, and shader_map.tsv maps it back to the shader; off by default (one string allocation per pass)
static int g_el_w, g_el_h, g_el_draws; static char g_el_key[12];
static int enc_label_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_ENC_LABEL"); v = e && atoi(e); } return v; }
static int g_nbcrt; static void bcrt_flush(void);   // shadows of block-compressed render targets used by the open pass (see bcrt_attach)
// A pass that draws into a texture the host uploads from its storage (tex_storage_dst) writes that storage on the hardware, as a GPU copy does (mirror_readback): what the pass drew is
// read back into the storage when it ends, so that a later upload from the storage, or the CPU reading or copying it, carries the drawing. Tears of the Kingdom draws the normal maps of
// its terrain tiles (260x260 RG8, 9 levels, in a CPU-cached pool) after a flush over their storage made them CPU-written. Its resource defragmentation (main+0x5bc988) later moves each
// tile with memcpy and re-creates the texture at the new address, which uploaded the zeros the storage held: a tile's grass got no length and its ground no light (dark, bare meadows a
// few seconds after loading)
static struct { tex_t t; void *tex; unsigned level, slice, n; int att; } g_rtm[8]; static int g_nrtm;   // n: the layers the pass draws from slice on; att: the colour slot
static int tex_storage_dst(const tex_t *t);
static void mirror_readback(const tex_t *t, id<MTLTexture> d, unsigned level, unsigned layer0, const NVNcopyRegion *r, const char *from, int absorb);
static void rt_mirror_add(const tex_t *t, id<MTLTexture> mt, NSUInteger level, NSUInteger slice, int att) {
    if (!mt || t->d.target == NVN_TEXTURE_TARGET_3D || !tex_storage_dst(t)) return;
    for (int i = 0; i < g_nrtm; i++) if (g_rtm[i].t.serial == t->serial && g_rtm[i].level == level && g_rtm[i].slice == slice) return;
    if (g_nrtm == (int)(sizeof g_rtm / sizeof g_rtm[0])) { RED_ONCE(131, "more than %d storage-backed render targets in one pass: the rest are not read back into their storage", g_nrtm); return; }
    g_rtm[g_nrtm++] = (typeof(g_rtm[0])){*t, (void *)CFBridgingRetain(mt), (unsigned)level, (unsigned)slice, 1, att};
}
// Once the pass's layers are known: a layered or slice-merged pass (renderTargetArrayLength) draws into that many layers of each attachment from its attached slice on, all read back
static void rt_mirror_layers(MTL4RenderPassDescriptor *pd) {
    for (int i = 0; i < g_nrtm; i++) { MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[g_rtm[i].att];
        g_rtm[i].slice = (unsigned)c.slice; g_rtm[i].n = pd.renderTargetArrayLength > 1 ? (unsigned)pd.renderTargetArrayLength : 1; }
}
// After the pass has ended. The readbacks open the copy encoder, which is closed again: callers of rp_end may open an encoder of their own next and rely on none being open
// (with it left open, Tears of the Kingdom stopped presenting on loading its opening at frame 1983)
static void rt_mirror_flush(void) {
    int n = g_nrtm; g_nrtm = 0;
    for (int i = 0; i < n; i++) {
        id<MTLTexture> mt = CFBridgingRelease(g_rtm[i].tex);
        NVNcopyRegion r = {0, 0, 0, (int)MAX(mt.width >> g_rtm[i].level, 1u), (int)MAX(mt.height >> g_rtm[i].level, 1u), (int)g_rtm[i].n};
        mirror_readback(&g_rtm[i].t, mt, g_rtm[i].level, g_rtm[i].slice, &r, "render", 1);
    }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
}
static void rp_end(void) { if (g_enc) { AP_B;
    if (enc_label_on()) { static uint64_t lf = ~0ull, base; uint64_t f = atomic_load(&g_cur_frame); if (f != lf) { lf = f; base = g_stat_pass - 1; }   // Intra-frame serial: the same pass has the same name every frame, so the table can aggregate by name
        [(id)g_enc setLabel:[NSString stringWithFormat:@"p%02llu %dx%d %s d%d", (unsigned long long)(g_stat_pass - base), g_el_w, g_el_h, g_el_key[0] ? g_el_key : "-", g_el_draws]]; }
#ifdef NVN_RT_RANGE_TEST
    int ranged=g_rt_range_active && !g_rt_range_pending;
#endif
    sd_finish(); enc_r_end_pass(g_enc); g_gp_bw_l0 = g_bw_load; g_gp_bw_s0 = g_bw_store;   // the profile's per-pass ledger counts from the previous pass end, profiled or not
    zskip_commit_clears(); g_enc = nil; g_enc_geo_nocolor = 0; g_rt_merged = 0; g_sm_pass = 0; g_rt_range_active = g_rt_range_pending = 0; pg_end_render(); ts_after(); ms_after(); if (g_lp.count) lp_finish(); fz_end();
    if (g_nubd && g_nubd >= ubd_lazy()) ubd_flush();   // the inline updates deferred in this pass, in order (see mtl_sub_update); across passes they wait for the next copy or dispatch (blit_raw)
    if (g_nbcrt) bcrt_flush();   // blocks drawn into shadows go back into their compressed textures
    if (g_nrtm) rt_mirror_flush();
#ifdef NVN_RT_RANGE_TEST
    rr_finish(ranged);
#endif
    AP_E(0);
} }
void pg_sub_begin_ext(uint64_t fence) { pg_sub_begin(fence); }
void pg_commit_ext(id<MTLCommandBuffer> cb) { pg_commit(cb); }
// The format an attachment is actually attached with: the view's format if the view changes the format and can be attached, otherwise the base texture's. Shared by attaching (attach) and pipeline prediction (pso_predict_rest) so both agree --
// prediction used the base format, so for a pass written through an sRGB view (Mario's last pass) the predicted pipeline had the wrong format and the real one was compiled at the first draw (from d2r-mobile e2a0c9a9b)
// A compressed texture as a render target (The Legend of Zelda: Breath of the Wild a 32x32 RGTC1 (BC4) texture, Tears of the Kingdom a 32x32x32 3D one drawn one depth plane per pass, both written
// through an RGBA16UI view: one pixel = one 8-byte block, i.e. BC4 encoded on the GPU). Metal can neither attach a compressed format as a colour attachment nor create compressed <-> uncompressed views
// (the pipeline descriptor validation aborts the process). A BC texture with an uncompressed view whose pixel size equals the block size is drawn through a shadow (bcrt_attach); any other compressed target
// is left out (a pass with no attachment at all skips its draws), with a red line
static int mtl_fmt_block(MTLPixelFormat f) { return (f >= MTLPixelFormatBC1_RGBA && f <= MTLPixelFormatBC7_RGBAUnorm_sRGB) || (f >= MTLPixelFormatPVRTC_RGB_2BPP && f < MTLPixelFormatGBGR422); }
static int nvn_fmt_bc(uint32_t f) { return f >= 0x41 && f <= 0x50; }   // NVN BC1–7 / BC6H formats (0x41 RGB_DXT1 … 0x50 BC6H unsigned)
static int bc_block_bytes(MTLPixelFormat f) {   // bytes per 4x4 block of a BC format, 0 for other formats
    if (f < MTLPixelFormatBC1_RGBA || f > MTLPixelFormatBC7_RGBAUnorm_sRGB) return 0;
    return f <= MTLPixelFormatBC1_RGBA_sRGB || f == MTLPixelFormatBC4_RUnorm || f == MTLPixelFormatBC4_RSnorm ? 8 : 16;
}
// A depth texture drawn through a colour view of the same pixel size (Luigi's Mansion 3: a D16 texture through an R16 view at boot) is drawn the same way: Metal can neither attach a depth format
// as a colour attachment nor make a colour view of a depth texture, and the pipeline descriptor validation aborts the process. Only formats whose Metal pixel is the NVN pixel qualify (D16 and D32F;
// D24S8 is Depth32Float_Stencil8 here, whose bytes are not the game's)
static int shrt_block(MTLPixelFormat f, int *side) {   // bytes per block of a texture drawn through a shadow, and the block's side in pixels; 0 for any other format
    int bb = bc_block_bytes(f); if (bb) { *side = 4; return bb; }
    *side = 1; return f == MTLPixelFormatDepth16Unorm ? 2 : f == MTLPixelFormatDepth32Float ? 4 : 0;
}
static int mtl_fmt_depth(MTLPixelFormat f) { return f == MTLPixelFormatDepth16Unorm || f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatX32_Stencil8 || f == MTLPixelFormatStencil8; }
// The shadow's format: the view's, when the base is BC (or a depth format, above) and the view is an uncompressed colour format with one block's bytes per pixel
static int bcrt_view(id<MTLTexture> base, const view_t *v, int has_v, MTLPixelFormat *pf_out) {
    uint32_t pf = 0; uint8_t swz[4]; int bw = 0, bh = 0, side = 0, bb = base ? shrt_block(base.pixelFormat, &side) : 0;
    if (!bb || !has_v || !v->set_format || !nvn_fmt_to_mtl(v->format, &pf, swz) || !pf || mtl_fmt_block((MTLPixelFormat)pf) || mtl_fmt_depth((MTLPixelFormat)pf)) return 0;
    if (nvn_fmt_bpb(v->format, &bw, &bh) != bb || bw != 1 || bh != 1) return 0;
    *pf_out = (MTLPixelFormat)pf; return 1;
}
// Drawing into a BC texture: the pass draws into an uncompressed shadow of blocks x blocks in the view's format. The level / layer / depth plane's blocks are copied into it through a buffer before the pass
// and back, byte for byte, after it (bcrt_flush, from rp_end). The shadow is an attachment of its own: always loaded and stored, and kept out of the load-skip / store-elision / zero-source bookkeeping,
// which keys by the BC texture's serial. Shadows and staging buffers stay resident and are reused per attachment number, format and size, so a pass per depth plane adds no residency churn.
// From PR #11 (digitalhivehub), where Tears of the Kingdom first drew this way
typedef struct { id<MTLTexture> base, shadow; id<MTLBuffer> buf; NSUInteger level, slice, z, w, h, bw, bh, bpb; } bcrt_t;
static bcrt_t g_bcrt[8];
static __strong id<MTLTexture> g_bcrt_sh[8]; static __strong id<MTLBuffer> g_bcrt_buf[8];
static int bcrt_is(id<MTLTexture> t) { for (int i = 0; i < g_nbcrt; i++) if (t && g_bcrt[i].shadow == t) return 1; return 0; }
static void bcrt_flush(void) {
    int n = g_nbcrt; g_nbcrt = 0;
    for (int i = 0; i < n; i++) { bcrt_t e = g_bcrt[i]; g_bcrt[i] = (bcrt_t){0};
        NSUInteger row = e.bw * e.bpb, img = row * e.bh;
        enc_b_copy_texture_buffer(blit(), e.shadow, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(e.bw, e.bh, 1), e.buf, 0, row, img);
        enc_b_copy_buffer_texture(blit(), e.buf, 0, row, img, MTLSizeMake(e.w, e.h, 1), e.base, e.slice, e.level, MTLOriginMake(0, 0, e.z));
    }
}
// a holds the BC texture with its level and layer / plane already set; returns 1 when a now holds the shadow
static int bcrt_attach(MTLRenderPassAttachmentDescriptor *a, id<MTLTexture> base, const view_t *v, int has_v) {
    MTLPixelFormat spf; int k = g_nbcrt; if (k >= 8 || !bcrt_view(base, v, has_v, &spf)) return 0;
    int is3d = base.textureType == MTLTextureType3D, side = 1;
    NSUInteger bpb = (NSUInteger)shrt_block(base.pixelFormat, &side);
    NSUInteger lv = a.level, sl = is3d ? 0 : a.slice, z = is3d ? a.depthPlane : 0, w = mip_extent(base.width, lv), h = mip_extent(base.height, lv), bw = (w + side - 1) / side, bh = (h + side - 1) / side;
    NSUInteger row = bw * bpb, img = row * bh;
    if (!g_bcrt_sh[k] || g_bcrt_sh[k].pixelFormat != spf || g_bcrt_sh[k].width != bw || g_bcrt_sh[k].height != bh) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:spf width:bw height:bh mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> sh = [g_dev newTextureWithDescriptor:d]; id<MTLBuffer> buf = [g_dev newBufferWithLength:img options:MTLResourceStorageModePrivate];
        if (!sh || !buf) { RED_ONCE(74, "render target in a compressed or depth texture: cannot create its %lux%lu-block shadow (Metal format %lu)", (unsigned long)bw, (unsigned long)bh, (unsigned long)spf); return 0; }
        if (g_bcrt_sh[k]) { defer_obj_release(g_bcrt_sh[k]); defer_obj_release(g_bcrt_buf[k]); }
        res_add(sh); res_add(buf); g_bcrt_sh[k] = sh; g_bcrt_buf[k] = buf;
    }
    id<MTLTexture> sh = g_bcrt_sh[k]; id<MTLBuffer> buf = g_bcrt_buf[k];
    enc_b_copy_texture_buffer(blit(), base, sl, lv, MTLOriginMake(0, 0, z), MTLSizeMake(w, h, 1), buf, 0, row, img);
    enc_b_copy_buffer_texture(blit(), buf, 0, row, img, MTLSizeMake(bw, bh, 1), sh, 0, 0, MTLOriginMake(0, 0, 0));
    g_bcrt[g_nbcrt++] = (bcrt_t){base, sh, buf, lv, sl, z, w, h, bw, bh, bpb};
    a.texture = sh; a.level = 0; a.slice = 0; a.depthPlane = 0;
    static int once[2]; if (!once[side == 1]++) MLOG("render target in a %s texture: view format %#x drawn into a %lux%lu-block shadow (Metal format %lu) and copied back after the pass (first case pass %llu)",
                                       side == 1 ? "depth" : "compressed", v->format, (unsigned long)bw, (unsigned long)bh, (unsigned long)spf, g_stat_pass + 1);
    return 1;
}
static MTLPixelFormat rt_view_fmt(id<MTLTexture> base, const view_t *v, int has_v) {
    MTLPixelFormat spf;
    if (bcrt_view(base, v, has_v, &spf)) return spf;
    if (base && mtl_fmt_block(base.pixelFormat)) {
        RED_ONCE(78, "render target is a compressed-format texture (Metal format %lu %lux%lu layers %lu, view format change %d → NVN %#x): not attached; draws to this attachment are lost", (unsigned long)base.pixelFormat, (unsigned long)base.width, (unsigned long)base.height,
                 (unsigned long)base.arrayLength, has_v ? v->set_format : 0, has_v ? v->format : 0);
        return MTLPixelFormatInvalid;
    }
    if (!base || !has_v || !v->set_format) return base.pixelFormat;
    uint32_t pf = 0; uint8_t swz[4];
    if (!nvn_fmt_to_mtl(v->format, &pf, swz) || !pf || pf == (uint32_t)base.pixelFormat) return base.pixelFormat;
    if (tex_need_pfv(base.pixelFormat, (MTLPixelFormat)pf) && !(base.usage & MTLTextureUsagePixelFormatView)) return base.pixelFormat;
    return (MTLPixelFormat)pf;
}
// The same texture level / layer attached at more than one colour slot: only the first is attached, later ones are not (their writes are dropped). The G-buffer passes of The Legend of Zelda: Breath of the Wild attach the albedo and another buffer twice each
// (c1 = c2, c3 = c4), and the channel write masks of c2 / c4 are always 0 (cmask 0x00f0f0ff). The Switch's ROP writes only the masked channels and the two slots do not interfere;
// on Apple's TBDR each attachment has its own tile memory and the whole tile is stored back at the end of the pass, so the copy stored later (loaded, never written) overwrites the writes of the earlier one -- the order differs per 32x32 tile,
// the albedo kept only scattered squares, and the whole 3D picture went black. A red line is printed when a dropped slot is used with a non-zero write mask (those writes are really lost)
static int rt_dup_slot(const p_set_rt *r, int i) {
    if (!r->color[i]) return 0;
    uint8_t lv = r->has_cview[i] && r->cview[i].set_levels ? r->cview[i].base_level : 0; uint16_t ly = r->has_cview[i] && r->cview[i].set_layers ? r->cview[i].min_layer : 0;
    for (int j = 0; j < i; j++) {
        if (!r->color[j] || r->color[j]->serial != r->color[i]->serial) continue;   // compare serials: records hold copies of the texture objects (tex_snap), so the pointers differ
        uint8_t lj = r->has_cview[j] && r->cview[j].set_levels ? r->cview[j].base_level : 0; uint16_t yj = r->has_cview[j] && r->cview[j].set_layers ? r->cview[j].min_layer : 0;
        if (lj == lv && yj == ly) return 1;
    }
    return 0;
}
static void attach(MTLRenderPassAttachmentDescriptor *a, const tex_t *t, const view_t *v, int has_v) {
    id<MTLTexture> base = tex_mtl(t);
    // A texture made without render-target use (packaged or CPU-written, uploaded from its storage) drawn into: give it that use, as a clear of one does (clear_tex). Metal's validation
    // stops at such an attachment; without it Tears of the Kingdom drew its terrain normal maps into textures made for reading only
    if (base && !(base.usage & MTLTextureUsageRenderTarget) && !mtl_fmt_block(base.pixelFormat) && base.sampleCount == 1) base = tex_upgrade(t, base, MTLTextureUsageRenderTarget, base.pixelFormat);
    a.texture = base;
    if (has_v && v->set_levels) a.level = v->base_level;
    if (has_v && v->set_layers) { if (t->d.target == 2) a.depthPlane = v->min_layer; else a.slice = v->min_layer; }
    if (base && mtl_fmt_block(base.pixelFormat)) { if (!bcrt_attach(a, base, v, has_v)) { rt_view_fmt(base, v, has_v); a.texture = nil; } return; }   // compressed formats: a shadow or nothing (see rt_view_fmt)
    if (base && mtl_fmt_depth(base.pixelFormat) && bcrt_attach(a, base, v, has_v)) return;   // a depth texture through a colour view: a shadow (see shrt_block)
    // A view that changes the format is attached as a Metal view of that format (pipeline formats follow the attachment). SMBW's last pass writes the RGBA8 swapchain texture through an RGBA8_SRGB view, so the hardware encodes sRGB on write;
    // attaching the base texture instead stored the linear values as they were and the whole picture came out dark (every value decoded from sRGB once more, on the Mac and the iPhone). The view lives until this submission completes
    if (base && has_v && v->set_format) {
        MTLPixelFormat pf = rt_view_fmt(base, v, has_v);
        if (pf != base.pixelFormat) {
            id<MTLTexture> vw = [base newTextureViewWithPixelFormat:pf];
            if (vw) { a.texture = vw; defer_obj_unref(vw); }
            else RED_ONCE(67, "cannot create the render target view: format %#x (base texture Metal format %lu)", v->format, (unsigned long)base.pixelFormat);
        } else { uint32_t want = 0; uint8_t swz[4];
            if (nvn_fmt_to_mtl(v->format, &want, swz) && want && want != (uint32_t)base.pixelFormat)
                RED_ONCE(68, "render target view format %#x cannot be attached (base texture Metal format %lu was created without the format-view usage): drawing in the base format", v->format, (unsigned long)base.pixelFormat); }
    }
}
// mode 1 (clear_only): a pass opened only to land pending clears, attaching only the attachments that have clears; mode 2: the lookahead proves this pass does not use depth-stencil, so depth is not attached.
// clear_only: a pass opened only to land pending clears, attaching only the attachments that have clears - the other attachments are not touched by this pass, and attaching them costs a pointless load and store on TBDR
// (Mac character select screen: the color-clear pass drags along a 400x225 depth load and store)
static id<MTL4RenderCommandEncoder> rp_begin_ex(int clear_only);
static id<MTL4RenderCommandEncoder> rp_begin(void) { return rp_begin_ex(0); }
static id<MTL4RenderCommandEncoder> rp_begin_ex_(int clear_only);
static id<MTL4RenderCommandEncoder> rp_begin_ex(int clear_only) { AP_B; id<MTL4RenderCommandEncoder> e = rp_begin_ex_(clear_only); AP_E(1); return e; }
static void bare_dims(NSUInteger *w, NSUInteger *h);
static id<MTL4RenderCommandEncoder> rp_begin_ex_(int clear_only) {
    if (g_enc) return g_enc;
    memset(g_zskip_cand_ok, 0, sizeof g_zskip_cand_ok);
    g_enc_mode = clear_only; g_enc_geo_nocolor = g_geo_omit_colors; g_pass_swrite = g_pass_order = g_pass_late_res = 0; g_sm_pass = 0;
    if (!R.has_rt) { RED_ONCE(3, "no render target set before the draw"); return nil; }
    if (g_imfit_npend) img_fit_upgrade();   // no pass is open here: the upgrade's copy may run (img_fit)
    // The version of the attachment-range candidate is taken before capturing any attachment; if it changes during setup, there is no continuation.
    uint64_t reg0 = 0, ser0 = 0, dirty0 = 0;
    if (rt_range_on()) { reg0 = g_reg_gen; ser0 = g_ser_gen; }
    if (rt_range_on()) dirty0=nvn_dirty_now();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    MTL4RenderPassDescriptor *pd = [MTL4RenderPassDescriptor new];
    int any = 0;
    for (int i = 0; i < R.rt.n; i++) {
        if (!R.rt.color[i] || (clear_only == 1 && !R.clear_c[i]) || (g_geo_omit_colors && !R.clear_c[i])) continue;
        if (rt_dup_slot(&R.rt, i)) continue;   // a slot attached twice is not attached (see rt_dup_slot)
        attach(pd.colorAttachments[i], R.rt.color[i], &R.rt.cview[i], R.rt.has_cview[i]);
        if (!pd.colorAttachments[i].texture) continue;
        any = 1;
        pd.colorAttachments[i].loadAction = R.clear_c[i] ? MTLLoadActionClear : MTLLoadActionLoad;
        pd.colorAttachments[i].clearColor = R.cc[i];
        if (R.clear_c[i]) PD("C %llu c%d cleared to %g,%g,%g,%g", g_stat_pass + 1, i, R.cc[i].red, R.cc[i].green, R.cc[i].blue, R.cc[i].alpha);
        if (bcrt_is(pd.colorAttachments[i].texture)) { pd.colorAttachments[i].storeAction = MTLStoreActionStore; continue; }   // a shadow (bcrt_attach): loaded and stored as is, outside the bookkeeping below
        int cov = tbdr_on() && !R.clear_c[i] && (g_fd_dw || g_fd_di) && fz_load_covers(i, pd.colorAttachments[i].texture, pd.colorAttachments[i].level);
        if (!R.clear_c[i] && (g_fd_dw || g_fd_di) && !cov) PD("Q %llu c%d load cannot be skipped why=%d", g_stat_pass + 1, i, g_fd_why);
        if (cov) {
            static int probe = -1; if (probe < 0) { probe = getenv("DM_NVN_LOADPROBE") ? atoi(getenv("DM_NVN_LOADPROBE")) : 0; if (probe) MLOG("DM_NVN_LOADPROBE: attachments whose load can be skipped are cleared to a sentinel color and read back to check"); }
            if (probe) { pd.colorAttachments[i].loadAction = MTLLoadActionClear; int pm = probe == 1 && pf_is_float(pd.colorAttachments[i].texture.pixelFormat) ? 3 : probe;   // Float formats use a NaN sentinel by default: a fixed sentinel value can be hit by real output (it has been hit in practice)
                pd.colorAttachments[i].clearColor = pm == 3 ? MTLClearColorMake(NAN, NAN, NAN, NAN) : probe == 2 ? MTLClearColorMake(0.777, 0.222, 0.555, 0.888) : MTLClearColorMake(0.123, 0.456, 0.789, 0.321);
                if (!g_lp) g_lp = [NSMutableArray array];
                [g_lp addObject:@[pd.colorAttachments[i].texture, @(pd.colorAttachments[i].level), @(pd.colorAttachments[i].slice), @(g_stat_pass + 1), @(i), @(R.rt.color[i]->serial), @(pm)]]; }
            else pd.colorAttachments[i].loadAction = MTLLoadActionDontCare;
            g_fd_n++;
        }
        xv_touch_add(R.rt.color[i]->serial, (uint32_t)pd.colorAttachments[i].level, (uint32_t)pd.colorAttachments[i].slice,
                     pd.colorAttachments[i].loadAction != MTLLoadActionLoad);
        if (g_xv_n) { xv_ent *xe = xv_find(R.rt.color[i]->serial, (uint32_t)pd.colorAttachments[i].level, (uint32_t)pd.colorAttachments[i].slice);
            if (xe && (xe->state == XV_INVALID || xe->state == XV_PROBE)) { if (pd.colorAttachments[i].loadAction == MTLLoadActionLoad) { xv_ent copy = *xe; xv_violation(&copy, "old content loaded as an attachment"); } else xv_good(xe); } }
        pd.colorAttachments[i].storeAction = MTLStoreActionStore;
        rt_mirror_add(R.rt.color[i], pd.colorAttachments[i].texture, pd.colorAttachments[i].level, pd.colorAttachments[i].slice, i);
    }
    if (g_sm_plan.on && !clear_only && pd.colorAttachments[0].texture && !R.rt.depth) {   // slice-merged pass (see g_sm_plan): the attachment holds every layer the draws reach
        MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[0];
        g_sm_cur = (int)c.slice; c.slice = (NSUInteger)g_sm_plan.lo; if (c.loadAction == MTLLoadActionDontCare) c.loadAction = MTLLoadActionLoad;   // the other layers' content stays
        pd.renderTargetArrayLength = (NSUInteger)(g_sm_plan.hi - g_sm_plan.lo + 1);
        g_sm_pass = 1; g_sm_lo = g_sm_plan.lo; g_sm_hi = g_sm_plan.hi; g_sm_applied = -1; g_sm_npass++;
        static int once; if (!once++) MLOG("slice-merged pass: draws into layers %d-%d of one texture level run as one layered pass (first case pass %llu; DM_NVN_SLICE_MERGE)", g_sm_lo, g_sm_hi, g_stat_pass + 1);
        PD("L %llu slice-merged pass: layers %d-%d, first draw into layer %d", g_stat_pass + 1, g_sm_lo, g_sm_hi, g_sm_cur);
    }
    // clear_only: 0 attach all 1 only for clears 2 neither depth nor stencil attached 3 stencil only 4 depth only (3 / 4: the pass_ds_need lookahead proves the whole pass uses only that one)
    if (R.rt.depth && clear_only == 3) {   // Stencil only: the depth plane of the combined format does not enter the tile and is neither read nor written (Metal allows an empty depth attachment with the combined texture on the stencil attachment)
        attach(pd.stencilAttachment, R.rt.depth, &R.rt.dview, R.rt.has_dview);
        MTLPixelFormat pf = pd.stencilAttachment.texture.pixelFormat;
        if (pf == MTLPixelFormatDepth32Float_Stencil8 || pf == MTLPixelFormatStencil8) {
            any = 1; pd.stencilAttachment.loadAction = R.clear_s ? MTLLoadActionClear : MTLLoadActionLoad;
            pd.stencilAttachment.clearStencil = R.cs; pd.stencilAttachment.storeAction = MTLStoreActionStore;
        } else pd.stencilAttachment.texture = nil;   // No stencil plane: the stencil test always passes, attach nothing
    } else if (R.rt.depth && clear_only != 2 && !(clear_only == 1 && !R.clear_d && !R.clear_s)) {
        attach(pd.depthAttachment, R.rt.depth, &R.rt.dview, R.rt.has_dview);
        if (pd.depthAttachment.texture) {
            any = 1;
            pd.depthAttachment.loadAction = R.clear_d ? MTLLoadActionClear : MTLLoadActionLoad;
            MTLPixelFormat pf = pd.depthAttachment.texture.pixelFormat;
            pd.depthAttachment.clearDepth = pf == MTLPixelFormatDepth16Unorm ? nvn_depth16_clear_value(R.cd) : R.cd; pd.depthAttachment.storeAction = MTLStoreActionStore;
            if (clear_only != 4 && (pf == MTLPixelFormatDepth32Float_Stencil8 || pf == MTLPixelFormatStencil8)) {   // 4: depth only
                pd.stencilAttachment.texture = pd.depthAttachment.texture; pd.stencilAttachment.level = pd.depthAttachment.level;
                pd.stencilAttachment.slice = pd.depthAttachment.slice;
                pd.stencilAttachment.loadAction = R.clear_s ? MTLLoadActionClear : MTLLoadActionLoad;
                pd.stencilAttachment.clearStencil = R.cs; pd.stencilAttachment.storeAction = MTLStoreActionStore;
            }
        }
    }
    // No target set at all (Tears of the Kingdom: an agl fragment shader that only stores to a storage buffer draws one triangle into a 1x1 viewport with zero render targets; the game copies the result
    // from the buffer afterwards): an attachment-less pass covering the viewport, else the scissor. A target that is set but cannot be attached still skips the pass below
    int bare = !any && clear_only != 1 && !R.rt.depth;
    for (int i = 0; i < R.rt.n && bare; i++) if (R.rt.color[i]) bare = 0;
    if (bare) {
        NSUInteger bw, bh; bare_dims(&bw, &bh);
        pd.renderTargetWidth = bw; pd.renderTargetHeight = bh; pd.defaultRasterSampleCount = 1;
        static int once; if (!once++) MLOG("attachment-less pass: the game draws with no render target set, pass opened at the viewport size %lux%lu (first case pass %llu)", (unsigned long)bw, (unsigned long)bh, g_stat_pass + 1);
        PD("P %llu attachment-less %lux%lu", g_stat_pass + 1, (unsigned long)bw, (unsigned long)bh);
        any = 1;
    }
    if (!any) {
        RED_ONCE(4, "no render target could get an MTLTexture");
        if (passdump()) {   // PASSDUMP: when a whole pass is skipped, list its targets (serial, format, compressed or not)
            char b[256]; size_t o = 0;
            for (int i = 0; i < R.rt.n && i < 8; i++) if (R.rt.color[i]) o += snprintf(b + o, sizeof b - o, " c%d=%llu/f%#x%s", i, (unsigned long long)R.rt.color[i]->serial, R.rt.color[i]->d.format, fmt_compressed(R.rt.color[i]->d.format) ? "(compr)" : "");
            if (R.rt.depth) o += snprintf(b + o, sizeof b - o, " d=%llu", (unsigned long long)R.rt.depth->serial);
            PD("N %llu pass skipped (no attachment could be attached) mode=%d%s", g_stat_pass + 1, clear_only, b);
        }
        return nil;
    }
    for (int i = 0; i < R.rt.n && i < 8; i++) if (R.rt.color[i] && zskip_zero_clear(i) && !bcrt_is(pd.colorAttachments[i].texture)) {
        g_zskip_cand_ok[i] = 1; g_zskip_cand[i] = (zskip_zero){R.rt.color[i]->serial, (uint16_t)(R.rt.has_cview[i] && R.rt.cview[i].set_levels ? R.rt.cview[i].base_level : 0), (uint16_t)(R.rt.has_cview[i] && R.rt.cview[i].set_layers ? R.rt.cview[i].min_layer : 0)};
    }
    // A pass covers only the smallest of its attachments, and so does a clear folded into its load action, while NVN clears the whole target. Bayonetta 2 (SDK 3.6) starts every frame with a
    // 32x32 or 256x256 colour target beside its 1280x720 depth buffer and clears the depth there: only that corner was cleared, the scene behind failed the depth test against the
    // previous frames' depth everywhere else, and its story narration showed in the top-left corner of a black screen. A cleared attachment larger than the pass is cleared in a pass of its own first
    {   NSUInteger pw, ph; pass_dims(pd, (NSUInteger)R.rt.n, &pw, &ph);
        for (NSUInteger i = 0; i < (NSUInteger)R.rt.n + 1; i++) {
            MTLRenderPassAttachmentDescriptor *a = i < (NSUInteger)R.rt.n ? pd.colorAttachments[i] : pd.depthAttachment.texture ? pd.depthAttachment : pd.stencilAttachment;
            int ds = i == (NSUInteger)R.rt.n, cd = ds && pd.depthAttachment.texture && pd.depthAttachment.loadAction == MTLLoadActionClear, cs = ds && pd.stencilAttachment.texture && pd.stencilAttachment.loadAction == MTLLoadActionClear;
            if (!a.texture || (ds ? !cd && !cs : a.loadAction != MTLLoadActionClear) || (mip_extent(a.texture.width, a.level) <= pw && mip_extent(a.texture.height, a.level) <= ph)) continue;
            MTL4RenderPassDescriptor *cp = [MTL4RenderPassDescriptor new];
            if (!ds) { MTLRenderPassColorAttachmentDescriptor *c = cp.colorAttachments[0], *o = pd.colorAttachments[i];
                c.texture = o.texture; c.level = o.level; c.slice = o.slice; c.depthPlane = o.depthPlane; c.clearColor = o.clearColor; c.loadAction = MTLLoadActionClear; c.storeAction = MTLStoreActionStore;
                o.loadAction = MTLLoadActionLoad; }
            else {
                if (cd) { MTLRenderPassDepthAttachmentDescriptor *d = cp.depthAttachment, *o = pd.depthAttachment;
                    d.texture = o.texture; d.level = o.level; d.slice = o.slice; d.clearDepth = o.clearDepth; d.loadAction = MTLLoadActionClear; d.storeAction = MTLStoreActionStore; o.loadAction = MTLLoadActionLoad; }
                if (cs) { MTLRenderPassStencilAttachmentDescriptor *s = cp.stencilAttachment, *o = pd.stencilAttachment;
                    s.texture = o.texture; s.level = o.level; s.slice = o.slice; s.clearStencil = o.clearStencil; s.loadAction = MTLLoadActionClear; s.storeAction = MTLStoreActionStore; o.loadAction = MTLLoadActionLoad; }
            }
            PD("C %llu %s%lu cleared in a pass of its own (%lux%lu, the pass covers %lux%lu)", g_stat_pass + 1, ds ? "d" : "c", (unsigned long)i, (unsigned long)mip_extent(a.texture.width, a.level),
               (unsigned long)mip_extent(a.texture.height, a.level), (unsigned long)pw, (unsigned long)ph);
            ser_gate(); enc_r_end(enc_r_begin(cp));
        }
    }
    // Bandwidth ledger: attachments with Load count as load, all attachments count as store (currently always Store)
    int nload = 0, natt = 0;
    for (int i = 0; i < R.rt.n; i++) {
        MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[i];
        if (!c.texture) continue;
        uint64_t b = att_bytes(c.texture, c.level); natt++; g_bw_store += b; if (c.loadAction == MTLLoadActionLoad) { g_bw_load += b; nload++; }
        if (R.rt.has_cview[i] && R.rt.cview[i].set_format) PD("V %llu c%d view format %#x (base texture NVN format %#x)", g_stat_pass + 1, i, R.rt.cview[i].format, R.rt.color[i]->d.format);
        PD("P %llu c%d ser=%llu fmt=%lu %lux%lu ms%lu lv%lu sl%lu %s mtl=%p", g_stat_pass + 1, i, R.rt.color[i]->serial, (unsigned long)c.texture.pixelFormat,
           (unsigned long)c.texture.width, (unsigned long)c.texture.height, (unsigned long)c.texture.sampleCount, (unsigned long)c.level, (unsigned long)c.slice,
           c.loadAction == MTLLoadActionClear ? "clear" : c.loadAction == MTLLoadActionDontCare ? "dontcare" : "load", (__bridge void *)(c.texture.parentTexture ? c.texture.parentTexture : c.texture));
    }
    {   // Depth and stencil planes are counted separately (combined format 5 bytes = depth 4 + stencil 1; when only one is attached only that one is counted)
        id<MTLTexture> dt = pd.depthAttachment.texture, stt = pd.stencilAttachment.texture;
        uint64_t whole = att_bytes(dt ? dt : stt, dt ? pd.depthAttachment.level : pd.stencilAttachment.level);
        int comb = (dt ? dt : stt).pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
        uint64_t db = dt ? (comb ? whole / 5 * 4 : whole) : 0, sb = stt && stt != dt ? (comb ? whole / 5 : whole) : stt ? whole - db : 0;
        if (dt) { natt++; g_bw_store += db; if (pd.depthAttachment.loadAction == MTLLoadActionLoad) { g_bw_load += db; nload++; } }
        if (stt) { natt += !dt; g_bw_store += sb; if (pd.stencilAttachment.loadAction == MTLLoadActionLoad) { g_bw_load += sb; nload += !dt; } }
        if (dt) PD("P %llu d ser=%llu fmt=%lu %lux%lu %s", g_stat_pass + 1, R.rt.depth->serial, (unsigned long)dt.pixelFormat,
                   (unsigned long)dt.width, (unsigned long)dt.height, pd.depthAttachment.loadAction == MTLLoadActionClear ? "clear" : "load");
        else if (stt) PD("P %llu s ser=%llu fmt=%lu %lux%lu %s (stencil only)", g_stat_pass + 1, R.rt.depth->serial, (unsigned long)stt.pixelFormat,
                   (unsigned long)stt.width, (unsigned long)stt.height, pd.stencilAttachment.loadAction == MTLLoadActionClear ? "clear" : "load");
    }
    if (natt && nload == natt) g_pass_full_rw++;
    g_nsd = 0;
    {
        for (int i = 0; i < R.rt.n && i < 8; i++) { MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[i]; if (!c.texture || bcrt_is(c.texture)) continue;
            g_sd[g_nsd] = (sd_att){0}; g_sd[g_nsd].ser = R.rt.color[i]->serial; g_sd[g_nsd].bytes = att_bytes(c.texture, c.level); g_sd[g_nsd].lv = (uint32_t)c.level; g_sd[g_nsd].sl = (uint32_t)(c.texture.textureType == MTLTextureType3D ? c.depthPlane : c.slice); g_sd[g_nsd].idx = i; g_sd[g_nsd].load = c.loadAction == MTLLoadActionLoad; g_sd[g_nsd].wrote = R.clear_c[i] != 0; g_nsd++;
            c.storeAction = MTLStoreActionUnknown; }
        id<MTLTexture> dt = pd.depthAttachment.texture, stt = pd.stencilAttachment.texture;
        uint64_t db = att_bytes(dt, pd.depthAttachment.level), sb = 0;
        if (stt) { int comb = stt.pixelFormat == MTLPixelFormatDepth32Float_Stencil8; sb = att_bytes(stt, pd.stencilAttachment.level) / (comb ? 5 : 1); if (stt == dt) db -= sb; }   // Combined format 5 bytes = depth 4 + stencil 1 (when only stencil is attached, only 1 is counted)
        if (dt) { g_sd[g_nsd] = (sd_att){0}; g_sd[g_nsd].ser = R.rt.depth->serial; g_sd[g_nsd].bytes = db; g_sd[g_nsd].lv = (uint32_t)pd.depthAttachment.level; g_sd[g_nsd].sl = (uint32_t)pd.depthAttachment.slice; g_sd[g_nsd].idx = -1; g_sd[g_nsd].depth = 1; g_sd[g_nsd].load = pd.depthAttachment.loadAction == MTLLoadActionLoad; g_sd[g_nsd].wrote = R.clear_d != 0; g_nsd++;
                  pd.depthAttachment.storeAction = MTLStoreActionUnknown; }
        if (stt) { g_sd[g_nsd] = (sd_att){0}; g_sd[g_nsd].ser = R.rt.depth->serial; g_sd[g_nsd].bytes = sb; g_sd[g_nsd].lv = (uint32_t)pd.stencilAttachment.level; g_sd[g_nsd].sl = (uint32_t)pd.stencilAttachment.slice; g_sd[g_nsd].idx = -1; g_sd[g_nsd].depth = 2; g_sd[g_nsd].load = pd.stencilAttachment.loadAction == MTLLoadActionLoad; g_sd[g_nsd].wrote = R.clear_s != 0; g_nsd++;
                   pd.stencilAttachment.storeAction = MTLStoreActionUnknown; }
    }
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
    {   // Pass info (same convention as the former per-draw fetch: colors take each attachment's base texture format; sample count takes the last color attachment, or depth if there is depth)
        uint64_t ser = g_pass.serial + 1; memset(&g_pass, 0, sizeof g_pass); g_pass.serial = ser;
        for (int i = 0; i < R.rt.n && i < 8; i++) { id<MTLTexture> t = R.rt.color[i] ? pd.colorAttachments[i].texture : nil;
            if (t) { g_pass.cfmt[i] = (uint32_t)t.pixelFormat; g_pass.samples = (uint32_t)t.sampleCount; } }
        id<MTLTexture> dt = R.rt.depth ? pd.depthAttachment.texture : nil, stt = R.rt.depth ? pd.stencilAttachment.texture : nil;   // Depth / stencil attachments are fetched separately (possibly only one of them is attached)
        g_pass.hd = dt && dt.pixelFormat != MTLPixelFormatStencil8; g_pass.hs = stt != nil;
        g_pass.dfmt = g_pass.hd ? (uint32_t)dt.pixelFormat : 0; g_pass.sfmt = stt ? (uint32_t)stt.pixelFormat : 0;
        if (dt || stt) g_pass.samples = (uint32_t)(dt ? dt : stt).sampleCount;
        pass_dims(pd, (NSUInteger)R.rt.n, &g_pass.tw, &g_pass.th);
        if (rt_range_on()) { g_pass.reg_gen = reg0; g_pass.ser_gen = ser0; }
        if (rt_range_on()) { g_pass.range_dirty=dirty0; g_pass.range_color=pd.colorAttachments[0].texture; g_pass.range_depth=pd.depthAttachment.texture; }
        g_enc_pso = nil; g_enc_dss = nil; g_ed.ok = 0;
    }
    if (g_vis_on) {
        // Slots used up (one submission exceeds VIS_SLOTS counting passes): continue in a new overflow buffer. A ring buffer must not be retired (it is still used in rotation, and retiring removes it from the residency set); only a previous overflow buffer is
        if (!g_vis || g_vis_n >= VIS_SLOTS) { if (g_vis && !vis_in_ring(g_vis)) defer_obj_release(g_vis); g_vis = [g_dev newBufferWithLength:VIS_SLOTS * 8 options:MTLResourceStorageModeShared]; res_add(g_vis); g_vis_n = 0; }
        pd.visibilityResultBuffer = g_vis;
    }
    g_enc_vis = 0; g_enc_has_vis = g_vis_on;
    // Layered rendering: a vertex shader that writes the layer selects the attachment layer per primitive only when the pass has renderTargetArrayLength; without it every layer went to the
    // attached one. Pokémon Legends: Z-A builds its 32x32x32 colour grading LUT this way (b29f08619f379a83, one instance per depth plane) and tone-maps through it: only plane 0 was written and the
    // whole picture came out black. The pass gets every layer the attachments have from their attached one on (3D: depth planes); the pass is split when the next draw's shader differs (draw)
    g_pass.layered = g_rp_layered;   // what the pass was opened for, even when the attachments have one layer (then the draws need no split)
    if (g_rp_layered) {
        NSUInteger nl = NSUIntegerMax;
        for (int i = 0; i < 8; i++) { MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[i]; id<MTLTexture> t = c.texture; if (!t) continue;
            NSUInteger have = t.textureType == MTLTextureType3D ? MAX(t.depth >> c.level, 1u) - c.depthPlane : t.arrayLength * (t.textureType == MTLTextureTypeCube || t.textureType == MTLTextureTypeCubeArray ? 6 : 1) - c.slice;
            if (have < nl) nl = have; }
        if (pd.depthAttachment.texture) { id<MTLTexture> t = pd.depthAttachment.texture; NSUInteger have = t.arrayLength * (t.textureType == MTLTextureTypeCube || t.textureType == MTLTextureTypeCubeArray ? 6 : 1) - pd.depthAttachment.slice; if (have < nl) nl = have; }
        if (nl != NSUIntegerMax && nl > 1) { pd.renderTargetArrayLength = nl; PD("L %llu layered pass: %lu layers", g_stat_pass + 1, (unsigned long)nl); }
    }
    if (g_nrtm) rt_mirror_layers(pd);
    pg_render(pd, (NSUInteger)R.rt.n);
    { void td_note(id pd, NSUInteger nc); td_note(pd, (NSUInteger)R.rt.n);
      void ts_note(id pd, NSUInteger nc); ts_note(pd, (NSUInteger)R.rt.n);
      void ms_note(id pd, NSUInteger nc); ms_note(pd, (NSUInteger)R.rt.n); }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }   // the copy encoder the shadows' blocks were moved in with (bcrt_attach)
    ser_gate();
    g_enc = enc_r_begin_pass(pd);
    args_dirty_all();
    g_stat_pass++;
    if (enc_label_on()) { id<MTLTexture> lt = pd.colorAttachments[0].texture ?: pd.depthAttachment.texture ?: pd.stencilAttachment.texture; g_el_w = (int)lt.width; g_el_h = (int)lt.height; g_el_draws = 0; g_el_key[0] = 0; }
    return g_enc;
}
// Empty-scissor precheck before opening a pass: the size of the selected level of the first valid target is an upper bound on the minimum bounds of the actual pass.
static void rt_dims(NSUInteger *tw, NSUInteger *th) {
    id<MTLTexture> t0 = nil; NSUInteger level = 0;
    for (int i = 0; i < R.rt.n && i < 8 && !t0; i++) if (R.rt.color[i]) {
        t0 = tex_mtl(R.rt.color[i]); level = R.rt.has_cview[i] && R.rt.cview[i].set_levels ? R.rt.cview[i].base_level : 0;
    }
    if (!t0 && R.rt.depth) { t0 = tex_mtl(R.rt.depth); level = R.rt.has_dview && R.rt.dview.set_levels ? R.rt.dview.base_level : 0; }
    *tw = t0 ? mip_extent(t0.width, level) : 16384; *th = t0 ? mip_extent(t0.height, level) : 16384;   // no targets: an attachment-less pass can be up to 16384 (bare_dims)
}
// There are pending clears but no draw in the pass: open a pass once to land the clears
static void rp_flush(void) {
    int pend = R.clear_d || R.clear_s;
    for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
    if (pend && !g_enc && g_cb) rp_begin_ex(1);
    rp_end();
}
static int scissor_full(const tex_t *t, const view_t *v, int has_v) {
    NSUInteger level = has_v && v->set_levels ? v->base_level : 0;
    NSUInteger w = mip_extent(t->d.width, level), h = mip_extent(t->d.target == 0 || t->d.target == 3 ? 1 : t->d.height, level);
    return !R.has_sc || (R.sc.x <= 0 && R.sc.y <= 0 && (int64_t)R.sc.x + R.sc.w >= (int64_t)w && (int64_t)R.sc.y + R.sc.h >= (int64_t)h);
}
// Scissored zero clear (see OP_CLEAR_COLOR): land pending clears first, then copy from the zero buffer into the scissor rectangle of the attachment's level / slice in the replay stream's copy encoder. Multisample, 3D, depth / compressed formats are not handled (returns 0 and takes the old path)
static int clear_rect_zero(int i) {
    const tex_t *t = R.rt.color[i]; id<MTLTexture> mt = tex_mtl(t); if (!mt) return 0;
    MTLTextureType tt = mt.textureType;
    if (tt == MTLTextureType2DMultisample || tt == MTLTextureType2DMultisampleArray || tt == MTLTextureType3D || tt == MTLTextureTypeTextureBuffer) return 0;
    size_t bpp = mtl_pf_bytes(mt.pixelFormat); if (!bpp) return 0;
    const view_t *v = &R.rt.cview[i]; int hv = R.rt.has_cview[i];
    NSUInteger lv = hv && v->set_levels ? v->base_level : 0, sl = hv && v->set_layers ? v->min_layer : 0;
    long lw = (long)MAX(mt.width >> lv, 1u), lh = (long)MAX(mt.height >> lv, 1u);
    long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
    if (x1 > lw) x1 = lw; if (y1 > lh) y1 = lh;
    if (x1 <= x0 || y1 <= y0) return 1;
    size_t row = (size_t)(x1 - x0) * bpp, img = row * (size_t)(y1 - y0);
    id<MTLBuffer> zb = zero_buf(img); if (!zb) return 0;
    rp_flush();   // a pending full clear on the same attachment must land first, or it would overwrite this one
    enc_b_copy_buffer_texture(blit(), zb, 0, row, img, MTLSizeMake((NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0), 1), mt, sl, lv, MTLOriginMake((NSUInteger)x0, (NSUInteger)y0, 0));
    static uint64_t n; if (!n++) MLOG("scissored clear: first case %ldx%ld @%ld,%ld (target %lux%lu level %lu)", x1 - x0, y1 - y0, x0, y0, (unsigned long)mt.width, (unsigned long)mt.height, (unsigned long)lv);
    return 1;
}


