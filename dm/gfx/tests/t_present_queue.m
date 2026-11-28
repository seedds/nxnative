// A present whose drawable the display has not released must not hold back the game's queue (DM_PRESENT_QUEUE=1).
// A present is held on the GPU through mtl_present_test_hold, which stands in for that drawable. Real Metal 4 queues
// and offscreen drawables; the display link's callback is called by hand, as in t_present_alloc.m.
// Non-snapshot mode (DM_PRESENT_SNAP=0, default here):
// 1. While a present is held, a game submission completes on the GPU (within 500 ms).
// 2. While a present is held, the game draws something else into the texture it showed once the next present is taken,
//    and that drawing runs: the held present still shows the old picture (it reads the copy, present_copy).
// 3. With three presents held, a fourth does not start in the ring slot of the first until that one has run: the first
//    still shows its own picture.
// Snapshot mode (DM_PRESENT_SNAP=1, arg "snap"):
// 4. Snapshot presents run on g_pq4 directly from g_snap with no g_pring copy ring allocated; a game submission runs while
//    a snapshot present is held; reusing a g_snap slot waits on g_pres_ev until its held present finishes, and resizing a
//    slot while its present is held keeps the old texture resident until the replacement completes.
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
static tex_t source_h(unsigned serial, unsigned height, uint8_t left, uint8_t right) {   // 8 x height grey, left 4 cols one value, right 4 another
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:height mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d]; NSMutableData *buf = [NSMutableData dataWithLength:32 * height]; uint8_t *bytes = buf.mutableBytes;
    for (unsigned y = 0; y < height; y++) for (unsigned x = 0; x < 8; x++) {
        uint8_t v = x < 4 ? left : right; size_t o = (y * 8 + x) * 4;
        bytes[o] = bytes[o + 1] = bytes[o + 2] = v; bytes[o + 3] = 255;
    }
    [mt replaceRegion:MTLRegionMake2D(0, 0, 8, height) mipmapLevel:0 withBytes:bytes bytesPerRow:32];
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=serial};
    t.d.width = 8; t.d.height = height; t.d.depth = t.d.levels = t.d.samples = 1;
    t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = NVN_FORMAT_RGBA8;
    serial_put(t.serial, mt); res_add(mt); res_commit(); return t;
}
static tex_t source(unsigned serial, uint8_t left, uint8_t right) { return source_h(serial, 8, left, right); }
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
// A game submission, on the queue when this returns (with the encode thread too). paint: a texture it fills with one grey value first (nil: it copies a buffer)
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

