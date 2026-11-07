// Flight recorder (DM_NVN_FLIGHTREC=<N>, 2026-11-07, off by default): the last N encoded compute dispatches and draws, kept in memory as fixed-size binary records, written out
// only when something goes wrong. Made for PRAGMATA's GPU wedges and page faults, where printf-style logging per dispatch (DM_NVN_BREADCRUMB's barrier, TEXSNAP, fprintf) moved the
// hang and only ever showed one call. Included by nvn_mtl.m before csp_mark; the declarations it needs earlier (g_fr_on, g_fr_x, EB_NOTE's extra fields, fr_dump) sit by the breadcrumb.
//
// Hot path (the encode thread, once per dispatch / draw): one 1 KB record filled in place in a CPU ring (no formatting, no I/O, no locks, no allocation):
//   serial, fence, frame, pass, encode time, shader key(s), grid or indirect address with the counts the CPU sees at encode, threadgroup size, every buffer binding (class, arg id,
//   NVN slot, rw, guest address + size, the bound GPU address, the Metal buffer's offset + length, how it was resolved: sparse span / physical redirect / old lookup short or
//   failed / zero bank / dummy / sink / frozen copy) and the texture / image handles (arg id, handle ID).
// GPU side (compute only, both default on with the recorder):
//   DM_NVN_FLIGHTREC_MARKS=1 (default): progress words in a shared ring of N x {start, end}, written by 8-byte GPU copies with no barrier of their own: the dispatch's serial goes
//     into start right before it, and the PREVIOUS dispatch's serial into its end at the same place, behind the barrier the copy encoder already puts before every op after a
//     dispatch (or a new encoder's queue barrier). So end(s) is written once s finished and s + 1 was reached; start(s) means s became eligible to run (a start of s + 1 also
//     means s finished). Cost ~0.5 us of GPU a dispatch (t_flightrec bench).
//   DM_NVN_FLIGHTREC_MARKS=2: exact: the dispatch waits for its start copy (blit -> dispatch barrier) and its end mark is written right after it (dispatch -> blit barrier);
//     two more barriers, ~4.7 us of GPU a dispatch (about 10 ms a frame at PRAGMATA's ~2,000 dispatches): unit tests, or a run where timing does not matter.
//   MARKS=0: off. A GPU write can reach the CPU late (t_flightrec: a start mark seen 7-240 ms after the kernel began), so a mark not seen proves nothing on its own; the
//     fence and the first dispatch after the last end mark are the robust part (the breadcrumb has the same limit).
//   DM_NVN_FLIGHTREC_SNAP=1 (default): a 1 KB slot per indirect dispatch in a ring of DM_NVN_FLIGHTREC_SNAPN (default min(N, 16384)) slots, filled by GPU copies right before the
//     dispatch: the raw indirect counts (12 B at the indirect address), the clamped counts the dispatch read (dpc_clamp output) and its owner (serial, written by the GPU too).
//   SNAP=2: every dispatch, plus the first 240 bytes of up to 4 uniform buffers (cbufs) each (~0.4-0.8 us of GPU a copy in the bench: a dispatch with 4 cbufs pays ~2-3 us).
// Dump: on the GPU watchdog, the first command-buffer error, the first loop-cap hit (DM_NVN_FLIGHTREC_LCAP=0 off), a crash or abort (crash.c on_crash), SIGUSR1, or the DM_CTL line
//   "flightdump [tag]": <dir>/flight_<pid>_<n>_<why>.bin + .json, dir = DM_NVN_FLIGHTREC_DIR, else <dir of DM_CTL>/flight, else ./flight. It only writes what is in memory and never
//   waits on the GPU. At most DM_NVN_FLIGHTREC_MAXDUMPS (6) dumps a run. Decoder / analyzer: sw2_tools/flight/flight_analyze.py.
// DM_NVN_FLIGHTREC_DRAWS=0: dispatches only (a draw record costs ~60 ns on the encode thread, a dispatch record ~90 ns; t_flightrec cpubench).
// Known limits: draws have no GPU marks, no bindings (keys, counts, indirect / index addresses only); a GPU write the hang keeps in a cache may not be visible to the CPU (as for the
// breadcrumb); records being written while a dump copies them are flagged torn (seq != seq_end) by the analyzer.

