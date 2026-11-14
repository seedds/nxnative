// Presents of snapshots on their own queue (g_qpres, DM_PRESENT_QUEUE): real Metal 4 queues and offscreen drawables, the display link's callback called by hand (no window, no game
// files). own: a present runs while the game's queue is held (a gate stands for the next frame's work and for the drawable wait in front of it). slot: the copy that reuses a ring
// slot waits for the present that read it, though that present was processed (committed) and the game's queue is free. Run with DM_PRESENT_QUEUE=0, own fails: the present waits
// behind the gate on g_q4
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
static NSMutableArray *g_layers;
static id<CAMetalDrawable> drawable(void) {   // a layer of its own per drawable (t_present_alloc), resident on both queues
    CAMetalLayer *l = offscreen_layer(); [g_q4 addResidencySet:l.residencySet]; if (g_qpres) [g_qpres addResidencySet:l.residencySet]; [g_layers addObject:l]; return [l nextDrawable];
}
static NvMetalDisplayLinkDelegate *g_delegate;
static id<CAMetalDrawable> tick(void) {   // one display-link callback (it does not look at the link)
    id<CAMetalDrawable> dr = drawable();
    FakeLinkUpdate *u = [FakeLinkUpdate new]; u.drawable = dr; u.targetPresentationTimestamp = CACurrentMediaTime() + 0.016;
    [g_delegate metalDisplayLink:(CAMetalDisplayLink *)(id)u needsUpdate:(CAMetalDisplayLinkUpdate *)u];
    return dr;
}
static int all_done(uint64_t ticket, double s) {   // every present completed up to ticket within s seconds
    pthread_mutex_lock(&g_pq_mu); struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += (time_t)s; ts.tv_nsec += (long)((s - (time_t)s) * 1e9);
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while ((g_pq_inflight || g_pq_done < ticket) && pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) != ETIMEDOUT) {}
    int ok = !g_pq_inflight && g_pq_done >= ticket; pthread_mutex_unlock(&g_pq_mu); return ok;
}
static int bad_pixels(id<MTLTexture> t, uint8_t left, uint8_t right) {
    id<MTLBuffer> out = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 8, 1)
        toBuffer:out destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const uint8_t *p = out.contents; int bad = 0;
    for (int i = 0; i < 64; i++) { uint8_t v = i % 8 < 4 ? left : right; bad += p[4 * i] != v || p[4 * i + 1] != v || p[4 * i + 2] != v; }
    if (bad) { printf("first row:"); for (int i = 0; i < 8; i++) printf(" %u", p[4 * i]); printf(" (want %u x4, %u x4)\n", left, right); }
    return bad;
}
static void copied(void) { snap_commit_wait(g_snap_v); CHECK("the snapshot's copy ran", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]); }
static void own_case(tex_t *a) {
    present_enqueue(a, 1. / 60, NULL, 0, NULL); copied();
    id<MTLSharedEvent> gate = [g_dev newSharedEvent];
    [g_q4 waitForEvent:gate value:1];   // the game's queue: nothing committed to it from here runs until the gate opens
    uint64_t tk = g_pq_in; id<CAMetalDrawable> dr = tick();
    int ran = all_done(tk, 1.0);
    CHECK("the present ran while the game's queue was held", ran);
    gate.signaledValue = 1;
    if (!ran) CHECK("the present ran once the gate opened", all_done(tk, 5.0));
    CHECK("the present shows its snapshot", bad_pixels(dr.texture, 50, 150) == 0);
}
static void slot_case(tex_t *a, tex_t *b, tex_t *c) {
    present_enqueue(c, 1. / 60, NULL, 0, NULL); copied(); tick(); CHECK("a first present ran", all_done(g_pq_in, 5.0));   // the first snapshot makes the present queue
    if (!g_qpres) { CHECK("the present queue exists", 0); return; }
    id<MTLSharedEvent> gate = [g_dev newSharedEvent];
    [g_qpres waitForEvent:gate value:1];   // the present queue: its presents wait (as for a drawable the screen still shows)
    uint64_t v0 = g_snap_v;
    present_enqueue(a, 1. / 60, NULL, 0, NULL); copied(); uint64_t t1 = g_pq_in; id<CAMetalDrawable> d1 = tick();
    present_enqueue(b, 1. / 60, NULL, 0, NULL); copied(); tick();   // taking the second present counts the first as processed (g_pq_done), as it is committed
    pthread_mutex_lock(&g_pq_mu); int processed = g_pq_done >= t1 && g_pq_inflight == 2; pthread_mutex_unlock(&g_pq_mu);
    CHECK("the first present is processed and both are still on the GPU", processed);
    present_enqueue(c, 1. / 60, NULL, 0, NULL);
    CHECK("the third present took a snapshot into the first one's ring slot", g_snap_v == v0 + 3);
    snap_commit_wait(g_snap_v); usleep(100000);
    CHECK("its copy waits for the present that read the slot", g_snap_ev.signaledValue < g_snap_v);
    gate.signaledValue = 1;
    copied();
    CHECK("the first two presents ran", all_done(t1 + 1, 5.0));
    CHECK("the first present shows its own picture, not the copy that came after it", bad_pixels(d1.texture, 50, 150) == 0);
    int slot = (int)((g_snap_next - 1) % (uint64_t)g_snap_n);
    CHECK("the ring slot holds the new copy", bad_pixels(g_snap[slot].tex, 200, 210) == 0);
    tick(); CHECK("the third present ran", all_done(g_pq_in, 5.0));
}
int main(int argc, char **argv) { @autoreleasepool {
    alarm(30); setbuf(stdout, NULL);
    if (argc != 2) return 2;
    setenv("DM_PRESENT_HZ", "0", 1); setenv("DM_PRESENT_SNAP", "1", 1);
    mtl_init();
    if (rec_on()) { state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); CHECK("prior submission completed", mtl_wait(mtl_sub_commit(), 2000000000ull)); }   // starts the encode thread
    g_test_layer = offscreen_layer(); g_layers = [NSMutableArray new];
    mtl_present_layer(8, 8);   // the picture fills the square drawables (present_shape)
    g_dm_present_link = 1; g_link_started = 1; g_link_owns_layer = 1; g_delegate = [NvMetalDisplayLinkDelegate new];   // display-link mode without a link thread (t_present_alloc)
    tex_t a = source(92001, 50, 150), b = source(92002, 100, 100), c = source(92003, 200, 210);
    if (!strcmp(argv[1], "own")) own_case(&a);
    else if (!strcmp(argv[1], "slot")) slot_case(&a, &b, &c);
    else return 2;
    printf("present queue: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
