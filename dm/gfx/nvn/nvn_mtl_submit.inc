// nvn_mtl.m, part: submit
// Submission and its helpers: conditional rendering, occlusion queries, pending-write tracking, the submission's begin, the barrier and GPU-profile counters, the encoder wrappers (enc_r_*, enc_c_*, enc_b_*) and the blit helpers.
// Included by nvn_mtl.m in its original order, as one translation unit: statics are shared with the other parts.
// ---------- Submission ----------
static void defer_reset(void); static void defer_fallback(void);
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; void *vis; uint32_t seg; } post_op;   // vis: the visibility slot table for kind 3; seg: the segment it was encoded in (sub_cut)
static uint32_t g_seg;   // Index of the segment being encoded in this submission (0 until the first sub_cut)
typedef struct seg_clk seg_clk; static seg_clk *g_sclk; static void seg_clk_put(seg_clk *c);   // Counter timestamps by segment (see gpu_ticks)
// ---------- Conditional rendering (profile render_cond) ----------
// SetRenderEnableConditional(mode, rec) enables rendering if the 64-bit counter of the report at rec equals (RENDER_IF_EQUAL) or differs from (RENDER_IF_NOT_EQUAL) the one at
// rec + 16, until the next SetRenderEnable(true). Tears of the Kingdom wraps nearly every ClearColor / ClearDepthStencil this way: rec is set to 1 from the CPU when the target
// needs a clear, each clear is followed by ReportCounter(TIMESTAMP_TOP, rec), and rec + 16 is never written. A timestamp report puts 0 in the counter field (the time goes in the
// second field), so after one clear the two compare equal and later conditional clears of that target are skipped. The host used to ignore the condition and write the time
// into both fields, so every clear ran: the first tile-lighting round, the half-resolution scene depth its light volumes test against and the light accumulation were cleared
// before the composite read them, and the 3D scene came out black.
// Metal has no predication, so the condition is evaluated here at replay, on the CPU, from the records in memory. A report replayed earlier in this submission is not written yet
// (that happens when the GPU completes): a timestamp one counts with its known counter 0; a samples-passed result (an occlusion query, written from the visibility buffers) is
// unknown, and a condition on it keeps drawing, as before (occlusion culling only saves work). Inside a block evaluated false, draws and clears are dropped
static int g_rc_skip;   // a render-enable block evaluated false is active
#define RC_PEND 256
static struct { uint64_t addr; int kind; } g_rc_pend[RC_PEND]; static int g_rc_npend;   // reports replayed in this submission: kind 1 = timestamp (counter 0), 2 = another counter (unknown)
static uint64_t g_rc_occ[4096];   // addresses ever written by a non-timestamp report (open addressing): conditions on them keep drawing
static void rc_occ_add(uint64_t a) { for (uint32_t i = (uint32_t)(a >> 4) & 4095, k = 0; k < 4096; k++, i = (i + 1) & 4095) { if (g_rc_occ[i] == a) return; if (!g_rc_occ[i]) { g_rc_occ[i] = a; return; } } }
static int rc_occ_has(uint64_t a) { for (uint32_t i = (uint32_t)(a >> 4) & 4095, k = 0; k < 4096; k++, i = (i + 1) & 4095) { if (g_rc_occ[i] == a) return 1; if (!g_rc_occ[i]) return 0; } return 0; }
static void rc_note(uint64_t a, int kind) {
    if (kind == 2) rc_occ_add(a);
    for (int i = 0; i < g_rc_npend; i++) if (g_rc_pend[i].addr == a) { g_rc_pend[i].kind = kind; return; }
    if (g_rc_npend < RC_PEND) { g_rc_pend[g_rc_npend].addr = a; g_rc_pend[g_rc_npend++].kind = kind; }
}
static int rc_value(uint64_t a, uint64_t *v) {   // 0 = unknown at encode time
    if (rc_occ_has(a)) return 0;
    for (int i = g_rc_npend - 1; i >= 0; i--) if (g_rc_pend[i].addr == a) { if (g_rc_pend[i].kind != 1) return 0; *v = 0; return 1; }
    *v = *(const volatile uint64_t *)a; return 1;
}
// ---------- Occlusion queries (NVN_COUNTER_TYPE_SAMPLES_PASSED) ----------
// NVN semantics: samples passed is a cumulative counter on the queue - ResetCounter zeroes it, every draw afterwards adds to it, and ReportCounter writes out the cumulative value at that moment (+ timestamp) when the GPU completes.
// Metal: every render pass attaches a visibility result buffer; draws during counting use counting mode, with a new slot per pass (no reliance on accumulation across passes); a reset empties the 'slot table since the reset',
// and a report hands the slot table together with its buffer (retained) to the completion callback to sum and write back; after a report a new slot is used, so later draws are not counted into the reported value.
// (Previously always wrote 0: occlusion query = 'fully occluded', campfires / light sources got culled - the character select screen calls ResetCounter 21 times per frame)
typedef struct { void *buf; uint32_t slot; } vis_slot;
typedef struct { int n; vis_slot s[]; } vis_list;
#define VIS_SLOTS 8192
typedef struct { __strong id<MTLBuffer> buf; uint64_t fence; } vis_buf;
// One visibility buffer per submission in flight, as many as command allocators: with 4, Age of Calamity (about twelve submissions a frame) waited in mtl_sub_begin for the previous frame's GPU work; its first battle went from 18.0 to 16.9 ms a frame on the Mac
enum { VIS_RING = ALLOC_MAX };
static vis_buf g_vis_ring[VIS_RING]; static int g_vis_ring_n, g_vis_ring_i = -1;
static id<MTLBuffer> g_vis; static uint32_t g_vis_n;   // This submission's visibility buffer and number of slots used
static int vis_in_ring(id<MTLBuffer> b) { for (int i = 0; i < g_vis_ring_n; i++) if (g_vis_ring[i].buf == b) return 1; return 0; }
static int g_vis_on, g_vis_counting, g_vis_cur = -1; static uint64_t g_vis_cur_pass;
static vis_slot *g_vs; static int g_nvs, g_vscap;      // Slots since the last reset
static uint64_t g_cnt_report[32], g_cnt_reset[32];       // Report / reset counts per counter type (diagnostics)
static void vis_clear(void) { for (int i = 0; i < g_nvs; i++) CFRelease(g_vs[i].buf); g_nvs = 0; g_vis_cur = -1; }
static vis_list *vis_snapshot(void) {
    vis_list *l = malloc(sizeof *l + g_nvs * sizeof(vis_slot)); l->n = g_nvs;
    for (int i = 0; i < g_nvs; i++) { l->s[i] = g_vs[i]; CFRetain(g_vs[i].buf); }
    return l;
}
// Target ranges of buffer writes (copies / inline uniform updates) already encoded in this submission but not yet executed on the GPU: reading these addresses on the CPU side at encode time yields old values (uv_proof avoids them when reading uniforms)
// Inline updates keep where their data sits in the update ring (g_pend_upd: index + 1 into g_upd), so a bindless handle they wrote can be read as the GPU will see it (pend_read).
// Monster Hunter Rise queues more than 256 writes in a submission (the old limit, after which every address counted as written). The writes are indexed by 4 KB page (a chain per hash
// bucket, newest first); a write over more than PEND_SPAN pages goes to a short list that every lookup scans
enum { PEND_MAX = 8192, PEND_HB = 2048, PEND_NODES = 16384, PEND_SPAN = 8, PEND_WIDE = 256 };
static uint64_t g_pend_lo[PEND_MAX], g_pend_hi[PEND_MAX]; static int g_npend, g_pend_over; static uint8_t g_pend_kind[PEND_MAX]; static uint16_t g_pend_upd[PEND_MAX];   // kind: 1 inline update, 2 copy, 3 fill
static int32_t g_pend_head[PEND_HB]; static uint32_t g_pend_hgen[PEND_HB], g_pend_gen = 1;   // a bucket's chain is valid when its generation is this submission's
static struct { int32_t e, next; } g_pend_node[PEND_NODES]; static int g_npnode;
static int32_t g_pend_wide[PEND_WIDE]; static int g_nwide;
typedef struct { uint64_t lo; size_t off; __unsafe_unretained id<MTLBuffer> buf; } upd_ent;   // the ring block lives until this submission completes (mtl_sub_update)
static upd_ent g_upd[PEND_MAX]; static int g_nupd;
// Other writes to guest memory in this submission, which the pend list does not hold: writable storage buffers bound to a shader, texture→buffer copies, counter reports (ubp_get).
// Kept by 4 KB page (Breath of the Wild's particle dispatches bind about ten storage buffers each, hundreds a frame): a set of pages tagged with the submission (g_pend_gen) and with what
// wrote them. A counter report does not count against this submission's shadows: the host writes reports on the CPU when the whole submission has completed, so no draw of it reads one
// (Fire Emblem Engage writes its query reports into the page of a uniform block); it counts for later submissions while this one runs (gw_commit)
enum { SBW_N = 16384, SBW_PAGES = 4096 };
static struct { uint64_t page; uint32_t gen; uint8_t src; } g_sbwp[SBW_N]; static int g_nsbw, g_sbw_over;   // src: SBW_* bits; pages in the set this submission; over: a write could not be kept (too large, set full, size not known)
static int sbw_slot(uint64_t pg, int add) {   // open addressing; −1 = not there (or no room to add)
    unsigned h = (unsigned)((pg * 0x9E3779B97F4A7C15ull) >> 50) & (SBW_N - 1);
    for (int i = 0; i < 64; i++, h = (h + 1) & (SBW_N - 1)) {
        if (g_sbwp[h].gen != g_pend_gen) { if (!add) return -1; g_sbwp[h].page = pg; g_sbwp[h].gen = g_pend_gen; g_sbwp[h].src = 0; g_nsbw++; return (int)h; }
        if (g_sbwp[h].page == pg) return (int)h; }
    return -1;
}
enum { SBW_GPU = 1, SBW_REPORT = 2 };
static uint32_t g_sbw_gen;   // bumped by every write noted that this submission's shadows must respect (a cached shadow made before it is checked again)
static void sbw_note(uint64_t lo, uint64_t n, int src) {
    if (src == SBW_GPU) g_sbw_gen++;
    if (!n || n > (uint64_t)SBW_PAGES << 12) { if (src == SBW_GPU) g_sbw_over = 1; return; }   // a report too large to keep cannot happen (16 bytes)
    for (uint64_t pg = lo >> 12; pg <= (lo + n - 1) >> 12; pg++) { int k = sbw_slot(pg, 1); if (k < 0) { g_sbw_over = 1; return; } g_sbwp[k].src |= (uint8_t)src; }
}
static int sbw_hits(uint64_t lo, uint64_t hi) { if (!g_nsbw) return 0; for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; pg++) { int k = sbw_slot(pg, 0); if (k >= 0 && (g_sbwp[k].src & SBW_GPU)) return 1; } return 0; }
static inline unsigned pend_bucket(uint64_t page) { return (unsigned)((page * 0x9E3779B97F4A7C15ull) >> 53) & (PEND_HB - 1); }
static void pend_reset(void) { g_npend = g_pend_over = g_nupd = g_npnode = g_nwide = g_nsbw = g_sbw_over = 0; if (!++g_pend_gen) { memset(g_pend_hgen, 0, sizeof g_pend_hgen); memset(g_sbwp, 0, sizeof g_sbwp); g_pend_gen = 1; } }
static void vc_gpu_write(uint64_t dst, uint64_t n);   // kept index ranges under the written range are not trusted again (ix_range)
static int pend_add(uint64_t dst, uint64_t n, int kind) {
    vc_gpu_write(dst, n);
    if (g_pend_over || g_npend == PEND_MAX) { g_pend_over = 1; return -1; }
    int e = g_npend; uint64_t p0 = dst >> 12, p1 = (dst + (n ? n : 1) - 1) >> 12;
    if (p1 - p0 < PEND_SPAN && g_npnode + (int)(p1 - p0 + 1) <= PEND_NODES)
        for (uint64_t pg = p0; pg <= p1; pg++) { unsigned b = pend_bucket(pg); int k = g_npnode++;
            g_pend_node[k].e = e; g_pend_node[k].next = g_pend_hgen[b] == g_pend_gen ? g_pend_head[b] : -1; g_pend_head[b] = k; g_pend_hgen[b] = g_pend_gen; }
    else if (g_nwide < PEND_WIDE) g_pend_wide[g_nwide++] = e;
    else { g_pend_over = 1; return -1; }
    g_pend_lo[e] = dst; g_pend_hi[e] = dst + n; g_pend_kind[e] = (uint8_t)kind; g_pend_upd[e] = 0; return g_npend++;
}
// The newest queued write overlapping [a, a+n): its index, -1 for none, -2 when it cannot be told (overflow)
static int pend_last(uint64_t a, uint64_t n) {
    if (g_pend_over) return -2;
    int best = -1; uint64_t p0 = a >> 12, p1 = (a + (n ? n : 1) - 1) >> 12;
    for (int i = 0; i < g_nwide; i++) { int e = g_pend_wide[i]; if (e > best && a < g_pend_hi[e] && a + n > g_pend_lo[e]) best = e; }
    if (p1 - p0 >= PEND_SPAN) { for (int e = g_npend - 1; e > best; e--) if (a < g_pend_hi[e] && a + n > g_pend_lo[e]) return e; return best; }
    for (uint64_t pg = p0; pg <= p1; pg++) { unsigned b = pend_bucket(pg); if (g_pend_hgen[b] != g_pend_gen) continue;
        for (int k = g_pend_head[b]; k >= 0 && g_pend_node[k].e > best; k = g_pend_node[k].next) { int e = g_pend_node[k].e; if (a < g_pend_hi[e] && a + n > g_pend_lo[e]) { best = e; break; } } }
    return best;
}
static int pend_hits(uint64_t a, uint64_t n) { return pend_last(a, n) != -1; }
// The n bytes at a as the last write queued in this submission leaves them, when that write is an inline update covering them all → 1; otherwise → 0 and *kind = what wrote them (0 = too many writes to tell)
static int pend_read(uint64_t a, uint64_t n, void *out, int *kind) {
    int e = pend_last(a, n); *kind = e >= 0 ? g_pend_kind[e] : 0;
    if (e < 0 || !g_pend_upd[e] || a < g_pend_lo[e] || a + n > g_pend_hi[e]) return 0;
    const upd_ent *u = &g_upd[g_pend_upd[e] - 1]; memcpy(out, (const uint8_t *)u->buf.contents + u->off + (a - u->lo), n); return 1;
}
typedef struct { uint64_t ser; uint16_t level, slice; } zskip_zero;
static zskip_zero g_zskip_zero[4096]; static int g_zskip_n, g_zskip_count;
static zskip_zero g_zskip_cand[8]; static uint8_t g_zskip_cand_ok[8];
static void zskip_clear(void);
static int tbdr_on(void);
static id g_cb;
static id<MTL4ComputeCommandEncoder> g_blit;
static NSMutableArray<dispatch_block_t> *g_commit_blocks;
static uint64_t g_alloc_fence[ALLOC_MAX]; static uint64_t g_alloc_waits, g_alloc_grown;
int mtl_wait(uint64_t fence, uint64_t timeout_ns);
static void up_flush(void);
// The upload batch belongs only to the submit thread (the thread that called mtl_sub_begin): the batch's encoder cannot be used across threads. Every entry point asserts current thread == submit thread,
// and on mismatch prints the stack and aborts on the spot - so that 'another thread touched the batch' is exposed deterministically on Mac too, not by timing (real iOS device stuck at the main menu: the present thread flushed the submit thread's batch via q_cb).
static pthread_t g_sub_thread; static _Atomic int g_sub_thread_set;
static int on_sub_thread(void) { return atomic_load(&g_sub_thread_set) && pthread_equal(pthread_self(), g_sub_thread); }
static void up_owner_check(const char *where) {
    if (!atomic_load(&g_sub_thread_set) || on_sub_thread()) return;
    char name[64] = ""; pthread_getname_np(pthread_self(), name, sizeof name);
    MLOG("[RED] upload batches belong to the submit thread only: %s called on thread '%s'", where, name);
    void *bt[32]; int n = backtrace(bt, 32); backtrace_symbols_fd(bt, n, 2);
    abort();
}
// Every command buffer created outside the replay / upload batch goes through this. On the submit thread: flush this batch's uploads first, so that commands that then read or write these textures are ordered after the uploads;
// other threads (the present thread etc.) creating a command buffer do not touch the batch: they do not read or write textures uploaded in this batch; commands that need these textures all come after this replay submission (the batch is flushed before it)
// Old-queue command buffers (readback / diagnostics / present; to be deleted once M4-4 and M4-5 are migrated away): uploads are already on the Metal 4 queue, and there is no ordering guarantee between the two queues -
// wait for the upload to complete after flushing it, and only then can the old queue read the upload result (mtl_read_texels waited first and flushed afterwards, the readback beat the upload, 4 failures on t_nvn upload content)
static void up_wait_zero(_Atomic int *v);
static id<MTLCommandBuffer> q_cb(void) { if (on_sub_thread()) { up_flush(); rec_drain(); up_wait_zero(&g_up_pending); } return [g_q commandBuffer]; }
static post_op *g_post; static int g_npost, g_postcap;
// One autorelease pool per submission: submissions run on a guest thread (long-lived, no run loop, never drains the implicit pool), so objects autoreleased in the Metal replay
// (command buffers, encoders, pass descriptors, etc.) are never released unless put in a pool. Measured on the character select screen of a real iOS device: the host heap grows by about 47 objects per frame, 0.22 MiB/s.
void *objc_autoreleasePoolPush(void); void objc_autoreleasePoolPop(void *);
static void *g_sub_pool;
static int tbdr_on(void);

