// A texture the game finalizes after presenting it, and a window texture presented before it was ever drawn into (Axiom Verge: MonoGame presents its first window texture, rebuilds its swap
// chain and finalizes the textures it just presented). Real Metal 4 queue and offscreen drawables; the display link's callback is called by hand (no window, no game files).
// A: the finalize of a texture with a present still queued returns only after that present ran on the GPU, and the drawable shows the texture the game had drawn, not a new one of undefined content.
// B: that wait ends when the GPU gets to the finalized texture's ticket (or past it, for a frame the link dropped), not when the queue is empty: a later present that stays in flight does not hold it.
// C: the submit thread (this thread) resolves and zeroes a live texture nothing has resolved yet where it queues the present, so the present thread does not; a texture whose storage the CPU
//    wrote is left alone there, and presenting it on another thread (as the present and display-link threads do) completes. What that present shows is not checked: a texture with CPU content
//    that nothing has resolved on the submit thread is still resolved off it, without its content (tex_mtl_ takes the storage only on the submit thread), as before this change.
// Every command buffer error counts: a [RED] line in the log, a failed readback and mtl_last_commit_error fail the test.
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
static tex_t source(unsigned serial, uint8_t left, uint8_t right) {   // 8x8 grey, the left four columns one value and the right four another, registered under its serial
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
static tex_t in_pool(unsigned serial, pool_t *pool, uint8_t fill) {   // a texture of a memory pool whose storage holds `fill` in every byte, never resolved
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=serial, .pool=pool};
    t.d.width = t.d.height = 8; t.d.depth = t.d.levels = t.d.samples = 1;
    t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = NVN_FORMAT_RGBA8;
    nvn_tex_layout(&t.d, &t.L);
    if (posix_memalign((void **)&t.mem, getpagesize(), 65536)) abort();
    memset(t.mem, fill, 65536); pool->mem = t.mem; pool->size = 65536; mtl_pool_add(t.mem, 65536); return t;
}
static CAMetalLayer *offscreen_layer(void) {
    CAMetalLayer *l = [CAMetalLayer layer]; l.device = g_dev; l.pixelFormat = MTLPixelFormatBGRA8Unorm; l.framebufferOnly = NO; l.drawableSize = CGSizeMake(8, 8); return l;
}
// Every present gets a drawable of its own layer (as in t_present_alloc): one layer hands an earlier present's picture to a later one
static NSMutableArray *g_layers;
static id<CAMetalDrawable> drawable(void) {
    CAMetalLayer *l = offscreen_layer(); [g_q4 addResidencySet:l.residencySet]; [g_layers addObject:l]; return [l nextDrawable];
}
static NvMetalDisplayLinkDelegate *g_delegate;
static id<CAMetalDrawable> tick_with(id<CAMetalDrawable> dr) {   // one display-link callback (it does not look at the link)
    FakeLinkUpdate *u = [FakeLinkUpdate new]; u.drawable = dr; u.targetPresentationTimestamp = CACurrentMediaTime() + 0.016;
    [g_delegate metalDisplayLink:(CAMetalDisplayLink *)(id)u needsUpdate:(CAMetalDisplayLinkUpdate *)u];
    return dr;
}
static id<CAMetalDrawable> tick(void) { return tick_with(drawable()); }
static id<CAMetalDrawable> g_held;
static void *held_tick(void *arg) { (void)arg; @autoreleasepool { tick_with(g_held); } return NULL; }
static id<CAMetalDrawable> tick_elsewhere(void) {   // the callback on a thread that is not the submit thread, as the display link's and the present thread are
    g_held = drawable(); id<CAMetalDrawable> dr = g_held; pthread_t th; pthread_create(&th, NULL, held_tick, NULL); pthread_join(th, NULL); return dr;
}
static int until(int ms, int (^cond)(void)) { for (int i = 0; i < ms; i++) { if (cond()) return 1; usleep(1000); } return cond(); }
static void pq_state(uint64_t *read, uint64_t *done, uint64_t *gpu, int *inflight) {
    pthread_mutex_lock(&g_pq_mu); *read = g_pq_read; *done = g_pq_done; *gpu = g_pq_gpu_done; *inflight = g_pq_inflight; pthread_mutex_unlock(&g_pq_mu);
}
static uint64_t pq_read(void) { uint64_t r, d, g; int i; pq_state(&r, &d, &g, &i); return r; }
static uint64_t pq_done(void) { uint64_t r, d, g; int i; pq_state(&r, &d, &g, &i); return d; }
static uint64_t pq_gpu(void) { uint64_t r, d, g; int i; pq_state(&r, &d, &g, &i); return g; }
static int pq_inflight(void) { uint64_t r, d, g; int i; pq_state(&r, &d, &g, &i); return i; }
// The finalize runs on a thread of its own, so the test sees whether it waits (and fails after a timeout instead of hanging when it never returns). It has found its texture's queued
// present once mtl_present_test_waiters counts it: what the test does next (another present, the callback) comes after that, not after a guess at how long the thread takes to start
#define FAIL_MS 2000   // how long a check waits for something that must happen: far below the ~3.5 s after which a wait on the queue gives up
static _Atomic int g_rel_returned;
static void *release_thread(void *arg) { @autoreleasepool { mtl_tex_release(arg); } atomic_store(&g_rel_returned, 1); return NULL; }
static pthread_t release_start(tex_t *t) { pthread_t th; atomic_store(&g_rel_returned, 0); pthread_create(&th, NULL, release_thread, t); return th; }
static int release_returns_within(int ms) { return until(ms, ^{ return atomic_load(&g_rel_returned); }); }
static int release_waits(void) { return until(FAIL_MS, ^{ return mtl_present_test_waiters() == 1 || atomic_load(&g_rel_returned); }) && !atomic_load(&g_rel_returned); }
static int bad_pixels(id<MTLTexture> t, uint8_t left, uint8_t right) {
    id<MTLBuffer> out = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    if (!out) return 64;
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 8, 1)
        toBuffer:out destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) { printf("readback command buffer status %ld\n", (long)cb.status); return 64; }
    const uint8_t *p = out.contents; int bad = 0;
    for (int i = 0; i < 64; i++) { uint8_t v = i % 8 < 4 ? left : right; bad += p[4 * i] != v || p[4 * i + 1] != v || p[4 * i + 2] != v; }
    if (bad) { printf("first row:"); for (int i = 0; i < 8; i++) printf(" %u", p[4 * i]); printf(" (want %u x4, %u x4)\n", left, right); }
    return bad;
}
// The log goes through a pipe to the real stderr, and a thread counts its [RED] lines (the display command buffer's error handler logs one)
static int g_err_saved; static _Atomic int g_red_lines;
static void *log_tee(void *arg) {
    FILE *f = fdopen((int)(intptr_t)arg, "r"); char *line = NULL; size_t cap = 0; ssize_t n;
    while (f && (n = getline(&line, &cap, f)) > 0) { if (strstr(line, "[RED]")) atomic_fetch_add(&g_red_lines, 1); if (write(g_err_saved, line, (size_t)n) < 0) break; }
    free(line); if (f) fclose(f); return NULL;
}

