// Real submission on a replay worker: its production index phase is accumulated
// in TLS, and commit must report/reset it on that worker. No game data or shaders.
#include "../nvn/nvn_mtl.m"
#include <unistd.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int stage;
static uint64_t calls_before_stats, calls_after_stats, calls_after_commit;
static uint64_t report_before, report_after, fence;
static int reset_all, completed;
static void *replay(void *unused) { (void)unused; @autoreleasepool {
    mtl_sub_begin();
    // Select an already elapsed report window without sleeping or bypassing the
    // production phase/commit APIs. The only injected state is the report clock.
    g_cp_last = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - 6000000000ull;
    report_before = g_cp_last;
    for (int i = 0; i < 7; i++) mtl_sub_index(NULL, 0);
    pthread_mutex_lock(&mu);
    calls_before_stats = g_cp_calls[7]; stage = 1; pthread_cond_broadcast(&cv);
    while (stage != 2) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    calls_after_stats = g_cp_calls[7];
    fence = mtl_sub_commit();
    calls_after_commit = g_cp_calls[7]; report_after = g_cp_last;
    reset_all = 1;
    for (int i = 0; i < 8; i++) reset_all &= !g_cp_calls[i] && !g_cp_cpu[i] && !g_cp_wall[i];
    completed = mtl_wait(fence, 5000000000ull) && !mtl_last_commit_error();
} return NULL; }

int main(void) { @autoreleasepool {
    alarm(30);
    setenv("DM_NVN_CPUPROF", "1", 1);
    setenv("DM_NVN_ARCHIVE", "0", 1); setenv("DM_NVN_PREBUILD", "0", 1);
    setenv("DM_NVN_PSO_MANIFEST", "0", 1); setenv("DM_NVN_PSO_USER", "/dev/null", 1);
    mtl_init();
    pthread_t thread; if (pthread_create(&thread, NULL, replay, NULL)) return 2;
    pthread_mutex_lock(&mu);
    while (stage != 1) pthread_cond_wait(&cv, &mu);
    CHECK(calls_before_stats == 7, "production indexing accumulated seven calls on the replay worker");
    // The worker is parked, so neither the stats globals nor TLS observations
    // race another encoder. Stats must not trigger even an empty caller report.
    uint64_t main_window = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - 6000000000ull;
    g_cp_last = main_window;
    uint64_t passes = UINT64_MAX, draws = UINT64_MAX;
    mtl_stats(&passes, &draws);
    CHECK(g_cp_last == main_window, "reading guest statistics does not report the caller's unrelated TLS window");
    CHECK(g_cp_calls[7] == 0, "guest statistics do not copy replay TLS into the reader");
    CHECK(passes == g_stat_pass && draws == g_stat_draw, "the ordinary statistics result is unchanged");
    stage = 2; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    if (pthread_join(thread, NULL)) return 2;
    CHECK(calls_after_stats == 7, "guest statistics leave the parked replay counters intact");
    CHECK(calls_after_commit == 0 && reset_all, "production commit reports and resets its own phase counters");
    CHECK(report_after > report_before, "commit advances the owning worker's report window");
    CHECK(fence && completed, "the worker's real GPU submission completes without error");
    CHECK(!pthread_cond_destroy(&cv) && !pthread_mutex_destroy(&mu), "fixture synchronization retires after the worker joins");
    printf("t_cpuprof: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
