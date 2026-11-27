// The queue waits behind the DM_NVN_TSV rows @pres_q_ms (mtl_present_queue_ms: a present's command buffer, from its commit to its GPU start) and @gpu_q_ms
// (mtl_gpu_queue_ms: a frame's first command buffer, from the frame's first commit to its GPU start). A present is held on the GPU through mtl_present_test_hold, which
// stands in for a drawable the display has not released. Real Metal 4 queue and offscreen drawables; the display link's callback is called by hand, as in t_present_alloc.m.
// Cases: with nothing held both waits are short; a present held 30 ms reports it; a game submission committed while a present is held waits behind it as long as the
// present's drawable wait is on the game's queue (PRESENT_WAIT_ON_GAME_QUEUE); with the encode thread, submissions it holds count from the commit that sends them
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
// 1 while a present's drawable wait is on the Metal 4 queue that carries the game's rendering: a game submission committed behind a held present then waits as long as it
#define PRESENT_WAIT_ON_GAME_QUEUE 1
enum { HOLD_MS = 30, HELD_MS = 25 };   // how long a present is held, and the least wait that counts as held
static tex_t source(unsigned serial, uint8_t v) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d]; uint8_t bytes[256];
    for (int i = 0; i < 64; i++) { bytes[4 * i] = bytes[4 * i + 1] = bytes[4 * i + 2] = v; bytes[4 * i + 3] = 255; }
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
static NvMetalDisplayLinkDelegate *g_delegate;
static uint64_t tick(void) {   // one display-link callback with a fresh drawable; returns the ticket it took (0 = none)
    CAMetalLayer *l = offscreen_layer(); [g_layers addObject:l];
    FakeLinkUpdate *u = [FakeLinkUpdate new]; u.drawable = [l nextDrawable]; u.targetPresentationTimestamp = CACurrentMediaTime() + 0.016; u.targetTimestamp = CACurrentMediaTime() + 0.008;
    pthread_mutex_lock(&g_pq_mu); uint64_t read0 = g_pq_read; pthread_mutex_unlock(&g_pq_mu);
    [g_delegate metalDisplayLink:(CAMetalDisplayLink *)(id)u needsUpdate:(CAMetalDisplayLinkUpdate *)u];
    pthread_mutex_lock(&g_pq_mu); uint64_t read = g_pq_read; pthread_mutex_unlock(&g_pq_mu);
    return read > read0 ? read : 0;
}
static int presents_done(void) {   // every committed present has completed, within 5 s
    pthread_mutex_lock(&g_pq_mu); struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 5;
    while (g_pq_inflight && pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) != ETIMEDOUT) {}
    int left = g_pq_inflight; pthread_mutex_unlock(&g_pq_mu);
    return left == 0;
}
// A game submission tagged with frame f. It copies a buffer: a command buffer with no work can report the same GPU start and end, which the frame's GPU span does not take
static id<MTLBuffer> g_copy_src, g_copy_dst;
static uint64_t submit_only(uint64_t f) {
    if (!g_copy_src) {
        g_copy_src = [g_dev newBufferWithLength:4 << 20 options:MTLResourceStorageModeShared]; g_copy_dst = [g_dev newBufferWithLength:4 << 20 options:MTLResourceStorageModePrivate];
        res_add(g_copy_src); res_add(g_copy_dst); res_commit();
    }
    mtl_set_frame(f);
    state_reset(); memset(&R, 0, sizeof R);
    mtl_sub_begin();
    enc_b_copy_buffer(blit(), g_copy_src, 0, g_copy_dst, 0, g_copy_src.length);
    return mtl_sub_commit();
}
static uint64_t submit(uint64_t f) {   // and on the queue when this returns, with the encode thread too
    uint64_t fence = submit_only(f);
    rec_wake_for_fence(fence); sub_commit_wait(fence);
    return fence;
}
static double present_q(uint64_t ticket) {   // the present's queue wait once its feedback is in (ms; -1 = none within 2 s)
    double ms = -1;
    for (int i = 0; i < 2000 && !mtl_present_queue_ms(ticket, &ms); i++) usleep(1000);
    return ms;
}
static double gpu_q(uint64_t f) {   // the frame's queue wait once all its command buffers have completed (ms; -1 = none within 2 s)
    double ms = -1;
    for (int i = 0; i < 2000 && (ms = mtl_gpu_queue_ms(f)) < 0; i++) usleep(1000);
    return ms;
}
int main(void) { @autoreleasepool {
    alarm(30); setvbuf(stdout, NULL, _IONBF, 0);
    setenv("DM_PRESENT_HZ", "0", 1); setenv("DM_PRESENT_SNAP", "0", 1);   // the game's texture is presented, not a snapshot of it
    mtl_init();
    printf("encode thread %s\n", rec_on() ? "on" : "off");
    mtl_wait(submit(10), 2000000000ull);   // before there is a layer: with one, the first submission runs the prepare step, which reads the profile's shader map
    g_test_layer = offscreen_layer(); g_layers = [NSMutableArray new];
    mtl_present_layer(8, 8);
    // Display-link mode without a link thread: the present thread leaves the queue to the callback (t_present_alloc.m)
    g_dm_present_link = 1; g_link_started = 1; g_link_owns_layer = 1; g_delegate = [NvMetalDisplayLinkDelegate new];
    tex_t a = source(94001, 120);
    char name[200];
    present_enqueue(&a, 1. / 60, NULL, 0, NULL); tick(); presents_done();   // the first present compiles the present pipeline

    // 1. Nothing held: both waits are short and known
    present_enqueue(&a, 1. / 60, NULL, 0, NULL); uint64_t tk = tick();
    uint64_t fence = submit(20);
    CHECK("an unheld submission completes", mtl_wait(fence, 2000000000ull));
    CHECK("an unheld present completes", tk && presents_done());
    double pq = present_q(tk), gq = gpu_q(20);
    snprintf(name, sizeof name, "unheld: present %.3f ms and submission %.3f ms on the queue, both known and under %d ms", pq, gq, HELD_MS);
    CHECK(name, pq >= 0 && pq < HELD_MS && gq >= 0 && gq < HELD_MS);

    // 2. A present held 30 ms on the GPU, and a game submission committed behind it
    id<MTLSharedEvent> hold = [g_dev newSharedEvent];
    mtl_present_test_hold(hold, 1);
    present_enqueue(&a, 1. / 60, NULL, 0, NULL); tk = tick();
    mtl_present_test_hold(nil, 0);
    fence = submit(30);
    usleep(HOLD_MS * 1000);
    hold.signaledValue = 1;
    CHECK("the submission completes after the hold", mtl_wait(fence, 2000000000ull));
    CHECK("the held present completes", tk && presents_done());
    pq = present_q(tk); gq = gpu_q(30);
    snprintf(name, sizeof name, "a present held %d ms waits %.3f ms on the queue (at least %d)", HOLD_MS, pq, HELD_MS);
    CHECK(name, pq >= HELD_MS);
    if (PRESENT_WAIT_ON_GAME_QUEUE) snprintf(name, sizeof name, "a submission committed behind the held present waits with it: %.3f ms (at least %d)", gq, HELD_MS);
    else snprintf(name, sizeof name, "a submission committed while a present is held does not wait for it: %.3f ms (under %d)", gq, HELD_MS);
    CHECK(name, PRESENT_WAIT_ON_GAME_QUEUE ? gq >= HELD_MS : gq >= 0 && gq < HELD_MS);

    // 3. Two submissions the encode thread holds and then commits in one call (nvn_rec.h rx_commit), as t_feedback.m's merge case: each frame takes the time of that
    // commit, which comes 30 ms after mtl_sub_commit
    if (rec_on()) {
        rec_drain(); uint64_t merged0 = g_rx_merged;
        dispatch_semaphore_t gate = dispatch_semaphore_create(0);
        atomic_fetch_add(&g_rx_busy_hold, (int)rx_hold_limit());   // the encode thread sees work in flight and holds what it is given
        rec_queue_block(^{ dispatch_semaphore_wait(gate, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)); }); rec_publish();   // it stops here meanwhile
        submit_only(40); uint64_t last = submit_only(41);
        usleep(HOLD_MS * 1000);
        dispatch_semaphore_signal(gate);
        for (int i = 0; i < 2000 && atomic_load(&g_sub_cmt) < last; i++) usleep(1000);   // held, then committed together once the GPU seems idle long enough
        atomic_fetch_sub(&g_rx_busy_hold, (int)rx_hold_limit()); rec_wake_for_fence(last); sub_commit_wait(last);
        CHECK("the held submissions complete", mtl_wait(last, 2000000000ull));
        double q40 = gpu_q(40), q41 = gpu_q(41);
        snprintf(name, sizeof name, "held submissions committed together: %llu merged, on the queue %.3f and %.3f ms from that commit (under %d)", (unsigned long long)(g_rx_merged - merged0), q40, q41, HELD_MS);
        CHECK(name, g_rx_merged - merged0 >= 2 && q40 >= 0 && q40 < HELD_MS && q41 >= 0 && q41 < HELD_MS);
    }

    printf("t_present_probes (encode thread %s): %d checks, %d failed\n", rec_on() ? "on" : "off", checks, failed);
    return failed != 0;
} }
