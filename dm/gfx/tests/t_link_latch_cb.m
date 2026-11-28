// The late latch through the display-link callback (nvn_link_latch.h, NvMetalDisplayLinkDelegate) on a timeline: a game thread waits in mtl_link_wait_ticks and presents with
// present_enqueue, and the test calls the callback by hand, as t_present_alloc.m does (real Metal 4 queue, offscreen drawables, a display-link object that never fires).
// The test paces its callbacks a 60 Hz period apart, as the drain counts ticks from a frame's wake to its take (nvn_link_latch_spare).
// Cases: a woken present is latched; a standing frame goes out first and nothing is dropped; a drain into the latch empties the queue after 2 s of early frames; a plain
// drain does not enter the latch, and the frame that stood again backs the drain into the latch off; the latch gives up after LINK_LATCH_GIVE_UP timeouts and probes again,
// counted in woken callbacks, with an acquire of two ticks. Queued frames also exercise the callback's hold at different link and present rates.
#include "../nvn/nvn_mtl.m"
@interface FakeLinkUpdate : NSObject
@property id<CAMetalDrawable> drawable;
@property CFTimeInterval targetPresentationTimestamp;
@property CFTimeInterval targetTimestamp;
@end
@implementation FakeLinkUpdate
@end
static CAMetalLayer *g_test_layer;
void *front_metal_layer(void) { return (__bridge void *)g_test_layer; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)
static tex_t g_tex;
static CAMetalLayer *offscreen_layer(void) {
    CAMetalLayer *l = [CAMetalLayer layer]; l.device = g_dev; l.pixelFormat = MTLPixelFormatBGRA8Unorm; l.framebufferOnly = NO; l.drawableSize = CGSizeMake(8, 8); return l;
}
static NSMutableArray *g_layers;
static NvMetalDisplayLinkDelegate *g_delegate;
static const double ROOM = 0.0165;   // a built-in ProMotion screen in its fixed 60 Hz mode: 16.5 ms from the callback to targetTimestamp
static double g_gap = 1. / 60;   // seconds between the test's callbacks
static double tick_at(double target) {   // one display-link callback, g_gap after the previous one; returns how long it took (seconds)
    static double last; while (CACurrentMediaTime() < last + g_gap) usleep(200);
    last = CACurrentMediaTime();
    static unsigned n; if (g_layers.count < 8) { CAMetalLayer *nl = offscreen_layer(); [g_q4 addResidencySet:nl.residencySet]; [g_layers addObject:nl]; }   // the queue takes at most 32 residency sets
    CAMetalLayer *l = g_layers[n++ % g_layers.count];
    FakeLinkUpdate *u = [FakeLinkUpdate new];
    u.drawable = [l nextDrawable]; double t0 = CACurrentMediaTime();
    u.targetTimestamp = t0 + ROOM; u.targetPresentationTimestamp = target > 0 ? target : t0 + 0.050;
    [g_delegate metalDisplayLink:(CAMetalDisplayLink *)(id)u needsUpdate:(CAMetalDisplayLinkUpdate *)u];
    return CACurrentMediaTime() - t0;
}
static double tick(void) { return tick_at(0); }
// The game thread: each frame waits `ticks` link ticks, then presents `delay_us` later
static int g_ticks, g_delay_us; static dispatch_semaphore_t t_go, t_done;
static void *game(void *arg) {
    (void)arg;
    for (;;) {
        dispatch_semaphore_wait(t_go, DISPATCH_TIME_FOREVER);
        @autoreleasepool { mtl_link_wait_ticks(g_ticks, 2000000000ull); if (g_delay_us) usleep(g_delay_us); present_enqueue(&g_tex, 1. / 60, NULL, 0, NULL); }
        dispatch_semaphore_signal(t_done);
    }
    return NULL;
}
static int waiting(int end) {   // the game's acquire blocks for a tick (end: for the end of a drain into the latch); 0 after a second without
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&g_link_mu); int w = end ? g_link_waiter_end : g_link_waiter != 0; pthread_mutex_unlock(&g_link_mu);
        if (w) return 1;
        usleep(1000);
    }
    return 0;
}
static int next_wakes(void) { pthread_mutex_lock(&g_link_mu); int r = g_link_waiter && g_link_tick_n + 1 >= g_link_waiter; pthread_mutex_unlock(&g_link_mu); return r; }   // the next callback wakes the acquire
static void frame_start(int ticks, int delay_us) { g_ticks = ticks; g_delay_us = delay_us; dispatch_semaphore_signal(t_go); }
static int frame_done(void) { return dispatch_semaphore_wait(t_done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) == 0; }
static uint64_t queued(void) { pthread_mutex_lock(&g_pq_mu); uint64_t n = g_pq_in - g_pq_read; pthread_mutex_unlock(&g_pq_mu); return n; }
static void hold_case(int mode, int screen_hz, int fps, int callbacks, const char *name) {
    g_link_mode = mode;
    g_link_hold_until = 0;
    double step = 1. / screen_hz, target = CACurrentMediaTime() + 0.050;
    const int rate = mtl_link_rate();
    g_link_vperiod = screen_hz == rate ? 0 : 1. / rate;
    g_link_vhalf = g_link_vperiod > 0 ? step / 2 : 0;
    if (g_link_vperiod > 0) {
        // On a 144 Hz screen, the virtual 120 Hz slot at 16.67 ms comes at 13.89 ms and the next at 27.78 ms.
        tick_at(target);
        tick_at(target + step);
        target += 2 * step;
    }
    double period = (1000000000ull / (uint64_t)fps) / 1e9;   // the same whole-nanosecond period as the guest presents
    uint64_t first = present_enqueue(&g_tex, period, NULL, 0, NULL);
    uint64_t second = present_enqueue(&g_tex, period, NULL, 0, NULL);
    tick_at(target);
    int waited = first && second == first + 1 && g_pq_read == first && queued() == 1;
    for (int i = 1; i < callbacks; i++) {
        tick_at(target + i * step);
        waited &= g_pq_read == first && queued() == 1;
    }
    tick_at(target + callbacks * step);
    CHECK(name, waited && g_pq_read == second && queued() == 0 && g_link_dropped == 0);
}
// One frame of a 1-tick acquire: the game blocks, the callback wakes it, the game presents delay_us later. Returns 0 when the game never blocked
static int frame1(int delay_us) { frame_start(1, delay_us); int w = waiting(0); tick(); return frame_done() && w; }
int main(void) { @autoreleasepool {
    alarm(120); if (setvbuf(stdout, NULL, _IONBF, 0)) return 2;
    setenv("DM_PRESENT_HZ", "0", 1); setenv("DM_PRESENT_LATCH", "1", 1); setenv("DM_PRESENT_DRAIN", "1", 1); setenv("DM_RT", "0", 1);
    setenv("DM_PRESENT_SNAP", "0", 1);   // a profile with present_snap on (Breath of the Wild) would turn the latch and the drain off (mtl_snap_on)
    mtl_init();
    g_test_layer = offscreen_layer(); g_layers = [NSMutableArray new];
    mtl_present_layer(8, 8);
    {   // the source texture every present shows
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
        d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d];
        g_tex = (tex_t){.magic=MAGIC('T','E','X',0), .serial=92001}; g_tex.d.width = g_tex.d.height = 8; g_tex.d.depth = g_tex.d.levels = g_tex.d.samples = 1;
        g_tex.d.target = NVN_TEXTURE_TARGET_2D; g_tex.d.format = NVN_FORMAT_RGBA8; serial_put(g_tex.serial, mt); res_add(mt); res_commit();
    }
    // A 60 Hz display link that never fires (the test calls the callback): mtl_link_wait_ticks waits on it, and present_enqueue leaves the queue to the callback
    g_link = [[CAMetalDisplayLink alloc] initWithMetalLayer:g_test_layer];
    g_dm_present_link = 1; g_link_started = 1; g_link_owns_layer = 1; g_link_mode = 1; g_delegate = [NvMetalDisplayLinkDelegate new];
    t_go = dispatch_semaphore_create(0); t_done = dispatch_semaphore_create(0);
    pthread_t th; pthread_create(&th, NULL, game, NULL);
    char name[200];

    // 1. A woken present is latched: the callback waits for it and shows it on the tick that woke its acquire
    CHECK("the game blocks in its acquire", frame1(1000));
    CHECK("woken present latched on its own tick", g_link_latched == 1 && queued() == 0 && g_link_dropped == 0);

    // 2. A standing frame goes out first: a frame queued before the woken callback is shown, the woken present stands, nothing is dropped
    frame_start(1, 0); int w = waiting(0);
    present_enqueue(&g_tex, 1. / 60, NULL, 0, NULL);   // a frame that missed the previous tick
    tick(); int done = frame_done();
    CHECK("standing frame: the callback did not latch over it", w && done && g_link_latched == 1 && g_link_dropped == 0 && queued() == 1);
    CHECK("the next woken callback takes the standing frame in order", frame1(500) && g_link_latched == 1 && g_link_dropped == 0 && queued() == 1);
    CHECK("a latched present then a standing frame with no drain into the latch: no backoff", !g_link_drain.back && !g_link_drain.in);

    // 3. The drain into the latch: early presents in order for 2 s, then one tick more for the acquire, then each woken callback latches its own present
    int frames = 0; uint64_t entered0 = g_link_drain.entries; const int need = (int)(LINK_DRAIN_SECS * 60);
    while (g_link_drain.entries == entered0 && frames < need + 10) { frames += frame1(500); }
    snprintf(name, sizeof name, "the drain into the latch comes after %d early frames in order (2 s: %d)", frames, need);
    CHECK(name, g_link_drain.entries == entered0 + 1 && frames >= need - 6 && frames <= need + 2 && g_link_dropped == 0 && (atomic_load(&g_link_drain_req) & LINK_DRAIN_LATCH));   // case 2's last frame counts too
    // Its drain: the acquire passes its own tick (the standing frame goes out, nothing latched) and blocks for one more, which ends the drain
    uint64_t latched0 = g_link_latched;
    frame_start(1, 500); usleep(3000); tick();
    CHECK("the drain's first tick shows the standing frame", waiting(1) && g_link_latched == latched0 && queued() == 0);
    tick(); done = frame_done();
    CHECK("the drain's end tick latches the woken present", done && g_link_latched == latched0 + 1 && queued() == 0 && g_link_dropped == 0);
    int ok = 1; for (int i = 0; i < 5; i++) ok &= frame1(500) && queued() == 0;
    CHECK("from then on every woken present is latched", ok && g_link_latched == latched0 + 6 && g_link_dropped == 0);

    // 4. A plain drain (no LINK_DRAIN_LATCH) does not enter the latch: it sets no waiter. The drain must have run (g_link_drained): a game thread that entered its acquire
    // after the tick would return at once and drop the request, and there would be no waiter with or without one
    frame_start(1, 0); waiting(0); present_enqueue(&g_tex, 1. / 60, NULL, 0, NULL); tick(); frame_done();   // a late frame: one stands again
    latched0 = g_link_latched; uint64_t drained0 = atomic_load(&g_link_drained);
    atomic_store(&g_link_drain_req, 1);
    frame_start(1, 500);
    for (int i = 0; i < 1000 && atomic_load(&g_link_drain_req); i++) usleep(1000);   // the acquire took the request
    usleep(3000); tick(); usleep(3000);
    pthread_mutex_lock(&g_link_mu); int gw = g_link_waiter_end, wt = g_link_waiter; pthread_mutex_unlock(&g_link_mu);
    tick(); done = frame_done();
    CHECK("a plain drain sets no latch waiter", done && atomic_load(&g_link_drained) == drained0 + 1 && !gw && !wt && g_link_latched == latched0 && g_link_dropped == 0);
    CHECK("the late frame 6 latched frames after the drain into the latch backed it off", g_link_drain.back == 1 && !g_link_drain.in);

    // 5. Give-up and probe with a 2-tick acquire: the tick in between takes the standing frame, the woken one finds the queue empty and its present comes after the wait
    uint64_t out0 = g_link_latch_out; int to = 0;
    // Callbacks 30 ms apart here, so a present 22 ms after its wake lies well past the wait (14.4 ms) and well before the next callback; frames stand longer than a tick
    // then, and the drain would ask for ticks of its own: keep it out
    g_gap = 0.030;
    #define NO_DRAIN() do { atomic_store(&g_link_drain_req, 0); g_link_drain.run = g_link_drain.erun = 0; } while (0)
    for (int i = 0; g_link_latch_misses < LINK_LATCH_GIVE_UP && i < 3 * LINK_LATCH_GIVE_UP; i++) {
        NO_DRAIN(); frame_start(2, 22000); waiting(0);
        while (!next_wakes()) tick();   // the tick in between takes the frame that stands
        tick(); to += frame_done();     // the woken one: its present comes 22 ms after the wake, past the wait (14.4 ms), and stands
    }
    snprintf(name, sizeof name, "%d presents 22 ms after their wake: %llu timeouts (%u in a row), %llu dropped", to, (unsigned long long)(g_link_latch_out - out0), g_link_latch_misses, (unsigned long long)g_link_dropped);
    CHECK(name, g_link_latch_out - out0 >= LINK_LATCH_GIVE_UP && g_link_dropped == 0 && g_link_latch_misses >= LINK_LATCH_GIVE_UP);
    // Given up: woken callbacks with an empty queue do not wait, until the LINK_LATCH_PROBE-th one; a prompt present there is latched and ends the give-up
    int waited_early = 0, n = 0; latched0 = g_link_latched;
    while (g_link_latched == latched0 && n < LINK_LATCH_PROBE + 2) {
        NO_DRAIN(); frame_start(2, 1000); waiting(0);
        while (!next_wakes()) tick();
        out0 = g_link_latch_out; tick(); n++; frame_done();
        waited_early += g_link_latch_out != out0;
    }
    snprintf(name, sizeof name, "a 2-tick acquire probes within %d woken callbacks (took %d), no waits in between (%d)", LINK_LATCH_PROBE, n, waited_early);
    CHECK(name, g_link_latched == latched0 + 1 && n <= LINK_LATCH_PROBE && waited_early == 0 && g_link_latch_misses == 0 && g_link_dropped == 0);

    // 6. Entry gate with GPU timing (issue #3251): early CPU presents whose GPU work finishes past room - tick / 8 never drain into the latch
    g_gap = 1. / 60; g_link_drain = (nvn_link_drain){0}; g_link_latch_health = (nvn_link_latch_health){0}; atomic_store(&g_link_drain_req, 0); g_link_latch_misses = 0;
    present_enqueue(&g_tex, 1. / 60, NULL, 0, NULL);   // put one standing frame in the queue
    uint64_t entries_gpu0 = g_link_drain.entries;
    for (int i = 0; i < need + 20; i++) {
        uint64_t f = 500 + (uint64_t)i;
        atomic_store(&g_cur_frame, f);
        frame_start(1, 500); waiting(0);
        double t_wake = CACurrentMediaTime();
        tick(); frame_done();
        // Record frame f's GPU ending 15.5 ms after its wake (> 14.4 ms limit on 16.5 ms room), read when the next tick takes frame f
        gpu_tag_begin(f);
        gpu_tag_done(f, t_wake + 0.005, t_wake + 0.0155);
    }
    atomic_store(&g_cur_frame, 0);
    CHECK("early CPU presents whose GPU runs past room - tick/8 do not drain into the latch", g_link_drain.entries == entries_gpu0 && !atomic_load(&g_link_drain_req) && queued() == 1);

    // 7. Runtime exit on real Metal 4 late GPU completion (f.GPUEndTime > update.targetTimestamp): immediately exits the latch, restores a standing frame, and backs off
    while (queued()) tick();   // empty the queue before arming the drain watch
    g_link_drain = (nvn_link_drain){0}; g_link_drain.in = 1; g_link_drain.held = 0; g_link_drain.stand = 0;
    g_link_latch_health = (nvn_link_latch_health){0}; g_link_latch_misses = 0;
    pthread_mutex_lock(&g_link_mu); g_link_started_tick = g_link_tick_n; pthread_mutex_unlock(&g_link_mu);
    id<MTLSharedEvent> gate = [g_dev newSharedEvent];
    [g_q4 waitForEvent:gate value:1];   // hold the latched present on the Metal 4 queue past ROOM (16.5 ms)
    uint64_t late_gpu0 = atomic_load(&g_link_latch_late_gpu), exits0 = g_link_latch_exits, latched_lg0 = g_link_latched;
    frame_start(1, 500); waiting(0); tick(); frame_done();
    CHECK("latched present queued while GPU is held", g_link_latched == latched_lg0 + 1 && queued() == 0);
    usleep(22000);   // past update.targetTimestamp (16.5 ms)
    gate.signaledValue = 1;
    for (int i = 0; i < 200 && atomic_load(&g_link_latch_late_gpu) == late_gpu0; i++) usleep(1000);   // wait for feedback handler
    CHECK("Metal 4 feedback detects GPUEndTime > update.targetTimestamp", atomic_load(&g_link_latch_late_gpu) == late_gpu0 + 1);
    // Next callback exits the latch without waiting, leaving the woken present standing in the queue
    frame_start(1, 500); waiting(0); tick(); frame_done();
    CHECK("late GPU forces immediate latch exit and restores a standing frame", g_link_latch_exits == exits0 + 1 && g_link_latch_misses == LINK_LATCH_GIVE_UP && queued() == 1);
    // Following callback takes the standing frame (age >= 0) and backs off the drain
    frame_start(1, 500); waiting(0); tick(); frame_done();
    CHECK("standing frame after late-GPU exit backs off the drain into the latch", g_link_drain.back == 1 && !g_link_drain.in && queued() == 1);

    // 8. Runtime exit on a skipped display-link callback right after entering a drain (even when late_gpu == 0)
    while (queued()) tick();   // empty the queue before arming the drain watch
    g_link_drain = (nvn_link_drain){0}; g_link_drain.in = 1; g_link_drain.held = 0; g_link_drain.stand = 0;
    g_link_latch_health = (nvn_link_latch_health){0}; g_link_latch_misses = 0;
    pthread_mutex_lock(&g_link_mu); g_link_started_tick = g_link_tick_n; pthread_mutex_unlock(&g_link_mu);
    exits0 = g_link_latch_exits;
    frame_start(1, 500); waiting(0); tick(); frame_done();   // frame 1 latched cleanly (g_link_drain.held == 1 < LINK_LATCH_GIVE_UP)
    usleep(35000);   // simulate CAMetalDisplayLink skipping a 60 Hz tick (cb_gap > 1.35 * 16.67 ms)
    frame_start(1, 500); waiting(0); tick(); frame_done();
    CHECK("skipped tick right after a drain into the latch forces immediate exit", g_link_latch_exits == exits0 + 1 && g_link_latch_misses == LINK_LATCH_GIVE_UP && queued() == 1);
    frame_start(1, 500); waiting(0); tick(); frame_done();
    CHECK("standing frame after skipped-tick exit backs off the drain", g_link_drain.back == 1 && !g_link_drain.in && queued() == 1);
    while (queued()) tick();   // leave queue empty for hold_case

    hold_case(2, 120, 60, 2, "60 fps on a 120 Hz link takes the queued frame on the second tick");
    hold_case(2, 144, 60, 3, "60 fps on a virtual 120 Hz link holds through an uneven first tick");
    hold_case(2, 120, 120, 1, "120 fps on a 120 Hz link can take a frame on the next tick");
    hold_case(2, 120, 75, 1, "75 fps on a 120 Hz link can take a frame on the next tick");
    hold_case(1, 60, 30, 2, "30 fps on a 60 Hz link takes the queued frame on the second tick");
    hold_case(3, 30, 30, 1, "30 fps on a 30 Hz link can take a frame on the next tick");
    // 9. mtl_gpu_done: frame 0 is distinct from an untouched zero-initialized slot, stays 0 while any CB is pending, and reports the latest GPUEndTime once all finish
    { uint64_t end = 123;
      CHECK("untouched frame 0 has no GPU record", mtl_gpu_done(0, &end) == -1 && mtl_gpu_ms(0) < 0);
      gpu_tag_begin(0); gpu_tag_begin(0);
      CHECK("frame 0 with pending CBs reports running", mtl_gpu_done(0, &end) == 0);
      gpu_tag_done(0, 10.0, 10.004);
      CHECK("frame 0 with one CB left still reports running", mtl_gpu_done(0, &end) == 0);
      gpu_tag_done(0, 10.002, 10.009);
      CHECK("frame 0 reports done with the latest GPUEndTime", mtl_gpu_done(0, &end) == 1 && end == (uint64_t)(10.009 * 1e9)); }
    printf("t_link_latch_cb: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