enum { FR_REC = 1024, FR_SNAP_SLOT = 1024, FR_SNAP_CB = 240, FR_SNAP_NCB = 4, FR_VERSION = 1 };
typedef struct { uint8_t kind, flag, aid, how, reg, rw, snapi, pad; uint32_t size, buf_off; uint64_t guest, gpu; uint32_t buf_len, pad2; } fr_bind;   // 40 B
typedef struct {
    uint64_t seq, fence, ind, t_ns, pass, ibuf;           //   0
    uint32_t frame; uint8_t kind, nb, nt, flags;          //  48  kind: 1 dispatch, 2 draw, 3 indexed draw, 4 mesh-task draw; flags: FRF_*
    char key[16], key2[16];                               //  56  dispatch: compute key; draw: vertex key, fragment key
    uint32_t x, y, z;                                     //  88  dispatch: grid (indirect: as passed, usually 0); draw: count, instances, first / base vertex
    uint16_t tgx, tgy, tgz, prim;                         // 100
    uint32_t cnt[3];                                      // 108  indirect dispatch: the counts the CPU saw at encode (GPU-written ones are usually written later in the frame)
    uint32_t snap;                                        // 120  GPU snapshot slot + 1 (0: none)
    uint32_t mdi;                                         // 124  draw: index in an indirect-count multi-draw; dispatch: arg-table bytes
    fr_bind b[FR_NB];                                     // 128
    fr_tex t[FR_NT];                                      // 928
    uint64_t seq_end;                                     // 1016 written last: a record with seq_end != seq was being written
} fr_rec;
_Static_assert(sizeof(fr_rec) == FR_REC, "flight record size");
enum { FRF_IND = 1, FRF_BTRUNC = 2, FRF_TTRUNC = 4, FRF_OWN = 8, FRF_MARK = 16, FRF_LCAP = 32, FRF_SPILL = 64, FRF_CLAMP = 128 };

static fr_rec *g_fr; static uint64_t g_fr_mask, g_fr_n; static _Atomic uint64_t g_fr_top;   // ring, N - 1, N, the newest serial written
static uint64_t g_fr_snap_n, g_fr_snap_mask, g_fr_prev; static int g_fr_marks_on, g_fr_snap_on, g_fr_draws = 1, g_fr_max = 6;
static __strong id<MTLBuffer> g_fr_marks, g_fr_snapb;   // GPU marks x N (MARKS=1 {start, the previous dispatch's serial}; MARKS=2 {start, end}); snapshot slots
static void *g_fr_marks_p, *g_fr_snap_p; static uint64_t g_fr_marks_gpu, g_fr_snap_gpu;   // their contents / GPU addresses, read without Objective-C in a signal handler
static const void *g_fr_lcap_p; static uint64_t g_fr_lcap_gpu, g_fr_lcap_len;   // the loop guard's buffer, cached at the first dispatch that has one (fr_dispatch)
static __strong id<MTLBuffer> g_fr_ab; static size_t g_fr_ao;   // MARKS=2: the arena words the end mark of the dispatch being encoded is copied from
static char g_fr_dir[1024]; static _Atomic int g_fr_dumps; static _Atomic int g_fr_busy;
static uint64_t g_fr_zb[2], g_fr_dummy[2];   // the zero bank's and the dummy's GPU ranges (bindings that resolved to them carry a flag too)
static dispatch_source_t g_fr_sig;
static uint64_t fr_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static void fr_key(char *d, const char *k) { int i = 0; if (k) for (; i < 16 && k[i]; i++) d[i] = k[i]; for (; i < 16; i++) d[i] = 0; }