// A: the present thread takes the frame and is held (mtl_present_test_stall(UINT64_MAX)) before it resolves the texture, until the finalize has found the queued present and the test has noted
// that nothing is resolved or committed yet; the GPU then holds the display command buffer behind a gate. The game finalizes the texture meanwhile. Without the wait it is gone when the present
// thread resolves its serial, and the picture is a new texture of the serial's description
static void finalize_waits(id<MTLSharedEvent> gate) {
    tex_t a = source(91101, 60, 180); mtl_tex_init(&a);
    [g_q4 waitForEvent:gate value:1];   // nothing committed to the Metal 4 queue from here runs until the gate reaches 1
    uint64_t tk = mtl_present(&a, 1. / 60, NULL);
    CHECK("A: the present is queued", tk != 0);
    mtl_present_test_stall(UINT64_MAX);
    g_held = drawable(); id<CAMetalDrawable> dr = g_held; pthread_t tick_th; pthread_create(&tick_th, NULL, held_tick, NULL);
    CHECK("A: the present thread took the frame", until(FAIL_MS, ^{ return pq_read() >= tk; }));
    pthread_t rel = release_start(&a);
    CHECK("A: the finalize found the queued present and waits", release_waits());
    int uncommitted = pq_inflight() == 0 && pq_done() < tk;   // the callback is held before it resolves the texture, so nothing of this present is on the queue
    CHECK("A: the finalize waits while the present thread has not resolved the texture", uncommitted && !release_returns_within(100) && mtl_present_test_waiters() == 1);
    mtl_present_test_stall(0);   // the callback goes on
    CHECK("A: the display command buffer is on the queue", until(FAIL_MS, ^{ return pq_inflight() == 1; }));
    CHECK("A: the finalize waits while the GPU holds the present", !release_returns_within(200) && pq_gpu() < tk && mtl_present_test_waiters() == 1);
    gate.signaledValue = 1;
    int returned = release_returns_within(FAIL_MS);
    CHECK("A: the finalize returns once its present ran on the GPU", returned && pq_gpu() >= tk);
    pthread_join(tick_th, NULL); pthread_join(rel, NULL); mtl_present_test_stall(0);
    int bad = bad_pixels(dr.texture, 60, 180); printf("A drawable: %d of 64 pixels wrong\n", bad);
    CHECK("A: the picture is the texture the game drew", bad == 0);
}

