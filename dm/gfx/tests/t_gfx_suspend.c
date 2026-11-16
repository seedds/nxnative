// CPU-only lifecycle regression: the real NVN submit entry and graphics gate, with the prepare step (mtl_prepare) held by semaphores and the Metal replay and drain stubbed.
// Going to the background while the first submission prepares must not wait for it, and a second submission queued behind it must not hold the suspend either (docs/reference/nvn-objects.md,
// "Going to the background during the prepare step").
// The suspend's limit (hle_nvn.c dm_gfx_suspend, after Ocarina of Time's test 3 froze and the background path never returned): with a graphics call that never returns (a faked
// in-flight count), a drain held where no wait has a limit, and a drain whose waits end at the limit (nvn_mtl.m mtl_drain_by, stubbed here), going to the background returns in
// about 3 s with "[RED] suspend: the GPU did not drain in 3 s" naming what, and the dump; graphics stay suspended; a resume while it still does not drain keeps them suspended
// within about 1 s and says so; once the drain finishes, the resume is a normal one, and a resume that could not drain is tried again in the foreground until it does (a suspend
// ends that). t_suspend_drain checks mtl_drain_by's own waits with real Metal
#include <pthread.h>
#include <stdatomic.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

static int observed_mutex_lock(pthread_mutex_t *mutex);
#define pthread_mutex_lock observed_mutex_lock
#define mtl_prepare test_prepare
#define mtl_sub_begin test_sub_begin
#define mtl_sub_commit test_sub_commit
#define mtl_queue_activate test_queue_activate
#define mtl_set_queue_origin_lower_left test_queue_origin
#define mtl_exec_rest test_exec_rest
#define mtl_exec test_exec
#define mtl_set_frame test_set_frame
#define mtl_drain_by test_drain_by
#include "../nvn/hle_nvn.c"
#undef pthread_mutex_lock
#undef mtl_prepare
#undef mtl_sub_begin
#undef mtl_sub_commit
#undef mtl_queue_activate
#undef mtl_set_queue_origin_lower_left
#undef mtl_exec_rest
#undef mtl_exec
#undef mtl_set_frame
#undef mtl_drain_by

module_t *g_mod; int g_nmod;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }   // the host's clock (hle_os.c: Mach absolute time), which Metal's timestamps are on

static dispatch_semaphore_t preparing, second_waiting, finish_prepare, prepared, suspended, returned, committed;
static __thread int second_submit;
static _Atomic int prepares, commits, drains;
static int inflight_in_prepare, host_work_in_prepare, inflight_at_serial_wait, host_work_at_serial_wait;
static int observed_mutex_lock(pthread_mutex_t *mutex) {
    if (mutex == &g_sub_mu && second_submit) {   // reached only when the lock is contended (dm_host_mutex_lock tries first)
        pthread_mutex_lock(&g_susp_mu);
        inflight_at_serial_wait = g_inflight_api; host_work_at_serial_wait = dm_host_work;
        pthread_mutex_unlock(&g_susp_mu);
        dispatch_semaphore_signal(second_waiting);
    }
    return pthread_mutex_lock(mutex);
}
void test_prepare(void) {   // the first submission's prepare step
    if (atomic_fetch_add(&prepares, 1)) return;
    pthread_mutex_lock(&g_susp_mu);
    inflight_in_prepare = g_inflight_api; host_work_in_prepare = dm_host_work;
    pthread_mutex_unlock(&g_susp_mu);
    dispatch_semaphore_signal(preparing);
    dispatch_semaphore_wait(finish_prepare, DISPATCH_TIME_FOREVER);
    dispatch_semaphore_signal(prepared);
}
void test_sub_begin(void) {}
uint64_t test_sub_commit(void) { atomic_fetch_add(&commits, 1); dispatch_semaphore_signal(committed); return 0; }
void test_queue_activate(uint64_t serial) { (void)serial; }
void test_queue_origin(int on) { (void)on; }
void test_exec_rest(const uint64_t *handles, int count) { (void)handles; (void)count; }
void test_exec(const uint8_t *commands) { (void)commands; }
void test_set_frame(uint64_t frame) { (void)frame; }
// The drain: counted; held (drain_hold 1: until drain_go, as one held by a lock) or ending at its limit with a step's name (drain_hold 2: as mtl_drain_by's waits do)
static _Atomic int drain_hold; static dispatch_semaphore_t drain_go;
const char *test_drain_by(uint64_t by) {
    atomic_fetch_add(&drains, 1);
    int hold = atomic_load(&drain_hold);
    if (hold == 1) { dispatch_semaphore_wait(drain_go, DISPATCH_TIME_FOREVER); return NULL; }
    if (hold == 2) { uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); if (by > now) usleep((useconds_t)((by - now) / 1000)); return "the GPU work up to the last fence (mtl_finish_gpu)"; }
    return NULL;
}