static uint64_t g_sub_serial;   // Submission index (used by the parallel precompile's 'predict only once per submission')
void mtl_sub_begin(void) {
    mtl_init(); g_sub_serial++; g_seg = 0; seg_clk_put(g_sclk); g_sclk = NULL; g_rc_npend = 0; g_rc_skip = 0;
    defer_drain();
    defer_reset();
    warm_claim();
    if (g_cb) MDIE("submit re-entered");
    g_sub_thread = pthread_self(); atomic_store(&g_sub_thread_set, 1);
    atomic_store(&g_last_commit_error, 0);
    g_sub_pool = objc_autoreleasePoolPush();
    g_cb = rec_cb_new();
    // Do not wait for the GPU mid-frame (hot real device: with a fixed rotation of 3, when the GPU cannot keep up every frame blocks here, and the submit thread runs at low frequency after waking, instruction count −18% but CPU time +49%):
    // pick an allocator the GPU has finished with; if all are in use create a new one; only at the cap wait for the oldest one. How far ahead it runs is bounded by the wait where the game acquires the swapchain texture, same as the old interface.
    int alloc_i = -1; uint64_t done = g_done;
    for (int i = 0; i < g_alloc_n; i++) if (g_alloc_fence[i] <= done) { alloc_i = i; break; }
    if (alloc_i < 0 && g_alloc_n < ALLOC_MAX) { NSError *ae = nil; MTL4CommandAllocatorDescriptor *ad = [MTL4CommandAllocatorDescriptor new];
        g_alloc4[g_alloc_n] = [g_dev newCommandAllocatorWithDescriptor:ad error:&ae]; if (!g_alloc4[g_alloc_n]) MDIE("failed to create a new MTL4CommandAllocator: %s", ae.localizedDescription.UTF8String);
        alloc_i = g_alloc_n++; g_alloc_grown++; }
    if (alloc_i < 0) { alloc_i = 0; for (int i = 1; i < g_alloc_n; i++) if (g_alloc_fence[i] < g_alloc_fence[alloc_i]) alloc_i = i; g_alloc_waits++; mtl_wait(g_alloc_fence[alloc_i], UINT64_MAX); }
    g_alloc_cur = alloc_i;
    [g_alloc4[alloc_i] reset];
    [g_cb beginCommandBufferWithAllocator:g_alloc4[alloc_i]];
    g_commit_blocks = [NSMutableArray array];
    // The overflow buffer created when the previous submission ran out of slots (see g_vis_n >= VIS_SLOTS): retire it. It used to be just cleared here and stayed in the residency set for good (from d2r-mobile 2eb575877)
    if (g_vis && !vis_in_ring(g_vis)) defer_obj_release(g_vis);
    g_npost = 0; g_vis = nil; g_vis_n = 0; g_vis_cur = -1; pend_reset();
    zskip_clear(); g_zskip_count = 0;
    int pick = -1;
    for (int i = 0; i < g_vis_ring_n; i++) if (g_vis_ring[i].fence <= g_done) { pick = i; break; }
    if (pick < 0 && g_vis_ring_n < VIS_RING) pick = g_vis_ring_n++;
    if (pick < 0) { uint64_t oldest = UINT64_MAX; for (int i = 0; i < VIS_RING; i++) if (g_vis_ring[i].fence < oldest) { oldest = g_vis_ring[i].fence; pick = i; } if (oldest > g_done) mtl_wait(oldest, UINT64_MAX); }
    g_vis_ring_i = pick; g_vis_ring[pick].fence = UINT64_MAX;
    if (!g_vis_ring[pick].buf) { g_vis_ring[pick].buf = [g_dev newBufferWithLength:VIS_SLOTS * 8 options:MTLResourceStorageModeShared]; res_add(g_vis_ring[pick].buf); }
    g_vis = g_vis_ring[pick].buf;
    void pg_sub_begin_ext(uint64_t fence); pg_sub_begin_ext(g_fence + 1);
    void arena_begin(void); arena_begin();
}

