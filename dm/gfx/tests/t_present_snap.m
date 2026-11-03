// Snapshot lifecycle: real NVN acquire/present/suspend, real Metal copies, and controlled encoder/GPU delays.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)
static tex_t source_tex(uint64_t serial, int n, int v) {   // an n x n RGBA8 texture of texels v, registered under serial
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:n height:n mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d]; uint8_t *bytes = malloc((size_t)n * n * 4); memset(bytes, v, (size_t)n * n * 4);
    [mt replaceRegion:MTLRegionMake2D(0, 0, n, n) mipmapLevel:0 withBytes:bytes bytesPerRow:n * 4]; free(bytes);
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=serial};
    t.d.width = t.d.height = n; t.d.depth = t.d.levels = t.d.samples = 1;
    t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = NVN_FORMAT_RGBA8;
    serial_put(t.serial, mt); res_add(mt); res_commit(); return t;
}
static tex_t source_n(unsigned i, int n) { return source_tex(90001 + i, n, 73 + i); }   // n x n texels of 73 + i
static tex_t source(unsigned i) { return source_n(i, 8); }
static id need(id o, const char *what) { if (!o) { fprintf(stderr, "t_present_snap: Metal returned no %s\n", what); exit(1); } return o; }   // a Metal object the test cannot go on without
static id<MTLSharedEvent> new_gate(void) { return need([g_dev newSharedEvent], "shared event"); }
static id<MTLBuffer> read_block(id<MTLTexture> t, NSUInteger x0, NSUInteger y0) {   // the 8 x 8 texels of t at (x0, y0), 4 bytes each
    id<MTLBuffer> out = need([g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared], "buffer");
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x0, y0, 0) sourceSize:MTLSizeMake(8, 8, 1)
        toBuffer:out destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted]; return out;
}
static void pixels(id<MTLTexture> t, int value) {
    id<MTLBuffer> out = read_block(t, 0, 0);
    int bad = 0; for (int i = 0; i < 256; i++) bad += ((uint8_t *)out.contents)[i] != value;
    CHECK("snapshot has the expected pixels", bad == 0);
}
static _Atomic uint64_t gate_note;
static void gate_encoder(void) {
    pthread_mutex_lock(&g_note_mu);
    rec_t *r = rec_put(RO_NOTE, 0); r->a[0] = (uint64_t)(uintptr_t)&gate_note; r->a[1] = 1; rec_publish();
}
static dispatch_semaphore_t suspend_started, suspend_done;
static void *suspend_thread(void *arg) {
    (void)arg; @autoreleasepool {
        dispatch_semaphore_signal(suspend_started); dm_gfx_suspend(1); dispatch_semaphore_signal(suspend_done);
    } return NULL;
}
static void suspend_case(void) {
    tex_t t = source(0);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    CHECK("prior submission completed", mtl_wait(mtl_sub_commit(), 2000000000ull));
    id<MTLSharedEvent> gpu_gate = new_gate();
    [g_q4 waitForEvent:gpu_gate value:1];
    gate_encoder();
    // The OS has stopped the display link before notifying the app to suspend.
    pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 1; pthread_mutex_unlock(&g_pq_mu);
    present_enqueue(&t, 1. / 60, NULL, 0); mtl_present_discard(1);
    suspend_started = dispatch_semaphore_create(0); suspend_done = dispatch_semaphore_create(0);
    pthread_t th; pthread_create(&th, NULL, suspend_thread, NULL);
    dispatch_semaphore_wait(suspend_started, DISPATCH_TIME_FOREVER);
    int returned = dispatch_semaphore_wait(suspend_done, dispatch_time(DISPATCH_TIME_NOW, 100000000)) == 0;
    CHECK("suspend waits for the queued snapshot commit", !returned);
    pthread_mutex_unlock(&g_note_mu); snap_commit_wait(g_snap_v);
    if (!returned) returned = dispatch_semaphore_wait(suspend_done, dispatch_time(DISPATCH_TIME_NOW, 100000000)) == 0;
    CHECK("suspend also waits for snapshot GPU completion", !returned);
    gpu_gate.signaledValue = 1;
    CHECK("snapshot completes after the GPU gate opens", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
    if (!returned) CHECK("suspend finishes after the snapshot", dispatch_semaphore_wait(suspend_done, dispatch_time(DISPATCH_TIME_NOW, 2000000000)) == 0);
    pthread_join(th, NULL); dm_gfx_suspend(0);
}
static void pacing_case(void) {
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    CHECK("prior submission completed", mtl_wait(mtl_sub_commit(), 2000000000ull));
    tex_t textures[4]; win_t win = {.ntex=4}; int indices[4];
    for (int i = 0; i < 4; i++) { textures[i] = source(i); win.tex[i] = (NVNtexture *)&textures[i]; }
    // Several acquires can precede their presents. A full snapshot ring must fall back to texture tickets.
    for (int i = 0; i < 4; i++) h_nvnWindowAcquireTexture((NVNwindow *)&win, NULL, &indices[i]);
    mtl_present_test_stall(150000000); double longest = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t start = now_ns(); h_nvnQueuePresentTexture(NULL, (NVNwindow *)&win, indices[i]);
        double ms = (now_ns() - start) / 1e6; if (ms > longest) longest = ms;
        printf("present %d: %.3f ms, ticket %llu\n", i, ms, (unsigned long long)win.ticket[indices[i]]);
    }
    CHECK("present does not wait for the stalled display", longest < 50);
    CHECK("full ring uses the original texture ticket", win.ticket[indices[2]] != 0 && win.ticket[indices[3]] != 0);
    uint64_t start = now_ns(); int idx; h_nvnWindowAcquireTexture((NVNwindow *)&win, NULL, &idx);
    double ms = (now_ns() - start) / 1e6; printf("acquire: %.3f ms\n", ms);
    CHECK("acquire waits for snapshot capacity", ms >= 50);
    h_nvnQueuePresentTexture(NULL, (NVNwindow *)&win, idx);
    CHECK("snapshot resumes after acquire frees capacity", win.ticket[idx] == 0);
    mtl_present_flush(); mtl_present_test_stall(0);
    for (int i = 0; i < 4; i++) {
        h_nvnWindowAcquireTexture((NVNwindow *)&win, NULL, &idx);
        h_nvnQueuePresentTexture(NULL, (NVNwindow *)&win, idx);
        mtl_present_flush(); snap_commit_wait(g_snap_v);
        CHECK("reused snapshot completed", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
        int slot = (g_snap_next - 1) % g_snap_n; pixels(g_snap[slot].tex, 73 + idx);
    }
}
static void pending_copy_case(void) {
    tex_t t = source(0);
    id<MTLSharedEvent> gpu_gate = new_gate(); [g_q4 waitForEvent:gpu_gate value:1];
    pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 1; pthread_mutex_unlock(&g_pq_mu);
    CHECK("first snapshot queued", present_enqueue(&t, 1. / 60, NULL, 0) == 0);
    CHECK("second snapshot queued", present_enqueue(&t, 1. / 60, NULL, 0) == 0);
    present_drop_for_suspend();   // Tickets are free, but the GPU still owns both allocators.
    uint64_t v = g_snap_v, start = now_ns(), ticket = present_enqueue(&t, 1. / 60, NULL, 0);
    CHECK("pending copy falls back without waiting", ticket != 0 && now_ns() - start < 50000000);
    CHECK("pending copy's allocator was not reused", g_snap_v == v);
    gpu_gate.signaledValue = 1; mtl_drain();
    CHECK("discarded copies finish before drain returns", g_snap_ev.signaledValue >= v);
    pixels(g_snap[0].tex, 73); pixels(g_snap[1].tex, 73);
}
static void snap_present(const tex_t *t) { present_enqueue(t, 1. / 60, NULL, 0); mtl_present_flush(); snap_commit_wait(g_snap_v); }
// A new presented size replaces the ring slot's texture while a present that sampled the old one may still be queued on the GPU (a gate holds the queue): the old texture stays
// resident until the slot's next use, which comes only after the replacing copy, and with it everything queued before, has run
static void resize_case(void) {
    tex_t small = source_n(0, 8), big = source_n(26, 16);
    snap_present(&small); snap_present(&small);
    CHECK("both ring slots copied", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
    id<MTLTexture> old0 = g_snap[0].tex, old1 = g_snap[1].tex;
    id<MTLSharedEvent> gpu_gate = new_gate(); [g_q4 waitForEvent:gpu_gate value:1];   // stands for display command buffers still queued
    snap_present(&big);
    CHECK("slot 0 has a texture of the new size", g_snap[0].tex != old0 && g_snap[0].tex.width == 16);
    CHECK("slot 0's old texture is still resident", [g_snap_res containsAllocation:old0]);
    snap_present(&big);
    CHECK("slot 1's old texture is still resident", [g_snap_res containsAllocation:old1]);
    uint64_t ticket = present_enqueue(&big, 1. / 60, NULL, 0); mtl_present_flush();
    CHECK("a present while slot 0's copy is queued falls back", ticket != 0);
    CHECK("the old texture stays resident while the copy is queued", [g_snap_res containsAllocation:old0]);
    gpu_gate.signaledValue = 1;
    CHECK("the replacing copies complete", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
    snap_present(&big);
    CHECK("slot 0's old texture leaves at the slot's next use", ![g_snap_res containsAllocation:old0] && !g_snap[0].old);
    CHECK("slot 0's new texture is resident", [g_snap_res containsAllocation:g_snap[0].tex]);
    CHECK("slot 1's old texture waits for its own slot", [g_snap_res containsAllocation:old1]);
    CHECK("the copy completes", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
    pixels(g_snap[0].tex, 99);
    snap_present(&big);
    CHECK("slot 1's old texture leaves at its next use", ![g_snap_res containsAllocation:old1] && !g_snap[1].old);
}
static tex_t source_flat(unsigned i, int n, int v) { return source_tex(90101 + i, n, v); }   // n x n texels of value v (any n), RGBA8
// The colour channels of an 8 x 8 block in the middle of t (BGRA8), all within tol of value
static void pixels_near(id<MTLTexture> t, int value, int tol, const char *what) {
    id<MTLBuffer> out = read_block(t, t.width / 2 - 4, t.height / 2 - 4);
    int bad = 0, worst = 0; for (int i = 0; i < 256; i++) { if (i % 4 == 3) continue; int d = abs((int)((uint8_t *)out.contents)[i] - value); if (d > worst) worst = d; bad += d > tol; }
    char b2[160]; snprintf(b2, sizeof b2, "%s (worst channel error %d)", what, worst); CHECK(b2, bad == 0);
}
// The MetalFX scaler at the snapshot (DM_PRESENT_FX_AT_SNAP): a 64 x 64 picture presented on a 256 x 256 canvas is scaled into the ring slot's up texture by the snapshot's command
// buffer, present_now would draw it 1:1 (snap_up_for), a canvas change replaces up (the old one resident until the slot's next use, the scaler remade once the queued snapshots ran),
// and a present on a canvas other than the snapshot's falls back to the raw copy
static void fx_snap_case(void) {
    if (fx_mode() != 1) { printf("present upscaling unavailable on this device: the MetalFX-at-snapshot case is skipped\n"); return; }
    g_snap_canvas_test[0] = g_snap_canvas_test[1] = 256; g_snap_canvas_test_pf = MTLPixelFormatBGRA8Unorm;
    tex_t t = source_flat(0, 64, 100);
    snap_present(&t); snap_present(&t);
    CHECK("both snapshots completed", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:4000]);
    CHECK("the snapshot scaler was made once", g_fx_snap.sc != nil && g_fx_snap.made == 1 && g_fx_snap.out == nil);
    CHECK("slot 0 has a scaled picture of the canvas rectangle's size", g_snap[0].up_valid && g_snap[0].up.width == 256 && g_snap[0].up.height == 256 && g_snap[0].up.pixelFormat == MTLPixelFormatBGRA8Unorm && g_snap[0].up_gen == 1);
    CHECK("slot 1 too, and both are resident", g_snap[1].up_valid && g_snap[1].up.width == 256 && [g_snap_res containsAllocation:g_snap[0].up] && [g_snap_res containsAllocation:g_snap[1].up]);
    pixels(g_snap[0].tex, 100);   // the raw copy stays (the fallback's and the screenshots' source); its 8 x 8 corner
    pixels_near(g_snap[0].up, 100, 3, "the scaled picture holds the flat colour");
    id<MTLTexture> up = nil; MTLViewport vp;
    CHECK("a present on the snapshot's canvas draws the scaled picture 1:1", snap_up_for(0, 1, 256, 256, MTLPixelFormatBGRA8Unorm, &up, &vp) && up == g_snap[0].up && vp.originX == 0 && vp.originY == 0 && vp.width == 256 && vp.height == 256);
    CHECK("a present on a resized canvas falls back to the raw copy", !snap_up_for(0, 1, 300, 256, MTLPixelFormatBGRA8Unorm, &up, &vp) && !snap_up_for(0, 1, 256, 256, MTLPixelFormatBGRA8Unorm_sRGB, &up, &vp));
    CHECK("another snapshot count of the slot does not match", !snap_up_for(0, 2, 256, 256, MTLPixelFormatBGRA8Unorm, &up, &vp));
    uint64_t fb = g_snap_fx_fallback; snap_up_stale(0, 1); snap_up_stale(0, 7); CHECK("a stale picture counts one fallback", g_snap_fx_fallback == fb + 1);
    // The canvas changes (a window resize): the next snapshots scale for 128 x 96 (a 4:3 canvas: the picture's rectangle is 96 x 96 at x 16), the old up textures stay resident until each slot's next use
    id<MTLTexture> old0 = g_snap[0].up, old1 = g_snap[1].up;
    id<MTLSharedEvent> gpu_gate = new_gate(); [g_q4 waitForEvent:gpu_gate value:1];   // stands for display command buffers still queued, which may read old0 / old1
    g_snap_canvas_test[0] = 128; g_snap_canvas_test[1] = 96;
    uint64_t t0 = now_ns(); snap_present(&t); double ms = (now_ns() - t0) / 1e6;
    CHECK("the scaler is remade for the new canvas", g_fx_snap.made == 2 && g_fx_snap.ow == 96 && g_fx_snap.oh == 96);
    CHECK("slot 0's scaled picture has the new rectangle's size, at its place on the canvas", g_snap[0].up != old0 && g_snap[0].up.width == 96 && g_snap[0].up.height == 96 && g_snap[0].up_vx == 16 && g_snap[0].up_vy == 0 && g_snap[0].up_gen == 2);
    CHECK("slot 0's old scaled picture is still resident", g_snap[0].old_up == old0 && [g_snap_res containsAllocation:old0]);
    printf("the first snapshot after the canvas change took %.1f ms (the scaler's remake waits for the queued snapshots: the gate was closed, so it waited for nothing queued after it)\n", ms);
    snap_present(&t);
    CHECK("slot 1's old scaled picture is still resident", g_snap[1].old_up == old1 && [g_snap_res containsAllocation:old1]);
    gpu_gate.signaledValue = 1;
    CHECK("the replacing snapshots complete", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:4000]);
    snap_present(&t);
    CHECK("slot 0's old scaled picture leaves at the slot's next use", !g_snap[0].old_up && ![g_snap_res containsAllocation:old0] && [g_snap_res containsAllocation:g_snap[0].up]);
    CHECK("slot 1's old scaled picture waits for its own slot", [g_snap_res containsAllocation:old1]);
    CHECK("the snapshot completes", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:4000]);
    pixels_near(g_snap[0].up, 100, 3, "the picture scaled for the new canvas holds the flat colour");
    CHECK("a present on the new canvas draws it 1:1 at x 16", snap_up_for(0, 3, 128, 96, MTLPixelFormatBGRA8Unorm, &up, &vp) && vp.originX == 16 && vp.width == 96 && vp.height == 96);
    // The snapshot's GPU time is recorded by its version, for @snap_gpu_ms
    CHECK("the snapshot command buffer's GPU time is recorded", g_snap_gpu[g_snap_v % PRING].v == g_snap_v && g_snap_gpu[g_snap_v % PRING].ms > 0);
    // With the switch off the raw copy alone is taken and the slot's picture is not valid for the present
    g_snap_canvas_test[0] = 0;   // no canvas: as with no layer
    snap_present(&t); CHECK("the snapshot completes without a canvas", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:4000]);
    CHECK("no canvas: the slot's scaled picture is not offered to the present", !g_snap[1].up_valid && !snap_up_for(1, g_snap[1].up_gen, 128, 96, MTLPixelFormatBGRA8Unorm, &up, &vp));
}
int main(int argc, char **argv) { @autoreleasepool {
    alarm(15); setbuf(stdout, NULL);
    if (argc != 2) return 2;
    setenv("DM_PRESENT_SNAP", "1", 1); setenv("DM_PRESENT_HZ", "0", 1);
    if (!strcmp(argv[1], "fxsnap")) setenv("DM_PRESENT_UPSCALE", "metalfx", 1);
    mtl_init();
    if (!strcmp(argv[1], "suspend")) suspend_case();
    else if (!strcmp(argv[1], "pacing")) pacing_case();
    else if (!strcmp(argv[1], "copy")) pending_copy_case();
    else if (!strcmp(argv[1], "resize")) resize_case();
    else if (!strcmp(argv[1], "fxsnap")) fx_snap_case();
    else return 2;
    printf("present snapshots: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
