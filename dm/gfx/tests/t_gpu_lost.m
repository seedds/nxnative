// GPU access revoked (nvn_mtl.m gpu_lost_check; issue #1326) and recoverable unsignaled fence completion (post_complete -> fence_ev_raise):
// - submission 2 skips signaling g_ev without revoking GPU access (imitating a recoverable command buffer error whose feedback completes without signaling g_ev), followed by a normal GPU-signaled submission 3;
// - from submission 4 on (DM_TEST_GPU_LOST=4), each submission's feedback reports access revoked in the shape the #1326 logs show, and the GPU does not signal its fence.
// Checks error shape matching, single front-end notification, immediate SyncWait wake on both recoverable and revoked unsignaled submissions, recovery on the next GPU-signaled submission, and stopped presents.
// Real Metal 4 queue, no window, no game files
#include "../nvn/nvn_mtl.m"
static _Atomic int g_layer_asks;   // present_now asks for the layer just before it takes a drawable; with none it returns, as without a window
void *front_metal_layer(void) { atomic_fetch_add(&g_layer_asks, 1); return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
extern _Atomic int test_gpu_lost_calls;   // t_gpu_lost_front.c
static int checks, failed;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)
#define WAIT_NS 2000000000ull   // the SyncWait's timeout: without the release, the wait on a fence the GPU never signals ran this long
static NSError *nested(NSString *domain, NSInteger code, NSError *under) {   // as Metal 4 reports a command buffer's error: the cause under NSUnderlyingErrorKey and NSMultipleUnderlyingErrorsKey
    return [NSError errorWithDomain:domain code:code userInfo:under ? @{NSUnderlyingErrorKey: under, NSMultipleUnderlyingErrorsKey: @[under]} : nil];
}
static int revoked(NSError *e) { return gpu_err_has(e, MTL4CommandQueueErrorAccessRevoked, 0); }
static uint64_t submit_wait(int *ok, double *ms) {   // one empty submission and the game's SyncWait on it
    mtl_sub_begin(); uint64_t f = mtl_sub_commit();
    uint64_t t0 = now_ns(); *ok = mtl_wait(f, WAIT_NS); *ms = (now_ns() - t0) / 1e6;
    return f;
}
static tex_t picture(void) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    // NOLINTBEGIN(clang-analyzer-osx.cocoa.RetainCount): serial_put retains the texture in g_by_serial through hm_put_id's CFBridgingRetain
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d];
    tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 92001};
    t.d.width = t.d.height = 8; t.d.depth = t.d.levels = t.d.samples = 1; t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = NVN_FORMAT_RGBA8;
    serial_put(t.serial, mt); res_add(mt); res_commit(); return t;
    // NOLINTEND(clang-analyzer-osx.cocoa.RetainCount)
}
static int present_asks(const tex_t *t) { int before = atomic_load(&g_layer_asks); mtl_present_wait(mtl_present(t, 1. / 60, NULL, NULL)); return atomic_load(&g_layer_asks) - before; }
int main(void) { @autoreleasepool {
    alarm(60); setvbuf(stdout, NULL, _IONBF, 0);
    setenv("DM_TEST_GPU_LOST", "4", 1);
    setenv("DM_PRESENT_LINK", "0", 1); setenv("DM_PRESENT_SNAP", "0", 1);   // presents on the present thread, straight from the game's texture

    NSString *mtl4 = MTL4CommandQueueErrorDomain, *iogpu = @"IOGPUCommandQueueErrorDomain";
    CHECK("the revoked access is found under Metal's top-level timeout", revoked(gpu_lost_test_error()));
    CHECK("an innocent victim, whose IOGPU error has code 5 too, is not taken for it", !revoked(nested(mtl4, MTL4CommandQueueErrorTimeout, nested(mtl4, MTL4CommandQueueErrorTimeout, nested(iogpu, 5, nil)))));
    CHECK("a page fault is not taken for it", !revoked(nested(mtl4, MTL4CommandQueueErrorTimeout, nested(mtl4, MTL4CommandQueueErrorNotPermitted, nested(iogpu, 11, nil)))));
    CHECK("a hang is not taken for it", !revoked(nested(mtl4, MTL4CommandQueueErrorTimeout, nested(mtl4, MTL4CommandQueueErrorTimeout, nested(iogpu, 3, nil)))));
    CHECK("no error is not taken for it", !revoked(nil));

    mtl_init();
    tex_t pic = picture();
    int ok = 0; double ms = 0; uint64_t f = 0;
    f = submit_wait(&ok, &ms);
    CHECK("normal submission before runs as before", ok && !atomic_load(&g_gpu_lost) && !atomic_load(&test_gpu_lost_calls) && !mtl_last_commit_error());

    // Recoverable unsignaled submission: hold the queue behind a gate so feedback cannot run before g_ev is restored, redirect rec_signal to a decoy event, then release.
    id<MTLSharedEvent> gate = [g_dev newSharedEvent], decoy = [g_dev newSharedEvent], real_ev = g_ev;
    CHECK("test shared events allocated", gate && decoy);
    if (!gate || !decoy) return 1;
    rec_queue_block(^{ [g_q4 waitForEvent:gate value:1]; });
    mtl_sub_begin();
    g_ev = decoy;
    f = mtl_sub_commit();
    g_ev = real_ev;
    gate.signaledValue = 1;
    uint64_t t0 = now_ns(); ok = mtl_wait(f, WAIT_NS); ms = (now_ns() - t0) / 1e6;
    printf("submission %llu (recoverable unsignaled): SyncWait %s in %.1f ms, shared event at %llu\n", (unsigned long long)f, ok ? "returned" : "timed out", ms, (unsigned long long)g_ev.signaledValue);
    CHECK("a recoverable unsignaled submission's SyncWait returns at its feedback", ok && ms < WAIT_NS / 2e6 && g_ev.signaledValue >= f);
    CHECK("a recoverable unsignaled submission does not trigger gpu_lost", !atomic_load(&g_gpu_lost) && !atomic_load(&test_gpu_lost_calls));

    f = submit_wait(&ok, &ms);
    CHECK("a normal GPU-signaled submission still completes after CPU event raise", ok && g_ev.signaledValue >= f && !atomic_load(&g_gpu_lost) && !atomic_load(&test_gpu_lost_calls));
    CHECK("a present before asks for the layer", present_asks(&pic) == 1);

    f = submit_wait(&ok, &ms);
    printf("submission %llu (revoked): SyncWait %s in %.1f ms, shared event at %llu\n", (unsigned long long)f, ok ? "returned" : "timed out", ms, (unsigned long long)g_ev.signaledValue);
    CHECK("the front end is told", atomic_load(&g_gpu_lost) && atomic_load(&test_gpu_lost_calls) == 1);
    CHECK("the first revoked submission's SyncWait returns at its feedback", ok && ms < WAIT_NS / 2e6 && g_ev.signaledValue >= f);
    for (int i = 0; i < 3; i++) {
        f = submit_wait(&ok, &ms);
        printf("submission %llu (revoked): SyncWait %s in %.1f ms, shared event at %llu\n", (unsigned long long)f, ok ? "returned" : "timed out", ms, (unsigned long long)g_ev.signaledValue);
        CHECK("a later revoked submission's SyncWait returns at its feedback", ok && ms < WAIT_NS / 2e6 && g_ev.signaledValue >= f);
    }
    CHECK("the front end is told only once", atomic_load(&test_gpu_lost_calls) == 1);
    CHECK("a present after asks for no layer, and its ticket completes", present_asks(&pic) == 0);
    printf("t_gpu_lost: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
