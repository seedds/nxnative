// A present whose drawable the display has not released must not hold back the game's queue. A present is held on the GPU through mtl_present_test_hold, which stands in
// for that drawable. Real Metal 4 queue and offscreen drawables; the display link's callback is called by hand, as in t_present_alloc.m. Cases:
// 1. While a present is held, a game submission completes on the GPU (within 500 ms).
// 2. While a present is held, the game draws something else into the texture it showed once the next present is taken, and that drawing runs: the held present still shows
//    the old picture (it reads the copy, nvn_mtl.m present_copy).
// 3. With three presents held, a fourth does not start in the ring slot of the first until that one has run: the first still shows its own picture.
// Cases 1 and 2 hold for presents on a queue of their own (the default; DM_PRESENT_QUEUE=0 fails them).
// Every wait here is bounded, so a present that holds the queue fails a check instead of hanging the test.
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
static tex_t source(unsigned serial, uint8_t left, uint8_t right) {   // 8x8 grey, the left four columns one value and the right four another
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d]; uint8_t bytes[256];
    for (int i = 0; i < 64; i++) { uint8_t v = i % 8 < 4 ? left : right; bytes[4 * i] = bytes[4 * i + 1] = bytes[4 * i + 2] = v; bytes[4 * i + 3] = 255; }
    [mt replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:bytes bytesPerRow:32];
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=serial};
    t.d.width = t.d.height = 8; t.d.depth = t.d.levels = t.d.samples = 1;
    t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = NVN_FORMAT_RGBA8;
    serial_put(t.serial, mt); res_add(mt); res_commit(); return t;
}
static CAMetalLayer *offscreen_layer(void) {
    CAMetalLayer *l = [CAMetalLayer layer]; l.device = g_dev; l.pixelFormat = MTLPixelFormatBGRA8Unorm; l.framebufferOnly = NO; l.drawableSize = CGSizeMake(8, 8); return l;
}
static NSMutableArray *g_layers;   // every present gets a drawable of its own layer (t_present_alloc.m)
static id<CAMetalDrawable> drawable(void) { CAMetalLayer *l = offscreen_layer(); [g_layers addObject:l]; return [l nextDrawable]; }
static NvMetalDisplayLinkDelegate *g_delegate;
static id<CAMetalDrawable> tick_with(id<CAMetalDrawable> dr) {   // one display-link callback
    FakeLinkUpdate *u = [FakeLinkUpdate new]; u.drawable = dr; u.targetPresentationTimestamp = CACurrentMediaTime() + 0.016; u.targetTimestamp = CACurrentMediaTime() + 0.008;
    [g_delegate metalDisplayLink:(CAMetalDisplayLink *)(id)u needsUpdate:(CAMetalDisplayLinkUpdate *)u];
    return dr;
}
static id<CAMetalDrawable> tick(void) { return tick_with(drawable()); }
static int presents_done(void) {   // every committed present has completed, within 5 s
    pthread_mutex_lock(&g_pq_mu); struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 5;
    while (g_pq_inflight && pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) != ETIMEDOUT) {}
    int left = g_pq_inflight; pthread_mutex_unlock(&g_pq_mu);
    return left == 0;
}
// A game submission, on the queue when this returns (with the encode thread too). paint: a texture it fills with one grey value first (nil: it copies a buffer, as a
// command buffer with no work is not the game's)
static id<MTLBuffer> g_copy_src, g_copy_dst;
static uint64_t submit(id<MTLTexture> paint, uint8_t v) {
    if (!g_copy_src) {
        g_copy_src = [g_dev newBufferWithLength:1 << 20 options:MTLResourceStorageModeShared]; g_copy_dst = [g_dev newBufferWithLength:1 << 20 options:MTLResourceStorageModePrivate];
        res_add(g_copy_src); res_add(g_copy_dst); res_commit();
    }
    state_reset(); memset(&R, 0, sizeof R);
    mtl_sub_begin();
    if (paint) {
        uint8_t *p = g_copy_src.contents; for (int i = 0; i < 64; i++) { p[4 * i] = p[4 * i + 1] = p[4 * i + 2] = v; p[4 * i + 3] = 255; }
        enc_b_copy_buffer_texture(blit(), g_copy_src, 0, 32, 256, MTLSizeMake(8, 8, 1), paint, 0, 0, MTLOriginMake(0, 0, 0));
    } else enc_b_copy_buffer(blit(), g_copy_src, 0, g_copy_dst, 0, g_copy_src.length);
    uint64_t fence = mtl_sub_commit();
    rec_wake_for_fence(fence); sub_commit_wait(fence);
    return fence;
}
static int bad_pixels(id<MTLTexture> t, uint8_t left, uint8_t right) {   // read back on the classic queue
    id<MTLBuffer> out = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    if (!out) return 64;
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 8, 1)
        toBuffer:out destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const uint8_t *p = out.contents; int bad = 0;
    for (int i = 0; i < 64; i++) { uint8_t v = i % 8 < 4 ? left : right; bad += p[4 * i] != v || p[4 * i + 1] != v || p[4 * i + 2] != v; }
    if (bad) { printf("first row:"); for (int i = 0; i < 8; i++) printf(" %u", p[4 * i]); printf(" (want %u x4, %u x4)\n", left, right); }
    return bad;
}
static id<MTLSharedEvent> hold_presents(void) { id<MTLSharedEvent> ev = [g_dev newSharedEvent]; mtl_present_test_hold(ev, 1); return ev; }
static void release_presents(id<MTLSharedEvent> ev) { mtl_present_test_hold(nil, 0); ev.signaledValue = 1; }
// The fourth present's tick runs on a thread of its own, so the test sees whether it waits; its drawable is taken before the thread starts
static id<CAMetalDrawable> g_held; static dispatch_semaphore_t g_held_done;
static void *held_tick(void *arg) { (void)arg; @autoreleasepool { tick_with(g_held); } dispatch_semaphore_signal(g_held_done); return NULL; }