static void fr_crash(int sig);
static void fr_init_(void) {
    const char *e = getenv("DM_NVN_FLIGHTREC"); long n = e ? atol(e) : 0;
    if (n <= 0) { g_fr_on = 0; return; }
    uint64_t c = 256; while (c < (uint64_t)n && c < (1u << 20)) c <<= 1;
    g_fr = calloc(c, sizeof(fr_rec)); if (!g_fr) { MLOG("[RED] DM_NVN_FLIGHTREC: cannot allocate %llu records", (unsigned long long)c); g_fr_on = 0; return; }
    g_fr_n = c; g_fr_mask = c - 1;
    const char *m = getenv("DM_NVN_FLIGHTREC_MARKS"), *s = getenv("DM_NVN_FLIGHTREC_SNAP"), *sn = getenv("DM_NVN_FLIGHTREC_SNAPN"), *l = getenv("DM_NVN_FLIGHTREC_LCAP"), *mx = getenv("DM_NVN_FLIGHTREC_MAXDUMPS"), *dr = getenv("DM_NVN_FLIGHTREC_DRAWS");
    g_fr_marks_on = !m ? 1 : atoi(m); g_fr_snap_on = !s ? 1 : atoi(s); g_fr_lcap_on = !l || atoi(l); if (mx) g_fr_max = atoi(mx); if (dr) g_fr_draws = atoi(dr);   // (SNAP: not !s || atoi(s), which read 2 as 1)
    if (g_fr_marks_on) {
        g_fr_marks = [g_dev newBufferWithLength:c * 16 options:MTLResourceStorageModeShared];
        if (!g_fr_marks) g_fr_marks_on = 0; else { memset(g_fr_marks.contents, 0, c * 16); res_add(g_fr_marks); g_fr_marks_p = g_fr_marks.contents; g_fr_marks_gpu = g_fr_marks.gpuAddress; }
    }
    if (g_fr_snap_on) {
        uint64_t want = sn && atol(sn) > 0 ? (uint64_t)atol(sn) : MIN(c, 16384ull), k = 64; while (k < want && k < c) k <<= 1;
        g_fr_snapb = [g_dev newBufferWithLength:k * FR_SNAP_SLOT options:MTLResourceStorageModeShared];
        if (!g_fr_snapb) g_fr_snap_on = 0; else { memset(g_fr_snapb.contents, 0, k * FR_SNAP_SLOT); res_add(g_fr_snapb); g_fr_snap_n = k; g_fr_snap_mask = k - 1; g_fr_snap_p = g_fr_snapb.contents; g_fr_snap_gpu = g_fr_snapb.gpuAddress; }
    }
    if (g_fr_marks_on || g_fr_snap_on) res_commit();
    g_fr_zb[0] = zero_bank_gpu(); g_fr_zb[1] = 65536; g_fr_dummy[0] = dummy_buf_gpu(); g_fr_dummy[1] = 65536;
    const char *d = getenv("DM_NVN_FLIGHTREC_DIR"), *ctl = getenv("DM_CTL");
    if (d && *d) snprintf(g_fr_dir, sizeof g_fr_dir, "%s", d);
    else if (ctl && *ctl && strrchr(ctl, '/')) snprintf(g_fr_dir, sizeof g_fr_dir, "%.*s/flight", (int)(strrchr(ctl, '/') - ctl), ctl);
    else snprintf(g_fr_dir, sizeof g_fr_dir, "flight");
    mkdir(g_fr_dir, 0755);
    g_fr_on = 1;
    { void crash_set_flight_hook(void (*fn)(int)); crash_set_flight_hook(fr_crash); }   // crash.c: a fatal signal dumps the ring
    signal(SIGUSR1, SIG_IGN);   // delivered through a dispatch source, on an ordinary thread
    g_fr_sig = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGUSR1, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    if (g_fr_sig) { dispatch_source_set_event_handler(g_fr_sig, ^{ fr_dump("usr1", 0); }); dispatch_resume(g_fr_sig); }
    MLOG("DM_NVN_FLIGHTREC=%llu: the last %llu dispatches / draws kept in memory (%llu MB), GPU marks %s, GPU snapshots %s (%llu slots); dumps to %s/ on the watchdog, a command buffer error, "
         "the first loop cap hit%s, a crash, SIGUSR1 (kill -USR1 %d) or DM_CTL flightdump", (unsigned long long)c, (unsigned long long)c, (unsigned long long)(c * FR_REC >> 20), g_fr_marks_on ? "on" : "off",
         g_fr_snap_on ? "on" : "off", (unsigned long long)g_fr_snap_n, g_fr_dir, g_fr_lcap_on ? "" : " (off)", (int)getpid());
}
static int fr_init(void) { if (g_fr_on < 0) fr_init_(); return g_fr_on > 0; }