static void snap_cases(tex_t *a, tex_t *b, tex_t *c) {
    char name[200];
    // 1. Game submission runs while snapshot present is held, and no g_pring texture is allocated
    id<MTLSharedEvent> ev = hold_presents();
    present_enqueue(a, 1. / 60, NULL, 0, 0, NULL); snap_commit_wait(g_snap_v);
    CHECK("snapshot copy completed before tick", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
    id<CAMetalDrawable> d1 = tick();
    uint64_t fence = submit(nil, 0);
    CHECK("a game submission completes while a snapshot present is held", mtl_wait(fence, 500000000ull));
    int pring_allocs = 0; for (int k = 0; k < 3; k++) pring_allocs += g_pring[k].tex != nil;
    CHECK("snapshot presents allocate no g_pring copy textures", pring_allocs == 0);
    // 2. Slot reuse waits on g_pres_ev while slot 0's present is held
    present_enqueue(b, 1. / 60, NULL, 0, 0, NULL); snap_commit_wait(g_snap_v);
    CHECK("second snapshot copy completed", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
    tick();   // advances g_pq_done past slot 0's ticket while slot 0's present is still held on g_pq4
    uint64_t v_before = g_snap_v;
    present_enqueue(c, 1. / 60, NULL, 0, 0, NULL);   // reuses slot 0; must wait on g_pres_ev before overwriting it
    snap_commit_wait(g_snap_v); usleep(80000);
    snprintf(name, sizeof name, "slot reuse copy waits for held snapshot present (snap_ev %llu < %llu)", (unsigned long long)g_snap_ev.signaledValue, (unsigned long long)g_snap_v);
    CHECK(name, g_snap_v == v_before + 1 && g_snap_ev.signaledValue < g_snap_v);
    release_presents(ev);
    CHECK("held snapshot presents and waiting copy complete", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000] && presents_done());
    CHECK("first held snapshot present kept its own picture", bad_pixels(d1.texture, 50, 150) == 0);
    tick(); CHECK("third snapshot present completed", presents_done());
    // 3. Resize while present is held keeps old texture resident until replacement copy completes
    tex_t tall = source_h(95010, 16, 110, 110), taller = source_h(95011, 24, 180, 180);
    int slot = (int)(atomic_load(&g_snap_next) % (uint64_t)g_snap_n);
    id<MTLTexture> t0 = g_snap[slot].tex;
    present_enqueue(&tall, 1. / 60, NULL, 0, 0, NULL); snap_commit_wait(g_snap_v);
    CHECK("first resize copy completed", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
    id<MTLTexture> t1 = g_snap[slot].tex;
    CHECK("first resize kept old texture in g_snap_res", t0 && t1 != t0 && g_snap[slot].old == t0 && [g_snap_res containsAllocation:t0]);
    ev = hold_presents();
    tick();   // holds present of t1 on g_pq4
    present_enqueue(a, 1. / 60, NULL, 0, 0, NULL); snap_commit_wait(g_snap_v);
    [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000];
    tick();   // advances g_pq_done past t1's ticket while t1's present is still held
    present_enqueue(&taller, 1. / 60, NULL, 0, 0, NULL); snap_commit_wait(g_snap_v);
    CHECK("second resize retired t0 and kept held t1 resident", t0 && ![g_snap_res containsAllocation:t0] && g_snap[slot].old == t1 && t1 && [g_snap_res containsAllocation:t1]);
    release_presents(ev);
    CHECK("resized snapshot copy and held presents completed", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000] && presents_done());
    tick(); CHECK("second resized present completed", presents_done());
    present_enqueue(a, 1. / 60, NULL, 0, 0, NULL); snap_commit_wait(g_snap_v); [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]; tick(); presents_done();
    present_enqueue(&taller, 1. / 60, NULL, 0, 0, NULL); snap_commit_wait(g_snap_v); [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]; tick(); presents_done();
    CHECK("held t1 retired from g_snap_res on next slot reuse", t1 && ![g_snap_res containsAllocation:t1] && !g_snap[slot].old);
    pring_allocs = 0; for (int k = 0; k < 3; k++) pring_allocs += g_pring[k].tex != nil;
    CHECK("no g_pring textures were allocated across snapshot presents", pring_allocs == 0);
}

int main(int argc, char **argv) { @autoreleasepool {
    alarm(30); setvbuf(stdout, NULL, _IONBF, 0);
    int snap = argc > 1 && !strcmp(argv[1], "snap");
    setenv("DM_PRESENT_HZ", "0", 1);
    setenv("DM_PRESENT_SNAP", snap ? "1" : "0", 1);
    setenv("DM_PRESENT_QUEUE", "1", 0);   // default on for this test unless overridden
    mtl_init();
    printf("encode thread %s, snap %s\n", rec_on() ? "on" : "off", snap ? "on" : "off");
    mtl_wait(submit(nil, 0), 2000000000ull);   // before there is a layer: with one, the first submission runs the prepare step, which reads the profile's shader map
    g_test_layer = offscreen_layer(); g_layers = [NSMutableArray new];
    mtl_present_layer(8, 8);
    // Display-link mode without a link thread: the present thread leaves the queue to the callback (t_present_alloc.m)
    g_dm_present_link = 1; g_link_started = 1; g_link_owns_layer = 1; g_delegate = [NvMetalDisplayLinkDelegate new];
    tex_t a = source(95001, 50, 150), b = source(95002, 100, 100), c = source(95003, 200, 200);
    present_enqueue(&a, 1. / 60, NULL, 0, 0, NULL);
    if (snap) { snap_commit_wait(g_snap_v); [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]; }
    tick(); presents_done();   // the first present compiles the present pipeline
    char name[200];

    if (snap) {
        snap_cases(&a, &b, &c);
        printf("t_present_queue (snap, encode thread %s, DM_PRESENT_QUEUE %s): %d checks, %d failed\n", rec_on() ? "on" : "off", g_pq4 ? "on" : "off", checks, failed);
        return failed != 0;
    }

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