// B: a texture whose present already ran is released at once even with another present in flight. A texture whose present is in flight when the next frame is taken: the callback then counts
// its ticket done before the GPU ran it (g_pq_done = ticket - 1), which wakes the finalize, and it waits for the GPU to report that ticket or a later one done. With presents continuing the
// queue is never empty, so waiting for no present in flight (the first version) never ended. The GPU gets to a's ticket while b's is still held, and the finalize must return then
// (observed: the feedback of the command buffer right before a wait on the queue came only when that wait gave up, about 3.5 s later; so c follows a, and b sits behind a second gate, which opens
// as soon as the finalize has returned)
static void finalize_liveness(id<MTLSharedEvent> gate) {
    tex_t x = source(91201, 80, 80), p = source(91202, 80, 80), a = source(91203, 80, 80), c = source(91204, 80, 80), b = source(91205, 80, 80);
    mtl_tex_init(&x); mtl_tex_init(&a);
    uint64_t tkx = mtl_present(&x, 1. / 60, NULL); tick();
    CHECK("B: a present that ran completes", until(FAIL_MS, ^{ return pq_done() >= tkx && pq_inflight() == 0; }));
    [g_q4 waitForEvent:gate value:2];
    mtl_present(&p, 1. / 60, NULL); tick();
    CHECK("B: the next present is held in flight", pq_inflight() == 1);
    pthread_t rel = release_start(&x);
    int returned = release_returns_within(FAIL_MS);
    CHECK("B: a finalize returns while another texture's present is in flight", returned && pq_inflight() == 1 && mtl_present_test_waiters() == 0);
    pthread_join(rel, NULL);
    gate.signaledValue = 2;
    CHECK("B: the held present completes", until(FAIL_MS, ^{ return pq_inflight() == 0; }));

    [g_q4 waitForEvent:gate value:3];   // a and c are held behind this gate, b behind the next
    uint64_t tka = mtl_present(&a, 1. / 60, NULL); tick();
    rel = release_start(&a);
    CHECK("B: the finalize found a's ticket and waits", release_waits());   // c is presented only after this: with its ticket taken first the finalize would find nothing to wait for
    uint64_t tkc = mtl_present(&c, 1. / 60, NULL); tick();
    CHECK("B: the callback took c after a", pq_read() == tkc && tkc == tka + 1 && pq_done() >= tka);
    [g_q4 waitForEvent:gate value:4];
    mtl_present(&b, 1. / 60, NULL); tick();
    CHECK("B: the finalize waits while the GPU is behind a's ticket", !release_returns_within(100) && pq_gpu() < tka && pq_inflight() == 3 && mtl_present_test_waiters() == 1);
    gate.signaledValue = 3;   // a and c run, b stays held
    returned = release_returns_within(FAIL_MS);
    int inflight = pq_inflight(); uint64_t gpu = pq_gpu();
    gate.signaledValue = 4;   // at once, also when the check failed: the thread must end
    CHECK("B: the finalize returns once the GPU is at its ticket, with b still in flight", returned && gpu >= tka && inflight >= 1);
    CHECK("B: every present completed", until(FAIL_MS, ^{ return pq_inflight() == 0; }));
    pthread_join(rel, NULL);
}

