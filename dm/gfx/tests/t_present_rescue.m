// Exercise the production watchdog with a silent display link and a controlled clock; no GPU or game files. --metal also checks real layer ownership.
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
#define CAMetalDisplayLink SilentDisplayLink
#define nvn_game_fps test_game_fps_now   // hle_nvn.c, linked here, has the strong nvn_game_fps; t_present_rescue_focus.c sets the rate
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#undef CAMetalDisplayLink
static uint64_t test_now;
int test_focus = 1;
int test_fullscreen;
int test_variable_refresh;
int test_game_fps = 60;
int test_screen_hz;   // the screen's maximumFramesPerSecond (front_screen_max_fps); 0 = not known, as the graphics layer's weak default
int test_match_fps;   // the last rate link_sync asked the screen for (front_screen_match)
uint64_t test_focus_since;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return test_now; }
static int checks, failed;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)
static void reset(void) {
    test_now = 10000000000ull; test_focus = 1; test_focus_since = 0;
    g_dm_present_link = 1; g_rescue_mode = 0; g_rescue_back_ns = 3000000000ull; g_rescue_n = 0;
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
    test_game_fps = 30; link_sync(); CHECK("30 fps in variable-refresh fullscreen keeps the 30 Hz link", g_link_mode == 3 && link.preferredFrameRateRange.preferred == 30 && test_match_fps == 30);
    test_game_fps = 60; link_sync(); CHECK("back at 60 fps: the 120 Hz link", g_link_mode == 2);
    test_fullscreen = 0; link_sync(); CHECK("leaving fullscreen: the 60 Hz link again", g_link_mode == 1 && link.preferredFrameRateRange.preferred == 60 && mtl_link_rate() == 60);
    g_dm_present_link = 2; link_sync(); CHECK("DM_PRESENT_LINK=2 keeps the 120 Hz link in a window", g_link_mode == 2 && test_match_fps == 120);
    g_dm_present_link = 1; test_screen_hz = 0; test_variable_refresh = 0; link_sync(); g_link = nil; g_link_started = 0;
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
    thresholds(); backoff(); settings(); lifecycle(); latency(); thirty(); vrr120();
    if (argc == 2) metal_lifecycle();
    printf("present rescue: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
