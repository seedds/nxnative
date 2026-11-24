// Residency changes and their commit: res_add / res_remove only queue a change, and res_commit applies and commits what is queued under a lock of its own. A commit that adds
// hundreds of MB takes tens of milliseconds; while it held the lock res_add takes, every res_add on any thread waited for it (Switch Sports Resort's court load: the submit
// thread's res_add waited 20-43 ms in one frame for the upload batches' completion handlers). Checks, with real Metal allocations:
//  - a res_add or res_remove while another thread's commit is under way returns at once (it waited for the whole commit before);
//  - after res_commit returns, what was added before it is in the set, also when another thread's commit took the change;
//  - a removed allocation stays alive until the commit that takes it out of the set, then goes.
// Slow commits report the allocations or call site and the thread, sampled after the first 20.
// A held commit lock delays upload staging retirement and slot reuse while later feedback continues.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; printf("FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
// stderr (MLOG) goes to an unlinked file for the whole run; log_new() returns what was written since its last call (and echoes it)
static char g_log[1 << 20]; static int g_logfd = -1; static off_t g_logoff;
static const char *log_new(void) {
    fflush(stderr); ssize_t n = g_logfd < 0 ? 0 : pread(g_logfd, g_log, sizeof g_log - 1, g_logoff); if (n < 0) n = 0;
    g_log[n] = 0; g_logoff += n; fputs(g_log, stdout); return g_log;
}
static int count(const char *s, const char *w) { int n = 0; for (const char *p = s; (p = strstr(p, w)); p += strlen(w)) n++; return n; }
static int wait_signal(dispatch_semaphore_t s) { return dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, 2 * (int64_t)NSEC_PER_SEC)) == 0; }
// The commit lock held on a thread of its own, as a long commit holds it, until release is signalled; returns once it is held (the returned semaphore: it let go)
static dispatch_semaphore_t hold_lock(dispatch_semaphore_t release) {
    dispatch_semaphore_t held = dispatch_semaphore_create(0), done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        pthread_mutex_lock(&g_res_commit_mu); dispatch_semaphore_signal(held); dispatch_semaphore_wait(release, DISPATCH_TIME_FOREVER); pthread_mutex_unlock(&g_res_commit_mu); dispatch_semaphore_signal(done); });
    dispatch_semaphore_wait(held, DISPATCH_TIME_FOREVER);
    return done;
}
// The feedback queue held by a block of its own until the returned semaphore is signalled
static dispatch_semaphore_t gate_feedback(void) {
    dispatch_semaphore_t gate = dispatch_semaphore_create(0), in = dispatch_semaphore_create(0);
    dispatch_async(g_feedback_q, ^{ dispatch_semaphore_signal(in); dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER); });
    dispatch_semaphore_wait(in, DISPATCH_TIME_FOREVER);
    return gate;
}
static int feedback_runs(void) { dispatch_semaphore_t s = dispatch_semaphore_create(0); dispatch_async(g_feedback_q, ^{ dispatch_semaphore_signal(s); }); return wait_signal(s); }

enum { BIG_BYTES = 48u << 20 };

static id<MTLBuffer> buf(size_t n) {
    id<MTLBuffer> b = [g_dev newBufferWithLength:n options:MTLResourceStorageModePrivate];
    if (!b) abort();
    return b;
}