// C: this thread is the submit thread. A live texture nothing has resolved, over zeroed storage, is resolved and zeroed by present_enqueue itself, and its present on another thread finds it;
// one over storage the CPU wrote (tex_cpu_content) is not resolved there, and its present on another thread completes (no abort in the upload ownership check)
static void enqueue_resolves(void) {
    g_sub_thread = pthread_self(); atomic_store(&g_sub_thread_set, 1);
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED}, pool2 = pool;
    tex_t z = in_pool(91301, &pool, 0); mtl_tex_init(&z);
    CHECK("C: the zeroed texture has not been resolved", !tex_get(z.serial));
    uint64_t zeroed = g_tex_zeroed;
    mtl_present(&z, 1. / 60, NULL);
    CHECK("C: it is resolved before the present returns", tex_get(z.serial) != nil);
    CHECK("C: and zeroed", g_tex_zeroed > zeroed);
    id<CAMetalDrawable> dr = tick_elsewhere();
    CHECK("C: the present on another thread ran", until(FAIL_MS, ^{ return pq_inflight() == 0; }));
    int bad = bad_pixels(dr.texture, 0, 0); printf("C zeroed drawable: %d of 64 pixels wrong\n", bad);
    CHECK("C: the picture is black", bad == 0);

    tex_t w = in_pool(91302, &pool2, 0x7f); mtl_tex_init(&w);
    zeroed = g_tex_zeroed;
    uint64_t tk = mtl_present(&w, 1. / 60, NULL);
    CHECK("C: a texture over CPU-written storage is not resolved at the enqueue", !tex_get(w.serial) && g_tex_zeroed == zeroed);
    tick_elsewhere();
    CHECK("C: its present on another thread completes", until(FAIL_MS, ^{ return pq_done() >= tk && pq_inflight() == 0; }));
    mtl_tex_release(&z); mtl_tex_release(&w); mtl_pool_del(z.mem); mtl_pool_del(w.mem); mtl_drain(); free(z.mem); free(w.mem);
}
int main(void) { @autoreleasepool {
    alarm(60); setvbuf(stdout, NULL, _IONBF, 0);
    int pipefd[2]; pthread_t tee; if (pipe(pipefd)) return 1;
    g_err_saved = dup(2); dup2(pipefd[1], 2); close(pipefd[1]); pthread_create(&tee, NULL, log_tee, (void *)(intptr_t)pipefd[0]);
    setenv("DM_PRESENT_HZ", "0", 1);
    mtl_init();
    mtl_present_layer(1, 1);   // square fixtures: a sized square layer, so the 8x8 picture fills the 8x8 drawable without the display's 16:9 bars (as t_present_crop)
    g_test_layer = offscreen_layer(); g_layers = [NSMutableArray new];
    // Display-link mode without a link thread: link_sync sees a started link that is not there yet, the present thread leaves the queue to the callback
    g_dm_present_link = 1; g_link_started = 1; g_link_owns_layer = 1; g_delegate = [NvMetalDisplayLinkDelegate new];
    id<MTLSharedEvent> gate = [g_dev newSharedEvent];
    if (!gate) { printf("no shared event\n"); return 1; }
    finalize_waits(gate);
    finalize_liveness(gate);
    enqueue_resolves();
    dup2(g_err_saved, 2); pthread_join(tee, NULL);   // the log is complete: the pipe's last writer is gone
    CHECK("no [RED] line in the log, which includes any display command buffer error", atomic_load(&g_red_lines) == 0);
    CHECK("no command buffer error of a submission", mtl_last_commit_error() == 0);
    printf("present finalize: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