static NVNqueue queues[2];
static const op_hdr work[] __attribute__((aligned(8))) = {{OP_BEGIN, 8}};
static uint64_t work_list;   // its control list: one piece (nvn_obj.h ctl_e)
static void *submit(void *which) {
    second_submit = which != NULL;
    work_list = ctl_e(CTL_CHUNK, work, sizeof work) | CTL_LAST;
    NVNcommandHandle handle = (uintptr_t)&work_list;
    h_nvnQueueSubmitCommands(&queues[second_submit], 1, &handle);
    dispatch_semaphore_signal(returned);
    return NULL;
}
static void *suspend_graphics(void *unused) {
    (void)unused; dm_gfx_suspend(1); dispatch_semaphore_signal(suspended); return NULL;
}
static int checks, failures;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failures += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)
// What the host logs (stderr) while fn runs, then for settle_ms more (the dump runs on a utility queue), and how long fn took (s)
static char g_out[32768];
static double logged(void (*fn)(int), int arg, int settle_ms) {
    char path[] = "/tmp/t_gfx_suspend.XXXXXX"; int fd = mkstemp(path); g_out[0] = 0; if (fd < 0) { fn(arg); return -1; }
    fflush(stderr); int saved = dup(2); if (saved < 0) { unlink(path); close(fd); fn(arg); return -1; }
    dup2(fd, 2);
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); fn(arg); double took = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e9;
    usleep((useconds_t)settle_ms * 1000);
    fflush(stderr); dup2(saved, 2); close(saved);
    FILE *f = fopen(path, "r"); unlink(path); close(fd);
    // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection): no lock is held here; the checker takes a lock in the suspend's paths as still held
    if (f) { char line[4096]; while (fgets(line, sizeof line, f)) strlcat(g_out, line, sizeof g_out); fclose(f); }
    fputs(g_out, stdout);
    return took;
}
static int has(const char *s) { return strstr(g_out, s) != NULL; }
static int suspended_now(void) { pthread_mutex_lock(&g_susp_mu); int s = g_susp; pthread_mutex_unlock(&g_susp_mu); return s; }
static void wait_open(int ms) { for (int i = 0; i < ms / 10 && suspended_now(); i++) usleep(10000); }   // up to ms for the gate to open (the resume's retry)
static void fake_call(int d) { pthread_mutex_lock(&g_susp_mu); g_inflight_api += d; pthread_cond_broadcast(&g_susp_cv); pthread_mutex_unlock(&g_susp_mu); }
static int reached(dispatch_semaphore_t semaphore, const char *name) {
    int ok = dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0;
    CHECK(name, ok); return ok;
}

