// Exercise the production watchdog with a silent display link and a controlled clock; no GPU or game files. --metal also checks real layer ownership.
// Checks: startup_rescue: a callback that enters during rescue leaves the stall clock and the ticks alone, the acquire after it returns, and a link published after the last enqueue keeps the layer until run-loop invalidation and then releases it.
#import <QuartzCore/CAMetalDisplayLink.h>
#import <QuartzCore/CAMetalLayer.h>
@interface SilentDisplayLink : NSObject
@property CAFrameRateRange preferredFrameRateRange;
@property(nonatomic) float preferredFrameLatency;
@property(nonatomic) NSUInteger latencyWrites;
@property (weak) id<CAMetalDisplayLinkDelegate> delegate;
@property BOOL paused;
@property BOOL invalidated;
- (instancetype)initWithMetalLayer:(CAMetalLayer *)layer;
- (void)addToRunLoop:(NSRunLoop *)loop forMode:(NSRunLoopMode)mode;
- (void)invalidate;
@end
@implementation SilentDisplayLink
@synthesize preferredFrameLatency = _preferredFrameLatency;
- (void)setPreferredFrameLatency:(float)value {
    self.latencyWrites++;
    _preferredFrameLatency = value < 1 ? 1 : value > 2 ? 2 : value;   // CAMetalDisplayLink accepts only 1 or 2
}
- (instancetype)initWithMetalLayer:(CAMetalLayer *)layer { (void)layer; return [super init]; }
- (void)addToRunLoop:(NSRunLoop *)loop forMode:(NSRunLoopMode)mode { (void)mode; [loop addPort:[NSMachPort port] forMode:NSDefaultRunLoopMode]; }
- (void)invalidate { self.invalidated = YES; }
@end
@interface RescueUpdate : NSObject
@property CFTimeInterval targetTimestamp;
@property CFTimeInterval targetPresentationTimestamp;
@end
@implementation RescueUpdate
@end
#define CAMetalDisplayLink SilentDisplayLink
#define nvn_game_fps test_game_fps_now   // hle_nvn.c, linked here, has the strong nvn_game_fps; t_present_rescue_focus.c sets the rate
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#undef CAMetalDisplayLink
static uint64_t test_now;
int test_focus = 1;
int test_fullscreen;
int test_covered;
int test_variable_refresh;
int test_game_fps = 60;
int test_screen_hz;   // the screen's maximumFramesPerSecond (front_screen_max_fps); 0 = not known, as the graphics layer's weak default
int test_match_fps;   // the last rate link_sync asked the screen for (front_screen_match)
int test_settling;
static void *test_layer;
uint64_t test_focus_since;
void *front_metal_layer(void) { return test_layer; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return test_now; }
static int checks, failed;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)
static void reset(void) {
    test_now = 10000000000ull; test_focus = 1; test_focus_since = 0; test_covered = 0; atomic_store(&g_covered, 0);
    g_dm_present_link = 1; g_rescue_mode = 0; g_rescue_back_ns = 3000000000ull; g_rescue_n = 0;
    g_link_probe_pending = 0; g_link_probe_at = 0; g_link_probe_fail = 0;
    atomic_store(&g_test_link_hold, 0);
    g_link_tick_ns = test_now - 1000000000ull; g_stall_t0 = g_link_tick_ns;
    g_stall_logged = g_stall_red = 0; g_stall_drop = 0;
    g_pq_in = g_pq_read = g_pq_done = 0; g_link_owns_layer = 1;
}
static void wait_frame(void) {
    uint64_t ticket = ++g_pq_in;
    mtl_present_wait(ticket);
    CHECK("stalled frame releases its acquire ticket", g_pq_done == ticket);
}
static void thresholds(void) {
    reset(); g_link = [SilentDisplayLink new];
    CHECK("a link paces before rescue", mtl_link_active());
    g_stall_t0 = g_link_tick_ns = test_now - 499999999ull; wait_frame();
    CHECK("499 ms does not trigger rescue", !g_rescue_mode);
    test_now++; wait_frame();
    CHECK("500 ms triggers rescue", g_rescue_mode == 1);
    CHECK("rescue preserves the requested mode", g_dm_present_link == 1);
    CHECK("rescue stops display-link pacing", !mtl_link_active());
    g_link = nil;
    uint64_t deadline = g_rescue_back_at;
    test_now = deadline - 1; link_sync(); CHECK("no retry before 3 s", g_rescue_mode == 1);
    test_now++; link_sync(); CHECK("retry after 3 s", !g_rescue_mode && g_dm_present_link == 1);
    CHECK("retry starts a fresh stall clock", !g_stall_t0 && g_link_tick_ns == test_now);
    reset(); test_focus = 2; wait_frame(); CHECK("inactive time does not trigger rescue", !g_rescue_mode);
    test_focus = 3; wait_frame(); CHECK("background time does not trigger rescue", !g_rescue_mode);
    test_focus = 1; test_focus_since = test_now; wait_frame(); CHECK("foreground starts a fresh grace period", !g_rescue_mode);
    test_now += 500000000ull; wait_frame(); CHECK("500 ms active after foreground triggers rescue", g_rescue_mode == 1);
    reset(); test_focus_since = test_now + 1; wait_frame();
    CHECK("a newer focus timestamp cannot underflow the grace period", !g_rescue_mode);
}
static void backoff(void) {
    reset(); const uint64_t seconds[] = {3, 6, 12, 24, 48, 60, 60};
    for (unsigned i = 0; i < sizeof seconds / sizeof seconds[0]; i++) {
        g_stall_t0 = g_link_tick_ns = test_now - 500000000ull; wait_frame();
        CHECK("retry delay doubles and stops at 60 s", g_rescue_back_at - test_now == seconds[i] * 1000000000ull);
        test_now = g_rescue_back_at; link_sync();
    }
}
static void settings(void) {
    for (int mode = 0; mode <= 2; mode += 2) {
        reset(); wait_frame(); g_dm_present_link = mode;
        uint64_t deadline = g_rescue_back_at;
        link_sync(); CHECK("a new runtime mode ends rescue immediately", !g_rescue_mode);
        test_now = deadline; link_sync();
        CHECK("retry honors a new runtime pacing mode", g_dm_present_link == mode && !g_rescue_mode);
    }
}
static void lifecycle(void) {
    reset(); SilentDisplayLink *link = [SilentDisplayLink new];
    g_link = link; g_link_started = 1; g_link_rl = CFRunLoopGetCurrent();
    link_sync(); wait_frame(); link_sync();
    CHECK("layer stays with the link until its run loop invalidates it", g_link_owns_layer);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
    CHECK("rescue invalidates the link before releasing the layer", link.invalidated && !g_link && !g_link_owns_layer);
    g_link_started = 0;
}
static void covered(void) {   // A covered window: the host's clock paces (mode 0) until it is visible, then the requested link again; the link's silence meanwhile is no stall
    reset(); SilentDisplayLink *link = [SilentDisplayLink new];
    g_link = link; g_link_started = 1; g_link_rl = CFRunLoopGetCurrent();
    link_sync(); CHECK("a visible window keeps the display link", mtl_link_active() && g_link_mode == 1);
    test_covered = 1; link_sync();
    CHECK("a covered window stops display-link pacing at once", !mtl_link_active() && !mtl_link_wait_tick(1, 1000));
    CHECK("covered keeps the requested mode and is no rescue", g_dm_present_link == 1 && !g_rescue_mode && g_link_mode == 0);
    // A callback that arrives while covered (before invalidation runs) must not touch the stall clock or ticks.
    g_stall_t0 = g_link_tick_ns = test_now - 1000000000ull;
    uint64_t tick0 = g_link_tick_n, last0 = g_link_tick_ns;
    RescueUpdate *upd = [RescueUpdate new]; upd.targetTimestamp = upd.targetPresentationTimestamp = CACurrentMediaTime();
    [[NvMetalDisplayLinkDelegate new] metalDisplayLink:(id)link needsUpdate:(id)upd];
    CHECK("a callback while covered leaves watchdog and ticks alone", g_stall_t0 && g_link_tick_ns == last0 && g_link_tick_n == tick0);
    wait_frame();
    CHECK("a covered window's silent link does not trigger rescue", !g_rescue_mode);
    // DM_PRESENT_SCREEN does not stall acquires while the window is covered.
    g_pq_in = 5; atomic_store(&g_pres_seen, 0);
    CHECK("covered window skips DM_PRESENT_SCREEN readiness wait", mtl_present_screen_ready());
    uint64_t sw = 999;
    CHECK("covered window skips DM_PRESENT_SCREEN timed wait", mtl_present_screen_wait_timeout(1.0 / 60, 1000000ull, &sw) && sw == 0);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
    CHECK("covered invalidates the link and releases the layer to the present thread", link.invalidated && !g_link && !g_link_owns_layer);
    g_link_started = 0;
    for (int frame = 0; frame < 3; frame++) link_sync();
    CHECK("no link while the window stays covered", g_link_mode == 0 && !g_link_started && !mtl_link_active());
    g_stall_t0 = test_now - 2000000000ull; g_link_tick_ns = test_now - 2000000000ull; test_now += 1000;
    test_covered = 0; link_sync();
    CHECK("visible again wants the requested link", g_link_mode == 1 && g_dm_present_link == 1);
    CHECK("visible again starts a fresh stall clock", !g_stall_t0 && g_link_tick_ns == test_now);
    g_link = [SilentDisplayLink new]; g_link_owns_layer = 1;   // as link_main does once the new link exists
    CHECK("the new link paces again", mtl_link_active());
    g_link = nil; g_link_started = 0;

    // Covered before link_sync samples front_window_covered still prevents probation/rescue from tripping.
    reset(); g_link = [SilentDisplayLink new]; g_link_started = 1; g_link_rl = CFRunLoopGetCurrent();
    link_sync();
    g_link_probe_at = test_now - LINK_PROBE_NS;
    g_stall_t0 = g_link_tick_ns = test_now - 500000000ull;
    test_covered = 1; wait_frame();
    CHECK("covering before link_sync does not trip probation or rescue", !g_rescue_mode && g_link_probe_fail == 0);
    link_sync();
    CHECK("covering clears active probation", !g_link_probe_pending && !g_link_probe_at);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
    g_link_started = 0; test_covered = 0; link_sync();

    // Timed rescue expiry waits while covered and enters probation once visible again.
    reset(); g_link = [SilentDisplayLink new]; g_link_started = 1; g_link_rl = CFRunLoopGetCurrent();
    link_sync(); wait_frame(); link_sync();
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false); g_link_started = 0;
    CHECK("rescue entered before cover", g_rescue_mode == 1);
    test_covered = 1; test_now = g_rescue_back_at + 1000; link_sync();
    CHECK("timed rescue retry waits while the window is covered", g_rescue_mode == 1 && !g_link_probe_pending);
    test_covered = 0; link_sync();
    CHECK("uncovering after rescue deadline ends rescue and queues probation", !g_rescue_mode && g_link_probe_pending && g_link_mode == 1);

    // Covered during startup (link published after last enqueue, applied still 0) still retires the link.
    reset(); g_link = nil; g_link_started = 0; g_link_owns_layer = 0; g_dm_present_link = 0; link_sync();
    g_dm_present_link = 1; g_link_started = 1; g_link_owns_layer = 1; g_link_rl = CFRunLoopGetCurrent();
    test_covered = 1; link_sync();   // applied is 0, link not published yet
    link = [SilentDisplayLink new]; g_link = link;
    link_sync();
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
    CHECK("covered during startup still invalidates a newly published link", link.invalidated && !g_link && !g_link_owns_layer);
    g_link_started = 0; test_covered = 0; link_sync();

    reset(); g_dm_present_link = 0; test_covered = 1; link_sync(); test_covered = 0; link_sync();
    CHECK("DM_PRESENT_LINK=0 stays without the link either way", !mtl_link_active() && !g_link_started);
}
static void startup_rescue(void) {
    reset(); g_link_started = 1; g_link_rl = CFRunLoopGetCurrent();
    test_fullscreen = 1; test_screen_hz = 144;
    link_sync();   // link_main owns the layer but has not published the link yet
    SilentDisplayLink *link = [SilentDisplayLink new];
    link.preferredFrameRateRange = link_range(g_link_mode);
    wait_frame(); link_sync();   // rescue requested before the first mode has been applied
    g_link = link;
    uint64_t tick = g_link_tick_n, last = g_link_tick_ns;
    RescueUpdate *update = [RescueUpdate new]; update.targetTimestamp = update.targetPresentationTimestamp = CACurrentMediaTime();
    [[NvMetalDisplayLinkDelegate new] metalDisplayLink:(id)link needsUpdate:(id)update];
    CHECK("a callback during rescue keeps the watchdog stalled", g_stall_t0 && g_link_tick_ns == last && g_link_tick_n == tick);
    wait_frame();   // the next acquire must return before there can be another enqueue
    test_screen_hz = 120;
    link_sync();
    CHECK("the newly published link still owns the layer until invalidated", g_link_owns_layer);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
    CHECK("rescue releases a link published after the last enqueue", link.invalidated && !g_link && !g_link_owns_layer);
    g_link_started = 0; g_link = nil; test_fullscreen = test_screen_hz = 0;
}
static void latency(void) {   // Only variable-refresh fullscreen needs the latency-2 workaround; a new link takes the current policy as link_main does.
    reset(); test_fullscreen = 0; test_variable_refresh = 1; g_present_latency = 0;
    SilentDisplayLink *link = [SilentDisplayLink new]; link.preferredFrameLatency = link_latency();
    g_link = link; g_link_started = 1;
    link_sync(); CHECK("a window keeps frame latency 1", link.preferredFrameLatency == 1);
    test_fullscreen = 1; link_sync(); CHECK("entering fullscreen sets frame latency 2", link.preferredFrameLatency == 2);
    test_fullscreen = 0; link_sync(); CHECK("leaving fullscreen sets frame latency 1 again", link.preferredFrameLatency == 1);
    test_fullscreen = 1; link = [SilentDisplayLink new]; link.preferredFrameLatency = link_latency(); g_link = link;
    link_sync(); CHECK("a link created in fullscreen keeps frame latency 2", link.preferredFrameLatency == 2);
    test_variable_refresh = 0; link_sync(); CHECK("moving fullscreen to a fixed-rate screen sets latency 1", link.preferredFrameLatency == 1);
    test_variable_refresh = 1; link_sync(); CHECK("moving fullscreen back to variable refresh restores latency 2", link.preferredFrameLatency == 2);
    test_variable_refresh = 0; link_sync(); CHECK("switching the screen to a fixed-rate mode sets latency 1", link.preferredFrameLatency == 1);
    link = [SilentDisplayLink new]; link.preferredFrameLatency = link_latency(); g_link = link;
    link_sync(); CHECK("a link created on a fixed-rate fullscreen screen keeps latency 1", link.preferredFrameLatency == 1);
    test_fullscreen = 0; link_sync();
    test_fullscreen = 1; link_sync(); CHECK("entering fullscreen on a fixed-rate screen keeps latency 1", link.preferredFrameLatency == 1);
    for (int lat = 1; lat <= 2; lat++) for (int variable = 0; variable <= 1; variable++) for (int full = 0; full <= 1; full++) {
        g_present_latency = lat; test_variable_refresh = variable; test_fullscreen = full; link_sync();
        CHECK("DM_PRESENT_LATENCY wins for every screen and window mode", link.preferredFrameLatency == lat);
    }
    for (int variable = 0; variable <= 1; variable++) for (int fullscreen = 0; fullscreen <= 1; fullscreen++) {
        test_variable_refresh = variable; test_fullscreen = fullscreen;
        const int requests[] = {3, INT_MAX};
        for (unsigned i = 0; i < sizeof requests / sizeof requests[0]; i++) {
            g_present_latency = requests[i];
            CHECK("an oversized override requests supported latency 2", link_latency() == 2);
            link_sync(); NSUInteger writes = link.latencyWrites;
            for (int frame = 0; frame < 10; frame++) link_sync();
            CHECK("a clamped override does not rewrite the link every frame", link.latencyWrites == writes);
        }
    }
    g_present_latency = 0; test_fullscreen = test_variable_refresh = 0; g_present_link_env = 0;
    mtl_set_fstep_fine(1); link_sync();
    CHECK("fine pacing at a 30 fps cap sets frame latency 2 when link rate is automatic", link.preferredFrameLatency == 2);
    g_present_link_env = 1; link_sync();
    CHECK("an explicit DM_PRESENT_LINK keeps frame latency 1 even while fine pacing is set", link.preferredFrameLatency == 1);
    g_present_link_env = 0; mtl_set_fstep_fine(0); link_sync();
    CHECK("clearing fine pacing restores frame latency 1", link.preferredFrameLatency == 1);
    g_present_latency = 0; test_fullscreen = test_variable_refresh = 0; link_sync(); g_link = nil; g_link_started = 0;
}
static void thirty(void) {   // A 30 fps target runs the link at 30 Hz, except in fullscreen on a fixed-rate screen, where it stays at 60 Hz and the present interval paces the 30.
    reset(); test_fullscreen = 0; test_variable_refresh = 0; test_game_fps = 30;
    SilentDisplayLink *link = [SilentDisplayLink new]; g_link = link; g_link_started = 1;
    g_link_drain_on = 0;   // standing frames not drained (DM_PRESENT_DRAIN=0, iOS): a window keeps the 30 Hz link
    link_sync(); CHECK("a window at 30 fps takes the 30 Hz link", g_link_mode == 3 && link.preferredFrameRateRange.preferred == 30 && mtl_link_rate() == 30);
    test_fullscreen = 1; link_sync(); CHECK("fullscreen on a fixed-rate screen at 30 fps keeps the 60 Hz link", g_link_mode == 1 && link.preferredFrameRateRange.preferred == 60 && mtl_link_rate() == 60);
    test_variable_refresh = 1; link_sync(); CHECK("fullscreen on a variable-refresh screen at 30 fps takes the 30 Hz link", g_link_mode == 3 && link.preferredFrameRateRange.preferred == 30);
    test_variable_refresh = 0; link_sync(); CHECK("back on a fixed-rate screen: the 60 Hz link again", g_link_mode == 1 && link.preferredFrameRateRange.preferred == 60);
    test_fullscreen = 0; link_sync(); CHECK("leaving fullscreen: the 30 Hz link again", g_link_mode == 3 && link.preferredFrameRateRange.preferred == 30);
    g_link_drain_on = 1;   // with the drain a window at 30 fps keeps the 60 Hz link, where a frame waits half as long for its tick; fullscreen is as before
    link_sync(); CHECK("with the drain a window at 30 fps keeps the 60 Hz link", g_link_mode == 1 && link.preferredFrameRateRange.preferred == 60 && mtl_link_rate() == 60);
    test_fullscreen = 1; test_variable_refresh = 1; link_sync(); CHECK("with the drain variable-refresh fullscreen at 30 fps still takes the 30 Hz link", g_link_mode == 3);
    test_variable_refresh = 0; link_sync(); CHECK("with the drain fixed-rate fullscreen at 30 fps keeps the 60 Hz link", g_link_mode == 1);
    test_fullscreen = 0; link_sync(); CHECK("with the drain, back in a window: the 60 Hz link", g_link_mode == 1 && link.preferredFrameRateRange.preferred == 60);
    test_game_fps = 60; link_sync(); CHECK("60 fps in a window: the 60 Hz link", g_link_mode == 1 && link.preferredFrameRateRange.preferred == 60);
    g_link = nil; g_link_started = 0;
}
static void vrr120(void) {   // 60 fps in fullscreen on a variable-refresh screen at a multiple of 120 Hz takes the 120 Hz link (a frame every other tick); everything else keeps the 60 Hz one.
    reset(); test_fullscreen = 0; test_variable_refresh = 1; test_screen_hz = 120; test_game_fps = 60; g_present_latency = 0; g_present_link_env = 0;
    SilentDisplayLink *link = [SilentDisplayLink new]; g_link = link; g_link_started = 1;
    link_sync(); CHECK("a window on a 120 Hz variable-refresh screen keeps the 60 Hz link", g_link_mode == 1 && link.preferredFrameRateRange.preferred == 60 && mtl_link_rate() == 60);
    test_fullscreen = 1; link_sync();
    CHECK("fullscreen on a 120 Hz variable-refresh screen takes the 120 Hz link", g_link_mode == 2 && link.preferredFrameRateRange.preferred == 120 && mtl_link_rate() == 120);
    CHECK("the 120 Hz link leaves the requested mode as it was", g_dm_present_link == 1);
    CHECK("the 120 Hz link keeps frame latency 2", link.preferredFrameLatency == 2);
    CHECK("the screen is asked for the game's 60, so it can go to a fixed 60 Hz mode", test_match_fps == 60);
    test_variable_refresh = 0; link_sync(); CHECK("fullscreen on a fixed-rate screen keeps the 60 Hz link", g_link_mode == 1 && link.preferredFrameRateRange.preferred == 60);
    test_variable_refresh = 1; test_screen_hz = 144; link_sync(); CHECK("a variable-refresh screen at 144 Hz keeps the 60 Hz link", g_link_mode == 1);
    test_screen_hz = 0; link_sync(); CHECK("a screen of unknown rate keeps the 60 Hz link", g_link_mode == 1);
    test_screen_hz = 240; link_sync(); CHECK("a variable-refresh screen at 240 Hz takes the 120 Hz link", g_link_mode == 2);
    test_screen_hz = 120; g_present_link_env = 1; link_sync(); CHECK("DM_PRESENT_LINK=1 keeps the 60 Hz link in fullscreen", g_link_mode == 1 && link.preferredFrameRateRange.preferred == 60);
    g_present_link_env = 0; link_sync(); CHECK("without DM_PRESENT_LINK the 120 Hz link again", g_link_mode == 2);
    test_game_fps = 30; link_sync(); CHECK("30 fps in variable-refresh fullscreen defaults to the 30 Hz link", g_link_mode == 3 && link.preferredFrameRateRange.preferred == 30 && test_match_fps == 30);
    g_present_link_env = 1; link_sync(); CHECK("DM_PRESENT_LINK=1 keeps the 60 Hz link at 30 fps", g_link_mode == 1 && link.preferredFrameRateRange.preferred == 60 && test_match_fps == 60);
    g_dm_present_link = 2; link_sync(); CHECK("DM_PRESENT_LINK=2 keeps the 120 Hz link at 30 fps", g_link_mode == 2 && link.preferredFrameRateRange.preferred == 120 && test_match_fps == 120);
    g_dm_present_link = 1; g_present_link_env = 0; link_sync(); CHECK("unset DM_PRESENT_LINK restores the 30 Hz power-saving link", g_link_mode == 3 && link.preferredFrameRateRange.preferred == 30 && test_match_fps == 30);
    test_game_fps = 60; link_sync(); CHECK("back at 60 fps: the 120 Hz link", g_link_mode == 2);
    test_fullscreen = 0; link_sync(); CHECK("leaving fullscreen: the 60 Hz link again", g_link_mode == 1 && link.preferredFrameRateRange.preferred == 60 && mtl_link_rate() == 60);
    g_dm_present_link = 2; link_sync(); CHECK("DM_PRESENT_LINK=2 keeps the 120 Hz link in a window", g_link_mode == 2 && test_match_fps == 120);
    g_dm_present_link = 1; test_screen_hz = 0; test_variable_refresh = 0; link_sync(); g_link = nil; g_link_started = 0;
}
static void forty(void) {   // 40 fps automatically selects the 120 Hz link (3 ticks = 25 ms) and asks front_screen_match(120); movie/thermal drops to 30/60 and back return to 120 Hz
    reset(); test_fullscreen = 0; test_variable_refresh = 0; test_screen_hz = 120; test_game_fps = 40; g_present_link_env = 0; g_link_drain_on = 0;
    SilentDisplayLink *link = [SilentDisplayLink new]; g_link = link; g_link_started = 1;
    link_sync();
    CHECK("40 fps automatically selects the 120 Hz link", g_link_mode == 2 && link.preferredFrameRateRange.preferred == 120 && mtl_link_rate() == 120);
    CHECK("40 fps asks the screen matcher for 120 Hz", test_match_fps == 120);
    CHECK("40 fps leaves g_dm_present_link untouched", g_dm_present_link == 1);
    test_game_fps = 30; link_sync();
    CHECK("a 30 fps movie during 40 fps drops to the 30 Hz link", g_link_mode == 3 && test_match_fps == 30);
    test_game_fps = 40; link_sync();
    CHECK("after the movie ends, 40 fps returns to the 120 Hz link and asks for 120 Hz", g_link_mode == 2 && link.preferredFrameRateRange.preferred == 120 && test_match_fps == 120);
    g_present_link_env = 1; g_dm_present_link = 1; link_sync();
    CHECK("explicit DM_PRESENT_LINK=1 keeps the 60 Hz link", g_link_mode == 1 && link.preferredFrameRateRange.preferred == 60);
    g_present_link_env = 0; test_game_fps = 60; g_link_drain_on = -1; test_screen_hz = 0; link_sync(); g_link = nil; g_link_started = 0;
}
static void settling(void) {
    reset(); g_link = nil; g_link_started = 0; g_link_owns_layer = 0; g_dm_present_link = 0; link_sync();
    NSObject *dummy_layer = [NSObject new]; test_layer = (__bridge void *)dummy_layer; test_settling = 1; g_dm_present_link = 1;
    link_sync();
    CHECK("display link does not start while the screen is settling", !g_link_started && !g_link_owns_layer);
    link_sync();
    CHECK("repeated presents while settling keep waiting", !g_link_started);
    test_settling = 0;
    link_sync();
    CHECK("display link starts once the screen settles", g_link_started == 1);
    for (int i = 0; i < 1000 && !g_link; i++) usleep(1000);
    CHECK("started display link thread publishes the link", g_link != nil && g_link_owns_layer);
    g_dm_present_link = 0; link_sync();
    for (int i = 0; i < 1000 && g_link_started; i++) usleep(1000);
    CHECK("invalidating the started display link stops its thread cleanly", !g_link_started && !g_link && !g_link_owns_layer);
    test_layer = NULL; test_settling = 0;
}
static void wait_link_started(void) {
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&g_pq_mu); int ready = g_link != nil; pthread_mutex_unlock(&g_pq_mu);
        if (ready) return;
        usleep(1000);
    }
}
static void wait_link_stopped(void) {
    for (int i = 0; i < 1000 && g_link_started; i++) usleep(1000);
}
static void probation(void) {
    reset(); g_link = nil; g_link_started = 0; g_link_owns_layer = 0; g_dm_present_link = 0; link_sync();
    NSObject *dummy_layer = [NSObject new]; test_layer = (__bridge void *)dummy_layer;
    g_dm_present_link = 1; link_sync(); wait_link_started();
    CHECK("initial display link is not on probation", g_link != nil && !g_link_probe_pending && !g_link_probe_at);

    // 1. Trigger rescue, let the link retire, then retry: a link silent for 67 ms after attach rescues at once.
    g_stall_t0 = g_link_tick_ns = test_now - 500000000ull; wait_frame();
    CHECK("initial stall triggers rescue", g_rescue_mode == 1);
    link_sync(); wait_link_stopped();
    CHECK("rescue retires the silent link", !g_link_started && !g_link && !g_link_owns_layer);
    test_now = g_rescue_back_at; link_sync(); wait_link_started();
    CHECK("retried link enters probation once live", g_link != nil && !g_link_probe_pending && g_link_probe_at == test_now);
    test_now += LINK_PROBE_NS; wait_frame();
    CHECK("a retried 60 Hz link silent for 67 ms rescues at once", g_rescue_mode == 1 && g_link_probe_fail == 1 && !g_link_probe_at);
    link_sync(); wait_link_stopped();

    // 2. A callback ends probation, restoring the normal 500 ms threshold.
    test_now = g_rescue_back_at; link_sync(); wait_link_started();
    CHECK("second retry enters probation", g_link != nil && g_link_probe_at == test_now);
    RescueUpdate *update = [RescueUpdate new]; update.targetTimestamp = update.targetPresentationTimestamp = CACurrentMediaTime();
    [[NvMetalDisplayLinkDelegate new] metalDisplayLink:(id)g_link needsUpdate:(id)update];
    CHECK("a callback clears probation", !g_link_probe_at);
    test_now += 100000000ull; wait_frame();
    CHECK("after a callback, 100 ms silence does not trigger probation rescue", !g_rescue_mode);
    test_now += 400000000ull; wait_frame();
    CHECK("after a callback, 500 ms silence triggers normal rescue", g_rescue_mode == 1 && g_link_probe_fail == 1);
    link_sync(); wait_link_stopped();

    // 3. front_screen_settling() delays probation until the link actually starts.
    test_settling = 1;
    test_now = g_rescue_back_at; link_sync();
    CHECK("settling holds probation pending without arming the clock", !g_rescue_mode && !g_link_started && g_link_probe_pending && !g_link_probe_at);
    test_now += 1500000000ull;
    test_settling = 0; link_sync(); wait_link_started();
    CHECK("probation starts when the link attaches after settling", g_link != nil && !g_link_probe_pending && g_link_probe_at == test_now);
    test_now += LINK_PROBE_NS; wait_frame();
    CHECK("settled retry rescues after its full 67 ms probation", g_rescue_mode == 1 && g_link_probe_fail == 2);
    link_sync(); wait_link_stopped();

    // 4. g_classic_busy delays probation until the handoff finishes and the link attaches.
    pthread_mutex_lock(&g_pq_mu); g_classic_busy = 1; pthread_mutex_unlock(&g_pq_mu);
    test_now = g_rescue_back_at; link_sync();
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&g_pq_mu); int owns = g_link_owns_layer; pthread_mutex_unlock(&g_pq_mu);
        if (owns) break;
        usleep(1000);
    }
    test_now += 200000000ull; wait_frame();
    CHECK("waiting for g_classic_busy does not trip probation before the link exists", !g_rescue_mode && g_link_probe_pending && !g_link_probe_at);
    pthread_mutex_lock(&g_pq_mu); g_classic_busy = 0; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
    wait_link_started();
    CHECK("probation starts clean once g_classic_busy clears", g_link != nil && !g_link_probe_pending && g_link_probe_at == test_now && !g_stall_t0);

    // 5. Focus loss and regain honor the foreground grace period during probation.
    test_focus = 2; test_now += 200000000ull; wait_frame();
    CHECK("inactive time during probation does not trigger rescue", !g_rescue_mode && g_link_probe_at != 0);
    test_focus = 1; test_focus_since = test_now; wait_frame();
    CHECK("regaining focus starts a fresh probation grace period", !g_rescue_mode);
    test_focus_since = test_now + 1; wait_frame();
    CHECK("a newer focus timestamp cannot underflow probation", !g_rescue_mode);
    test_focus_since = test_now; test_now += LINK_PROBE_NS - 1; wait_frame();
    CHECK("67 ms - 1 ns active after focus does not trigger probation rescue", !g_rescue_mode);
    test_now++; wait_frame();
    CHECK("67 ms active after focus triggers probation rescue", g_rescue_mode == 1 && g_link_probe_fail == 3);
    link_sync(); wait_link_stopped();

    // 6. Mode 0 during rescue or probation clears probation so a later re-enable starts without stale probation.
    g_dm_present_link = 0; link_sync();
    CHECK("switching to mode 0 during rescue clears probation", !g_rescue_mode && !g_link_probe_pending && !g_link_probe_at);
    test_now += 500000000ull;
    g_dm_present_link = 1; link_sync(); wait_link_started();
    CHECK("re-enabling after mode 0 does not inherit stale probation", g_link != nil && !g_link_probe_pending && !g_link_probe_at);
    g_stall_t0 = g_link_tick_ns = test_now - 500000000ull; wait_frame();
    link_sync(); wait_link_stopped();
    test_now = g_rescue_back_at; link_sync(); wait_link_started();
    CHECK("retry enters probation before mode 0 switch", g_link != nil && g_link_probe_at == test_now);
    g_dm_present_link = 0; link_sync(); wait_link_stopped();
    CHECK("switching to mode 0 during probation clears probation", !g_link_probe_pending && !g_link_probe_at);
    test_now += 500000000ull;
    g_dm_present_link = 1; link_sync(); wait_link_started();
    CHECK("re-enabling after leaving probation starts clean", g_link != nil && !g_link_probe_pending && !g_link_probe_at);

    // 7. A 30 Hz link gets a 134 ms (4-refresh) probation window, and g_test_link_hold starts retried links paused.
    test_game_fps = 30; g_link_drain_on = 0; link_sync();
    CHECK("30 fps non-draining window selects the 30 Hz link", g_link_mode == 3);
    g_stall_t0 = g_link_tick_ns = test_now - 500000000ull; wait_frame();
    link_sync(); wait_link_stopped();
    atomic_store(&g_test_link_hold, 1);
    test_now = g_rescue_back_at; link_sync(); wait_link_started();
    CHECK("DM_TEST_LINK_STALL starts a retried link paused", g_link != nil && g_link.paused && g_link_probe_at == test_now);
    test_now += 2 * LINK_PROBE_NS - 1; wait_frame();
    CHECK("a 30 Hz link silent for 134 ms - 1 ns does not trigger probation rescue", !g_rescue_mode);
    test_now++; wait_frame();
    CHECK("a 30 Hz link silent for 134 ms triggers probation rescue", g_rescue_mode == 1 && g_link_probe_fail == 4);
    atomic_store(&g_test_link_hold, 0);
    g_dm_present_link = 0; link_sync(); wait_link_stopped();
    CHECK("probation cleanup stops the display link cleanly", !g_link_started && !g_link && !g_link_owns_layer && !g_link_probe_at);
    test_game_fps = 60; g_link_drain_on = -1; test_layer = NULL; test_settling = 0;
}
static void metal_lifecycle(void) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    CHECK("Metal device available", device != nil); if (!device) return;
    CAMetalLayer *layer = [CAMetalLayer layer]; layer.device = device; layer.drawableSize = CGSizeMake(8, 8);
    // Use the real class despite the compile-time double above. A paused link on an offscreen layer needs no visible window.
    reset(); g_link = [[NSClassFromString(@"CAMetalDisplayLink") alloc] initWithMetalLayer:layer];
    g_link.paused = YES; [g_link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
    g_link_started = 1; g_link_rl = CFRunLoopGetCurrent();
    link_sync(); wait_frame(); link_sync();
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
    CHECK("real link releases the layer during rescue", !g_link && !g_link_owns_layer);
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    CHECK("present thread can take a real drawable after rescue", drawable != nil);
    [drawable present]; g_link_started = 0;
}
int main(int argc, char **argv) { @autoreleasepool {
    alarm(10); setbuf(stdout, NULL);
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--metal"))) return 2;
    thresholds(); backoff(); settings(); startup_rescue(); lifecycle(); covered(); latency(); thirty(); vrr120(); forty(); settling(); probation();
    if (argc == 2) metal_lifecycle();
    printf("present rescue: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
