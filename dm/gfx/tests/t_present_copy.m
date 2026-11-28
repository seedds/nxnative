// What a present shows when it reads the copy of its picture (nvn_mtl.m present_copy, DM_PRESENT_QUEUE): real Metal 4 queues and offscreen drawables, the display link's
// callback called by hand, as in t_present_queue.m. Each case reads the drawable back. Cases:
// 1. An sRGB source shows its stored bytes, as the game's own texture did (the slot's non-sRGB view, present_raw_pf).
// 2. A swizzled source shows its swizzle once: the slot's view takes the swizzle, and a texture copy from a swizzled view writes the storage as stored.
// 3. A new size and then a new format remake each ring slot: the picture is right, and the old textures leave the residency set.
// 4. A present whose crop lies outside its texture copies nothing.
// 5. With MetalFX upscaling, the scaler reads the ring slot in place: a held present shows its picture after the game draws over its texture.
// 6. The suspend drain frees the copy ring, and the next present makes its slot again.
// Cases 1 and 2 hold on either queue; 3-6 need the present's own queue and are skipped with DM_PRESENT_QUEUE=0. Every wait is bounded.
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
static unsigned g_serial = 96000;
// A w x h texture of one value per channel (r, g, b; alpha 255), resident, presentable as serial t
static MTLTextureSwizzleChannels g_swizzle = {MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha};   // the next source's
static tex_t source_f(MTLPixelFormat pf, int w, int h, uint8_t r, uint8_t g, uint8_t b, id<MTLTexture> *out) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView; d.swizzle = g_swizzle;
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d]; NSMutableData *px = [NSMutableData dataWithLength:(NSUInteger)(4 * w * h)]; uint8_t *p = px.mutableBytes;
    int bgra = pf == MTLPixelFormatBGRA8Unorm;
    for (int i = 0; i < w * h; i++) { p[4 * i] = bgra ? b : r; p[4 * i + 1] = g; p[4 * i + 2] = bgra ? r : b; p[4 * i + 3] = 255; }
    [mt replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:p bytesPerRow:4 * w];
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=g_serial++};
    t.d.width = w; t.d.height = h; t.d.depth = t.d.levels = t.d.samples = 1;
    t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = NVN_FORMAT_RGBA8;
    serial_put(t.serial, mt); res_add(mt); res_commit(); if (out) *out = mt; return t;
}
static CAMetalLayer *offscreen_layer(int size) {
    CAMetalLayer *l = [CAMetalLayer layer]; l.device = g_dev; l.pixelFormat = MTLPixelFormatBGRA8Unorm; l.framebufferOnly = NO; l.drawableSize = CGSizeMake(size, size); return l;
}
static NSMutableArray *g_layers;   // every present gets a drawable of its own layer (t_present_alloc.m)
static int g_dsize = 8;   // the drawables' size
static NvMetalDisplayLinkDelegate *g_delegate;
static id<CAMetalDrawable> tick(void) {   // one display-link callback with a fresh drawable
    CAMetalLayer *l = offscreen_layer(g_dsize); [g_layers addObject:l]; id<CAMetalDrawable> dr = [l nextDrawable];
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
static uint64_t present(const tex_t *t, const int crop[4], id<CAMetalDrawable> *dr) {   // queued and taken by the next tick; returns its ticket
    present_enqueue(t, 1. / 60, crop, 0, 0, NULL); id<CAMetalDrawable> d = tick(); if (dr) *dr = d;
    pthread_mutex_lock(&g_pq_mu); uint64_t tk = g_pq_read; pthread_mutex_unlock(&g_pq_mu); return tk;
}
// The drawable's pixels against one colour (BGRA bytes), each channel within tol; prints the first pixel when they differ
static int wrong(id<MTLTexture> t, uint8_t r, uint8_t g, uint8_t b, int tol) {
    NSUInteger w = t.width, h = t.height; id<MTLBuffer> out = [g_dev newBufferWithLength:4 * w * h options:MTLResourceStorageModeShared];
    if (!out) return (int)(w * h);
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:4 * w destinationBytesPerImage:4 * w * h];
    [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const uint8_t *p = out.contents; int bad = 0;
    for (NSUInteger i = 0; i < w * h; i++) bad += abs(p[4 * i] - b) > tol || abs(p[4 * i + 1] - g) > tol || abs(p[4 * i + 2] - r) > tol;
    if (bad) printf("first pixel r g b: %u %u %u (want %u %u %u)\n", p[2], p[1], p[0], r, g, b);
    return bad;
}
// A game submission that fills a texture with one value per channel (RGBA8 storage), on the queue when this returns
static id<MTLBuffer> g_fill;
static uint64_t paint(id<MTLTexture> t, uint8_t v) {
    NSUInteger n = 4 * t.width * t.height;
    if (!g_fill) { g_fill = [g_dev newBufferWithLength:4 * 64 * 64 options:MTLResourceStorageModeShared]; res_add(g_fill); res_commit(); }
    memset(g_fill.contents, v, n);
    state_reset(); memset(&R, 0, sizeof R);
    mtl_sub_begin();
    enc_b_copy_buffer_texture(blit(), g_fill, 0, 4 * t.width, n, MTLSizeMake(t.width, t.height, 1), t, 0, 0, MTLOriginMake(0, 0, 0));
    uint64_t fence = mtl_sub_commit();
    rec_wake_for_fence(fence); sub_commit_wait(fence);
    return fence;
}
int main(void) { @autoreleasepool {
    alarm(60); setvbuf(stdout, NULL, _IONBF, 0);
    setenv("DM_PRESENT_HZ", "0", 1); setenv("DM_PRESENT_SNAP", "0", 1);   // the game's texture is presented, not a snapshot of it
    setenv("DM_FX_DIRECT", "0", 1);   // MetalFX writes only private textures, and these drawables are not (a window's take no shader writes and are never written by it)
    mtl_init();
    printf("encode thread %s, presents on %s\n", rec_on() ? "on" : "off", g_pq4 ? "a queue of their own" : "the game's queue");
    { id<MTLTexture> w; tex_t warm = source_f(MTLPixelFormatRGBA8Unorm, 8, 8, 1, 1, 1, &w); mtl_wait(paint(w, 1), 2000000000ull); (void)warm; }   // before there is a layer: the prepare step reads the profile's shader map
    g_test_layer = offscreen_layer(8); g_layers = [NSMutableArray new];
    mtl_present_layer(8, 8);
    g_dm_present_link = 1; g_link_started = 1; g_link_owns_layer = 1; g_delegate = [NvMetalDisplayLinkDelegate new];   // display-link mode without a link thread (t_present_alloc.m)
    char name[200]; id<CAMetalDrawable> dr; int bad;

    // 1. sRGB: the stored bytes, not their decoded values
    tex_t srgb = source_f(MTLPixelFormatRGBA8Unorm_sRGB, 8, 8, 50, 100, 150, NULL);
    uint64_t tk = present(&srgb, NULL, &dr); presents_done();
    bad = wrong(dr.texture, 50, 100, 150, 1); snprintf(name, sizeof name, "an sRGB source shows its stored bytes (%d of 64 pixels wrong)", bad); CHECK(name, bad == 0);
    if (g_pq4) { id<MTLTexture> v = g_pring[tk % 3].view;   // the slot's own view, not present_raw_src's cache of views of the game's textures
        CHECK("the present samples the slot through its non-sRGB view", v && v.parentTexture == g_pring[tk % 3].tex && v.pixelFormat == MTLPixelFormatRGBA8Unorm); }

    // 2. A swizzled source: (B, G, R, 1) over storage (10, 20, 30) samples as (30, 20, 10). The host's texture lookup drops a view it did not make (own_view_drop_stale), so
    // the presented source swizzles as a texture; the copy from a view is checked on its own below
    g_swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed, MTLTextureSwizzleOne);
    tex_t swz = source_f(MTLPixelFormatRGBA8Unorm, 8, 8, 10, 20, 30, NULL);
    g_swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha);
    present(&swz, NULL, &dr); presents_done();
    bad = wrong(dr.texture, 30, 20, 10, 1); snprintf(name, sizeof name, "a swizzled source shows its swizzle once (%d of 64 pixels wrong)", bad); CHECK(name, bad == 0);
    {   // the copy present_copy encodes, from a swizzled view of a texture to a plain one, on the game's queue: what it writes is the view's storage, not its swizzle
        id<MTLTexture> base; source_f(MTLPixelFormatRGBA8Unorm, 8, 8, 10, 20, 30, &base);
        id<MTLTexture> view = [base newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)
                                                           swizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed, MTLTextureSwizzleOne)];
        id<MTLTexture> dst; source_f(MTLPixelFormatRGBA8Unorm, 8, 8, 0, 0, 0, &dst);
        id<MTL4CommandAllocator> al = [g_dev newCommandAllocator]; id<MTL4CommandBuffer> cb = [g_dev newCommandBuffer]; id<MTLSharedEvent> ev = [g_dev newSharedEvent];
        if (!al || !cb || !ev || !view) { CHECK("the copy's command buffer and view are made", 0); return 1; }
        [cb beginCommandBufferWithAllocator:al];
        id<MTL4ComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce copyFromTexture:view sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 8, 1) toTexture:dst destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [ce endEncoding]; cb_end(cb, nil);
        pthread_mutex_lock(&g_res_commit_mu); [g_q4 commit:&cb count:1]; [g_q4 signalEvent:ev value:1]; pthread_mutex_unlock(&g_res_commit_mu);
        CHECK("a texture copy from a swizzled view writes the view's storage as stored", [ev waitUntilSignaledValue:1 timeoutMS:2000] && wrong(dst, 30, 20, 10, 0) == 0);   // RGBA bytes 10 20 30, read in wrong's BGRA order
    }

    if (!g_pq4) { printf("DM_PRESENT_QUEUE=0: cases 3-6 need the present's own queue\n"); printf("t_present_copy: %d checks, %d failed\n", checks, failed); return failed != 0; }

    // 3. Remakes: every slot at 8x8 RGBA8, then at 16x16, then in BGRA8
    tex_t small = source_f(MTLPixelFormatRGBA8Unorm, 8, 8, 60, 60, 60, NULL), big = source_f(MTLPixelFormatRGBA8Unorm, 16, 16, 90, 90, 90, NULL), bgra = source_f(MTLPixelFormatBGRA8Unorm, 16, 16, 40, 80, 160, NULL);
    for (int i = 0; i < 3; i++) present(&small, NULL, NULL);
    presents_done();
    id<MTLTexture> old[3]; for (int k = 0; k < 3; k++) old[k] = g_pring[k].tex;
    int ok = 1; for (int i = 0; i < 3; i++) { present(&big, NULL, &dr); presents_done(); ok &= wrong(dr.texture, 90, 90, 90, 1) == 0; }
    int remade = 1; for (int k = 0; k < 3; k++) remade &= g_pring[k].tex != old[k] && g_pring[k].tex.width == 16 && ![g_res containsAllocation:old[k]] && [g_res containsAllocation:g_pring[k].tex];
    CHECK("a new size remakes every slot, the picture is right and the old textures left the residency set", ok && remade);
    for (int k = 0; k < 3; k++) old[k] = g_pring[k].tex;
    ok = 1; for (int i = 0; i < 3; i++) { present(&bgra, NULL, &dr); presents_done(); ok &= wrong(dr.texture, 40, 80, 160, 1) == 0; }
    remade = 1; for (int k = 0; k < 3; k++) remade &= g_pring[k].tex != old[k] && g_pring[k].tex.pixelFormat == MTLPixelFormatBGRA8Unorm && ![g_res containsAllocation:old[k]];
    CHECK("a new format remakes every slot, the picture is right and the old textures left the residency set", ok && remade);
    old[0] = old[1] = old[2] = nil;

    // 4. A crop outside the texture: no copy (the copy event does not reach its ticket once the game's queue has run everything)
    static const int out_of_bounds[4] = {4, 4, 8, 8};
    tk = present(&small, out_of_bounds, NULL);
    { id<MTLTexture> w; source_f(MTLPixelFormatRGBA8Unorm, 8, 8, 1, 1, 1, &w); mtl_wait(paint(w, 1), 2000000000ull); }
    snprintf(name, sizeof name, "a present whose crop lies outside its texture copies nothing (copy event %llu, ticket %llu)", (unsigned long long)g_pring_ev.signaledValue, (unsigned long long)tk);
    CHECK(name, g_pring_ev.signaledValue < tk && presents_done());
    present(&small, NULL, &dr); presents_done();
    CHECK("the next present shows its picture", wrong(dr.texture, 60, 60, 60, 1) == 0);

    // 5. MetalFX: a 32x32 picture in a 64x64 drawable, held while the game draws over its texture
    g_dsize = 64; mtl_present_layer(32, 32);
    id<MTLTexture> fxm; tex_t fxs = source_f(MTLPixelFormatRGBA8Unorm, 32, 32, 120, 120, 120, &fxm);
    if (![MTLFXSpatialScalerDescriptor supportsMetal4FX:g_dev]) printf("case 5: no Metal 4 MetalFX on this GPU, skipped\n");
    else {
        g_fx_mode = 1;   // DM_PRESENT_UPSCALE=metalfx, which fx_mode reads once
        present(&fxs, NULL, NULL); presents_done();   // makes the scaler
        uint64_t inplace0 = g_fx_present.in_place_n;
        id<MTLSharedEvent> hold = [g_dev newSharedEvent]; mtl_present_test_hold(hold, 1);
        present(&fxs, NULL, &dr);
        mtl_present_test_hold(nil, 0);
        uint64_t fence = paint(fxm, 220);
        CHECK("the game's drawing runs while the upscaled present is held", mtl_wait(fence, 500000000ull));
        hold.signaledValue = 1;
        CHECK("the upscaled present completes", presents_done());
        bad = wrong(dr.texture, 120, 120, 120, 4); snprintf(name, sizeof name, "MetalFX scaled the picture the present was given (%d of 4096 pixels wrong)", bad); CHECK(name, bad == 0);
        snprintf(name, sizeof name, "MetalFX read the ring slot in place (%llu presents in place, no input copy)", (unsigned long long)(g_fx_present.in_place_n - inplace0));
        CHECK(name, g_fx_present.in_place_n > inplace0 && g_fx_present.in == nil);
    }

    // 6. Suspended, the copy ring goes (nvn_mtl.m present_suspend_wait); the next present after the resume makes its slot again
    id<MTLTexture> held[3]; for (int k = 0; k < 3; k++) held[k] = g_pring[k].tex;
    mtl_set_present_suspended(1); mtl_suspend_drain();
    int gone = 0, made = 0; for (int k = 0; k < 3; k++) { made += held[k] != nil; gone += !g_pring[k].tex && !g_pring[k].view && (!held[k] || ![g_res containsAllocation:held[k]]); }
    snprintf(name, sizeof name, "the suspend drain frees the copy ring (%d of 3 slots made, %d of 3 freed)", made, gone);
    CHECK(name, made == 3 && gone == 3);
    held[0] = held[1] = held[2] = nil;
    mtl_set_present_suspended(0);
    paint(fxm, 120); tk = present(&fxs, NULL, &dr); presents_done();
    CHECK("after the resume a present makes its slot again and shows its picture", g_pring[tk % 3].tex && wrong(dr.texture, 120, 120, 120, 4) == 0);

    printf("t_present_copy (encode thread %s, DM_PRESENT_QUEUE %s): %d checks, %d failed\n", rec_on() ? "on" : "off", g_pq4 ? "on" : "off", checks, failed);
    return failed != 0;
} }