int main(void) { @autoreleasepool {
    alarm(30); setvbuf(stdout, NULL, _IONBF, 0);
    setenv("DM_PRESENT_HZ", "0", 1); setenv("DM_PRESENT_SNAP", "0", 1);   // the game's texture is presented, not a snapshot of it
    mtl_init();
    printf("encode thread %s\n", rec_on() ? "on" : "off");
    mtl_wait(submit(nil, 0), 2000000000ull);   // before there is a layer: with one, the first submission runs the prepare step, which reads the profile's shader map
    g_test_layer = offscreen_layer(); g_layers = [NSMutableArray new];
    mtl_present_layer(8, 8);
    // Display-link mode without a link thread: the present thread leaves the queue to the callback (t_present_alloc.m)
    g_dm_present_link = 1; g_link_started = 1; g_link_owns_layer = 1; g_delegate = [NvMetalDisplayLinkDelegate new];
    tex_t a = source(95001, 50, 150), b = source(95002, 100, 100), c = source(95003, 200, 200);
    present_enqueue(&a, 1. / 60, NULL, 0, 0, NULL); tick(); presents_done();   // the first present compiles the present pipeline
    char name[200];

    // 1. The game's queue runs while a present is held
    id<MTLSharedEvent> ev = hold_presents();
    present_enqueue(&a, 1. / 60, NULL, 0, 0, NULL); tick();
    uint64_t fence = submit(nil, 0);
    int ran = mtl_wait(fence, 500000000ull);
    snprintf(name, sizeof name, "a game submission completes while a present is held (fence %llu %s after 500 ms)", (unsigned long long)fence, ran ? "done" : "not done");
    CHECK(name, ran);
    release_presents(ev);
    CHECK("after the hold, the submission and the present complete", mtl_wait(fence, 2000000000ull) && presents_done());

    // 2. A held present keeps its picture when the game draws into the texture it showed
    ev = hold_presents();
    present_enqueue(&a, 1. / 60, NULL, 0, 0, NULL); id<CAMetalDrawable> shown = tick();
    present_enqueue(&b, 1. / 60, NULL, 0, 0, NULL); tick();   // the next present is taken: the texture is the game's again
    id<MTLTexture> am = serial_get(a.serial);
    fence = submit(am, 220);
    CHECK("the drawing runs while the present is held", mtl_wait(fence, 500000000ull));
    release_presents(ev);
    CHECK("after the hold, the drawing and the presents complete", mtl_wait(fence, 2000000000ull) && presents_done());
    int bad = bad_pixels(shown.texture, 50, 150); printf("held drawable: %d of 64 pixels wrong\n", bad);
    CHECK("the held present shows the picture it was given", bad == 0);
    CHECK("the game's drawing landed in its texture", bad_pixels(am, 220, 220) == 0);

    // 3. Three presents held: the fourth uses the ring slot of the first, and waits for that one to run (g_present_alloc_done), so it cannot copy its picture over the first's
    ev = hold_presents();
    present_enqueue(&b, 1. / 60, NULL, 0, 0, NULL); id<CAMetalDrawable> first_dr = tick();
    present_enqueue(&c, 1. / 60, NULL, 0, 0, NULL); tick();
    present_enqueue(&a, 1. / 60, NULL, 0, 0, NULL); tick();
    pthread_mutex_lock(&g_pq_mu); int inflight = g_pq_inflight; uint64_t first = g_pq_read - 2; pthread_mutex_unlock(&g_pq_mu);
    snprintf(name, sizeof name, "three presents held (%d in flight)", inflight); CHECK(name, inflight == 3);
    present_enqueue(&c, 1. / 60, NULL, 0, 0, NULL);
    g_held = drawable(); g_held_done = dispatch_semaphore_create(0); pthread_t th; pthread_create(&th, NULL, held_tick, NULL);
    int returned = dispatch_semaphore_wait(g_held_done, dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC)) == 0;
    snprintf(name, sizeof name, "the fourth present waits for ring slot %llu while ticket %llu is held", (unsigned long long)((first + 3) % 3), (unsigned long long)first);
    CHECK(name, !returned);
    mtl_wait(submit(nil, 0), 2000000000ull);   // the game's queue has run what was committed to it: a copy the fourth present made would have landed
    release_presents(ev);
    if (!returned) returned = dispatch_semaphore_wait(g_held_done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0;
    CHECK("the fourth present goes once the held ones have run", returned);
    if (returned) pthread_join(th, NULL);
    CHECK("every present completed", presents_done());
    bad = bad_pixels(first_dr.texture, 100, 100); printf("first held drawable: %d of 64 pixels wrong\n", bad);
    CHECK("the first held present shows its own picture, not the fourth's", bad == 0);

    printf("t_present_queue (encode thread %s, DM_PRESENT_QUEUE %s): %d checks, %d failed\n", rec_on() ? "on" : "off", g_pq4 ? "on" : "off", checks, failed);
    return failed != 0;
} }
