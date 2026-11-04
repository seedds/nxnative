// tex_fit's type-compatible view of a texture that is a view itself (the view cache's: a level and layer range, a swizzle) is made from the texture under it, with the same levels,
// layers and swizzle. Metal takes a view's residency from the texture it was made from: a view of a view read as not resident (shader validation) although the texture under both was
// in the residency set (Breath of the Wild's 64x64 cube read from a 2D-array view, found with nvn_replay)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
enum { W = 8, LEVELS = 3, LAYERS = 4, TID = 5 };
static uint8_t texel(NSUInteger level, NSUInteger layer, int c) { return (uint8_t)(16 * level + 4 * layer + c + 1); }   // every level, layer and channel its own value
static void read2d(id<MTLComputePipelineState> pipeline, id<MTLTexture> t, uint32_t level, float out[4]) {
    id<MTLBuffer> b = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; memset(b.contents, 0xcd, 16);
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:pipeline]; [e setTexture:t atIndex:0]; [e setBytes:&level length:sizeof level atIndex:0]; [e setBuffer:b offset:0 atIndex:1];
    [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted && !cb.error, "the GPU reads the fitted view");
    memcpy(out, b.contents, 16);
}
static int same(const float v[4], NSUInteger level, NSUInteger layer, const int ch[4]) {
    for (int c = 0; c < 4; c++) if (fabsf(v[c] - texel(level, layer, ch[c]) / 255.0f) > 1e-4f) return 0;
    return 1;
}
// Observe the compatible Metal view itself, not its parent (which the serial/view
// caches legitimately retain). Each scope drains ARC/autoreleased GPU encoders
// before testing the weak reference. Full unregister is the positive control.
static void finish_retirement(void) {
    mtl_sub_commit();
    @autoreleasepool { mtl_drain(); dispatch_sync(g_feedback_q, ^{}); defer_drain(); }
}
static void check_fitted_pixels(id<MTLComputePipelineState> pipeline, id<MTLTexture> fit) {
    const int swapped[4] = {2, 1, 0, 3}; float v[4];
    CHECK(fit != nil, "registered compatible Metal view exists");
    if (!fit) return;
    read2d(pipeline, fit, 0, v); CHECK(same(v, 1, 2, swapped), "registered fitted view reads level 1, layer 2");
    read2d(pipeline, fit, 1, v); CHECK(same(v, 2, 2, swapped), "registered fitted view reads level 2, layer 2");
}
static void retirement_lifetime(id<MTLComputePipelineState> pipeline, id<MTLTexture> root) {
    const char *names[] = {"unregister control", "descriptor gone", "pool gone", "descriptor gone then same registration", "pool gone then same registration"};
    for (int mode = 0; mode < 5; mode++) {
        fprintf(stderr, "compatible-view lifetime: %s\n", names[mode]);
        pool_t pool = {0};
        tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial(), .pool = &pool};
        t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = NVN_FORMAT_RGBA8,
            .width = W, .height = W, .depth = LAYERS, .levels = LEVELS, .samples = 1};
        nvn_tex_layout(&t.d, &t.L);
        view_t v = {.set_levels = 1, .base_level = 1, .nlevels = 2,
            .set_layers = 1, .min_layer = 2, .nlayers = 2, .set_swizzle = 1,
            .swz = {MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed, MTLTextureSwizzleAlpha}};
        int tid = TID + 1 + mode;
        __weak id<MTLTexture> retired;
        mtl_tex_init(&t); serial_put(t.serial, root); mtl_reg_tex(tid, &t, &v);
        mtl_sub_begin();
        @autoreleasepool {
            uint64_t rid = 0;
            os_unfair_lock_lock(&g_rmu);
            id<MTLTexture> primary = tex_by_id(tid);
            id<MTLTexture> fitted = tex_fit(tid, primary, MTLTextureType2D, &rid);
            os_unfair_lock_unlock(&g_rmu);
            CHECK(primary != root && primary.parentTexture == root, "real registered mip/layer/swizzle cache populated");
            CHECK(fitted && fitted != primary && fitted.parentTexture == root, "compatible view is a distinct real Metal object");
            retired = fitted;
            check_fitted_pixels(pipeline, fitted);
            if (mode == 0) mtl_reg_tex(tid, NULL, NULL);
            else if (mode == 1 || mode == 3) mtl_reg_tex_gone(tid);
            else mtl_alias_drop_pool(&pool);
            if (mode >= 3) mtl_reg_tex(tid, &t, &v);
        }
        @autoreleasepool {
            defer_drain();   // No submission fence has completed: the view must still be usable.
            CHECK(retired != nil, "retirement preserves the compatible view before its submission completes");
            check_fitted_pixels(pipeline, retired);
        }
        finish_retirement();
        @autoreleasepool {
            CHECK(retired == nil, "compatible Metal view released after gone/unregister and submission completion");
        }
        mtl_sub_begin();
        @autoreleasepool {
            if (mode >= 3) {
                uint64_t rid = 0;
                os_unfair_lock_lock(&g_rmu);
                id<MTLTexture> primary = tex_by_id(tid);
                id<MTLTexture> fitted = tex_fit(tid, primary, MTLTextureType2D, &rid);
                os_unfair_lock_unlock(&g_rmu);
                check_fitted_pixels(pipeline, fitted);
            }
            // Also a per-case release control: a failed gone assertion must not
            // be confused with Metal retaining every compatible view regardless.
            mtl_reg_tex(tid, NULL, NULL);
            mtl_tex_release(&t); mtl_vc_drop(t.serial);
        }
        finish_retirement();
        @autoreleasepool {
            CHECK(retired == nil, "full unregister control releases the original compatible Metal view");
        }
    }
}
int main(void) { @autoreleasepool {
    mtl_init(); NSError *error = nil;
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void rd(texture2d<float> t [[texture(0)]], constant uint& level [[buffer(0)]], device float4* out [[buffer(1)]]) { out[0] = t.read(uint2(0, 0), level); }\n";
    id<MTLLibrary> library = [g_dev newLibraryWithSource:source options:nil error:&error];
    id<MTLComputePipelineState> pipeline = library ? [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"rd"] error:&error] : nil;
    if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = MTLTextureType2DArray; d.width = d.height = W; d.arrayLength = LAYERS; d.mipmapLevelCount = LEVELS;
    d.pixelFormat = MTLPixelFormatRGBA8Unorm; d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> root = [g_dev newTextureWithDescriptor:d];
    for (NSUInteger lv = 0; lv < LEVELS; lv++) for (NSUInteger l = 0; l < LAYERS; l++) {
        NSUInteger w = W >> lv; uint8_t px[W * W * 4];
        for (NSUInteger i = 0; i < w * w; i++) for (int c = 0; c < 4; c++) px[4 * i + c] = texel(lv, l, c);
        [root replaceRegion:MTLRegionMake2D(0, 0, w, w) mipmapLevel:lv slice:l withBytes:px bytesPerRow:4 * w bytesPerImage:4 * w * w];
    }
    // the view cache's view: levels 1-2, layers 2-3, red and blue swapped
    MTLTextureSwizzleChannels sw = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed, MTLTextureSwizzleAlpha);
    id<MTLTexture> view = [root newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(1, 2) slices:NSMakeRange(2, 2) swizzle:sw];
    os_unfair_lock_lock(&g_rmu);
    g_treg = calloc(TID + 1, sizeof *g_treg); g_ntreg = TID + 1;
    uint64_t rid = 0, rid2 = 0;
    id<MTLTexture> fit = tex_fit(TID, view, MTLTextureType2D, &rid);   // a shader that declares 2D reads the 2D array's first layer
    id<MTLTexture> again = tex_fit(TID, view, MTLTextureType2D, &rid2);
    os_unfair_lock_unlock(&g_rmu);
    CHECK(fit && fit.textureType == MTLTextureType2D, "a 2D view of the 2D-array view");
    CHECK(fit.parentTexture == root, "made from the texture under the view, not from the view");
    CHECK(fit.parentRelativeLevel == 1 && fit.mipmapLevelCount == 2 && fit.parentRelativeSlice == 2, "the view's levels and first layer");
    CHECK(fit.swizzle.red == MTLTextureSwizzleBlue && fit.swizzle.blue == MTLTextureSwizzleRed, "the view's swizzle");
    CHECK(rid == fit.gpuResourceID._impl && again == fit && rid2 == rid, "kept for the next binding of the same texture");
    const int swapped[4] = {2, 1, 0, 3};
    float v[4];
    read2d(pipeline, fit, 0, v); CHECK(same(v, 1, 2, swapped), "level 0 of the fitted view is level 1, layer 2, swizzled");
    read2d(pipeline, fit, 1, v); CHECK(same(v, 2, 2, swapped), "level 1 of the fitted view is level 2, layer 2, swizzled");
    retirement_lifetime(pipeline, root);
    printf("tex_fit views from the root: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
