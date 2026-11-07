// DM_NVN_FLIGHTREC (nvn_flight.inc, 2026-11-07): the flight recorder's records, GPU progress marks and snapshots, and its dumps; plus a microbench of its cost.
// GPU work: a few hundred tiny dispatches and one bounded spin kernel (it ends on a CPU flag within ~50 ms, or at its own cap) — take the GPU lock first:
//   zsh sw2_tools/gpu_lock.sh take ftest; MTL_DEBUG_LAYER=1 ./t_flightrec; ./t_flightrec bench; zsh sw2_tools/gpu_lock.sh release ftest
// ./t_flightrec cpubench submits no GPU work (the CPU cost of a record only)
// The dump it writes is kept in $FLIGHT_TEST_DIR (default /tmp/t_flightrec) for sw2_tools/flight/test_flight_analyze.py --real <dir>.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
static void finish(void) { CHECK("submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK("no GPU error", mtl_last_commit_error() == 0); }
static id<MTLComputePipelineState> pso(NSString *src, NSString *name) {
    NSError *e = nil; id<MTLLibrary> l = [g_dev newLibraryWithSource:src options:nil error:&e]; if (!l) { fprintf(stderr, "%s\n", e.localizedDescription.UTF8String); abort(); }
    id<MTLComputePipelineState> p = [g_dev newComputePipelineStateWithFunction:[l newFunctionWithName:name] error:&e]; if (!p) abort(); return p;
}
static NSString *const SRC = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "kernel void touch(device uint *w [[buffer(0)]], uint t [[thread_position_in_grid]]) { if (t == 0) w[1] += 1; }\n"
    // ~10-20 us of ALU work a dispatch (the bench's "heavy" rows: does a recorder copy overlap a dispatch that is not tiny?)
    "kernel void heavy(device uint *w [[buffer(0)]], uint t [[thread_position_in_grid]]) { uint x = t; for (uint i = 0; i < w[4]; i++) x = x * 1664525u + 1013904223u; if (x == 7u) w[5] = x; }\n"
    // bounded spin: thread 0 waits for w[0] != 0 at most w[2] polls (the cap ends it if the CPU never sets the flag), w[3] = polls made
    "kernel void spin(device atomic_uint *w [[buffer(0)]], uint t [[thread_position_in_grid]]) { if (t) return; uint cap = atomic_load_explicit(&w[2], memory_order_relaxed), i = 0;\n"
    "  for (; i < cap; i++) if (atomic_load_explicit(&w[0], memory_order_relaxed)) break; atomic_store_explicit(&w[3], i, memory_order_relaxed); }\n";
static NvShader *mkshader(const char *key) { NvShader *s = [NvShader new]; snprintf(s->key, sizeof s->key, "%s", key); s->rf.tg[0] = 32; s->rf.tg[1] = 1; s->rf.tg[2] = 1; return s; }
static const volatile uint64_t *mark(uint64_t s) { return (const volatile uint64_t *)g_fr_marks.contents + 2 * (s & g_fr_mask); }
static const uint8_t *snapslot(const fr_rec *r) { return (const uint8_t *)g_fr_snapb.contents + (uint64_t)(r->snap - 1) * FR_SNAP_SLOT; }
static void bindings(id<MTLBuffer> cb, uint64_t cbo, uint64_t guest) {   // what encode_args_ would have noted: a uniform buffer (snapshotted), a storage buffer, a texture
    g_eb_n = 0; g_fr_nt = 0;
    g_eb[0] = (eb_ent){'c', 0, 2, guest, 0x100, cb.gpuAddress, cb.length}; g_fr_x[0] = (fr_x){0, 0, 0, 0}; g_fr_mb[0] = (__bridge void *)cb; g_fr_mo[0] = cbo; g_fr_bound[0] = cb.gpuAddress + cbo;
    g_eb[1] = (eb_ent){'U', 0, 7, guest + 0x10000, 0, cb.gpuAddress, cb.length}; g_fr_x[1] = (fr_x){21, 1, 'H', 0}; g_fr_mb[1] = NULL; g_fr_bound[1] = cb.gpuAddress + 0x2000;
    g_eb[2] = (eb_ent){'p', 'D', 8, guest + 0x20000, 0x40000, dummy_buf_gpu(), 65536}; g_fr_x[2] = (fr_x){3, 1, 0, 0}; g_fr_mb[2] = NULL; g_fr_bound[2] = dummy_buf_gpu();
    g_eb_n = 3; g_fr_t[0] = (fr_tex){'t', 12, 0, 0x486}; g_fr_nt = 1;
}
static double tsec(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9; }

static int bench(int gpu) {
    // CPU: the hot-path cost of a record (marks and snapshots off: no encoder work), dispatches with 3 bindings and draws
    id<MTLBuffer> cb = [g_dev newBufferWithLength:1 << 16 options:MTLResourceStorageModeShared]; res_add(cb); res_commit();
    NvShader *cs = mkshader("52794ff6409fc9a2"); p_dispatch dp = {4, 1, 1, 0, 0};
    int mk = g_fr_marks_on, sn = g_fr_snap_on; g_fr_marks_on = g_fr_snap_on = 0;
    enum { NCPU = 2000000 }; bindings(cb, 0x100, 0x380000000ull);
    double t0 = tsec(); for (int i = 0; i < NCPU; i++) fr_dispatch(nil, cs, &dp, nil, 0, nil, 0, 0); double t1 = tsec();
    p_draw dw = {.prim = 4, .count = 3, .instances = 1}; S.prog = 0;
    double t2 = tsec(); for (int i = 0; i < NCPU; i++) fr_draw(&dw, NULL); double t3 = tsec();
    printf("bench CPU: dispatch record %.1f ns, draw record %.1f ns (%d each, ring %llu)\n", (t1 - t0) / NCPU * 1e9, (t3 - t2) / NCPU * 1e9, NCPU, (unsigned long long)g_fr_n);
    g_fr_marks_on = mk; g_fr_snap_on = sn;
    if (!gpu) return 0;
    // GPU: K tiny dispatches in one copy-encoder-style compute encoder, each behind a barrier as the host's are (dispatch_enc); the recorder's copies + end barrier on or off
    id<MTLComputePipelineState> pt = pso(SRC, @"touch"), ph = pso(SRC, @"heavy"); id<MTLBuffer> w = [g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared]; res_add(w); res_commit();
    ((uint32_t *)w.contents)[4] = 4096;
    for (int heavy = 0; heavy < 2; heavy++) { id<MTLComputePipelineState> p = heavy ? ph : pt;
    enum { K = 2000, REPS = 5 }; double best[5] = {1e9, 1e9, 1e9, 1e9, 1e9}, sum[5] = {0};
    // every 4th dispatch counts as indirect for the recorder (its counts snapshotted from cb); 1 cbuf a dispatch (bindings)
    for (int rep = 0; rep < REPS; rep++) for (int mode = 0; mode < 5; mode++) {   // 0 off; 1 marks=1 only; 2 the default (marks=1 snap=1); 3 marks=1 snap=2 (cbufs); 4 marks=2 snap=2
        static const int mk_[5] = {0, 1, 1, 1, 2}, sn_[5] = {0, 0, 1, 2, 2}; g_fr_marks_on = mk_[mode]; g_fr_snap_on = sn_[mode];
        mtl_sub_begin(); id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL); enc_c_pso(ce, p);
        for (int i = 0; i < K; i++) {
            if (i) [ce barrierAfterEncoderStages:MTLStageDispatch | MTLStageBlit beforeEncoderStages:MTLStageDispatch | MTLStageBlit visibilityOptions:resource_vis()];
            enc_c_args(ce, w.gpuAddress, 0);
            uint64_t s = mode ? fr_dispatch(ce, cs, &dp, (i & 3) ? nil : cb, 0, (i & 3) ? nil : cb, 64, 0) : 0;
            enc_c_dispatch(ce, MTLSizeMake(4, 1, 1), MTLSizeMake(32, 1, 1));
            fr_dispatch_end(ce, s);
        }
        enc_c_end(ce); double a = tsec(); finish(); double d = tsec() - a;
        if (d < best[mode]) best[mode] = d; sum[mode] += d;
    }
    g_fr_marks_on = mk; g_fr_snap_on = sn;
    const char *nm[5] = {"off", "marks=1", "default", "snap=2", "marks=2"};
    for (int m = 0; m < 5; m++) printf("bench GPU %s %-10s: %d dispatches best %.2f ms mean %.2f ms = %.2f us/dispatch (+%.2f us vs off)\n", heavy ? "heavy" : "tiny ", nm[m], K, best[m] * 1e3, sum[m] / REPS * 1e3, best[m] / K * 1e6, (best[m] - best[0]) / K * 1e6);
    }
    return 0;
}

int main(int argc, char **argv) { @autoreleasepool {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *dir = getenv("FLIGHT_TEST_DIR"); if (!dir) dir = "/tmp/t_flightrec";
    mkdir(dir, 0755);
    const char *fn = getenv("FLIGHT_N"); setenv("DM_NVN_FLIGHTREC", fn ? fn : "1000", 1); setenv("DM_NVN_FLIGHTREC_DIR", dir, 1); setenv("DM_NVN_FLIGHTREC_SNAPN", "256", 1); setenv("DM_NVN_FLIGHTREC_MAXDUMPS", "8", 1); setenv("DM_NVN_FLIGHTREC_MARKS", "2", 1); setenv("DM_NVN_FLIGHTREC_SNAP", "2", 1);   // exact marks for the checks; section 1b and the bench switch to the default (1)
    mtl_init();
    CHECK("recorder on", fr_init() == 1); CHECK("ring rounded to a power of 2", fn || g_fr_n == 1024); CHECK("marks on", g_fr_marks_on && g_fr_marks); CHECK("snapshots on", g_fr_snap_on && g_fr_snap_n == 256);
    if (argc > 1 && !strcmp(argv[1], "cpucheck")) {   // the records of section 1, encoded and never committed: no GPU work
        id<MTLBuffer> cb = [g_dev newBufferWithLength:1 << 16 options:MTLResourceStorageModeShared]; res_add(cb); res_commit();
        NvShader *cs = mkshader("52794ff6409fc9a2"); mtl_sub_begin(); id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL);
        bindings(cb, 0x100, 0x380000000ull); p_dispatch d1 = {4, 1, 1, 0x1234, 0}; uint64_t s1 = fr_dispatch(ce, cs, &d1, nil, 0, nil, 0, 0);
        const fr_rec *r = &g_fr[s1 & g_fr_mask];
        printf("cpucheck: seq %llu nb %d snap %u flags %#x\n", (unsigned long long)r->seq, r->nb, r->snap, r->flags);
        for (int i = 0; i < r->nb; i++) printf("  b%d kind %c flag %d aid %d how %d reg %d rw %d snapi %d words %d size %#x off %#x guest %#llx gpu %#llx len %#x\n", i, r->b[i].kind, r->b[i].flag, r->b[i].aid, r->b[i].how,
            r->b[i].reg, r->b[i].rw, r->b[i].snapi, r->b[i].pad, r->b[i].size, r->b[i].buf_off, (unsigned long long)r->b[i].guest, (unsigned long long)r->b[i].gpu, r->b[i].buf_len);
        CHECK("cpucheck: record 1 bindings", r->nb == 3 && r->b[0].kind == 'c' && r->b[0].aid == 2 && r->b[0].guest == 0x380000000ull && r->b[0].buf_off == 0x100 && r->b[0].size == 0x100 && r->b[0].snapi == 1);
        CHECK("cpucheck: snapshot slot", r->snap != 0);
        enc_c_end(ce); printf("%s: %d checks, %d failed\n", failed ? "FAIL" : "PASS", checks, failed); _exit(failed != 0); }
    if (argc > 1 && (!strcmp(argv[1], "bench") || !strcmp(argv[1], "cpubench"))) { int r = bench(!strcmp(argv[1], "bench")); printf("%s: %d checks, %d failed\n", failed ? "FAIL" : "PASS", checks, failed); return r || failed; }
    id<MTLComputePipelineState> touch = pso(SRC, @"touch"), spin = pso(SRC, @"spin");
    id<MTLBuffer> cb = [g_dev newBufferWithLength:1 << 16 options:MTLResourceStorageModeShared], w = [g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared],
                  ind = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    res_add(cb); res_add(w); res_add(ind); res_commit();
    for (int i = 0; i < 16384; i++) ((uint32_t *)cb.contents)[i] = 0xc0de0000u + (uint32_t)i;
    uint32_t raw[3] = {70000, 1, 1}, cl[3] = {65535, 1, 1}; memcpy(ind.contents, raw, 12); memcpy((uint8_t *)ind.contents + 64, cl, 12);
    NvShader *cs = mkshader("52794ff6409fc9a2"), *cs2 = mkshader("9afa34b0c2072bfb");
    // 1. a direct and an indirect dispatch: records, snapshots, marks
    mtl_sub_begin(); id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL); enc_c_pso(ce, touch); enc_c_args(ce, w.gpuAddress, 0);
    bindings(cb, 0x100, 0x380000000ull); p_dispatch d1 = {4, 1, 1, 0x1234, 0};
    uint64_t s1 = fr_dispatch(ce, cs, &d1, nil, 0, nil, 0, 0); enc_c_dispatch(ce, MTLSizeMake(4, 1, 1), MTLSizeMake(32, 1, 1)); fr_dispatch_end(ce, s1);
    [ce barrierAfterEncoderStages:MTLStageDispatch | MTLStageBlit beforeEncoderStages:MTLStageDispatch | MTLStageBlit visibilityOptions:resource_vis()];
    bindings(cb, 0x200, 0x390000000ull); p_dispatch d2 = {0, 0, 0, 0x5678, 0x3a0000040ull};
    uint64_t s2 = fr_dispatch(ce, cs2, &d2, ind, 0, ind, 64, FRF_IND | FRF_CLAMP); enc_c_dispatch(ce, MTLSizeMake(2, 1, 1), MTLSizeMake(32, 1, 1)); fr_dispatch_end(ce, s2);
    enc_c_end(ce); finish();
    CHECK("serials", s1 == 1 && s2 == 2 && atomic_load(&g_fr_top) == 2);
    const fr_rec *r1 = &g_fr[s1 & g_fr_mask], *r2 = &g_fr[s2 & g_fr_mask];
    CHECK("record 1 complete", r1->seq == 1 && r1->seq_end == 1 && r1->kind == 1 && !memcmp(r1->key, "52794ff6409fc9a2", 16));
    CHECK("record 1 grid / threadgroup", r1->x == 4 && r1->y == 1 && r1->z == 1 && r1->tgx == 32 && r1->ind == 0);
    CHECK("record 1 bindings", r1->nb == 3 && r1->b[0].kind == 'c' && r1->b[0].aid == 2 && r1->b[0].guest == 0x380000000ull && r1->b[0].buf_off == 0x100 && r1->b[0].size == 0x100 && r1->b[0].snapi == 1);
    CHECK("record 1 SBUFU span flags", r1->b[1].kind == 'U' && r1->b[1].how == 'H' && r1->b[1].reg == 21 && r1->b[1].rw == 1 && r1->b[1].buf_off == 0x2000 && r1->b[1].gpu == cb.gpuAddress + 0x2000);
    CHECK("record 1 dummy binding", r1->b[2].flag == 'D' && r1->b[2].gpu == dummy_buf_gpu() && r1->b[2].snapi == 0);
    CHECK("record 1 texture", r1->nt == 1 && r1->t[0].kind == 't' && r1->t[0].aid == 12 && r1->t[0].tid == 0x486);
    CHECK("record 2 indirect", r2->ind == 0x3a0000040ull && (r2->flags & FRF_IND) && r2->cnt[0] == 70000 && r2->cnt[1] == 1);
    CHECK("marks: both finished", mark(s1)[0] == s1 && mark(s1)[1] == s1 && mark(s2)[0] == s2 && mark(s2)[1] == s2);
    CHECK("snapshot slots", r1->snap && r2->snap && r1->snap != r2->snap);
    { const uint32_t *a = (const uint32_t *)snapslot(r1); CHECK("snapshot 1 cbuf words", a[16] == 0xc0de0000u + 0x40 && a[16 + 59] == 0xc0de0000u + 0x40 + 59); CHECK("snapshot 1 owner", a[8] == 1 && a[10] == 1); }
    { const uint32_t *a = (const uint32_t *)snapslot(r2); CHECK("snapshot 2 raw counts", a[0] == 70000 && a[1] == 1 && a[2] == 1); CHECK("snapshot 2 clamped counts", a[4] == 65535);
      CHECK("snapshot 2 cbuf words", a[16] == 0xc0de0000u + 0x80); CHECK("snapshot 2 owner flags", a[8] == 2 && a[11] == 3); }
    // 1b. MARKS=1 (the default): an end mark is written before the next dispatch, behind the barrier between them; the last one has none yet
    {   g_fr_marks_on = 1; g_fr_snap_on = 1; mtl_sub_begin(); id<MTL4ComputeCommandEncoder> e = enc_c_begin(NULL); enc_c_pso(e, touch); enc_c_args(e, w.gpuAddress, 0);
        bindings(cb, 0x100, 0x380000000ull); p_dispatch d = {4, 1, 1, 0, 0};
        uint64_t a = fr_dispatch(e, cs, &d, nil, 0, nil, 0, 0); enc_c_dispatch(e, MTLSizeMake(4, 1, 1), MTLSizeMake(32, 1, 1)); fr_dispatch_end(e, a);
        [e barrierAfterEncoderStages:MTLStageDispatch | MTLStageBlit beforeEncoderStages:MTLStageDispatch | MTLStageBlit visibilityOptions:resource_vis()];   // as dispatch_enc
        uint64_t b = fr_dispatch(e, cs, &d, nil, 0, nil, 0, 0); enc_c_dispatch(e, MTLSizeMake(4, 1, 1), MTLSizeMake(32, 1, 1)); fr_dispatch_end(e, b);
        enc_c_end(e); finish();
        CHECK("marks=1: the first one's start mark", mark(a)[0] == a);
        CHECK("marks=1: the second one's mark names the first as finished", mark(b)[0] == b && mark(b)[1] == a);
        CHECK("marks=1 / snap=1: a direct dispatch takes no snapshot", g_fr[a & g_fr_mask].snap == 0 && g_fr[b & g_fr_mask].snap == 0);
        g_fr_marks_on = 2; g_fr_snap_on = 2; }
    // 2. draws: CPU records only
    { p_draw dw = {.prim = 4, .count = 36, .instances = 2, .first = 5}; p_draw_idx di = {.prim = 4, .itype = 2, .count = 99, .instances = 1, .ibuf = 0x3b0000000ull, .ind = 0x3b0001000ull, .mdi_i = 3};
      S.prog = 0; fr_draw(&dw, NULL); fr_draw(NULL, &di);
      const fr_rec *a = &g_fr[5 & g_fr_mask], *b = &g_fr[6 & g_fr_mask];
      CHECK("draw records", a->kind == 2 && a->x == 36 && a->y == 2 && a->z == 5 && a->nb == 0 && b->kind == 3 && b->ibuf == 0x3b0000000ull && b->ind == 0x3b0001000ull && b->mdi == 3 && (b->flags & FRF_IND)); }
    // 3. a dispatch still running: start mark written, end mark not. The spin polls a flag the CPU sets, bounded by its own cap (~0.2 s). A GPU write reaches the CPU late or only
    //    at completion (seen here: the start mark visible after 7-35 ms in most runs, at the end in some), so up to 3 tries; each try's latency is printed
    {   uint32_t *ww = (uint32_t *)w.contents; int got = 0, dumped = 0;
        for (int tr = 0; tr < 3 && !got; tr++) {
            ww[0] = 0; ww[2] = 1u << 21; ww[3] = 0xffffffffu;
            mtl_sub_begin(); id<MTL4ComputeCommandEncoder> e = enc_c_begin(NULL); enc_c_pso(e, spin); enc_c_args(e, w.gpuAddress, 0);
            bindings(cb, 0x100, 0x380000000ull); p_dispatch d3 = {1, 1, 1, 0x9abc, 0};
            uint64_t s3 = fr_dispatch(e, cs2, &d3, nil, 0, nil, 0, 0); enc_c_dispatch(e, MTLSizeMake(1, 1, 1), MTLSizeMake(32, 1, 1)); fr_dispatch_end(e, s3);
            enc_c_end(e); uint64_t f = mtl_sub_commit();
            int started = 0; double t0 = tsec(); while (tsec() - t0 < 0.5 && !(started = mark(s3)[0] == s3)) usleep(100);
            double tstart = tsec() - t0; usleep(2000);
            int still = started && mark(s3)[1] != s3 && ww[3] == 0xffffffffu && !mtl_wait(f, 0);
            if (still) { fr_dump("inflight", 0); dumped = 1; got = 1; }   // a dump while it runs: the analyzer must list s3 as RUNNING (test_flight_analyze.py --real)
            __atomic_store_n(&ww[0], 1u, __ATOMIC_RELEASE);
            CHECK("spin submission completed", mtl_wait(f, 5000000000ull)); CHECK("no GPU error (spin)", mtl_last_commit_error() == 0);
            double tspin = tsec() - t0;   // a CPU write reaches a running kernel late too (playbook: the CPU abort, 300-830 ms), so the cap usually ends it
            CHECK("end mark after it finished", mark(s3)[1] == s3);
            printf("spin try %d: #%llu start mark seen %d after %.1f ms, still running at the check %d, polls %u (%s), done after %.0f ms\n", tr + 1, (unsigned long long)s3, started, tstart * 1e3, still, ww[3],
                   ww[3] < ww[2] ? "ended on the CPU flag" : "ended at its cap", tspin * 1e3);
        }
        CHECK("in-flight dispatch: start mark without an end mark seen while it ran (3 tries)", got);
        if (!dumped) fr_dump("inflight", 0); }
    // 4. dumps: DM_CTL entry, SIGUSR1 (dispatch source), the signal-context path
    {   dm_gfx_flight_dump("unit test");
        int before = atomic_load(&g_fr_dumps); kill(getpid(), SIGUSR1);
        for (int i = 0; i < 200 && atomic_load(&g_fr_dumps) == before; i++) usleep(10000);
        for (int i = 0; i < 200 && atomic_load(&g_fr_busy); i++) usleep(10000);
        CHECK("SIGUSR1 dumped", atomic_load(&g_fr_dumps) == before + 1);
        fr_crash(SIGABRT);
        char p[1400]; struct stat st;
        snprintf(p, sizeof p, "%s/flight_%d_1_unit_test.bin", dir, (int)getpid()); CHECK("ctl dump bin", !stat(p, &st) && (uint64_t)st.st_size == g_fr_n * FR_REC + g_fr_n * 16 + g_fr_snap_n * FR_SNAP_SLOT + (uint64_t)g_npool * 32);
        snprintf(p, sizeof p, "%s/flight_%d_1_unit_test.json", dir, (int)getpid()); CHECK("ctl dump json", !stat(p, &st) && st.st_size > 200);
        snprintf(p, sizeof p, "%s/flight_%d_2_usr1.json", dir, (int)getpid()); CHECK("usr1 dump json", !stat(p, &st));
        snprintf(p, sizeof p, "%s/flight_%d_3_abort.bin", dir, (int)getpid()); CHECK("signal-context dump bin", !stat(p, &st) && (uint64_t)st.st_size >= g_fr_n * FR_REC);
        printf("dumps in %s (pid %d)\n", dir, (int)getpid()); }
    // 5. off: no records, nothing noted
    {   g_fr_on = 0; uint64_t top = atomic_load(&g_fr_top); p_dispatch d = {1, 1, 1, 0, 0}; CHECK("off: no record", fr_dispatch(nil, cs, &d, nil, 0, nil, 0, 0) == 0 && atomic_load(&g_fr_top) == top); }
    printf("%s: %d checks, %d failed\n", failed ? "FAIL" : "PASS", checks, failed);
    return failed != 0;
} }