static void rp_end(void);
// DM_NVN_PASSGPU_SERIAL=1 (for measurement): before creating each encoder on the main command buffer, encode 'signal n+1 → wait for signal n+1' - the signal is only raised once all prior work has completed,
// so the following encoder starts only after the previous one has fully finished; on TBDR passes no longer overlap in the pipeline, and the per-pass start-to-end span of DM_NVN_PASSGPU is that pass's exclusive time (the whole-frame total is inflated; look only at relative cost)
static void ser_gate(void) {
    static int on = -1; static id<MTLEvent> ev; static uint64_t n;
    if (on < 0) { on = getenv("DM_NVN_PASSGPU_SERIAL") != NULL; if (on) { ev = [g_dev newEvent]; MLOG("DM_NVN_PASSGPU_SERIAL: passes serialized (for measurement; times read high)"); } }
    if (!on) return;
    (void)ev; (void)n;
}
// Metal 4 conservative barriers (first version): a consumer barrier at the start of an encoder (all prior work → this encoder) and a producer barrier at the end (this encoder and earlier → later encoders).
// Only the one at the start is not enough: t_rt_range 'pass 1 → pass 2 → copy' read back black, and after adding the end barrier it is 19/19. Old-queue encoders (upload / diagnostics / present) do not go through these barriers.
static int g_pass_swrite;   // the open draw pass has shader writes (swrite_note)
static void img_fit_upgrade(void); static int g_imfit_npend;   // textures an image slot needs a format view of (img_fit), upgraded before the next draw pass opens
static int g_last_end_relaxed, g_game_barrier;   // the last encoder ended was a draw pass with the relaxed end barrier (nothing else since); the game issued nvnCommandBufferBarrier since the last draw pass began
static uint64_t g_bar_skip, g_bar_full;            // draw passes begun without / with the consumer barrier (DM_NVN_BARRIER=game)
static void gp_end(id e);
static inline void m4_end(id e) {
    gp_end(e);
    if ([e conformsToProtocol:@protocol(MTL4CommandEncoder)]) [(id<MTL4CommandEncoder>)e barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [e endEncoding]; g_last_end_relaxed = 0;
}
// DM_NVN_BARRIER=game (default full): draw passes are ordered by the game's own barriers. A draw pass ends with a barrier from its vertex and fragment stages to the fragment, compute and copy stages of
// later work, so the next pass's vertex work can overlap this pass's fragments (with the full barriers every pass waited for the previous one to finish completely: about 350 passes a frame in Breath of
// the Wild). The next draw pass adds the full consumer barrier when the game called nvnCommandBufferBarrier since the last one (NVN asks for one before results of rendering are read as textures, shader data or
// indirect arguments), or when anything other than a draw pass ran in between (compute, copies and uploads keep their full barriers). Only vertex-stage reads of the previous pass's results without a barrier change order
static int barrier_game(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_BARRIER"); v = e && !strcmp(e, "game"); if (v) MLOG("DM_NVN_BARRIER=game: draw passes are ordered by the game's own barriers (fragment work still has barriers between passes; vertex work may overlap the previous pass's fragments)"); } return v; }
void mtl_barrier_stats(uint64_t *skip, uint64_t *full) { *skip = g_bar_skip; *full = g_bar_full; }
uint64_t mtl_cut_count(void);
// DM_NVN_GPUPROF=first frame[-last frame] (diagnostic): GPU time of every encoder of the main command buffers in those frames. A relaxed timestamp is written into a counter heap when an
// encoder begins (before its first draw or dispatch) and when it ends (after its fragment stage); nothing is serialized beyond the barriers the replay already sets, so the frame runs
// as usual. Printed as "[gpuprof]" lines (one per encoder, then the frame's sum by kind and size) once the frame's last command buffer completes
enum { GP_MAX = 4096 };   // the largest timestamp heap Metal makes
typedef struct { char kind; uint16_t w, h; uint8_t nc, depth, loads; uint32_t draws, seg; uint64_t pass; } gp_lab;   // seg: the command buffer (segment) of the submission it is in
static id<MTL4CounterHeap> g_gp_heap; static gp_lab g_gp_lab[GP_MAX / 2]; static int g_gp_n, g_gp_state, g_gp_cur = -1; static long g_gp_f0 = -2, g_gp_f1; static uint64_t g_gp_frame;
static uint64_t g_stat_pass, g_stat_draw; static _Atomic uint64_t g_cur_frame; static uint32_t g_seg; static uint32_t g_gp_copies, g_gp_disp, g_gp_ubd;   // copies / dispatches encoded (for the compute encoders' labels)
static int gp_active(void) {
    if (__builtin_expect(g_gp_f0 == -1, 1)) return 0;
    if (g_gp_f0 == -2) { const char *e = getenv("DM_NVN_GPUPROF"); g_gp_f0 = e ? atol(e) : -1; g_gp_f1 = e && strchr(e, '-') ? atol(strchr(e, '-') + 1) : g_gp_f0; if (g_gp_f0 < 0) return 0; }
    long f = (long)atomic_load(&g_cur_frame); return f >= g_gp_f0 && f <= g_gp_f1;
}
static void gp_begin(id e, char kind, MTL4RenderPassDescriptor *d) {   // right after an encoder of the main command buffer is made
    if (!gp_active() || g_gp_state == 2) return;
    if (!g_gp_heap) { MTL4CounterHeapDescriptor *hd = [MTL4CounterHeapDescriptor new]; hd.type = MTL4CounterHeapTypeTimestamp; hd.count = GP_MAX; NSError *er = nil;
        g_gp_heap = [g_dev newCounterHeapWithDescriptor:hd error:&er]; if (!g_gp_heap) { MLOG("[RED] GPUPROF: no counter heap: %s", er.description.UTF8String); g_gp_f0 = -1; return; } }
    if (!g_gp_state) { g_gp_state = 1; g_gp_n = 0; g_gp_frame = atomic_load(&g_cur_frame); [g_gp_heap invalidateCounterRange:NSMakeRange(0, GP_MAX)]; }
    if (2 * g_gp_n + 2 > GP_MAX) return;
    gp_lab l = {kind, 0, 0, 0, 0, 0, kind == 'R' ? (uint32_t)g_stat_draw : g_gp_copies, g_seg, g_stat_pass}; if (kind != 'R') { l.w = (uint16_t)g_gp_disp; l.h = (uint16_t)g_gp_ubd; }
    if (d) { for (int i = 0; i < 8; i++) { MTLRenderPassColorAttachmentDescriptor *c = d.colorAttachments[i]; if (!c.texture) continue;
            l.nc++; l.w = (uint16_t)MAX(c.texture.width >> c.level, 1); l.h = (uint16_t)MAX(c.texture.height >> c.level, 1); l.loads += c.loadAction == MTLLoadActionLoad; }
        MTLRenderPassDepthAttachmentDescriptor *z = d.depthAttachment;
        if (z.texture) { l.depth = 1; l.w = (uint16_t)MAX(z.texture.width >> z.level, 1); l.h = (uint16_t)MAX(z.texture.height >> z.level, 1); l.loads += z.loadAction == MTLLoadActionLoad; } }
    g_gp_lab[g_gp_n] = l; g_gp_cur = g_gp_n;
    if (kind == 'R') [(id<MTL4RenderCommandEncoder>)e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed afterStage:MTLRenderStageVertex intoHeap:g_gp_heap atIndex:2 * g_gp_n];
    else [(id<MTL4ComputeCommandEncoder>)e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed intoHeap:g_gp_heap atIndex:2 * g_gp_n];
    g_gp_n++;
}
static void gp_end(id e) {   // right before that encoder ends
    if (g_gp_cur < 0) return;
    int k = g_gp_cur; g_gp_cur = -1; if (g_gp_lab[k].kind == 'R') g_gp_lab[k].draws = (uint32_t)g_stat_draw - g_gp_lab[k].draws; else { g_gp_lab[k].draws = g_gp_copies - g_gp_lab[k].draws; g_gp_lab[k].w = (uint16_t)(g_gp_disp - g_gp_lab[k].w); g_gp_lab[k].h = (uint16_t)(g_gp_ubd - g_gp_lab[k].h); }
    if (g_gp_lab[k].kind == 'R') [(id<MTL4RenderCommandEncoder>)e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed afterStage:MTLRenderStageFragment intoHeap:g_gp_heap atIndex:2 * k + 1];
    else [(id<MTL4ComputeCommandEncoder>)e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed intoHeap:g_gp_heap atIndex:2 * k + 1];
}
static void gp_report(int n, gp_lab *lab, uint64_t frame) {   // the frame's last command buffer has completed
    NSData *r = [g_gp_heap resolveCounterRange:NSMakeRange(0, 2 * n)]; const uint64_t *t = r.bytes; if (!t) { free(lab); return; }
    double tick_ns = 1e9 / (double)[g_dev queryTimestampFrequency], tot[2] = {0}, span = 0; uint64_t t0 = 0, t1 = 0;
    for (int i = 0; i < n; i++) { uint64_t a = t[2 * i], b = t[2 * i + 1]; if (!a || !b || b < a) continue;
        double us = (b - a) * tick_ns / 1e3; const gp_lab *l = &lab[i]; tot[l->kind != 'R'] += us;
        if (!t0 || a < t0) t0 = a; if (b > t1) t1 = b;
        if (l->kind == 'R') MLOG("[gpuprof] F%llu %4d %8.1f us render pass %llu %ux%u colors %u depth %u loads %u draws %u start %+.1f seg %u", (unsigned long long)frame, i, us, (unsigned long long)l->pass, l->w, l->h, l->nc, l->depth, l->loads, l->draws, (a - t[0]) * tick_ns / 1e3, l->seg);
        else MLOG("[gpuprof] F%llu %4d %8.1f us compute / copy after pass %llu start %+.1f seg %u copies %u (uniform updates %u) dispatches %u", (unsigned long long)frame, i, us, (unsigned long long)l->pass, (a - t[0]) * tick_ns / 1e3, l->seg, l->draws, l->h, l->w); }
    span = (t1 - t0) * tick_ns / 1e3;
    MLOG("[gpuprof] frame %llu: render %.2f ms, compute / copy %.2f ms over %d encoders, first start to last end %.2f ms", (unsigned long long)frame, tot[0] / 1e3, tot[1] / 1e3, n, span / 1e3);
    free(lab);
}
static inline id<MTL4RenderCommandEncoder> enc_r_begin_pass(MTL4RenderPassDescriptor *d) {   // the draw passes (rp_begin_ex); see barrier_game
    id<MTL4RenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:d];
    if (g_last_end_relaxed && !g_game_barrier) g_bar_skip++;
    else { [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice]; g_bar_full++; }
    g_game_barrier = 0;
    [e setArgumentTable:g_vt atStages:MTLRenderStageVertex]; [e setArgumentTable:g_ft atStages:MTLRenderStageFragment]; gp_begin(e, 'R', d); return e;
}
static inline void enc_r_end_pass(id e) {
    if (!barrier_game()) { m4_end(e); return; }
    gp_end(e); [(id<MTL4CommandEncoder>)e barrierAfterStages:MTLStageVertex | MTLStageFragment beforeQueueStages:MTLStageFragment | MTLStageDispatch | MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
    [e endEncoding]; g_last_end_relaxed = 1;
}
static inline id<MTL4RenderCommandEncoder> enc_r_begin(MTL4RenderPassDescriptor *d) { id<MTL4RenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:d]; [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice]; [e setArgumentTable:g_vt atStages:MTLRenderStageVertex]; [e setArgumentTable:g_ft atStages:MTLRenderStageFragment]; gp_begin(e, 'R', d); return e; }
static inline void enc_r_end(id e) { m4_end(e); }
static inline void enc_r_pso(id<MTL4RenderCommandEncoder> e, id<MTLRenderPipelineState> p) { [e setRenderPipelineState:p]; }
static inline void enc_r_dss(id<MTL4RenderCommandEncoder> e, id<MTLDepthStencilState> s) { [e setDepthStencilState:s]; }
static inline void enc_r_viewport(id<MTL4RenderCommandEncoder> e, MTLViewport v) { [e setViewport:v]; }
static inline void enc_r_scissor(id<MTL4RenderCommandEncoder> e, MTLScissorRect r) { [e setScissorRect:r]; }
static inline void enc_r_cull(id<MTL4RenderCommandEncoder> e, MTLCullMode m) { [e setCullMode:m]; }
static inline void enc_r_winding(id<MTL4RenderCommandEncoder> e, MTLWinding w) { [e setFrontFacingWinding:w]; }
static inline void enc_r_fill(id<MTL4RenderCommandEncoder> e, MTLTriangleFillMode m) { [e setTriangleFillMode:m]; }
static inline void enc_r_depth_bias(id<MTL4RenderCommandEncoder> e, float u, float s, float c) { [e setDepthBias:u slopeScale:s clamp:c]; }
static inline void enc_r_depth_clip(id<MTL4RenderCommandEncoder> e, BOOL v) { [e setDepthClipMode:v ? MTLDepthClipModeClip : MTLDepthClipModeClamp]; }
static inline void enc_r_stencil_ref(id<MTL4RenderCommandEncoder> e, uint32_t f, uint32_t b) { (void)b; [e setStencilReferenceValue:f]; }
static inline void enc_r_blend_color(id<MTL4RenderCommandEncoder> e, float r, float g, float b, float a) { [e setBlendColorRed:r green:g blue:b alpha:a]; }
static inline void enc_r_visibility(id<MTL4RenderCommandEncoder> e, MTLVisibilityResultMode m, NSUInteger o) { [e setVisibilityResultMode:m offset:o]; }
// The table persists across encoders and submissions. Track every write to it, including converted streams,
// transform feedback and shader arguments; helper passes with their own tables do not change this state.
static inline void enc_r_vbuf(id<MTL4RenderCommandEncoder> e, NSUInteger i, uint64_t gpu) {
    (void)e; uint32_t bit = 1u << i;
    if (g_vbuf_cache && (g_vt_valid & bit) && g_vt_addr[i] == gpu) { g_vbuf_skip++; return; }
    if (rec_on()) rec_table_op(RO_T_ADDR, g_vt, gpu, i); else [g_vt setAddress:gpu atIndex:i];
    g_vt_addr[i] = gpu; g_vt_valid |= bit; g_vbuf_set++;
}
static inline void enc_r_args(id<MTL4RenderCommandEncoder> e, MTLRenderStages s, uint64_t gpu, NSUInteger i) {
    if (s == MTLRenderStageVertex) enc_r_vbuf(e, i, gpu); else if (rec_on()) rec_table_op(RO_T_ADDR, g_ft, gpu, i); else [g_ft setAddress:gpu atIndex:i];
}
static inline void enc_r_draw(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, NSUInteger v, NSUInteger n) { [e drawPrimitives:p vertexStart:v vertexCount:n]; }
static inline void enc_r_draw_instanced(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, NSUInteger v, NSUInteger n, NSUInteger i, NSUInteger b) { [e drawPrimitives:p vertexStart:v vertexCount:n instanceCount:i baseInstance:b]; }
static inline void enc_r_draw_indirect(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, uint64_t gpu) { [e drawPrimitives:p indirectBuffer:gpu]; }
static inline void enc_r_draw_indexed(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, NSUInteger n, MTLIndexType t, uint64_t gpu, NSUInteger len, NSUInteger i, NSInteger bv, NSUInteger bi) { [e drawIndexedPrimitives:p indexCount:n indexType:t indexBuffer:gpu indexBufferLength:len instanceCount:i baseVertex:bv baseInstance:bi]; }
static inline void enc_r_draw_indexed_indirect(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, MTLIndexType t, uint64_t gpu, NSUInteger len, uint64_t ind) { [e drawIndexedPrimitives:p indexType:t indexBuffer:gpu indexBufferLength:len indirectBuffer:ind]; }
static void ubd_land_compute(void);
static inline id<MTL4ComputeCommandEncoder> enc_c_begin(MTLComputePassDescriptor *d) { (void)d; ubd_land_compute(); id<MTL4ComputeCommandEncoder> e = [g_cb computeCommandEncoder]; [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice]; [e setArgumentTable:g_ct]; gp_begin(e, 'C', nil); return e; }
static inline id<MTLComputeCommandEncoder> enc_c_begin_concurrent(id<MTLCommandBuffer> cb) { return [cb computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent]; }
static inline void enc_c_end(id e) { m4_end(e); }
static inline void enc_c_pso(id e, id<MTLComputePipelineState> p) { [e setComputePipelineState:p]; }
static inline void enc_c_args(id e, uint64_t gpu, NSUInteger i) { (void)e; if (rec_on()) rec_table_op(RO_T_ADDR, g_ct, gpu, i); else [g_ct setAddress:gpu atIndex:i]; }
static inline void enc_c_dispatch(id e, MTLSize n, MTLSize t) { [e dispatchThreadgroups:n threadsPerThreadgroup:t]; }
static inline void enc_c_dispatch_indirect(id e, uint64_t ind, MTLSize t) { [(id<MTL4ComputeCommandEncoder>)e dispatchThreadgroupsWithIndirectBuffer:ind threadsPerThreadgroup:t]; }
static inline id enc_b_begin(id cb) { return [cb respondsToSelector:@selector(beginCommandBufferWithAllocator:)] ? [cb computeCommandEncoder] : [cb blitCommandEncoder]; }
static inline void enc_b_end(id e) { m4_end(e); }
static inline void enc_b_copy_buffer(id e, id<MTLBuffer> s, NSUInteger so, id<MTLBuffer> d, NSUInteger doff, NSUInteger n) { [e copyFromBuffer:s sourceOffset:so toBuffer:d destinationOffset:doff size:n]; }
static inline void enc_b_copy_buffer_texture(id e, id<MTLBuffer> s, NSUInteger so, NSUInteger row, NSUInteger img, MTLSize z, id<MTLTexture> d, NSUInteger sl, NSUInteger lv, MTLOrigin o) { [e copyFromBuffer:s sourceOffset:so sourceBytesPerRow:row sourceBytesPerImage:img sourceSize:z toTexture:d destinationSlice:sl destinationLevel:lv destinationOrigin:o]; }
static inline void enc_b_copy_texture_buffer(id e, id<MTLTexture> s, NSUInteger sl, NSUInteger lv, MTLOrigin o, MTLSize z, id<MTLBuffer> d, NSUInteger doff, NSUInteger row, NSUInteger img) { [e copyFromTexture:s sourceSlice:sl sourceLevel:lv sourceOrigin:o sourceSize:z toBuffer:d destinationOffset:doff destinationBytesPerRow:row destinationBytesPerImage:img]; }
static inline void enc_b_copy_texture(id e, id<MTLTexture> s, NSUInteger sl, NSUInteger lv, MTLOrigin so, MTLSize z, id<MTLTexture> d, NSUInteger dl, NSUInteger dv, MTLOrigin doo) { [e copyFromTexture:s sourceSlice:sl sourceLevel:lv sourceOrigin:so sourceSize:z toTexture:d destinationSlice:dl destinationLevel:dv destinationOrigin:doo]; }
static void blit_note(void);   // Per-pass dump: a copy / buffer update ended the render pass (defined after PD)
// Ordering inside the copy encoder: Metal 4 does not order copies / fills within one encoder (no automatic hazard tracking); an operation reading or writing a range an earlier one wrote needs an intra-encoder barrier.
// Without it, SMBW's two updates per frame to the same uniform slot ("write 4 bytes −1, then 16 bytes of coefficients") occasionally landed in reverse order; the fountain water wave simulation took −1 as its self coefficient, diverged, and the water vertices were pushed to the top of the screen.
// Buffer operations check conflicts by guest address range: a barrier when an operation reads a range written earlier, or writes a range read or written earlier; texture operations get
// one whenever the encoder already holds an operation. Reads were not tracked at first, so a copy writing bytes an earlier copy reads could run first (seen in Cuphead while its pool
// allocator gave out overlapping blocks, see hle_nvn.c zcull_size; NVN runs copies in order for any game)
enum { BW_MAX = 64 };
static uint64_t g_bw_lo[BW_MAX], g_bw_hi[BW_MAX], g_br_lo[BW_MAX], g_br_hi[BW_MAX]; static int g_bw_n, g_br_n, g_blit_ops; static uint64_t g_blit_barriers;
static uint64_t g_blit_splits;   // copies / buffer updates / fills that ended an open draw pass (the residency set line; a pass ended this way stores and the next one reloads its attachments)
uint64_t mtl_blit_splits(void) { return g_blit_splits; }
static int enc_open(void);
static int g_blit_ct;   // the open copy encoder has the compute argument table set (dispatches share it, dispatch_enc)
static id<MTL4ComputeCommandEncoder> blit_open(void) { if (!g_blit) { ser_gate(); g_blit = enc_b_begin(g_cb); [g_blit barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice]; gp_begin(g_blit, 'C', nil); g_bw_n = g_br_n = 0; g_blit_ops = 0; g_blit_ct = 0; } return g_blit; }
static void ubd_flush(void); static int g_nubd;   // the inline updates deferred (see mtl_sub_update)
static int g_ubd_landing;
static id<MTL4ComputeCommandEncoder> blit_raw(void) {
    blit_note(); if (enc_open()) g_blit_splits++; rp_end(); id<MTL4ComputeCommandEncoder> e = blit_open();
    if (g_nubd && !g_ubd_landing) { g_ubd_landing = 1; ubd_flush(); g_ubd_landing = 0; }   // deferred inline updates come before this copy or dispatch in command order
    return e;
}
// Land the deferred inline updates before other GPU work outside the copy encoder (helper compute passes, the end of the submission, a GPU wait)
static void ubd_land(void) { if (g_nubd && !g_ubd_landing) { rp_end(); g_ubd_landing = 1; ubd_flush(); g_ubd_landing = 0; } }
static void ubd_land_compute(void) { if (g_nubd && !g_ubd_landing) { ubd_land(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; } } }   // before a compute encoder of its own (no other encoder may be open then)
static void blit_barrier(void) {
    static int off = -1; if (off < 0) { const char *e = getenv("DM_NVN_BLIT_ORDER"); off = e && !strcmp(e, "0"); if (off) MLOG("DM_NVN_BLIT_ORDER=0: no barriers inside the copy encoder (diagnostic; brings back the fountain's vertical stripes)"); }
    // Only the stages a compute encoder has: MTLStageAll here fails Metal validation (macOS 27: "must be a valid combination of MTLStages for the current encoder type")
    if (!off) [g_blit barrierAfterEncoderStages:MTLStageDispatch | MTLStageBlit beforeEncoderStages:MTLStageDispatch | MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
    g_bw_n = g_br_n = 0; g_blit_ops = 0; g_blit_barriers++;
}
uint64_t mtl_blit_barriers(void) { return g_blit_barriers; }
static int bw_hit(uint64_t lo, uint64_t hi) { for (int i = 0; i < g_bw_n; i++) if (lo < g_bw_hi[i] && g_bw_lo[i] < hi) return 1; return 0; }
static int br_hit(uint64_t lo, uint64_t hi) { for (int i = 0; i < g_br_n; i++) if (lo < g_br_hi[i] && g_br_lo[i] < hi) return 1; return 0; }
// buffer → buffer / fill: reads [rlo, rhi) (0, 0 when nothing is read), writes [wlo, whi)
static uint64_t g_wr_epoch;   // Bumped by every buffer write outside a render pass (copy / update / fill, texture → buffer, dispatch): indirect-draw conversions made before it are stale (ixc_run)
static uint64_t g_mdx_epoch;  // Bumped by GPU writes that can change an indirect-count multi-draw's records or count (copy, fill, texture → buffer, dispatch; see mdx_run)
static id<MTL4ComputeCommandEncoder> blit_buf(uint64_t rlo, uint64_t rhi, uint64_t wlo, uint64_t whi) {
    g_wr_epoch++;
    id<MTL4ComputeCommandEncoder> e = blit_raw();
    if (g_blit_ops && (g_bw_n == BW_MAX || g_br_n == BW_MAX || (rhi > rlo && bw_hit(rlo, rhi)) || bw_hit(wlo, whi) || br_hit(wlo, whi))) blit_barrier();
    g_bw_lo[g_bw_n] = wlo; g_bw_hi[g_bw_n++] = whi; g_blit_ops++; g_gp_copies++;
    if (rhi > rlo) { g_br_lo[g_br_n] = rlo; g_br_hi[g_br_n++] = rhi; }
    return e;
}
// texture copies / readbacks: barrier if anything came before, and later operations must wait for it too
static id<MTL4ComputeCommandEncoder> blit(void) {
    id<MTL4ComputeCommandEncoder> e = blit_raw();
    if (g_blit_ops) blit_barrier();
    g_bw_lo[0] = 0; g_bw_hi[0] = ~0ull; g_bw_n = 1; g_br_n = 0; g_blit_ops = 1;   // record as "everything written": the next operation always gets a barrier
    return e;
}

void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    pend_add(dst, n, 2); g_mdx_epoch++;
    void *sb, *db; uint64_t so, dof;
    if (!mtl_addr(src, n, &sb, &so) || !mtl_addr(dst, n, &db, &dof))
        MDIE("buffer copy address is in no memory pool src=%#llx dst=%#llx n=%#llx", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n);
    if (src != dst && src < dst + n && dst < src + n) { static int once;   // Metal leaves an overlapping copy undefined; seen only in Cuphead while its pool allocator gave out overlapping blocks (hle_nvn.c zcull_size)
        if (!once++) MLOG("[RED] buffer copy with overlapping source and destination (src %#llx dst %#llx, %llu bytes): the result is undefined", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n); }
    enc_b_copy_buffer(blit_buf(src, src + n, dst, dst + n), (__bridge id<MTLBuffer>)sb, so, (__bridge id<MTLBuffer>)db, dof, n);
}

