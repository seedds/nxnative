// Diagnostic DM_NVN_GPUTIME=1 (included by nvn_mtl.m): the GPU time of every compute dispatch and every draw pass of the main command buffers, named by shader key, so a job that
// grows towards the GPU watchdog (4 s) is named long before it: PRAGMATA's software triangle rasterizer 26166d37ed56d6bb ran for seconds at the tower ring (PRAGMATA_PORT_NOTES.md 18:1x-18:3x)
// and took a dozen runs to find.
//   How: Metal 4 counter heaps (MTL4CounterHeapTypeTimestamp). Each guest dispatch gets a timestamp just before it and one just after it (MTL4ComputeCommandEncoder
//   writeTimestampWithGranularity:Precise, in the encoder it already shares with other dispatches and copies; no encoder split, no extra barrier). A stamp waits for the work before it
//   in the encoder, not the work after it: job time = (all earlier work done -> this dispatch done). A dispatch that overlaps the one before it is undercounted by the overlap; a
//   job that keeps the GPU for long is counted whole. Each draw pass gets a stamp when it begins (before its vertex stage) and after its fragment stage when it ends (render encoders
//   have no stamp between draws that a tile-based GPU can honour without ending the pass), attributed to the draws' shader keys. DM_NVN_GPUTIME_DRAWS=1 also stamps before and after
//   each guest draw (afterStage fragment, precise: Metal may split the pass for it, slow; only for finding the draw inside a slow pass).
//   The device's legacy counter-sampling support (MTLCounterSamplingPointAt{Dispatch,Draw,Stage,Blit}Boundary) is logged at start: Apple GPUs answer stage boundaries only, which is
//   why the Metal 4 per-command stamps are used. The first report says whether successive dispatches of one encoder got distinct stamps (dispatch granularity) or the same ones
//   (collapsed to encoder boundaries); t_gputime measures it on the machine.
//   Reports: "[gputime] frame F: ..." with the DM_NVN_GPUTIME_TOP (5) slowest jobs of every DM_NVN_GPUTIME_EVERY-th (60) frame and the slowest job since the last report; a job
//   of DM_NVN_GPUTIME_RED_MS (100) ms or more prints "[RED] slow GPU job: <key> <ms> ms (grid x y z)" (or "(draw pass ...)" / "(draw ...)"), at most DM_NVN_GPUTIME_RED_MAX (300)
//   lines. At the GPU watchdog the stuck submission's stamps are read as they are: the job begun and not ended is named with its grid (the precise frontier).
//   Cost: off, one cached int test per dispatch / draw / pass. On, two stamps a job, the records are written into preallocated heaps (GT_HEAPS heaps of GT_JOBS jobs, claimed by
//   a submission and released by its completion handler, which resolves them; no wait: with every heap busy a job is not timed and counted as such), an indirect dispatch also
//   copies its 12-byte group counts on the GPU (so the logged grid is the one that ran)
enum { GT_SLOTS = 4096, GT_JOBS = GT_SLOTS / 2, GT_HEAPS = 32, GT_SUBS = 256, GT_KEYS = 4, GT_TOPMAX = 32, GT_LAST = 64 };
typedef struct {
    const char *k[GT_KEYS]; char kk[GT_KEYS];   // shader keys (interned by the replay: they live as long as the process) and their stage letters (c v f m t)
    char kind;            // 'c' dispatch, 'R' draw pass, 'd' draw, 'm' mesh draw
    uint8_t nk, ind, valid;   // keys kept, indirect (grid read back from the GPU copy), CPU-side grid known
    uint16_t more;        // distinct keys past GT_KEYS
    uint32_t x, y, z;     // dispatch: groups (indirect: the copy at resolve time); pass: width, height; draw: count, instances; mesh: groups
    uint32_t draws;       // draws of a pass
    uint32_t enc;         // the encoder serial (the granularity check compares successive dispatches of one encoder)
    uint64_t frame, fence;
} gt_job;
typedef struct { id<MTL4CounterHeap> heap; id<MTLBuffer> grid; gt_job *job; int n, next; } gt_heap;
typedef struct { const char *key; char kind; double ms; uint64_t frame; uint32_t x, y, z; } gt_rec;   // a resolved job (the reports, the test's view)
static int g_gt = -1, g_gt_draws, g_gt_top = 5, g_gt_every = 60, g_gt_red_left = 300; static double g_gt_red_ms = 100; static MTL4TimestampGranularity g_gt_gran = MTL4TimestampGranularityPrecise;
static gt_heap g_gt_h[GT_HEAPS]; static _Atomic uint32_t g_gt_free;   // bit i: heap i is free
static double g_gt_tick_ns = 1; static NSUInteger g_gt_stride = 8;
static int g_gt_cur = -1, g_gt_first = -1, g_gt_pass = -1, g_gt_pass_h = -1; static const void *g_gt_pass_e; static uint32_t g_gt_enc; static const void *g_gt_enc_e;   // submit thread
static const char *g_gt_dk[2]; static char g_gt_dkk[2]; static uint32_t g_gt_dn, g_gt_di; static uint8_t g_gt_dind; static int g_gt_dpend;   // the guest draw about to be encoded (gt_draw_note; 2: a task / mesh draw)
static struct { _Atomic uint64_t fence; int first; } g_gt_sub[GT_SUBS];   // committed submissions not resolved yet (the watchdog reads them)
static _Atomic uint64_t g_gt_untimed, g_gt_jobs;
static pthread_mutex_t g_gt_mu = PTHREAD_MUTEX_INITIALIZER;   // the reports (completion handlers)
static gt_rec g_gt_last[GT_LAST]; static uint64_t g_gt_last_n;   // the newest resolved jobs (t_gputime)
static uint64_t g_gt_red_n;   // RED lines printed (t_gputime)
static int gt_init(void);
static inline int gt_on(void) { if (__builtin_expect(g_gt >= 0, 1)) return g_gt; return gt_init(); }
static int gt_init(void) {
    const char *e = getenv("DM_NVN_GPUTIME"); g_gt = e && *e && strcmp(e, "0");
    if (!g_gt) return 0;
    const char *v;
    if ((v = getenv("DM_NVN_GPUTIME_TOP")) && atoi(v) >= 0) g_gt_top = MIN(atoi(v), GT_TOPMAX);
    if ((v = getenv("DM_NVN_GPUTIME_EVERY")) && atoi(v) > 0) g_gt_every = atoi(v);
    if ((v = getenv("DM_NVN_GPUTIME_RED_MS")) && atof(v) > 0) g_gt_red_ms = atof(v);
    if ((v = getenv("DM_NVN_GPUTIME_RED_MAX")) && atoi(v) >= 0) g_gt_red_left = atoi(v);
    if ((v = getenv("DM_NVN_GPUTIME_DRAWS"))) g_gt_draws = *v && strcmp(v, "0");
    if ((v = getenv("DM_NVN_GPUTIME_GRAN")) && !strcmp(v, "relaxed")) g_gt_gran = MTL4TimestampGranularityRelaxed;
    if (!g_dev) { g_gt = 0; return 0; }
    MLOG("DM_NVN_GPUTIME: legacy counter sampling on this device: dispatch boundary %d, draw boundary %d, stage boundary %d, blit boundary %d, tile dispatch boundary %d",
         [g_dev supportsCounterSampling:MTLCounterSamplingPointAtDispatchBoundary], [g_dev supportsCounterSampling:MTLCounterSamplingPointAtDrawBoundary],
         [g_dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary], [g_dev supportsCounterSampling:MTLCounterSamplingPointAtBlitBoundary],
         [g_dev supportsCounterSampling:MTLCounterSamplingPointAtTileDispatchBoundary]);
    MTL4CounterHeapDescriptor *hd = [MTL4CounterHeapDescriptor new]; hd.type = MTL4CounterHeapTypeTimestamp; hd.count = GT_SLOTS;
    gt_job *jobs = calloc((size_t)GT_HEAPS * GT_JOBS, sizeof *jobs); if (!jobs) { MLOG("[RED] DM_NVN_GPUTIME: no memory for the job records"); g_gt = 0; return 0; }
    uint32_t mask = 0;
    for (int i = 0; i < GT_HEAPS; i++) { NSError *er = nil;
        g_gt_h[i].heap = [g_dev newCounterHeapWithDescriptor:hd error:&er];
        if (!g_gt_h[i].heap) { MLOG("[RED] DM_NVN_GPUTIME: no counter heap: %s (off)", er.description.UTF8String); free(jobs); g_gt = 0; return 0; }
        g_gt_h[i].grid = [g_dev newBufferWithLength:12 * GT_JOBS options:MTLResourceStorageModeShared]; res_add(g_gt_h[i].grid);
        g_gt_h[i].job = jobs + (size_t)i * GT_JOBS; g_gt_h[i].next = -1; mask |= 1u << i; }
    res_commit(); atomic_store(&g_gt_free, mask);
    g_gt_tick_ns = 1e9 / (double)[g_dev queryTimestampFrequency]; g_gt_stride = [g_dev sizeOfCounterHeapEntry:MTL4CounterHeapTypeTimestamp]; if (g_gt_stride < 8) g_gt_stride = 8;
    MLOG("DM_NVN_GPUTIME: Metal 4 counter heaps (%d x %d stamps), %s timestamps before and after each compute dispatch (in its shared encoder), at the start and end of each draw pass%s; "
         "top %d jobs every %d frames, RED at %.0f ms (at most %d lines)", GT_HEAPS, GT_SLOTS, g_gt_gran == MTL4TimestampGranularityPrecise ? "precise" : "relaxed",
         g_gt_draws ? " and around each guest draw (DM_NVN_GPUTIME_DRAWS)" : "", g_gt_top, g_gt_every, g_gt_red_ms, g_gt_red_left);
    return 1;
}
// A job slot of this submission (submit thread): the current heap, or a free one chained to it. -1: every heap is busy (the job is not timed)
static gt_job *gt_slot(int *hi, int *ji) {
    if (g_gt_cur < 0 || g_gt_h[g_gt_cur].n >= GT_JOBS) {
        uint32_t m = atomic_load(&g_gt_free), b;
        do { if (!m) { atomic_fetch_add(&g_gt_untimed, 1); return NULL; } b = (uint32_t)__builtin_ctz(m); } while (!atomic_compare_exchange_weak(&g_gt_free, &m, m & ~(1u << b)));
        gt_heap *h = &g_gt_h[b]; h->n = 0; h->next = -1; [h->heap invalidateCounterRange:NSMakeRange(0, GT_SLOTS)];
        if (g_gt_cur >= 0) g_gt_h[g_gt_cur].next = (int)b; else g_gt_first = (int)b;
        g_gt_cur = (int)b; }
    gt_heap *h = &g_gt_h[g_gt_cur]; *hi = g_gt_cur; *ji = h->n++;
    gt_job *j = &h->job[*ji]; memset(j, 0, sizeof *j);
    j->frame = atomic_load(&g_cur_frame); j->fence = atomic_load(&g_fence) + 1; atomic_fetch_add(&g_gt_jobs, 1);
    return j;
}
static void gt_key_add(gt_job *j, char kind, const char *key) {
    if (!key) return;
    for (int i = 0; i < j->nk; i++) if (j->k[i] == key && j->kk[i] == kind) return;
    if (j->nk < GT_KEYS) { j->k[j->nk] = key; j->kk[j->nk++] = kind; } else if (j->more < 0xffff) j->more++;
}
static inline void gt_stamp_c(id e, gt_heap *h, NSUInteger i) { [(id<MTL4ComputeCommandEncoder>)e writeTimestampWithGranularity:g_gt_gran intoHeap:h->heap atIndex:i]; }
static inline void gt_stamp_r(id e, gt_heap *h, NSUInteger i, MTLRenderStages st) { [(id<MTL4RenderCommandEncoder>)e writeTimestampWithGranularity:g_gt_gran afterStage:st intoHeap:h->heap atIndex:i]; }
// A compute dispatch about to be encoded into ce (after its clamp, before its dispatch): its begin stamp, and for an indirect one a GPU copy of the 12-byte counts it will run with
// (src + so: the clamped counts, ordered after the clamp by the clamp's own barrier, or the game's record). Returns the job (heap << 16 | index) for gt_cs_end, or -1
static int gt_cs_begin(id ce, const char *key, uint32_t x, uint32_t y, uint32_t z, id<MTLBuffer> src, uint64_t so) {
    int hi, ji; gt_job *j = gt_slot(&hi, &ji); if (!j) return -1;
    if ((__bridge const void *)ce != g_gt_enc_e) { g_gt_enc_e = (__bridge const void *)ce; g_gt_enc++; }
    j->kind = 'c'; j->enc = g_gt_enc; gt_key_add(j, 'c', key); j->x = x; j->y = y; j->z = z; j->ind = src != nil; j->valid = !src;
    gt_heap *h = &g_gt_h[hi]; gt_stamp_c(ce, h, 2 * (NSUInteger)ji);
    if (src) { uint32_t *g = (uint32_t *)h->grid.contents + 3 * ji; g[0] = g[1] = g[2] = ~0u;
        [(id<MTL4ComputeCommandEncoder>)ce copyFromBuffer:src sourceOffset:(NSUInteger)so toBuffer:h->grid destinationOffset:12 * (NSUInteger)ji size:12]; }
    return hi << 16 | ji;
}
static void gt_cs_end(id ce, int t) {
    if (t < 0) return;
    gt_stamp_c(ce, &g_gt_h[t >> 16], 2 * (NSUInteger)(t & 0xffff) + 1);
}
// A draw pass just begun on the main command buffer (enc_r_begin_pass / enc_r_begin): its begin stamp (before its vertex stage begins)
static void gt_pass_begin(id e, MTL4RenderPassDescriptor *d) {
    g_gt_pass = -1; g_gt_pass_e = NULL; g_gt_enc_e = NULL;
    if (g_gt_draws) { g_gt_pass_e = (__bridge const void *)e; return; }   // the draws are the jobs
    int hi, ji; gt_job *j = gt_slot(&hi, &ji); if (!j) return;
    j->kind = 'R'; j->enc = ++g_gt_enc;
    id<MTLTexture> t = d.colorAttachments[0].texture; NSUInteger lv = d.colorAttachments[0].level; if (!t) { t = d.depthAttachment.texture; lv = d.depthAttachment.level; }
    if (t) { j->x = (uint32_t)MAX(t.width >> lv, 1u); j->y = (uint32_t)MAX(t.height >> lv, 1u); } else { j->x = (uint32_t)d.renderTargetWidth; j->y = (uint32_t)d.renderTargetHeight; }
    j->valid = 1; gt_stamp_r(e, &g_gt_h[hi], 2 * (NSUInteger)ji, MTLRenderStageVertex);
    g_gt_pass = ji; g_gt_pass_h = hi; g_gt_pass_e = (__bridge const void *)e;
}
// Right before an encoder of the main command buffer ends (m4_end, enc_r_end_pass): the open pass's end stamp, after its fragment stage
static void gt_enc_end(id e) {
    if ((__bridge const void *)e == g_gt_enc_e) g_gt_enc_e = NULL;
    if ((__bridge const void *)e != g_gt_pass_e) return;
    g_gt_pass_e = NULL;
    if (g_gt_pass < 0) return;
    if (![e respondsToSelector:@selector(writeTimestampWithGranularity:afterStage:intoHeap:atIndex:)]) { g_gt_pass = -1; return; }   // a compute encoder at a freed pass's address: never send it the render stamp (#531)
    gt_stamp_r(e, &g_gt_h[g_gt_pass_h], 2 * (NSUInteger)g_gt_pass + 1, MTLRenderStageFragment); g_gt_pass = -1;
}
// A guest draw passed the skips (draw_, at its wk_note; a task / mesh draw through it): what the next draw call encodes (the render pass may open between the two).
// n / i: count and instances (a mesh draw: groups), ind: an indirect one
static void gt_draw_note(char k0, const char *key0, const char *key1, uint32_t n, uint32_t i, int ind) {
    g_gt_dk[0] = key0; g_gt_dk[1] = key1; g_gt_dkk[0] = k0; g_gt_dkk[1] = 'f'; g_gt_dn = n; g_gt_di = i; g_gt_dind = (uint8_t)ind; g_gt_dpend = k0 == 'm' ? 2 : 1;
}
// At the first draw call after gt_draw_note (BL_CALL, the mesh draw calls): its keys join the open pass's job, or (DM_NVN_GPUTIME_DRAWS) it is a job of its own with stamps around it
static int gt_draw_pre(id e) {
    if (!g_gt_dpend || (__bridge const void *)e != g_gt_pass_e) return -1;
    int m = g_gt_dpend == 2; g_gt_dpend = 0;
    if (!g_gt_draws) { if (g_gt_pass >= 0) { gt_job *j = &g_gt_h[g_gt_pass_h].job[g_gt_pass]; j->draws++; gt_key_add(j, g_gt_dkk[0], g_gt_dk[0]); gt_key_add(j, 'f', g_gt_dk[1]); } return -1; }
    int hi, ji; gt_job *j = gt_slot(&hi, &ji); if (!j) return -1;
    j->kind = m ? 'm' : 'd'; gt_key_add(j, g_gt_dkk[0], g_gt_dk[0]); gt_key_add(j, 'f', g_gt_dk[1]); j->x = g_gt_dn; j->y = g_gt_di; j->ind = g_gt_dind; j->valid = 1; j->enc = g_gt_enc;
    gt_stamp_r(e, &g_gt_h[hi], 2 * (NSUInteger)ji, MTLRenderStageFragment);
    return hi << 16 | ji;
}
static void gt_draw_post(id e, int t) { if (t >= 0) gt_stamp_r(e, &g_gt_h[t >> 16], 2 * (NSUInteger)(t & 0xffff) + 1, MTLRenderStageFragment); }
#define GT_CALL(e, CALL) do { int gt_t_ = __builtin_expect(g_gt_dpend, 0) ? gt_draw_pre(e) : -1; CALL; if (gt_t_ >= 0) gt_draw_post(e, gt_t_); } while (0)