int main(void) {
    preparing = dispatch_semaphore_create(0); second_waiting = dispatch_semaphore_create(0);
    finish_prepare = dispatch_semaphore_create(0); prepared = dispatch_semaphore_create(0);
    suspended = dispatch_semaphore_create(0); returned = dispatch_semaphore_create(0);
    committed = dispatch_semaphore_create(0);
    NVNqueueBuilder qb = {0}; h_nvnQueueBuilderSetDefaults(&qb);
    h_nvnQueueInitialize(&queues[0], &qb); h_nvnQueueInitialize(&queues[1], &qb);
    pthread_t first, second, background;
    pthread_create(&first, NULL, submit, NULL);
    if (!reached(preparing, "the first submission prepares")) return 1;
    CHECK("the prepare step runs outside the graphics gate", inflight_in_prepare == 0);
    CHECK("the prepare step is host work for the guest scheduler", host_work_in_prepare > 0);
    pthread_create(&second, NULL, submit, (void *)1);
    if (!reached(second_waiting, "a second submission waits for the submit lock")) return 1;
    CHECK("a queued submission is not in-flight graphics work", inflight_at_serial_wait == 0);
    CHECK("a contended submit lock is host work for the guest scheduler", host_work_at_serial_wait > 0);
    if (failures) return 1;   // the suspend below would wait forever
    pthread_create(&background, NULL, suspend_graphics, NULL);
    if (!reached(suspended, "suspend completes while the prepare step holds the submit lock")) return 1;
    pthread_join(background, NULL);
    CHECK("suspend drains graphics once", atomic_load(&drains) == 1);
    dispatch_semaphore_signal(finish_prepare);
    if (!reached(prepared, "the prepare step finishes in the background")) return 1;
    // Submit now owns a queued continuation and may return before GPU replay.
    // Suspension constrains replay, not whether the guest has enqueued work.
    CHECK("queued work stays outside the GPU while suspended", dispatch_semaphore_wait(committed, dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC)) != 0);
    CHECK("no submission commits while graphics are suspended", atomic_load(&commits) == 0);
    dm_gfx_suspend(0);
    if (!reached(returned, "a submission returns after resume") || !reached(returned, "both submissions return after resume")) return 1;
    pthread_join(first, NULL); pthread_join(second, NULL);
    if (!reached(committed, "first queued submission commits after resume") || !reached(committed, "second queued submission commits after resume")) return 1;
    CHECK("both submissions commit", atomic_load(&commits) == 2);
    h_nvnQueueFinish(&queues[0]); h_nvnQueueFinish(&queues[1]);
    CHECK("graphics entries are balanced", g_inflight_api == 0);

    // An unencoded wait must not make suspend wait for a future producer.
    // Releasing that event in the background must still respect the closed gate.
    _Atomic uint32_t word = 0;
    struct { op_hdr wait; volatile uint32_t *word; uint32_t mode, value; op_hdr begin; } parked __attribute__((aligned(8))) = {
        {OP_WAIT_EVENT, 24}, (volatile uint32_t *)&word, 0, 1, {OP_BEGIN, 8}
    };
    static uint64_t parked_list; parked_list = ctl_e(CTL_CHUNK, &parked, sizeof parked) | CTL_LAST;
    NVNcommandHandle parked_handle = (uintptr_t)&parked_list;
    h_nvnQueueSubmitCommands(&queues[0], 1, &parked_handle);
    pthread_create(&background, NULL, suspend_graphics, NULL);
    if (!reached(suspended, "suspend returns with an unresolved queue event")) return 1;
    pthread_join(background, NULL);
    nvn_event_publish((volatile uint32_t *)&word, NVN_EVENT_SIGNAL_MODE_WRITE, 1);
    CHECK("an event release cannot submit GPU work in the background", dispatch_semaphore_wait(committed, dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC)) != 0);
    dm_gfx_suspend(0);
    if (!reached(committed, "the released continuation commits after resume")) return 1;
    h_nvnQueueFinish(&queues[0]);
    CHECK("exactly the three intended command prefixes committed", atomic_load(&commits) == 3);
    CHECK("resume leaves the graphics gate balanced", g_inflight_api == 0);

    // The suspend's limit. A graphics call that never returns: the background path returns in about 3 s, red line and dump, nothing drained, the gate closed
    drain_go = dispatch_semaphore_create(0);
    int d0 = atomic_load(&drains);
    fake_call(1);
    double took = logged(dm_gfx_suspend, 1, 300);
    CHECK("a call that never returns: the suspend returns at its 3 s limit", took >= 2.9 && took < 3.5);
    CHECK("the red line names the call in progress", has("[RED] suspend: the GPU did not drain in 3 s (at ") && has("still waiting for 1 graphics call in progress"));
    CHECK("the dump follows (the graphics wait points without the detector)", has("wait points: main fence") && has("[nvn] suspend: dump end"));
    CHECK("no drain is started behind a call in progress", atomic_load(&drains) == d0);
    CHECK("graphics stay suspended", suspended_now() && g_susp_stuck);
    took = logged(dm_gfx_suspend, 0, 0);
    CHECK("a resume while it still does not drain returns in about 1 s", took >= 0.9 && took < 1.5);
    CHECK("and says graphics stay suspended", has("[RED] resume: the GPU has still not drained (1 graphics call in progress") && !has("graphics resumed"));
    CHECK("the gate stays closed", suspended_now());
    fake_call(-1);   // the call returns: the next resume drains and resumes as normal
    took = logged(dm_gfx_suspend, 0, 0);
    CHECK("once it drains, the resume is a normal one", has("has finished since") && has("[nvn] graphics resumed") && !suspended_now() && !g_susp_stuck && took < 0.5);
    CHECK("that resume drained once", atomic_load(&drains) == d0 + 1);

    // A drain held where no wait has a limit (a lock): the main thread still returns at the limit, plus the drain's grace
    atomic_store(&drain_hold, 1); d0 = atomic_load(&drains);
    took = logged(dm_gfx_suspend, 1, 300);
    CHECK("a drain that never returns: the suspend returns at its limit", took >= 2.9 && took < 3.5);
    CHECK("the red line names the held drain", has("[RED] suspend: the GPU did not drain in 3 s") && has("still waiting for the drain's thread, held where no wait has a limit"));
    CHECK("no memory trim or archive hand-over after a failed drain", !has("graphics suspended: in-flight GPU work drained"));
    took = logged(dm_gfx_suspend, 0, 0);
    CHECK("a resume with that drain still held keeps graphics suspended, in about 1 s", has("[RED] resume: the GPU has still not drained (an earlier drain, still held") && suspended_now() && took < 1.5);
    atomic_store(&drain_hold, 0); dispatch_semaphore_signal(drain_go);   // it finishes: the next resume starts a new drain behind it and resumes
    took = logged(dm_gfx_suspend, 0, 0);
    CHECK("the held drain let go: the resume is a normal one", has("has finished since") && has("[nvn] graphics resumed") && !suspended_now() && took < 0.5);
    CHECK("the held drain and the resume's drain ran", atomic_load(&drains) == d0 + 2);

    // A drain whose waits end at the limit (mtl_drain_by): the red line names the step
    atomic_store(&drain_hold, 2);
    took = logged(dm_gfx_suspend, 1, 300);
    CHECK("a drain ending at its limit: the suspend returns in about 3 s", took >= 2.9 && took < 3.5);
    CHECK("the red line names the step", has("[RED] suspend: the GPU did not drain in 3 s") && has("still waiting for the GPU work up to the last fence (mtl_finish_gpu)"));
    atomic_store(&drain_hold, 0);
    logged(dm_gfx_suspend, 0, 0);
    CHECK("resume after it drains", has("[nvn] graphics resumed") && !suspended_now());
    took = logged(dm_gfx_suspend, 1, 0);
    CHECK("a suspend that drains is as before", has("[nvn] graphics suspended: in-flight GPU work drained") && !has("[RED]") && took < 0.5);
    logged(dm_gfx_suspend, 0, 0);
    CHECK("and so is its resume", has("[nvn] graphics resumed") && !has("has finished since") && !suspended_now());
    CHECK("the limit cases leave the graphics gate balanced", g_inflight_api == 0);

    // A resume that could not drain is tried again in the foreground (susp_retry_main): once the call returns, the gate opens by itself, with no further suspend or resume
    fake_call(1);
    logged(dm_gfx_suspend, 1, 300);
    took = logged(dm_gfx_suspend, 0, 0);
    CHECK("a resume that cannot drain says it is tried again", has("[RED] resume: the GPU has still not drained") && has("tried again every second") && suspended_now() && took < 1.5);
    d0 = atomic_load(&drains);
    fake_call(-1);
    took = logged(wait_open, 3000, 0);
    CHECK("the retry opens the gate once the drain finishes", !suspended_now() && !g_susp_stuck && has("has finished since (") && has("graphics resumed") && took < 2.5);
    CHECK("the retry drained once", atomic_load(&drains) == d0 + 1);
    usleep(50000);
    CHECK("the retry's thread has ended", !g_susp_retry && !g_susp_want);

    // A suspend ends the retry: with the call still in progress the app goes to the background again, and when the call returns there the gate stays closed (no resume is owed
    // in the background) until the next resume, which is a normal one
    fake_call(1);
    logged(dm_gfx_suspend, 1, 300);
    logged(dm_gfx_suspend, 0, 0);
    took = logged(dm_gfx_suspend, 1, 300);
    CHECK("a second suspend with the call still in progress returns at its limit", took >= 2.9 && took < 3.5 && has("[RED] suspend: the GPU did not drain in 3 s"));
    fake_call(-1);
    took = logged(wait_open, 2500, 0);
    CHECK("in the background, no retry opens the gate", suspended_now() && g_susp_stuck && !has("has finished since") && !g_susp_retry && !g_susp_want);
    logged(dm_gfx_suspend, 0, 0);
    CHECK("the next resume drains and resumes", has("has finished since") && has("[nvn] graphics resumed") && !suspended_now() && !g_susp_stuck);
    CHECK("the retry cases leave the graphics gate balanced", g_inflight_api == 0);
    h_nvnQueueFinalize(&queues[0]); h_nvnQueueFinalize(&queues[1]);
    printf("graphics suspend: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
