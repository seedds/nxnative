// CPU phase profiler (DM_NVN_CPUPROF=1, PR #1999): counters stay thread-local on the replay thread and report at the end of mtl_sub_commit (when no phase is open), rather than from
// mtl_stats on the presenting thread. Checks, with DM_NVN_ENC_THREAD=0 and DM_NVN_ENC_THREAD=1:
// 1. Replay phases recorded on a dedicated replay thread accumulate on that thread across submissions.
// 2. Calling mtl_stats() or tex_mtl() on another (presenting) thread emits no [cpuprof] line and does not perturb the replay thread's counters.
// 3. When the 5 s window elapses at mtl_sub_commit(), [cpuprof] reports the replay thread's non-empty phase counts and geometry counters and resets every counter cleanly in one boundary.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failed;
#define CHECK(label, ok) do { int _ok = !!(ok); checks++; if (!_ok) { failed++; fprintf(stdout, "FAIL: %s\n", label); } } while (0)

enum { SIZE = 16, POOL = 1 << 20 };
typedef struct {
    void *mem;
    pool_t pool;
    tex_t rt;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int step;
    int done;
    uint64_t snap_calls[8];
    uint64_t snap_geo_waited, snap_geo_safe;
    int snap_after_reset_nonzero;
} test_ctx;

static FILE *g_cap_file;
static int g_saved_stderr = -1;

static void log_capture_begin(void) {
    fflush(stderr);
    g_cap_file = tmpfile();
    if (!g_cap_file) abort();
    g_saved_stderr = dup(STDERR_FILENO);
    if (g_saved_stderr < 0 || dup2(fileno(g_cap_file), STDERR_FILENO) < 0) abort();
}

static void log_capture_end(char *buf, size_t cap) {
    fflush(stderr);
    if (dup2(g_saved_stderr, STDERR_FILENO) < 0) abort();
    close(g_saved_stderr);
    g_saved_stderr = -1;
    if (fseek(g_cap_file, 0, SEEK_SET)) abort();
    size_t n = fread(buf, 1, cap - 1, g_cap_file);
    if (ferror(g_cap_file)) abort();
    buf[n] = '\0';
    fclose(g_cap_file);
    g_cap_file = NULL;
}

static void run_pass_submission(test_ctx *c) {
    state_reset();
    memset(&R, 0, sizeof R);
    mtl_sub_begin();
    R.has_rt = 1;
    R.rt.n = 1;
    R.rt.color[0] = &c->rt;
    void *enc = rp_begin_ex_ref(0);
    CHECK("render pass began", enc != NULL);
    rp_end();
    uint64_t fence = mtl_sub_commit();
    CHECK("submission completed", mtl_wait(fence, 5000000000ull));
    CHECK("submission had no GPU error", !mtl_last_commit_error());
}

static void wait_step(test_ctx *c, int want) {
    pthread_mutex_lock(&c->mu);
    while (c->step < want) pthread_cond_wait(&c->cv, &c->mu);
    pthread_mutex_unlock(&c->mu);
}