// ---- resolving (completion handlers, the watchdog) ----
static int gt_desc(const gt_job *j, const uint32_t *g, char *t, size_t n) {   // the part in parentheses
    int o = 0; char ks[200] = ""; int ko = 0;
    for (int i = 0; i < j->nk; i++) ko += snprintf(ks + ko, sizeof ks - ko, "%s%c:%.16s", i ? " " : "", j->kk[i], j->k[i]);
    if (j->more) snprintf(ks + ko, sizeof ks - ko, " +%u more", j->more);
    switch (j->kind) {
    case 'c': if (g && g[0] == ~0u) o = snprintf(t, n, "grid indirect, counts not copied yet");
              else o = snprintf(t, n, "grid %u %u %u%s", g ? g[0] : j->x, g ? g[1] : j->y, g ? g[2] : j->z, j->ind ? ", indirect" : ""); break;
    case 'R': o = snprintf(t, n, "draw pass %ux%u, %u draws: %s", j->x, j->y, j->draws, j->nk ? ks : "no guest draw"); break;
    case 'm': o = snprintf(t, n, "mesh draw %s groups %s%u", ks, j->ind ? "indirect, " : "", j->x); break;
    default: if (j->ind) o = snprintf(t, n, "draw %s indirect", ks); else o = snprintf(t, n, "draw %s count %u instances %u", ks, j->x, j->y); break; }
    return o;
}
static const char *gt_name(const gt_job *j) {   // the key a line names: the kernel; a draw's fragment shader (else its first key)
    for (int i = 0; i < j->nk; i++) if (j->kk[i] == 'f' && j->kind != 'R') return j->k[i];
    return j->nk ? j->k[0] : "-";
}
static uint64_t gt_ts(const uint8_t *b, int i) { uint64_t v; memcpy(&v, b + (size_t)i * g_gt_stride, 8); return v; }
static inline int gt_ok(uint64_t v) { return v && v != ~0ull; }
typedef struct { double ms; gt_job j; uint32_t g[3]; int hasg; } gt_top_ent;
static gt_top_ent g_gt_tt[GT_TOPMAX], g_gt_worst; static int g_gt_tn; static uint64_t g_gt_tframe = ~0ull, g_gt_fjobs, g_gt_worst_frame; static double g_gt_fms;
static uint64_t g_gt_pairs, g_gt_collapsed; static int g_gt_gran_said;
static void gt_line(const char *pre, const gt_top_ent *t) {
    char d[400]; gt_desc(&t->j, t->hasg ? t->g : NULL, d, sizeof d);
    MLOG("%s%8.3f ms %c %.16s (%s) frame %llu fence %llu", pre, t->ms, t->j.kind, gt_name(&t->j), d, (unsigned long long)t->j.frame, (unsigned long long)t->j.fence);
}
static void gt_frame_report(void) {   // under g_gt_mu: the frame g_gt_tframe is complete
    if (g_gt_tframe == ~0ull || g_gt_tframe % (uint64_t)g_gt_every) return;
    MLOG("[gputime] frame %llu: %llu jobs timed, %.2f ms summed (dispatch / pass / draw times; overlapping work is counted once each), %llu jobs untimed so far (every heap busy); slowest:",
         (unsigned long long)g_gt_tframe, (unsigned long long)g_gt_fjobs, g_gt_fms, (unsigned long long)atomic_load(&g_gt_untimed));
    for (int i = 0; i < g_gt_tn; i++) gt_line("[gputime]   ", &g_gt_tt[i]);
    if (g_gt_worst.ms > 0) { gt_line("[gputime]   slowest since the last report: ", &g_gt_worst); g_gt_worst.ms = 0; }
    if (!g_gt_gran_said && g_gt_pairs >= 64) { g_gt_gran_said = 1;
        MLOG("[gputime] granularity: %llu successive dispatches in one encoder, %llu with the same stamps as the dispatch before (%s)", (unsigned long long)g_gt_pairs, (unsigned long long)g_gt_collapsed,
             g_gt_collapsed * 2 > g_gt_pairs ? "collapsed: the times are per encoder, the keys of one encoder share them" : "per dispatch"); }
}
static void gt_record(const gt_job *j, double ms, const uint32_t *g) {   // under g_gt_mu
    if (j->frame != g_gt_tframe) { gt_frame_report(); g_gt_tframe = j->frame; g_gt_tn = 0; g_gt_fjobs = 0; g_gt_fms = 0; }
    g_gt_fjobs++; g_gt_fms += ms;
    gt_top_ent e = {ms, *j, {0, 0, 0}, g != NULL}; if (g) memcpy(e.g, g, 12);
    if (g_gt_top > 0 && (g_gt_tn < g_gt_top || ms > g_gt_tt[g_gt_tn - 1].ms)) {
        int k = g_gt_tn < g_gt_top ? g_gt_tn++ : g_gt_tn - 1;
        while (k > 0 && g_gt_tt[k - 1].ms < ms) { g_gt_tt[k] = g_gt_tt[k - 1]; k--; } g_gt_tt[k] = e; }
    if (ms > g_gt_worst.ms) g_gt_worst = e;
    gt_rec *r = &g_gt_last[g_gt_last_n++ % GT_LAST]; r->key = gt_name(j); r->kind = j->kind; r->ms = ms; r->frame = j->frame;
    r->x = g ? g[0] : j->x; r->y = g ? g[1] : j->y; r->z = g ? g[2] : j->z;
    if (ms >= g_gt_red_ms && g_gt_red_left > 0) { g_gt_red_left--; g_gt_red_n++;
        char d[400]; gt_desc(j, g, d, sizeof d);
        MLOG("[RED] slow GPU job: %.16s %.1f ms (%s) frame %llu fence %llu", gt_name(j), ms, d, (unsigned long long)j->frame, (unsigned long long)j->fence); }
}
// A submission's heaps, once its last command buffer has completed: every job with both stamps is recorded; the heaps go back to the pool
static void gt_resolve(int first) {
    pthread_mutex_lock(&g_gt_mu);
    for (int hi = first; hi >= 0;) { gt_heap *h = &g_gt_h[hi]; int next = h->next, n = h->n;
        NSData *r = n ? [h->heap resolveCounterRange:NSMakeRange(0, 2 * (NSUInteger)n)] : nil; const uint8_t *b = r.bytes;
        const uint32_t *gr = (const uint32_t *)h->grid.contents;
        uint64_t pa = 0, pb = 0; uint32_t penc = 0;
        for (int i = 0; b && i < n; i++) { const gt_job *j = &h->job[i]; uint64_t a = gt_ts(b, 2 * i), e = gt_ts(b, 2 * i + 1);
            if (!gt_ok(a) || !gt_ok(e) || e < a) continue;
            if (j->kind == 'c') { if (i && penc == j->enc && pa) { g_gt_pairs++; if (a == pa && e == pb) g_gt_collapsed++; } pa = a; pb = e; penc = j->enc; }
            gt_record(j, (e - a) * g_gt_tick_ns / 1e6, j->ind ? gr + 3 * i : NULL); }
        h->n = 0; h->next = -1; atomic_fetch_or(&g_gt_free, 1u << hi); hi = next; }
    pthread_mutex_unlock(&g_gt_mu);
}
// mtl_sub_commit, before the commit: the submission's heaps are handed to its completion (o: its commit options); the watchdog can find them by fence
static void gt_commit(uint64_t fence, MTL4CommitOptions *o) {
    int first = g_gt_first; g_gt_first = g_gt_cur = -1; g_gt_pass = -1; g_gt_pass_e = NULL; g_gt_enc_e = NULL; g_gt_dpend = 0;
    if (first < 0) return;
    g_gt_sub[fence % GT_SUBS].first = first; atomic_store(&g_gt_sub[fence % GT_SUBS].fence, fence);
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) { (void)f; gt_resolve(first); }];
}
// The GPU watchdog: the stuck submission's stamps as the GPU left them. The job begun and not ended is what the GPU is running (or the first of several running together)
static void gt_watchdog(uint64_t fence) {
    if (g_gt <= 0) return;
    if (atomic_load(&g_gt_sub[fence % GT_SUBS].fence) != fence) { MLOG("[RED] gputime at the watchdog fence=%llu: no timed jobs in it", (unsigned long long)fence); return; }
    int nj = 0, nd = 0, shown = 0; gt_top_ent top[5] = {0}; char d[400];
    for (int hi = g_gt_sub[fence % GT_SUBS].first; hi >= 0; hi = g_gt_h[hi].next) { gt_heap *h = &g_gt_h[hi];
        NSData *r = h->n ? [h->heap resolveCounterRange:NSMakeRange(0, 2 * (NSUInteger)h->n)] : nil; const uint8_t *b = r.bytes; if (!b) continue;
        const uint32_t *gr = (const uint32_t *)h->grid.contents;
        for (int i = 0; i < h->n; i++) { const gt_job *j = &h->job[i]; uint64_t a = gt_ts(b, 2 * i), e = gt_ts(b, 2 * i + 1); nj++;
            if (gt_ok(a) && gt_ok(e) && e >= a) { nd++; double ms = (e - a) * g_gt_tick_ns / 1e6;
                if (ms > top[4].ms) { int k = 4; while (k > 0 && top[k - 1].ms < ms) { top[k] = top[k - 1]; k--; } top[k] = (gt_top_ent){ms, *j, {0, 0, 0}, j->ind}; if (j->ind) memcpy(top[k].g, gr + 3 * i, 12); } }
            else if (gt_ok(a) && shown < 8) { shown++; gt_desc(j, j->ind ? gr + 3 * i : NULL, d, sizeof d);
                MLOG("[RED] gputime at the watchdog fence=%llu: begun, not ended: %c %.16s (%s), job %d of the submission", (unsigned long long)fence, j->kind, gt_name(j), d, nj); } } }
    MLOG("[RED] gputime at the watchdog fence=%llu: %d jobs timed in it, %d ended, %d begun and not ended (shown above); slowest ended:", (unsigned long long)fence, nj, nd, shown);
    for (int k = 0; k < 5 && top[k].ms > 0; k++) gt_line("[RED]   ", &top[k]);
}