int main(void) { @autoreleasepool {
    alarm(120); setvbuf(stdout, NULL, _IONBF, 0);
    { char path[] = "/tmp/t_res_commit.XXXXXX"; g_logfd = mkstemp(path); if (g_logfd >= 0) { unlink(path); fflush(stderr); dup2(g_logfd, 2); } }
    setenv("DM_NVN_ENC_THREAD", "0", 1);   // an upload batch's queue commit is rec_commit_upload's own, not the encode thread's
    mtl_init();
    CHECK("the slow commit threshold is 100 ms", g_slow_commit_ns == 100000000ull);
    CHECK("a residency set", g_res != nil);

    // Added, committed, in the set
    id<MTLBuffer> a = buf(1u << 20);
    res_add(a); res_commit();
    CHECK("an allocation added and committed is in the set", [g_res containsAllocation:a]);

    // Removed: alive until its commit, gone after it
    __weak id<MTLBuffer> weak_r = nil;
    @autoreleasepool { id<MTLBuffer> r = buf(1u << 20); weak_r = r; res_add(r); res_commit(); res_remove(r); }
    CHECK("a removed allocation stays alive until its removal is committed", weak_r != nil);
    @autoreleasepool { res_commit(); }
    CHECK("and goes once it is", weak_r == nil);

    // A res_add while another thread's commit is under way: a commit alone takes well under a millisecond here, but tens of milliseconds in a game whose GPU is busy (10-25 ms
    // each in Switch Sports Resort's court load), so one is stood in for by holding the commit's lock for 100 ms, as a commit holds it
    NSMutableArray *small = [NSMutableArray array];
    dispatch_semaphore_t held = dispatch_semaphore_create(0), released = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        pthread_mutex_lock(&g_res_commit_mu); dispatch_semaphore_signal(held); usleep(100000); pthread_mutex_unlock(&g_res_commit_mu); dispatch_semaphore_signal(released);
    });
    dispatch_semaphore_wait(held, DISPATCH_TIME_FOREVER);
    uint64_t worst = 0, t_all = bp_now();
    for (int i = 0; i < 200; i++) { id<MTLBuffer> s = buf(4096); [small addObject:s]; uint64_t t0 = bp_now(); res_add(s); res_remove(s); res_add(s); uint64_t d = bp_now() - t0; if (d > worst) worst = d; }
    uint64_t adds = bp_now() - t_all;
    printf("200 res_add / res_remove while a commit holds its lock: %.2f ms in all, the slowest %.3f ms\n", adds / 1e6, worst / 1e6);
    CHECK("res_add and res_remove do not wait for a commit under way", adds < 50000000ull);
    uint64_t t_c = bp_now(); res_commit(); uint64_t waited = bp_now() - t_c;
    CHECK("res_commit waits for the commit under way, then commits its own changes", waited > 20000000ull && dispatch_semaphore_wait(released, DISPATCH_TIME_NOW) == 0);
    int all = 1; for (id<MTLBuffer> s in small) all &= [g_res containsAllocation:s];
    CHECK("everything added is in the set after the commit", all);

    // A change another thread's commit took is in the set (and any removal's allocation is released) when this thread's res_commit returns
    for (int round = 0; round < 50; round++) {
        id<MTLBuffer> x = buf(BIG_BYTES / 4), y = buf(4096);
        __weak id<MTLBuffer> weak_d = nil;
        @autoreleasepool { id<MTLBuffer> d = buf(BIG_BYTES / 8); weak_d = d; res_add(d); res_commit(); res_remove(d); }
        res_add(x);
        dispatch_semaphore_t go = dispatch_semaphore_create(0), fin = dispatch_semaphore_create(0);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ dispatch_semaphore_signal(go); res_commit(); dispatch_semaphore_signal(fin); });
        dispatch_semaphore_wait(go, DISPATCH_TIME_FOREVER);
        res_add(y); res_commit();
        int ok = [g_res containsAllocation:x] && [g_res containsAllocation:y] && weak_d == nil;
        dispatch_semaphore_wait(fin, DISPATCH_TIME_FOREVER);
        if (!ok) { CHECK("a change committed by another thread is in the set and released when res_commit returns", 0); break; }
        res_remove(x); res_remove(y); res_commit();
    }
    CHECK("racing commits leave nothing queued", !res_pending());
    log_new();

    // Slow residency commits (the threshold lowered to 0: every commit with changes says it): what it applied, its thread, the feedback queue
    atomic_store(&g_slow_res_commits, 0);
    atomic_store(&g_slow_queue_commits, 0);
    const char *log;
    g_slow_commit_ns = 0;
    { id<MTLBuffer> c = buf(1u << 20); res_add(c); res_commit(); log = log_new();
      CHECK("a slow residency commit's line: one add of 1 MB, no removal, not on the feedback queue", count(log, "[RED] slow residency commit:") == 1 && strstr(log, "(adds 1, 1.0 MB; removes 0, 0.0 MB) on thread ") && !strstr(log, "on the feedback queue"));
      res_remove(c); dispatch_sync(g_feedback_q, ^{ res_commit(); }); log = log_new();
      CHECK("one on the feedback queue says so", count(log, "[RED] slow residency commit:") == 1 && strstr(log, "(adds 0, 0.0 MB; removes 1, 1.0 MB)") && strstr(log, ", on the feedback queue (#2)")); }
    { id<MTLBuffer> c = buf(4096); for (int i = 0; i < 98; i++) { if (i & 1) res_remove(c); else res_add(c); res_commit(); }
      log = log_new();
      CHECK("the first 20, then every 100th: 18 more lines in the next 98, the 100th among them", count(log, "[RED] slow residency commit:") == 19 && strstr(log, "(#20)") && !strstr(log, "(#21)") && strstr(log, "(#100)")); }
    // A slow queue commit: an upload batch's, committed on this thread (no encode thread)
    { up_begin(); up_flush(); up_wait_zero(&g_up_pending, WS_DIAG); log = log_new();
      CHECK("a slow queue commit's line, with its site", count(log, "[RED] slow queue commit:") == 1 && strstr(log, "at rec_commit_upload (upload batch) on thread")); }
    g_slow_commit_ns = 100000000ull;
    log_new();

    // Gate feedback until the commits are submitted and the residency lock is held.
    dispatch_semaphore_t gate = gate_feedback();
    __weak id<MTLBuffer> weak_staging = nil;
    uint64_t gpu_ns = atomic_load(&g_upload_gpu_ns);
    up_begin();
    unsigned staging_slot = g_up_cur_slot;
    @autoreleasepool {
        id<MTLBuffer> staging = buf(4u << 20);
        weak_staging = staging;
        res_add(staging);
        [g_up_keep addObject:staging];
        [g_up_ce fillBuffer:staging range:NSMakeRange(0, staging.length) value:1];
        up_flush();
    }
    up_begin();
    unsigned empty_slot = g_up_cur_slot;
    up_flush();
    CHECK("both batches are pending", atomic_load(&g_up_pending) == 2);
    dispatch_semaphore_t release = dispatch_semaphore_create(0), done = hold_lock(release);
    dispatch_semaphore_signal(gate);
    for (int i = 0; i < 200 && (atomic_load(&g_upload_gpu_ns) == gpu_ns || atomic_load(&g_up_alloc_busy[empty_slot])); i++) usleep(10000);
    CHECK("the staging batch reached its feedback handler", atomic_load(&g_upload_gpu_ns) > gpu_ns);
    CHECK("the feedback queue runs on while the residency lock is held", feedback_runs());
    CHECK("the batch with no staging completed on the feedback queue", !atomic_load(&g_up_alloc_busy[empty_slot]));
    @autoreleasepool {
        CHECK("the staging batch holds its slot and stays pending until retirement",
              weak_staging != nil && atomic_load(&g_up_alloc_busy[staging_slot]) && atomic_load(&g_up_pending) == 1);
    }
    dispatch_semaphore_signal(release);
    CHECK("the commit lock was released", wait_signal(done));
    up_wait_zero(&g_up_pending, WS_DIAG);
    CHECK("the worker retires staging before freeing the slot", weak_staging == nil && !atomic_load(&g_up_alloc_busy[staging_slot]));
    log_new();
    for (id<MTLBuffer> s in small) res_remove(s);
    res_remove(a); res_commit();
    printf("t_res_commit: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
