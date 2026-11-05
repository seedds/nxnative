// Acquiring an already returned swapchain image must not fence unrelated later
// game work. Real Metal queues; the producer and later work are event-gated.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft

uint64_t g_t0_ns;
static CAMetalLayer *layer;
void *front_metal_layer(void) { return (__bridge void *)layer; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
static int test_rr, worker_rr;
static int is_rr45(void) {
    int policy = 0; struct sched_param p = {0};
    return !pthread_getschedparam(pthread_self(), &policy, &p) && policy == SCHED_RR && p.sched_priority == 45;
}
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)
static win_t window;
static NVNsync acquired;
static dispatch_semaphore_t acquired_done;
static void *acquire(void *arg) {
    (void)arg;
    @autoreleasepool {
        int index = -1;
        h_nvnWindowAcquireTexture((NVNwindow *)&window, &acquired, &index);
        worker_rr = is_rr45();
    }
    dispatch_semaphore_signal(acquired_done);
    return NULL;
}

int main(int argc, char **argv) { @autoreleasepool {
    alarm(30); if (setvbuf(stdout, NULL, _IONBF, 0)) return 1;
    test_rr = argc > 1 && !strcmp(argv[1], "rr");
    setenv("DM_PRESENT_LINK", "0", 1); setenv("DM_PRESENT_SNAP", "0", 1); setenv("DM_RT", test_rr ? "rr" : "0", 1);
    setenv("DM_PSO_PREPARE", "0", 1);
    mtl_init();
    layer = [CAMetalLayer layer]; layer.device = g_dev; layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.drawableSize = CGSizeMake(8, 8); layer.framebufferOnly = NO;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> src = [g_dev newTextureWithDescriptor:d];
    id<MTLSharedEvent> gate = [g_dev newSharedEvent];
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!src || !gate || !drawable) return 1;
    uint8_t pixels[256]; memset(pixels, 127, sizeof pixels);
    [src replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:pixels bytesPerRow:32];
    res_add(src); res_commit();
    [g_q4 waitForEvent:gate value:1];
    present_classic_prefix(1);
    CHECK("source present submitted", present_classic_drawable(src, drawable, NULL, 0, 1));
    window = (win_t){.ntex=1, .interval=1, .ticket={1}};
    acquired_done = dispatch_semaphore_create(0);
    pthread_t thread; if (pthread_create(&thread, NULL, acquire, NULL)) { gate.signaledValue = 1; return 1; }
    int early = dispatch_semaphore_wait(acquired_done, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) == 0;
    CHECK("acquire keeps the image while its display read is pending", !early);
    gate.signaledValue = 1;
    int ready = early || dispatch_semaphore_wait(acquired_done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0;
    CHECK("acquire returns after the display read completes", ready);
    if (!ready) return 1;
    pthread_join(thread, NULL);
    if (test_rr) CHECK("worker acquire receives fixed round-robin policy", worker_rr);

    // This submission does not use the returned image. Its fence must still
    // block an ordinary queue sync, but not the image's acquire sync.
    [g_q4 waitForEvent:gate value:2];
    mtl_sub_begin(); uint64_t later = mtl_sub_commit();
    NVNsync queue_sync; h_nvnSyncInitialize(&queue_sync, NULL);
    queue_t queue = {0};   // No scheduled NVN submissions: this queue fences the latest Metal work.
    h_nvnQueueFenceSync((NVNqueue *)&queue, &queue_sync, 0, 0);
    int index = -1;
    h_nvnWindowAcquireTexture((NVNwindow *)&window, &acquired, &index);
    if (test_rr) {
        CHECK("another acquire thread receives policy with unchanged frame period", is_rr45());
        CHECK("round-robin does not declare a real-time computation", g_rt_comp_ms == 0);
        render_sched_apply();
        CHECK("repeated acquire preserves round-robin policy", is_rr45());
    }
    CHECK("correct image reacquired", index == 0);
    CHECK("unrelated later GPU work is still blocked", !mtl_wait(later, 0));
    CHECK("acquire sync is already signaled for the returned image", h_nvnSyncWait(&acquired, 0) == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
    CHECK("queue sync still waits for the later work", h_nvnSyncWait(&queue_sync, 0) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
    gate.signaledValue = 2;
    CHECK("ordinary queue sync completes when its gate opens", h_nvnSyncWait(&queue_sync, 5ull * NSEC_PER_SEC) != NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
    printf("acquire sync: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