// The bound GPU address of each binding noted for this dispatch (the slots as encode_args_ wrote them, after every rewrite: uniform shadows, frozen copies). Called at the end of the
// binding loop of a compute stage
static void fr_bound(const uint64_t *slots) { for (int i = 0; i < g_eb_n; i++) g_fr_bound[i] = slots[g_eb[i].aid]; }

static inline fr_rec *fr_open(uint64_t *seqp) {
    uint64_t s = atomic_load_explicit(&g_fr_top, memory_order_relaxed) + 1; fr_rec *r = &g_fr[s & g_fr_mask];
    r->seq = s; atomic_thread_fence(memory_order_release); *seqp = s; return r;
}
static inline void fr_close(fr_rec *r, uint64_t s) { atomic_store_explicit((_Atomic uint64_t *)&r->seq_end, s, memory_order_release); atomic_store_explicit(&g_fr_top, s, memory_order_release); }
static inline void fr_copy(id<MTL4ComputeCommandEncoder> e, id<MTLBuffer> src, uint64_t so, uint64_t dof, uint64_t n) {
    if (!src || !n || (so & 3) || (n & 3) || so + n > src.length) return;
    [e copyFromBuffer:src sourceOffset:so toBuffer:g_fr_snapb destinationOffset:dof size:n];
}
// A compute dispatch about to be encoded in ce: the record, the snapshot copies and the start mark. ind_b / ind_o: the indirect counts as the game wrote them; cl_b / cl_o: the clamped
// ones the dispatch reads (NULL when not clamped). Returns the serial for fr_dispatch_end (0: the recorder is off)
static uint64_t fr_dispatch(id ce_, const NvShader *cs, const p_dispatch *dp, id<MTLBuffer> ind_b, uint64_t ind_o, id<MTLBuffer> cl_b, uint64_t cl_o, int flags) {
    if (!fr_init()) return 0;
    id<MTL4ComputeCommandEncoder> ce = (id<MTL4ComputeCommandEncoder>)ce_;
    uint64_t s; fr_rec *r = fr_open(&s);
    r->fence = atomic_load_explicit(&g_fence, memory_order_relaxed) + 1; r->ind = dp->ind; r->t_ns = fr_ns(); r->pass = g_stat_pass; r->ibuf = 0;
    r->frame = (uint32_t)atomic_load_explicit(&g_cur_frame, memory_order_relaxed); r->kind = 1;
    fr_key(r->key, cs->key); memset(r->key2, 0, 16);
    r->x = (uint32_t)dp->x; r->y = (uint32_t)dp->y; r->z = (uint32_t)dp->z; r->tgx = cs->rf.tg[0]; r->tgy = cs->rf.tg[1]; r->tgz = cs->rf.tg[2]; r->prim = 0;
    r->cnt[0] = r->cnt[1] = r->cnt[2] = 0;
    if (ind_b && ind_b.contents && ind_o + 12 <= ind_b.length) memcpy(r->cnt, (const uint8_t *)ind_b.contents + ind_o, 12);
    if (__builtin_expect(!g_fr_lcap_p, 0) && g_lcap_buf) { g_fr_lcap_gpu = g_lcap_buf.gpuAddress; g_fr_lcap_len = g_lcap_buf.length; g_fr_lcap_p = g_lcap_buf.contents; }
    r->mdi = 0;
    int nb = g_eb_n < FR_NB ? g_eb_n : FR_NB, ncb = 0;
    uint64_t slot = s & g_fr_snap_mask, sb = slot * FR_SNAP_SLOT;
    for (int i = 0; i < nb; i++) {
        const eb_ent *e = &g_eb[i]; fr_bind *o = &r->b[i];
        uint64_t bound = g_fr_bound[i], base = e->gpu, off = bound >= base && bound - base < (e->len ? e->len : 1) ? bound - base : 0xffffffffull;
        *o = (fr_bind){(uint8_t)e->kind, (uint8_t)e->flag, e->aid, g_fr_x[i].how, g_fr_x[i].reg, g_fr_x[i].rw, 0, 0, (uint32_t)MIN(e->size, 0xffffffffull), (uint32_t)MIN(off, 0xffffffffull), e->guest, bound,
                       (uint32_t)MIN(e->len, 0xffffffffull), 0};
        if (off == 0xffffffffull && !e->flag) o->how = 's';   // bound elsewhere than the buffer resolved (a uniform shadow)
        if (g_fr_snap_on == 2 && e->kind == 'c' && !e->flag && g_fr_mb[i] && ncb < FR_SNAP_NCB) {
            id<MTLBuffer> mb = (__bridge id<MTLBuffer>)g_fr_mb[i]; uint64_t mo = g_fr_mo[i], n = MIN((uint64_t)FR_SNAP_CB, e->size ? e->size : FR_SNAP_CB) & ~3ull;
            if (mo + n > mb.length) n = mb.length > mo ? (mb.length - mo) & ~3ull : 0;
            if (n && !(mo & 3)) { fr_copy(ce, mb, mo, sb + 64 + (uint64_t)ncb * FR_SNAP_CB, n); o->snapi = (uint8_t)(ncb + 1); o->pad = (uint8_t)(n / 4); ncb++; }   // pad: words copied
        }
    }
    r->nb = (uint8_t)nb; if (g_eb_n > FR_NB) flags |= FRF_BTRUNC;
    int nt = g_fr_nt < FR_NT ? g_fr_nt : FR_NT; memcpy(r->t, g_fr_t, (size_t)nt * sizeof *g_fr_t); r->nt = (uint8_t)nt; if (g_fr_nt > FR_NT) flags |= FRF_TTRUNC;
    r->snap = 0;
    // What the GPU copies come from: 32 bytes of the frame arena (valid until the GPU is done with the frame, as csp_mark's), never a ring slot the CPU may reuse before the
    // GPU runs the copy: {serial, previous dispatch's serial} for the mark, {serial, cbufs | flags << 32} for the snapshot slot's owner (written by the GPU in order with its data)
    int snap = g_fr_snap_on && (ind_b || cl_b || ncb || g_fr_snap_on == 2);
    if (g_fr_marks_on || snap) {
        __strong id<MTLBuffer> ab; size_t ao = arena_alloc(32, &ab); uint64_t *q = (uint64_t *)(void *)(g_cur_arena->cpu + ao);
        q[0] = s; q[1] = g_fr_prev; q[2] = s; q[3] = (uint64_t)ncb | (uint64_t)((ind_b ? 1u : 0u) | (cl_b ? 2u : 0u)) << 32;
        if (snap) {
            if (ind_b) fr_copy(ce, ind_b, ind_o, sb, 12);
            if (cl_b) fr_copy(ce, cl_b, cl_o, sb + 16, 12);
            [ce copyFromBuffer:ab sourceOffset:ao + 16 toBuffer:g_fr_snapb destinationOffset:sb + 32 size:16];
            r->snap = (uint32_t)slot + 1;
        }
        if (g_fr_marks_on) {   // MARKS=1: {start, the previous dispatch} in one copy: it runs behind the barrier the copy encoder (or a new encoder's queue barrier) put after that one
            uint64_t ms = s & g_fr_mask;
            [ce copyFromBuffer:ab sourceOffset:ao toBuffer:g_fr_marks destinationOffset:ms * 16 size:g_fr_marks_on == 1 ? 16 : 8];
            flags |= FRF_MARK; g_fr_prev = s; g_fr_ab = ab; g_fr_ao = ao;
        }
    }
    // MARKS=2: the dispatch waits for the copies. Without the barrier the GPU may run the start mark (and the snapshot) after the dispatch, so a start mark only says the
    // dispatch became eligible to run. It costs ~2.3 us of GPU a dispatch (t_flightrec bench), so it is not the default
    if (g_fr_marks_on == 2) [ce barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageDispatch visibilityOptions:resource_vis()];
    r->flags = (uint8_t)flags;
    fr_close(r, s);
    return s;
}
// After the dispatch (and its redo, if any): the end mark, behind the dispatch
static void fr_dispatch_end(id ce_, uint64_t s) {
    if (!s || g_fr_marks_on != 2 || !g_fr_ab) return;   // MARKS=1: the next dispatch's mark proves this one finished (fr_dispatch)
    id<MTL4ComputeCommandEncoder> ce = (id<MTL4ComputeCommandEncoder>)ce_; uint64_t ms = s & g_fr_mask;
    [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageBlit visibilityOptions:resource_vis()];
    [ce copyFromBuffer:g_fr_ab sourceOffset:g_fr_ao toBuffer:g_fr_marks destinationOffset:ms * 16 + 8 size:8];
    g_fr_ab = nil;
}
// A draw (CPU record only): the program's vertex and fragment keys, counts, indirect and index buffer addresses
static void fr_draw(const p_draw *dw, const p_draw_idx *di) {
    if (!fr_init() || !g_fr_draws) return;
    uint64_t s; fr_rec *r = fr_open(&s);
    NvProg *p = g_geo_program ? g_geo_program : S.prog ? prog_get(S.prog) : nil;
    r->fence = atomic_load_explicit(&g_fence, memory_order_relaxed) + 1; r->t_ns = fr_ns(); r->pass = g_stat_pass;
    r->frame = (uint32_t)atomic_load_explicit(&g_cur_frame, memory_order_relaxed);
    fr_key(r->key, p && p->sh[0] ? p->sh[0]->key : NULL); fr_key(r->key2, p && p->sh[1] ? p->sh[1]->key : NULL);
    if (dw) { r->kind = dw->prim == DM_PRIM_MESH_TASKS ? 4 : 2; r->ind = dw->ind; r->ibuf = 0; r->x = (uint32_t)dw->count; r->y = (uint32_t)dw->instances; r->z = (uint32_t)dw->first; r->prim = (uint16_t)dw->prim; r->mdi = (uint32_t)dw->mdi_i; }
    else { r->kind = 3; r->ind = di->ind; r->ibuf = di->ibuf; r->x = (uint32_t)di->count; r->y = (uint32_t)di->instances; r->z = (uint32_t)di->base_vertex; r->prim = (uint16_t)di->prim; r->mdi = (uint32_t)di->mdi_i; }
    r->tgx = r->tgy = r->tgz = 0; r->cnt[0] = r->cnt[1] = r->cnt[2] = 0; r->snap = 0; r->nb = r->nt = 0;
    r->flags = (uint8_t)((dw ? dw->ind : di->ind) ? FRF_IND : 0);
    fr_close(r, s);
}