static void mark_done(test_ctx *c, int val) {
    pthread_mutex_lock(&c->mu);
    c->done = val;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

static void advance_and_wait(test_ctx *c, int step) {
    pthread_mutex_lock(&c->mu);
    c->step = step;
    pthread_cond_broadcast(&c->cv);
    while (c->done < step) pthread_cond_wait(&c->cv, &c->mu);
    pthread_mutex_unlock(&c->mu);
}

static void *replay_thread(void *arg) {
    @autoreleasepool {
        test_ctx *c = arg;
        // Step 1: initial submission sets g_cp_last and accumulates replay phases without reporting yet.
        wait_step(c, 1);
        run_pass_submission(c);
        g_cp_geo_waited += 1;
        g_cp_geo_safe += 2;
        memcpy(c->snap_calls, g_cp_calls, sizeof c->snap_calls);
        c->snap_geo_waited = g_cp_geo_waited;
        c->snap_geo_safe = g_cp_geo_safe;
        mark_done(c, 1);

        // Step 2: backdate the 5 s window, run one more pass submission, and verify mtl_sub_commit() reports and resets all counters.
        wait_step(c, 2);
        CHECK("presenting thread did not perturb replay counters",
              memcmp(c->snap_calls, g_cp_calls, sizeof c->snap_calls) == 0 &&
              g_cp_geo_waited == c->snap_geo_waited &&
              g_cp_geo_safe == c->snap_geo_safe);
        g_cp_last = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - 5100000000ull;
        run_pass_submission(c);
        int nonzero = 0;
        for (int i = 0; i < 8; i++) if (g_cp_calls[i] || g_cp_cpu[i] || g_cp_wall[i]) nonzero = 1;
        if (g_cp_geo_waited || g_cp_geo_safe) nonzero = 1;
        c->snap_after_reset_nonzero = nonzero;
        mark_done(c, 2);

        // Step 3: next interval starts clean and reports only submission 3's single pass.
        wait_step(c, 3);
        g_cp_last = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - 5100000000ull;
        run_pass_submission(c);
        mark_done(c, 3);
    }
    return NULL;
}

int main(void) {
    @autoreleasepool {
        alarm(30);
        setvbuf(stdout, NULL, _IONBF, 0);
        if (!getenv("DM_NVN_CPUPROF")) setenv("DM_NVN_CPUPROF", "1", 1);
        mtl_init();
        {
            const char *e = getenv("DM_NVN_ENC_THREAD");
            if (e) CHECK("encode thread matches DM_NVN_ENC_THREAD", rec_on() == (atoi(e) > 0));
        }

        test_ctx c = {0};
        pthread_mutex_init(&c.mu, NULL);
        pthread_cond_init(&c.cv, NULL);
        if (posix_memalign(&c.mem, getpagesize(), POOL)) abort();
        memset(c.mem, 0, POOL);
        c.pool = (pool_t){.magic = MAGIC('P', 'O', 'O', 'L'), .mem = c.mem, .size = POOL, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
        mtl_pool_add(c.mem, POOL);
        nvn_dirty_add_range(c.mem, POOL);
        c.rt = (tex_t){.magic = MAGIC('T', 'E', 'X', 0), .serial = 9101, .pool = &c.pool, .off = 0, .mem = c.mem};
        c.rt.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = SIZE, .height = SIZE, .depth = 1, .levels = 1, .samples = 1};
        nvn_tex_layout(&c.rt.d, &c.rt.L);
        mtl_tex_init(&c.rt);

        pthread_t th;
        if (pthread_create(&th, NULL, replay_thread, &c)) abort();

        char logbuf[16384];
        // Step 1: initial replay submission should record phases on the replay thread and emit no report yet.
        log_capture_begin();
        advance_and_wait(&c, 1);
        log_capture_end(logbuf, sizeof logbuf);
        CHECK("first submission initializes window without reporting", strstr(logbuf, "[cpuprof]") == NULL);
        CHECK("replay thread recorded pass_begin", c.snap_calls[1] == 1);
        CHECK("replay thread recorded pass_end", c.snap_calls[0] == 1);
        CHECK("replay thread recorded texture lookups", c.snap_calls[4] > 0);

        // Presenting thread calls mtl_stats() (even with an expired local timer) and resolves the texture via tex_mtl():
        // mtl_stats() must not emit [cpuprof], and tex_mtl() on this thread must not touch the replay thread's counters.
        log_capture_begin();
        g_cp_last = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - 5100000000ull;
        uint64_t passes = 0, draws = 0;
        mtl_stats(&passes, &draws);
        id<MTLTexture> resolved_on_present = tex_mtl(&c.rt);
        mtl_stats(&passes, &draws);
        log_capture_end(logbuf, sizeof logbuf);
        CHECK("mtl_stats returns pass count", passes >= 1);
        CHECK("presenting thread resolved texture", resolved_on_present != nil);
        CHECK("presenting thread mtl_stats emits no [cpuprof]", strstr(logbuf, "[cpuprof]") == NULL);

        // Step 2: 5 s elapsed on the replay thread -> mtl_sub_commit reports both submissions' phases and resets all counters.
        log_capture_begin();
        advance_and_wait(&c, 2);
        log_capture_end(logbuf, sizeof logbuf);
        CHECK("mtl_sub_commit emits [cpuprof] header", strstr(logbuf, "[cpuprof] inclusive phases over ") != NULL);
        CHECK("report includes pass_end 2 calls", strstr(logbuf, "pass_end 2 calls ") != NULL);
        CHECK("report includes pass_begin 2 calls", strstr(logbuf, "pass_begin 2 calls ") != NULL);
        CHECK("report includes texture calls", strstr(logbuf, "texture ") != NULL);
        CHECK("report includes geometry CPU reads", strstr(logbuf, "[cpuprof] geometry CPU reads: waited 1 / proven safe 2") != NULL);
        CHECK("all replay counters reset cleanly at report boundary", !c.snap_after_reset_nonzero);

        // Step 3: next interval reports only submission 3's single pass and no stale geometry reads.
        log_capture_begin();
        advance_and_wait(&c, 3);
        log_capture_end(logbuf, sizeof logbuf);
        CHECK("next interval reports pass_end 1 calls", strstr(logbuf, "pass_end 1 calls ") != NULL);
        CHECK("next interval reports pass_begin 1 calls", strstr(logbuf, "pass_begin 1 calls ") != NULL);
        CHECK("next interval has no stale geometry line", strstr(logbuf, "geometry CPU reads:") == NULL);

        pthread_join(th, NULL);
        mtl_tex_release(&c.rt);
        mtl_alias_drop_pool(&c.pool);
        nvn_dirty_remove_range(c.mem);
        mtl_pool_del(c.mem);
        mtl_drain();
        free(c.mem);
        pthread_mutex_destroy(&c.mu);
        pthread_cond_destroy(&c.cv);

        printf("t_cpuprof (enc_thread=%d): %d checks, %d failed\n", rec_on(), checks, failed);
        return failed ? 1 : 0;
    }
}
