// The acquire's waits on the display link, counted in its own ticks (mtl_link_wait_ticks) or in the game's 60 Hz vsyncs (mtl_link_wait_tick): a 120 Hz link takes two of its
// ticks per vsync, so present_interval display at DM_PRESENT_HZ=120 needs the first to reach one frame per tick. The real acquire also covers fractional rates and the game/step
// pacing branches. A controlled clock and timed wait deliver one tick per wake: a sleeping ticker can miss the acquire's real deadline on a busy CI runner
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>
static uint64_t test_now = 10000000000ull;
static unsigned test_waits, test_spurious;
static int test_silent;
static int test_clock_gettime(clockid_t clock, struct timespec *t) {
    (void)clock;
    *t = (struct timespec){.tv_sec = test_now / 1000000000ull, .tv_nsec = test_now % 1000000000ull};
    return 0;
}
static int test_pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
#define clock_gettime test_clock_gettime
#define pthread_cond_timedwait test_pthread_cond_timedwait
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#undef pthread_cond_timedwait
#undef clock_gettime
#define g_ft g_ft_hle
#undef DM_GAME_FPS_MAX
static int test_fps_max = 120;
#define DM_GAME_FPS_MAX test_fps_max
#include "../nvn/hle_nvn.c"
#undef g_ft
int test_screen_hz, test_capable_hz, test_low_power;   // t_link_ticks_screen.c: the screen's current / native rate (0 = not known) and Low Power Mode
static void *test_layer;   // a window only while the 40 fps screen check runs (fps40_refused); the acquires below run without one
void *front_metal_layer(void) { return test_layer; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return test_now; }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
// Only the OS wait is replaced: the production wait loop, timeout calculation,
// tick history and acquire conversion all run. No other thread advances time.
static int test_pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline) {
    if (cond != &g_link_cv || mutex != &g_link_mu) { fprintf(stderr, "unexpected timed wait\n"); abort(); }
    test_waits++;
    if (test_spurious) { test_spurious--; return 0; }
    uint64_t end = (uint64_t)deadline->tv_sec * 1000000000ull + deadline->tv_nsec;
    if (test_silent || test_now + 2000000ull >= end) { test_now = end; return ETIMEDOUT; }
    test_now += 2000000ull; g_link_tick_n++;
    return 0;
}
// Ticks per wait over 50 waits of the given kind (frames that take no time: each wait is the whole frame)
static double per_wait(int own, int n) {
    uint64_t t0 = g_link_tick_n;
    for (int i = 0; i < 50; i++) own ? mtl_link_wait_ticks(n, 100000000ull) : mtl_link_wait_tick(n, 100000000ull);
    return (g_link_tick_n - t0) / 50.0;
}
// Exercise the real acquire so the conversion from the requested frame rate to link ticks is covered too.
static double per_acquire(int hz, int display, int interval) {
    nvn_present_config(hz, display);
    win_t win = {.interval = interval}; int idx;
    uint64_t t0 = g_link_tick_n;
    for (int i = 0; i < 60; i++) h_nvnWindowAcquireTexture((NVNwindow *)&win, NULL, &idx);
    return (g_link_tick_n - t0) / 60.0;
}
static void acquire_ticks(int hz, int display, int interval, double want, const char *label) {
    double got = per_acquire(hz, display, interval);
    printf("%s: %.2f ticks per acquire (expected %.2f)\n", label, got, want);
    CHECK(got == want, label);
}
static void acquire_lateness(void) {
    char *rows = NULL;
    size_t size = 0;
    FILE *tsv = open_memstream(&rows, &size);
    CHECK(tsv != NULL, "open a capture for the acquire timing rows");
    if (!tsv) return;
    g_tsv = tsv;
    g_link_mode = 1;
    nvn_present_config(60, 0);
    win_t win = {.interval = 1, .ntex = 1}; int idx;
    // The test wait delivers each link tick 2 ms after entering the acquire.
    win.avail[0] = test_now + 3000000ull;
    h_nvnWindowAcquireTexture((NVNwindow *)&win, NULL, &idx);
    win.avail[0] = test_now + 750000ull;
    h_nvnWindowAcquireTexture((NVNwindow *)&win, NULL, &idx);
    g_tsv = NULL;
    const char *line_end = fclose(tsv) == 0 && rows ? strchr(rows, '\n') : NULL;
    const char early[] = "0\t@late_ms\t0.000\n";
    CHECK(line_end && !strncmp(rows, early, sizeof early - 1), "a link wake 1 ms before the slot logs zero lateness");
    CHECK(line_end && !strcmp(line_end + 1, "0\t@late_ms\t1.250\n"), "a link wake 1.25 ms after the slot logs 1.250 ms");
    free(rows);
}
// Deferred acquire wait (NVN semantics): AcquireTexture with a non-null NVNsync returns without blocking and executes the display-link wait in nvnSyncWait (or on the next
// AcquireTexture if the game only uses nvnQueueWaitSync). Finite timeouts do not consume the pending wait; WindowFinalize cleanly detaches it
static void deferred_acquire_sync(void) {
    g_link_mode = 1;
    nvn_present_config(60, 0);
    win_t win = {.interval = 2, .ntex = 2};
    NVNsync s;
    h_nvnSyncInitialize(&s, NULL);
    int idx = -1;
    // Align g_link_started_tick with g_link_tick_n via a baseline synchronous wait
    mtl_link_wait_ticks(1, 100000000ull);
    uint64_t t0 = g_link_tick_n; unsigned w0 = test_waits;
    CHECK(h_nvnWindowAcquireTexture((NVNwindow *)&win, &s, &idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS, "deferred acquire succeeds");
    CHECK(g_link_tick_n == t0 && test_waits == w0, "deferred acquire with NVNsync takes zero ticks and zero waits");
    CHECK(h_nvnSyncWait(&s, 0) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED, "zero-timeout SyncWait before ticks elapse returns TIMEOUT_EXPIRED");
    CHECK(g_link_tick_n == t0 && test_waits == w0, "zero-timeout SyncWait takes zero ticks and zero waits");
    CHECK(h_nvnSyncWait(&s, 1000000ull) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED, "short finite timeout before tick returns TIMEOUT_EXPIRED");
    CHECK(g_link_tick_n == t0, "short finite timeout consumes no ticks");
    CHECK(h_nvnSyncWait(&s, UINT64_MAX) == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED, "blocking SyncWait finishes the deferred acquire wait");
    CHECK(g_link_tick_n - t0 == 2, "deferred acquire at interval 2 takes two 60 Hz ticks in SyncWait");
    t0 = g_link_tick_n; w0 = test_waits;
    CHECK(h_nvnSyncWait(&s, 0) == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED, "second SyncWait returns ALREADY_SIGNALED");
    CHECK(g_link_tick_n == t0 && test_waits == w0, "second SyncWait takes zero ticks and zero waits");
    // Already elapsed ticks: zero-timeout SyncWait succeeds without blocking and advances the tick baseline
    t0 = g_link_tick_n; w0 = test_waits;
    h_nvnWindowAcquireTexture((NVNwindow *)&win, &s, &idx);
    g_link_tick_n += 2;
    CHECK(h_nvnSyncWait(&s, 0) == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED && test_waits == w0, "elapsed ticks satisfy zero-timeout SyncWait without waiting");
    // Games that never call CPU nvnSyncWait (only nvnQueueWaitSync): the next AcquireTexture drains the prior pending acquire
    t0 = g_link_tick_n;
    h_nvnWindowAcquireTexture((NVNwindow *)&win, &s, &idx);
    CHECK(g_link_tick_n == t0, "first unwaited acquire takes zero ticks");
    h_nvnWindowAcquireTexture((NVNwindow *)&win, &s, &idx);
    CHECK(g_link_tick_n - t0 == 2, "second acquire drains the first unwaited acquire (two ticks)");
    t0 = g_link_tick_n;
    CHECK(h_nvnSyncWait(&s, UINT64_MAX) == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED && g_link_tick_n - t0 == 2, "SyncWait then drains the second acquire (two ticks)");
    // WindowFinalize detaches a pending deferred acquire cleanly
    t0 = g_link_tick_n; w0 = test_waits;
    h_nvnWindowAcquireTexture((NVNwindow *)&win, &s, &idx);
    h_nvnWindowFinalize((NVNwindow *)&win);
    CHECK(h_nvnSyncWait(&s, 0) == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED && g_link_tick_n == t0 && test_waits == w0, "SyncWait after WindowFinalize is signaled with zero waits");
    h_nvnSyncFinalize(&s);
}
// Present snapshots (mtl_snap_on, fixed for the process: run as "t_link_ticks snap"): the acquire takes the display link's wait as without them, the ring slot wait only on top.
// It used to wait for a free slot alone, and the link frees slots by dropping frames, so a game ran ahead of its present interval (Donkey Kong Bananza at the 30 fps cap: 43-65 fps).
// While the link is stalled the slot time paces instead (no link wait)
static int snap_case(void) {
    setenv("DM_RT", "0", 1); setenv("DM_PRESENT_SNAP", "2", 1); setenv("DM_FPS_TARGET", "0", 1);
    g_dm_present_link = 1; g_link = (CAMetalDisplayLink *)[NSObject new]; g_link_mode = 1;
    CHECK(mtl_snap_on() && mtl_link_active() && !mtl_link_stalled(), "snapshots on, the link active and ticking");
    acquire_ticks(60, 0, 2, 2, "snapshots: game interval 2 waits two ticks of a 60 Hz link");
    acquire_ticks(60, 0, 1, 1, "snapshots: game interval 1 waits one tick");
    g_stall_t0 = 1;
    acquire_ticks(60, 0, 2, 0, "snapshots with the link stalled: no link wait");
    g_stall_t0 = 0;
    printf("t_link_ticks snap: %d checks, %d failed\n", checks, failed); return failed != 0;
}
int main(int argc, char **argv) { @autoreleasepool {
    if (argc > 1 && !strcmp(argv[1], "snap")) return snap_case();
    setenv("DM_RT", "0", 1); setenv("DM_PRESENT_SNAP", "0", 1);
    // Test uncapped display pacing independently of the selected game's default.
    setenv("DM_FPS_TARGET", "0", 1);
    g_dm_present_link = 2; g_link = (CAMetalDisplayLink *)[NSObject new];   // a link is "on" while the object is set; nothing calls it here
    CHECK(mtl_link_active(), "the link counts as active");
    g_link_mode = 2;
    CHECK(mtl_link_rate() == 120, "a 120 Hz link reports 120");
    double a = per_wait(0, 1), b = per_wait(1, 1), c = per_wait(1, 2);
    printf("120 Hz link: interval 1 waits %.2f ticks, one own tick %.2f, two own ticks %.2f\n", a, b, c);
    CHECK(a == 2, "a 60 Hz vsync is two ticks of a 120 Hz link");
    CHECK(b == 1, "one own tick per frame: 120 frames a second");
    CHECK(c == 2, "two own ticks");
    acquire_ticks(120, 1, 2, 1, "display 120 Hz ignores the game's interval");
    acquire_ticks(90, 1, 1, 4. / 3, "display 90 Hz preserves fractional ticks");
    acquire_ticks(75, 1, 1, 1.6, "display 75 Hz preserves fractional ticks");
    acquire_ticks(120, 0, 2, 4, "game mode keeps interval 2 in 60 Hz vsyncs");
    acquire_ticks(60, 0, 0, 0, "game interval 0 waits for no tick (no vsync on Horizon)");
    acquire_ticks(60, 1, 0, 2, "display mode paces interval 0 at 60 Hz: two ticks of a 120 Hz link");
    acquire_ticks(60, 1, 2, 2, "display 60 Hz ignores the game's interval");
    acquire_ticks(240, 1, 1, 1, "a rate above the link's waits one tick");
    setenv("DM_FPS_TARGET", "30", 1); g_present_cap = -1;
    acquire_ticks(120, 1, 1, 4, "native 30 fps cap waits four 120 Hz ticks");
    CHECK(nvn_game_fps() == 30, "native 30 fps cap configures the display link");
    setenv("DM_FPS_TARGET", "60", 1); g_present_cap = -1;
    acquire_ticks(120, 1, 1, 2, "native 60 fps cap waits two 120 Hz ticks");
    CHECK(nvn_game_fps() == 60, "native 60 fps cap configures the display link");
    setenv("DM_FPS_TARGET", "0", 1); g_present_cap = -1;
    _Alignas(8) float state[32] = {60}; dm_fps_state = (uint8_t *)state;   // include the fixed-step flag at +0x24 read by rt_period_ns, and the step's unit at +0x38 (0: vsyncs)
    acquire_ticks(120, 1, 1, 2, "the 60 fps frame step keeps 60 Hz vsyncs");
    acquire_ticks(60, 0, 0, 2, "the 60 fps frame step paces game interval 0: two 120 Hz ticks");
    state[0] = 30;
    acquire_ticks(120, 1, 1, 4, "the 30 fps frame step keeps two 60 Hz vsyncs");
    state[0] = 40;
    CHECK(fps_step_cap() == 30 && nvn_game_fps() == 30, "40 in a page in vsyncs (no FRAME_STEP_120 stub): the frame step holds 30");
    // 40 fps: a page whose step is in ticks of 1/120 s (+0x38 120, a FRAME_STEP_120 stub). Headless (no window) the pacer's clock holds 25 ms
    uint32_t unit = 120; memcpy((uint8_t *)state + FSTEP_UNIT_OFF, &unit, 4);
    CHECK(fstep_t120() && fstep_hz() == 120, "a page in ticks: the frame step counts 120 a second");
    CHECK(fps_step_cap() == 40 && nvn_game_fps() == 40, "40 fps without a window: cap 40, the target the display link reads");
    acquire_ticks(120, 0, 1, 3, "40 fps: each acquire waits 3 ticks of a 120 Hz link (25 ms), whatever interval the game asks for");
    acquire_ticks(120, 0, 2, 3, "  game interval 2 too");
    win_t w40 = {.interval = 1}; nvn_present_config(120, 0);
    CHECK(present_itv(&w40) == 3 && present_period(&w40) == 25000000ull && rt_period_ns(&w40, 25000000ull) == 25000000ull, "  present interval 3 ticks, period 25 ms, the scheduler's deadline 25 ms");
    atomic_store(&g_fs_k, 4);
    acquire_ticks(120, 0, 1, 4, "  40 falling back to 30 under load (hold 4): four ticks");
    CHECK(rt_period_ns(&w40, present_period(&w40)) == 25000000ull, "  the scheduler keeps the 40 fps deadline while the hold is longer");
    atomic_store(&g_fs_k, 3); g_link_mode = 1;
    acquire_ticks(120, 0, 1, 2, "  a 60 Hz link (while the link changes to 120): 3 ticks round up to 2 refreshes (33 ms), never 1 (16.7 ms)");
    g_link_mode = 2; atomic_store(&g_fs_k, 0);
    state[0] = 60; acquire_ticks(120, 0, 1, 2, "60 fps in ticks: two 120 Hz ticks");
    g_link_mode = 1; acquire_ticks(120, 0, 1, 1, "  one tick of a 60 Hz link");
    g_link_mode = 3; state[0] = 30; acquire_ticks(120, 0, 1, 1, "30 fps in ticks: one tick of the 30 Hz link");
    CHECK(nvn_game_fps() == 30, "  30 fps in ticks configures the 30 Hz link");
    g_link_mode = 2; acquire_ticks(120, 0, 1, 4, "  four ticks of a 120 Hz link");
    // The screen check with a window: a screen at a multiple of 120 Hz shows 40; 60 Hz, 144 Hz, Low Power Mode, and DM_PRESENT_LINK=1 hold 30;
    // a Mac ProMotion display temporarily matched to fixed 60 Hz (capable 120 Hz) still allows 40
    state[0] = 40; int dummy_layer; test_layer = &dummy_layer;
    test_screen_hz = 120; test_capable_hz = 0; CHECK(fps_step_cap() == 40, "a window on a 120 Hz screen: 40");
    test_screen_hz = 240; CHECK(fps_step_cap() == 40, "  on a 240 Hz screen: 40");
    test_screen_hz = 60; CHECK(fps_step_cap() == 30, "  on a 60 Hz screen: 30 (a line says why)");
    test_screen_hz = 60; test_capable_hz = 120; CHECK(fps_step_cap() == 40, "  ProMotion temporarily matched to 60 Hz (capable 120 Hz): 40");
    test_capable_hz = 0; test_screen_hz = 144; CHECK(fps_step_cap() == 30, "  on a 144 Hz screen: 30 (25 ms is not a whole number of its refreshes)");
    test_screen_hz = 120; test_low_power = 1; CHECK(fps_step_cap() == 30, "  in Low Power Mode: 30");
    test_low_power = 0; CHECK(fps_step_cap() == 40, "  out of it: 40 again");
    setenv("DM_PRESENT_LINK", "1", 1); CHECK(fps_step_cap() == 30, "  DM_PRESENT_LINK=1 (pinned 60 Hz link): 30");
    setenv("DM_PRESENT_LINK", "2", 1); CHECK(fps_step_cap() == 40, "  DM_PRESENT_LINK=2 (120 Hz link): 40");
    unsetenv("DM_PRESENT_LINK");
    test_layer = NULL; test_screen_hz = 0; test_capable_hz = 0;
    dm_fps_state = NULL;
    // 120 fps: the frame step's hold is in ticks of 1/120 s, whatever interval the game asks for, and the target the host writes follows it
    setenv("DM_FPS_TARGET", "120", 1); g_present_cap = -1; g_fps_high = -1;
    CHECK(fps_high(), "a game checked at 120 takes the 120 fps target");
    atomic_store(&g_high_cap, 120); atomic_store(&g_fs_k, 1);
    acquire_ticks(120, 0, 2, 1, "120 fps: hold 1 is one tick, whatever interval the game asks for");
    CHECK(nvn_game_fps() == 120 && nvn_fps_high_now() == 120, "120 fps: hold 1 sets a target of 120");
    atomic_store(&g_fs_k, 2);
    acquire_ticks(120, 0, 1, 2, "120 fps: hold 2 is two ticks");
    CHECK(nvn_fps_high_now() == 60, "120 fps: hold 2 sets a target of 60, so the game's step doubles with the time on screen");
    atomic_store(&g_high_cap, 60); g_link_mode = 1;
    acquire_ticks(120, 0, 1, 1, "a screen below 120 Hz: hold 2 is one tick of a 60 Hz link");
    CHECK(nvn_game_fps() == 60, "a screen below 120 Hz configures a 60 Hz link");
    g_link_mode = 2; test_fps_max = 60; g_present_cap = -1; g_fps_high = -1;
    CHECK(!fps_high(), "a game not checked at 120 does not take it");
    atomic_store(&g_fs_k, 0);
    acquire_ticks(60, 0, 1, 2, "a game not checked at 120 presents at 60");
    CHECK(nvn_game_fps() == 60, "a game not checked at 120 reports 60");
    test_fps_max = 120; setenv("DM_FPS_TARGET", "0", 1); g_present_cap = -1; g_fps_high = -1;
    g_link_mode = 1;
    CHECK(mtl_link_rate() == 60, "a 60 Hz link reports 60");
    a = per_wait(0, 1);
    CHECK(a == 1, "a 60 Hz link: one tick per vsync");
    acquire_ticks(120, 1, 1, 1, "a 60 Hz link limits display 120 Hz to one tick");
    g_link_mode = 3;
    CHECK(mtl_link_rate() == 30, "the 30 Hz power-saving link reports 30");
    acquire_ticks(120, 1, 1, 1, "a 30 Hz link limits display 120 Hz to one tick");
    acquire_ticks(60, 0, 2, 1, "a 30 Hz link keeps game interval 2 at one tick");
    acquire_ticks(60, 0, 3, 2, "a 30 Hz link rounds game interval 3 up to two ticks");
    unsigned waits = test_waits; g_link_tick_n += 4;
    CHECK(mtl_link_wait_ticks(2, 10000000ull) && test_waits == waits, "already elapsed ticks need no wait");
    uint64_t tick = g_link_tick_n; waits = test_waits; test_spurious = 1;
    CHECK(mtl_link_wait_ticks(2, 10000000ull) && g_link_tick_n - tick == 2 && test_waits - waits == 3, "a spurious wake cannot finish a tick wait");
    test_silent = 1; tick = g_link_tick_n; uint64_t start = test_now;
    CHECK(!mtl_link_wait_ticks(1, 5000000ull), "a silent link times out");
    CHECK(g_link_tick_n == tick && test_now - start == 5000000ull, "timeout reaches its deadline without inventing ticks");
    test_silent = 0;
    CHECK(mtl_link_wait_ticks(2, 10000000ull) && g_link_tick_n - tick == 2, "ticks resume from the last successful wait after timeout");
    // A drain of standing frames the display-link callback asked for (nvn_link_drain.h): one wait takes the extra ticks, once, and reports them as pacing
    atomic_store(&g_link_drain_req, 1); tick = g_link_tick_n;
    CHECK(mtl_link_wait_ticks(1, 100000000ull) && g_link_tick_n - tick == 2, "a drain request makes one wait a tick longer");
    CHECK(atomic_load(&g_link_drained) == 1 && !atomic_load(&g_link_drain_req), "the drain is counted and its request used up");
    (void)mtl_link_drain_take_ns();
    CHECK(mtl_link_drain_take_ns() == 0, "the drain's time is handed out once");
    tick = g_link_tick_n;
    CHECK(mtl_link_wait_ticks(1, 100000000ull) && g_link_tick_n - tick == 1, "the wait after a drain is one tick again");
    atomic_store(&g_link_drain_req, 2); tick = g_link_tick_n;
    CHECK(mtl_link_wait_ticks(1, 100000000ull) && g_link_tick_n - tick == 3 && atomic_load(&g_link_drained) == 2, "a drain of two ticks");
    atomic_store(&g_link_drain_req, 1); g_link_tick_n += 4; waits = test_waits;
    CHECK(mtl_link_wait_ticks(1, 100000000ull) && test_waits == waits, "a game that fell behind does not wait for a drain");
    CHECK(atomic_load(&g_link_drained) == 2 && !atomic_load(&g_link_drain_req) && atomic_load(&g_link_blocked_ns) == 0, "and the request is dropped, with no acquire wait reported");
    acquire_lateness();
    deferred_acquire_sync();
    pid_t pid; int st = 1; extern char **environ;   // the snapshot cases in a process of their own (mtl_snap_on is decided once)
    CHECK(!posix_spawn(&pid, argv[0], NULL, NULL, (char *[]){argv[0], "snap", NULL}, environ) && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && !WEXITSTATUS(st), "the snapshot cases pass");
    printf("t_link_ticks: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