// ---------- dump ----------
static int fr_write_all(int fd, const void *p, size_t n) { const uint8_t *b = p; while (n) { ssize_t w = write(fd, b, n > (1u << 24) ? (1u << 24) : n); if (w <= 0) return 0; b += w; n -= (size_t)w; } return 1; }
typedef struct { uint64_t lo, hi, base, gpu; } fr_pool;
// sig: called from a signal handler (crash / abort): no allocation, no locks waited on, no Objective-C (contents and GPU addresses cached at init), no floating-point formatting,
// the records written straight from the ring
static void fr_dump(const char *why, int sig) {
    if (g_fr_on <= 0 || !g_fr) return;
    int k = atomic_fetch_add(&g_fr_dumps, 1);
    if (k >= g_fr_max) { if (!sig) MLOG("[flightrec] dump %s skipped: %d dumps already (DM_NVN_FLIGHTREC_MAXDUMPS)", why, g_fr_max); return; }
    if (atomic_exchange(&g_fr_busy, 1)) {   // one dump at a time (a crash during a dump: skipped; another dump: waited for up to 2 s)
        int got = 0; if (!sig) for (int i = 0; i < 2000 && !(got = !atomic_exchange(&g_fr_busy, 1)); i++) usleep(1000);
        if (!got) { if (!sig) MLOG("[flightrec] dump %s skipped: another dump is still being written", why); return; } }
    uint64_t t0 = fr_ns(), top0 = atomic_load(&g_fr_top), fence = atomic_load(&g_fence), done = atomic_load(&g_done), frame = atomic_load(&g_cur_frame);
    // the GPU-visible words first: they keep moving while the CPU ring is copied
    uint8_t *marks = NULL, *snap = NULL, *recs = NULL; size_t mlen = g_fr_marks_on ? g_fr_n * 16 : 0, slen = g_fr_snap_on ? g_fr_snap_n * FR_SNAP_SLOT : 0, rlen = g_fr_n * FR_REC;
    const void *mp = g_fr_marks_on ? g_fr_marks_p : NULL, *sp = g_fr_snap_on ? g_fr_snap_p : NULL;
    if (!sig) { marks = mlen ? malloc(mlen) : NULL; snap = slen ? malloc(slen) : NULL; recs = malloc(rlen);
        if (marks) memcpy(marks, mp, mlen); if (recs) memcpy(recs, g_fr, rlen); if (snap) memcpy(snap, sp, slen); }
    uint64_t top1 = atomic_load(&g_fr_top);
    if (!sig && !g_fr_lcap_p && g_lcap_buf) { g_fr_lcap_gpu = g_lcap_buf.gpuAddress; g_fr_lcap_len = g_lcap_buf.length; g_fr_lcap_p = g_lcap_buf.contents; }
    uint32_t lw[16] = {0}; uint64_t lg = g_fr_lcap_gpu, ll = g_fr_lcap_len; const void *lp = g_fr_lcap_p; if (lp) memcpy(lw, lp, sizeof lw);
    size_t llen = lp ? (size_t)MIN(ll, 1ull << 20) : 0;   // the loop guard's whole buffer (records of hit loops: key, site, bound) as the GPU left it
    static fr_pool pools[4096]; int np = 0, plock = 0;
    for (int i = 0; i < (sig ? 1 : 50) && !(plock = !pthread_mutex_trylock(&g_mu)); i++) usleep(1000);
    if (plock) { for (int i = 0; i < g_npool && np < 4096; i++) pools[np++] = (fr_pool){g_pools[i].lo, g_pools[i].hi, g_pools[i].base, g_pools[i].gpu}; pthread_mutex_unlock(&g_mu); }
    char path[1200]; snprintf(path, sizeof path, "%s/flight_%d_%d_%s.bin", g_fr_dir, (int)getpid(), k, why);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644), ok = fd >= 0;
    uint64_t o_rec = 0, o_marks = rlen, o_snap = o_marks + mlen, o_pools = o_snap + slen, o_lcap = o_pools + (uint64_t)np * sizeof(fr_pool);
    if (ok) ok = fr_write_all(fd, recs ? recs : (const void *)g_fr, rlen) && (!mlen || fr_write_all(fd, marks ? marks : mp, mlen)) && (!slen || fr_write_all(fd, snap ? snap : sp, slen))
                 && fr_write_all(fd, pools, (size_t)np * sizeof *pools) && (!llen || fr_write_all(fd, lp, llen));
    if (fd >= 0) close(fd);
    free(marks); free(snap); free(recs);
    static char js[16384]; int o = 0;
    #define J(...) do { if (o < (int)sizeof js - 1) o += snprintf(js + o, sizeof js - (size_t)o, __VA_ARGS__); } while (0)
    J("{\n \"magic\": \"DMFLREC\", \"version\": %d, \"why\": \"%s\", \"pid\": %d, \"dump\": %d, \"bin\": \"%s\", \"chain\": \"%s\", \"signal_context\": %d,\n", FR_VERSION, why, (int)getpid(), k, strrchr(path, '/') + 1, DMSH_CHAIN, sig);
    J(" \"rec_size\": %d, \"rec_n\": %llu, \"top_before\": %llu, \"top_after\": %llu, \"copied\": %d,\n", FR_REC, (unsigned long long)g_fr_n, (unsigned long long)top0, (unsigned long long)top1, !sig);
    J(" \"fence_committed\": %llu, \"fence_done\": %llu, \"frame\": %llu, \"t_ns\": %llu, \"dump_us\": %llu, \"last_commit_error\": %d, \"watchdog_halt\": %d,\n", (unsigned long long)fence, (unsigned long long)done,
      (unsigned long long)frame, (unsigned long long)t0, (unsigned long long)((fr_ns() - t0) / 1000), (int)atomic_load(&g_last_commit_error), (int)atomic_load(&g_wd_halt));
    J(" \"sections\": {\"records\": [%llu, %llu], \"marks\": [%llu, %llu], \"snap\": [%llu, %llu], \"pools\": [%llu, %llu], \"loop_guard\": [%llu, %llu]},\n", (unsigned long long)o_rec, (unsigned long long)rlen, (unsigned long long)o_marks,
      (unsigned long long)mlen, (unsigned long long)o_snap, (unsigned long long)slen, (unsigned long long)o_pools, (unsigned long long)np * sizeof *pools, (unsigned long long)o_lcap, (unsigned long long)llen);
    J(" \"marks_mode\": %d, \"marks_n\": %llu, \"snap_n\": %llu, \"snap_slot\": %d, \"snap_cb\": %d, \"snap_ncb\": %d, \"pools_n\": %d, \"pools_locked\": %d,\n", g_fr_marks_on, (unsigned long long)(mlen / 16), (unsigned long long)g_fr_snap_n,
      FR_SNAP_SLOT, FR_SNAP_CB, FR_SNAP_NCB, np, plock);
    J(" \"ranges\": {\"loop_guard\": [%llu, %llu], \"zero_bank\": [%llu, %llu], \"dummy\": [%llu, %llu], \"marks\": [%llu, %llu], \"snap\": [%llu, %llu]},\n", (unsigned long long)lg, (unsigned long long)ll,
      (unsigned long long)g_fr_zb[0], (unsigned long long)g_fr_zb[1], (unsigned long long)g_fr_dummy[0], (unsigned long long)g_fr_dummy[1],
      (unsigned long long)g_fr_marks_gpu, (unsigned long long)mlen, (unsigned long long)g_fr_snap_gpu, (unsigned long long)slen);
    J(" \"loop_guard_words\": ["); for (int i = 0; i < 16; i++) J("%s%u", i ? ", " : "", lw[i]); J("], \"loop_cap_set\": %u,\n", g_lcap_cap);
    J(" \"ok\": %d\n}\n", ok);
    #undef J
    snprintf(path + strlen(path) - 4, 6, ".json");
    int jf = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (jf >= 0) { fr_write_all(jf, js, (size_t)o); close(jf); }
    path[strlen(path) - 5] = 0;
    if (sig) { char b[1400]; int n = snprintf(b, sizeof b, "[dm] [flightrec] %s dump %s.bin (%s)\n", ok ? "wrote" : "FAILED", path, why); if (write(2, b, (size_t)n) < 0) {} }
    else MLOG("[RED] [flightrec] %s dump %d (%s): %s.bin + .json, records up to #%llu, fence %llu done of %llu, %.1f ms", ok ? "wrote" : "FAILED to write", k, why, path, (unsigned long long)top1,
              (unsigned long long)done, (unsigned long long)fence, (fr_ns() - t0) / 1e6);
    atomic_store(&g_fr_busy, 0);
}
static void fr_dump_async(const char *why) { if (g_fr_on <= 0) return; dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ fr_dump(why, 0); }); }
void dm_gfx_flight_dump(const char *tag) {   // DM_CTL "flightdump [tag]" (hle_hid.c)
    char w[40] = "ctl"; if (tag && *tag) { int i = 0; for (; i < 36 && tag[i] && tag[i] != '\n'; i++) w[i] = isalnum((unsigned char)tag[i]) || tag[i] == '-' || tag[i] == '_' ? tag[i] : '_'; w[i] = 0; }
    if (g_fr_on <= 0) { MLOG("DM_CTL flightdump: the flight recorder is off (DM_NVN_FLIGHTREC=<N>)"); return; }
    fr_dump(w, 0);
}
static void fr_crash(int sig) {   // crash.c on_crash (crash_set_flight_hook): a fatal signal, after the guest fault hooks declined it and the crash state was printed
    if (sig == SIGALRM) return;   // DM_WATCH's time limit, the normal end of a timed run: not a crash
    const char *w = sig == SIGABRT ? "abort" : sig == SIGSEGV ? "segv" : sig == SIGBUS ? "bus" : sig == SIGILL ? "ill" : sig == SIGTRAP ? "trap" : sig == SIGALRM ? "alarm" : "signal";
    fr_dump(w, 1);
}
