// The acquire's waits on the display link, counted in its own ticks (mtl_link_wait_ticks) or in the game's 60 Hz vsyncs (mtl_link_wait_tick): a 120 Hz link takes two of its
// ticks per vsync, so present_interval display at DM_PRESENT_HZ=120 needs the first to reach one frame per tick. The real acquire also covers fractional rates and the game/step
// pacing branches. A controlled clock and timed wait deliver one tick per wake: a sleeping ticker can miss the acquire's real deadline on a busy CI runner
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
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
#include "../nvn/hle_nvn.c"
#undef g_ft
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
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
// The link callback's hold, simulated on its tick grid with its 2 ms tolerance: how many ticks after the one that presented a frame of this period take no new frame
static int held(int mode, double period) {
    g_link_mode = mode; g_link_vperiod = 0;
    double hold = link_hold_s(period), t0 = 100.0, until = hold > 0 ? t0 + hold : 0, tick = link_tick_s(); int n = 0;
    if (until > 0) while (t0 + (n + 1) * tick < until - 0.002) n++;
    return n;
}
int main(void) { @autoreleasepool {
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
    acquire_ticks(60, 1, 2, 2, "display 60 Hz ignores the game's interval");
    acquire_ticks(240, 1, 1, 1, "a rate above the link's waits one tick");
    setenv("DM_FPS_TARGET", "30", 1); g_present_cap = -1;
    acquire_ticks(120, 1, 1, 4, "native 30 fps cap waits four 120 Hz ticks");
    CHECK(nvn_game_fps() == 30, "native 30 fps cap configures the display link");
    setenv("DM_FPS_TARGET", "60", 1); g_present_cap = -1;
    acquire_ticks(120, 1, 1, 2, "native 60 fps cap waits two 120 Hz ticks");
    CHECK(nvn_game_fps() == 60, "native 60 fps cap configures the display link");
    setenv("DM_FPS_TARGET", "0", 1); g_present_cap = -1;
    _Alignas(8) float state[10] = {60}; dm_fps_state = (uint8_t *)state;   // include the fixed-step flag at +0x24 read by rt_period_ns
    acquire_ticks(120, 1, 1, 2, "the 60 fps frame step keeps 60 Hz vsyncs");
    state[0] = 30;
    acquire_ticks(120, 1, 1, 4, "the 30 fps frame step keeps two 60 Hz vsyncs");
    dm_fps_state = NULL;
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
    {   // The hold of the link callback (link_hold_s): a presented frame stays on screen for its whole period when that covers two or more of the link's ticks, so a 60 fps game takes
        // exactly two ticks of a 120 Hz link (one frame per 16.7 ms, game time 1.000 of real time; with present snapshots the slot each present frees paces the game). Ticks are
        // simulated on the callback's grid with its 2 ms tolerance: the number of ticks held is the ticks per frame minus the one that presented it
        CHECK(held(2, 1.0 / 60) == 1, "a 60 fps frame on a 120 Hz link is held for the second tick: two ticks per frame");
        CHECK(held(2, 2.0 / 60) == 3, "an interval-2 frame on a 120 Hz link holds four ticks");
        CHECK(held(2, 1.0 / 120) == 0, "a 120 fps frame on a 120 Hz link takes every tick");
        CHECK(held(1, 1.0 / 60) == 0, "a 60 fps frame on a 60 Hz link takes every tick");
        CHECK(held(1, 2.0 / 60) == 1, "an interval-2 frame on a 60 Hz link holds two ticks, as before");
        CHECK(held(3, 1.0 / 30) == 0 && held(3, 1.0 / 60) == 0, "the 30 Hz power-saving link never holds a 60 or 30 fps frame");
        g_link_mode = 2; g_link_vperiod = 1.0 / 60; CHECK(link_hold_s(1.0 / 60) == 0, "counted 1/60 s virtual ticks: a 60 fps frame takes every counted tick");
        g_link_mode = 1; g_link_vperiod = 0;
    }
    {   // The shift judge (link_late_note): frames on their targets, or half of them shifted (Breath of the Wild run 6 held 60 fps there), never move pacing; a full window of
        // 120 presents with three quarters or more 2 ms late does
        link_shift_t s = {0}; int due = 0;
        for (int i = 0; i < 3 * LINK_SHIFT_N; i++) due += link_shift_judge(&s, 0.0);
        CHECK(due == 0, "frames on their targets: pacing stays");
        for (int i = 0; i < LINK_SHIFT_N - 1; i++) due += link_shift_judge(&s, 4.17);
        CHECK(due == 0, "a window is judged only when full");
        CHECK(link_shift_judge(&s, 4.17) == 1, "120 presents shown 4.17 ms late: pacing moves");
        link_shift_t m = {0}; due = 0;
        for (int i = 0; i < 4 * LINK_SHIFT_N; i++) due += link_shift_judge(&m, (i / 60) % 2 ? 4.15 : 0.0);
        CHECK(due == 0, "the offset flipping 0 <-> 4.15 ms each second (half the frames late) is not the steady shift");
        link_shift_t r = {0}; due = 0;   // run 7 after the shift: eleven of every twelve presents 4.17 ms late, one on its target
        for (int i = 0; i < LINK_SHIFT_N; i++) due += link_shift_judge(&r, i % 12 == 11 ? 0.0 : 4.17);
        CHECK(due == 1, "eleven in twelve presents shifted: pacing moves");
    }
    {   // Where pacing may leave the link for the shift (mode 0): fullscreen on a variable-refresh screen at any rate, and nowhere else
        CHECK(link_leave_ok(1, 1), "fullscreen on a variable-refresh screen (ProMotion, adaptive sync at 60-240 Hz): leaves the link");
        CHECK(!link_leave_ok(1, 0), "fullscreen on a fixed-rate screen (60, 120, 144 Hz; frame latency 1, where the shift was not seen): keeps the link");
        CHECK(!link_leave_ok(0, 1) && !link_leave_ok(0, 0), "a window: keeps the link");
    }
    {   // The offset tracker behind the log line: it dates the shift to its first present, skips isolated presents at the old offset or a refresh off, and stays quiet otherwise
        link_offset_t o = {0}; uint64_t first = 0, pn = 0; double was = -1; int moved = 0;
        for (int i = 0; i < 600; i++) moved += link_offset_note(&o, i % 50 == 7 ? 8.33 : 0.0, ++pn, &first, &was);
        CHECK(moved == 0, "frames on their targets with an odd one a refresh late: no move");
        uint64_t at = pn + 1;
        for (int i = 0; i < 600 && !moved; i++) moved += link_offset_note(&o, i % 12 == 11 ? 0.0 : i % 25 == 3 ? 12.5 : 4.17, ++pn, &first, &was);
        CHECK(moved == 1 && first == at && was == 0.0 && o.level == 4.17, "the shift as run 7 had it (one in twelve on target, a few a refresh off): dated to its first present");
        moved = 0;
        for (int i = 0; i < 600; i++) moved += link_offset_note(&o, i % 12 == 11 ? 0.0 : 4.17, ++pn, &first, &was);
        CHECK(moved == 0, "the shifted offset held: no more moves");
        moved = 0;
        for (int i = 0; i < 40; i++) moved += link_offset_note(&o, 0.0, ++pn, &first, &was);
        CHECK(moved == 1 && was == 4.17 && o.level == 0.0, "back on the targets: one move back");
    }
    printf("t_link_ticks: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
