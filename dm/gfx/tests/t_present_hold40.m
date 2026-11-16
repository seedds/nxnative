// The display link's hold (nvn_mtl.m g_link_hold_until, HOLD_PERIOD_MIN) on a 120 Hz link: a present of 25 ms (40 fps, Ocarina of Time's frame step in ticks of 1/120 s) stays on
// screen exactly three ticks while the next present already waits (the game runs ahead of the screen), 33.3 ms four, and 16.7 ms is not held (the next present is taken at the
// next tick, as before). Real Metal 4 queue and offscreen drawables; the link's callback is called by hand with target times 1/120 s apart (no window, no game files)
#include "../nvn/nvn_mtl.m"
@interface FakeLinkUpdate : NSObject
@property id<CAMetalDrawable> drawable;
@property CFTimeInterval targetPresentationTimestamp;
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
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%-90s %s\n", name, pass ? "PASS" : "FAIL"); } while (0)
static tex_t source(unsigned serial) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d];
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=serial};
    t.d.width = t.d.height = 8; t.d.depth = t.d.levels = t.d.samples = 1;
    t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = NVN_FORMAT_RGBA8;
    serial_put(t.serial, mt); res_add(mt); res_commit(); return t;
}
static CAMetalLayer *offscreen_layer(void) {
    CAMetalLayer *l = [CAMetalLayer layer]; l.device = g_dev; l.pixelFormat = MTLPixelFormatBGRA8Unorm; l.framebufferOnly = NO; l.drawableSize = CGSizeMake(8, 8); return l;
}
static NSMutableArray *g_layers;
static NvMetalDisplayLinkDelegate *g_delegate;
static double g_t;   // the link's target time of the last tick
#define NLAYERS 16   // the queue takes at most 32 residency sets: the ticks take drawables from these layers in turn
static void tick(void) {   // one 120 Hz display-link callback, 1/120 s after the one before
    static unsigned n;
    if (g_layers.count < NLAYERS) { CAMetalLayer *l = offscreen_layer(); [g_q4 addResidencySet:l.residencySet]; [g_layers addObject:l]; }
    CAMetalLayer *l = g_layers[n++ % g_layers.count];
    FakeLinkUpdate *u = [FakeLinkUpdate new]; u.drawable = [l nextDrawable]; g_t += 1.0 / 120; u.targetPresentationTimestamp = g_t;
    [g_delegate metalDisplayLink:(CAMetalDisplayLink *)(id)u needsUpdate:(CAMetalDisplayLinkUpdate *)u];
}
static uint64_t shown(void) { pthread_mutex_lock(&g_pq_mu); uint64_t r = g_pq_read; pthread_mutex_unlock(&g_pq_mu); return r; }
// A present of the given period is shown at a tick while the next one already waits: the ticks until the next is taken
static int ticks_held(tex_t *a, tex_t *b, double period) {
    for (int i = 0; i < 6; i++) tick();   // the case before is over: its last present's hold has ended and nothing waits
    present_enqueue(a, period, NULL, 0, NULL); tick();
    uint64_t first = shown();
    present_enqueue(b, period, NULL, 0, NULL);
    int n = 0; while (shown() == first && n < 8) { tick(); n++; }
    return n;
}
int main(void) { @autoreleasepool {
    alarm(30); setbuf(stdout, NULL);
    setenv("DM_PRESENT_HZ", "0", 1);
    mtl_init();
    g_test_layer = offscreen_layer(); g_layers = [NSMutableArray new];
    mtl_present_layer(8, 8);
    // Display-link mode without a link thread: link_sync sees a started link that is not there yet, the present thread leaves the queue to the callback
    g_dm_present_link = 2; g_link_mode = 2; g_link_started = 1; g_link_owns_layer = 1; g_delegate = [NvMetalDisplayLinkDelegate new];
    g_t = CACurrentMediaTime();
    tex_t a = source(92001), b = source(92002);
    char name[160];
    int n = ticks_held(&a, &b, 3.0 / 120);
    snprintf(name, sizeof name, "40 fps: a 25 ms present stays 3 ticks of the 120 Hz link with the next one waiting (%d)", n); CHECK(name, n == 3);
    n = ticks_held(&a, &b, 3.0 / 120);
    snprintf(name, sizeof name, "  and again: every 25 ms frame, not only the first (%d)", n); CHECK(name, n == 3);
    n = ticks_held(&a, &b, 4.0 / 120);
    snprintf(name, sizeof name, "30 fps (40 fps falling back, 4 ticks): 33.3 ms stays 4 ticks (%d)", n); CHECK(name, n == 4);
    n = ticks_held(&a, &b, 2.0 / 120);
    snprintf(name, sizeof name, "60 fps: a 16.7 ms present is not held, the next is taken at the next tick (%d)", n); CHECK(name, n == 1);
    // A present of 25 ms whose tick comes up to 2 ms early (the link's own jitter) still ends the hold at the third tick: the tolerance is the hold's
    for (int i = 0; i < 6; i++) tick();
    present_enqueue(&a, 3.0 / 120, NULL, 0, NULL); tick(); uint64_t first = shown(); present_enqueue(&b, 3.0 / 120, NULL, 0, NULL);
    g_t -= 0.0015; tick(); tick(); int early = shown() != first; tick();
    snprintf(name, sizeof name, "  a third tick 1.5 ms early ends the 25 ms hold, the second does not (second %s, third %s)", early ? "took it" : "held", shown() != first ? "took it" : "held");
    CHECK(name, !early && shown() != first);
    pthread_mutex_lock(&g_pq_mu); struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 5;
    while (g_pq_inflight && pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) != ETIMEDOUT) {}
    int left = g_pq_inflight; pthread_mutex_unlock(&g_pq_mu);
    CHECK("every present completed", left == 0);
    CHECK("no frame dropped", g_link_dropped == 0);
    printf("present hold at 40 fps: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
