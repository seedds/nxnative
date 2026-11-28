// HDR presents from the copy of their picture (nvn_mtl.m present_copy, DM_PRESENT_QUEUE=1) into an RGBA16Float drawable, the HDR layer's format. Each drawable is read back
// against the same picture drawn by present_offscreen_wait from the game's own texture on g_q4, which takes the same route (present_route) and constants. Real Metal 4 queues
// and offscreen drawables; the display link's callback is called by hand, as in t_present_copy.m. Cases:
// 1. A PQ window (RGB10A2 queued after an HDR answer from vi, ROUTE_PQ_LINEAR): the ring slot keeps the 10-bit format and the drawable matches.
// 2. The same window queued after an SDR answer, with vi's answer HDR again by the time the present runs (ROUTE_ITM on its values as 8-bit ones): the answer the frame was queued
//    with decides the route, and the picture differs from case 1.
// 3. An sRGB 8-bit window (ROUTE_ITM): sampled through the slot's non-sRGB view, so the inverse tone map decodes the stored bytes once.
// The mode is Game's HDR (mtl_hdr_set_requested) on a screen with EDR headroom. The drawable checks hold with DM_PRESENT_QUEUE=0 too (no ring). Every wait is bounded.
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
enum { N = 8 };
static unsigned g_serial = 97000;
// An N x N texture of 32-bit pixels px (left half px[0], right half px[1]), resident, presentable as its serial
static tex_t source(MTLPixelFormat pf, unsigned nvn_format, const uint32_t px[2], id<MTLTexture> *out) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:N height:N mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d]; uint32_t bytes[N * N];
    for (int i = 0; i < N * N; i++) bytes[i] = px[i % N >= N / 2];
    [mt replaceRegion:MTLRegionMake2D(0, 0, N, N) mipmapLevel:0 withBytes:bytes bytesPerRow:4 * N];
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=g_serial++};
    t.d.width = t.d.height = N; t.d.depth = t.d.levels = t.d.samples = 1;
    t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = nvn_format;
    serial_put(t.serial, mt); res_add(mt); res_commit(); *out = mt; return t;
}
static CAMetalLayer *hdr_layer(void) {
    CAMetalLayer *l = [CAMetalLayer layer]; l.device = g_dev; l.pixelFormat = MTLPixelFormatRGBA16Float; l.framebufferOnly = NO; l.drawableSize = CGSizeMake(N, N); return l;
}
static NSMutableArray *g_layers;   // every present gets a drawable of its own layer (t_present_alloc.m)
static NvMetalDisplayLinkDelegate *g_delegate;
static id<CAMetalDrawable> tick(void) {   // one display-link callback with a fresh drawable
    CAMetalLayer *l = hdr_layer(); [g_layers addObject:l]; id<CAMetalDrawable> dr = [l nextDrawable];
    FakeLinkUpdate *u = [FakeLinkUpdate new]; u.drawable = dr; u.targetPresentationTimestamp = CACurrentMediaTime() + 0.016; u.targetTimestamp = CACurrentMediaTime() + 0.008;
    [g_delegate metalDisplayLink:(CAMetalDisplayLink *)(id)u needsUpdate:(CAMetalDisplayLinkUpdate *)u];
    return dr;
}
static int presents_done(void) {   // every committed present has completed, within 5 s
    pthread_mutex_lock(&g_pq_mu); struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 5;
    while (g_pq_inflight && pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) != ETIMEDOUT) {}
    int left = g_pq_inflight; pthread_mutex_unlock(&g_pq_mu);
    return left == 0;
}
// An RGBA16Float texture's pixels as floats, read back on the classic queue
static void read_half(id<MTLTexture> t, float out[4 * N * N]) {
    memset(out, 0, sizeof(float) * 4 * N * N);
    if (!t) return;
    id<MTLBuffer> b = [g_dev newBufferWithLength:8 * N * N options:MTLResourceStorageModeShared];
    if (!b) return;
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(N, N, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:8 * N destinationBytesPerImage:8 * N * N];
    [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const _Float16 *h = b.contents; for (int i = 0; i < 4 * N * N; i++) out[i] = (float)h[i];
}
static float max_diff(const float *a, const float *b) { float m = 0; for (int i = 0; i < 4 * N * N; i++) m = fmaxf(m, fabsf(a[i] - b[i])); return m; }
// Queued with vi's answer vi_hdr and taken by the next tick, while vi's last answer is now_hdr; out: the drawable's pixels. Returns the ticket
static uint64_t present(const tex_t *t, int vi_hdr, int now_hdr, float out[4 * N * N]) {
    present_enqueue(t, 1. / 60, NULL, 0, vi_hdr, NULL);
    atomic_store(&g_hdr_vi_on, now_hdr);
    id<CAMetalDrawable> dr = tick();
    pthread_mutex_lock(&g_pq_mu); uint64_t tk = g_pq_read; pthread_mutex_unlock(&g_pq_mu);
    if (!presents_done()) printf("ticket %llu: the present did not complete within 5 s\n", (unsigned long long)tk);
    read_half(dr.texture, out); return tk;
}
// The game's own texture drawn on g_q4 for vi's answer vi_hdr, into an RGBA16Float target as the drawable
static int reference(id<MTLTexture> src, int vi_hdr, float out[4 * N * N]) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:N height:N mipmapped:NO];
    d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> o = [g_dev newTextureWithDescriptor:d];
    t_present_vi = vi_hdr; int kind = present_offscreen_wait(src, NULL, o); t_present_vi = -1;
    read_half(o, out); return kind == PRESENT_STANDARD;
}
int main(void) { @autoreleasepool {
    alarm(60); setvbuf(stdout, NULL, _IONBF, 0);
    setenv("DM_PRESENT_HZ", "0", 1); setenv("DM_PRESENT_SNAP", "0", 1);   // the game's texture is presented, not a snapshot of it
    setenv("DM_PRESENT_QUEUE", "1", 0);   // default on for this test unless overridden
    mtl_init();
    printf("encode thread %s, presents on %s\n", rec_on() ? "on" : "off", g_pq4 ? "a queue of their own" : "the game's queue");
    g_test_layer = hdr_layer(); g_layers = [NSMutableArray new];
    mtl_present_layer(N, N);
    g_dm_present_link = 1; g_link_started = 1; g_link_owns_layer = 1; g_delegate = [NvMetalDisplayLinkDelegate new];   // display-link mode without a link thread (t_present_alloc.m)
    mtl_hdr_set_requested(2); mtl_present_edr(4, 4);   // Game's HDR on a screen with EDR headroom: hdr_layer_update wants the HDR layer, as the front end would have it
    g_hdr_test_peak = 4;   // the inverse tone map's headroom, the same for the present and its reference
    char name[200]; static float got[4 * N * N], want[4 * N * N], pq[4 * N * N];

    // 1. A PQ window queued after an HDR answer
    id<MTLTexture> pm; const uint32_t pq_px[2] = {594u | 500u << 10 | 400u << 20 | 3u << 30, 300u | 700u << 10 | 520u << 20 | 3u << 30};
    tex_t pt = source(MTLPixelFormatRGB10A2Unorm, NVN_FORMAT_RGB10A2, pq_px, &pm);
    uint64_t tk = present(&pt, 1, 0, pq);
    int ref = reference(pm, 1, want);
    snprintf(name, sizeof name, "a PQ window shows the PQ route's picture (drawable against the direct present: %.5f apart; red %.3f)", max_diff(pq, want), pq[0]);
    CHECK(name, ref && max_diff(pq, want) < 1e-3 && pq[0] > 0.5f);
    if (g_pq4) CHECK("the ring slot keeps the 10-bit format and is sampled as it is", g_pring[tk % 3].tex.pixelFormat == MTLPixelFormatRGB10A2Unorm && g_pring[tk % 3].view == g_pring[tk % 3].tex);

    // 2. The same window queued after an SDR answer, presented while vi's last answer is HDR
    present(&pt, 0, 1, got);
    ref = reference(pm, 0, want);
    snprintf(name, sizeof name, "queued after an SDR answer it takes the inverse tone map (%.5f apart from its direct present, %.3f from the PQ picture)", max_diff(got, want), max_diff(got, pq));
    CHECK(name, ref && max_diff(got, want) < 1e-3 && max_diff(got, pq) > 0.05f);
    atomic_store(&g_hdr_vi_on, 0);

    // 3. An sRGB window
    id<MTLTexture> sm; const uint32_t srgb_px[2] = {200u | 118u << 8 | 50u << 16 | 255u << 24, 20u | 180u << 8 | 240u << 16 | 255u << 24};
    tex_t st = source(MTLPixelFormatRGBA8Unorm_sRGB, NVN_FORMAT_RGBA8_SRGB, srgb_px, &sm);
    tk = present(&st, 0, 0, got);
    ref = reference(sm, 0, want);
    snprintf(name, sizeof name, "an sRGB window shows the inverse tone map of its stored bytes (%.5f apart from its direct present)", max_diff(got, want));
    CHECK(name, ref && max_diff(got, want) < 1e-3 && got[0] > 0);
    if (g_pq4) { id<MTLTexture> v = g_pring[tk % 3].view;
        CHECK("the ring slot is sampled through its non-sRGB view", v && v.parentTexture == g_pring[tk % 3].tex && v.pixelFormat == MTLPixelFormatRGBA8Unorm); }

    printf("t_present_hdr_copy (encode thread %s, DM_PRESENT_QUEUE %s): %d checks, %d failed\n", rec_on() ? "on" : "off", g_pq4 ? "on" : "off", checks, failed);
    return failed != 0;
} }
