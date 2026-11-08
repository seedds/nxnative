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
enum { W = 8, LEVELS = 3, LAYERS = 4, TID = 5, CUBE_LAYERS = 12, CUBE_CA = 6, CUBE_2A = 7, CA_2D = 8, CA_2A = 9, CA_CUBE = 10, NTID = 11 };
static uint8_t texel(NSUInteger level, NSUInteger layer, int c) { return (uint8_t)(64 * level + 4 * layer + c + 1); }   // every level, layer and channel its own value
static void read_args(id<MTLComputePipelineState> pipeline, id<MTLTexture> t, const void *args, size_t n, float out[4]) {
    id<MTLBuffer> b = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; memset(b.contents, 0xcd, 16);
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:pipeline]; [e setTexture:t atIndex:0]; [e setBytes:args length:n atIndex:0]; [e setBuffer:b offset:0 atIndex:1];
    [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted && !cb.error, "the GPU reads the fitted view");
    memcpy(out, b.contents, 16);
}
static void read2d(id<MTLComputePipelineState> pipeline, id<MTLTexture> t, uint32_t level, float out[4]) { read_args(pipeline, t, &level, sizeof level, out); }
static int same(const float v[4], NSUInteger level, NSUInteger layer, const int ch[4]) {
    for (int c = 0; c < 4; c++) if (fabsf(v[c] - texel(level, layer, ch[c]) / 255.0f) > 1e-4f) return 0;
    return 1;
}
static id<MTLComputePipelineState> pipeline_of(id<MTLLibrary> library, NSString *name) {
    NSError *error = nil; id<MTLComputePipelineState> p = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:name] error:&error];
    if (!p) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); exit(2); }
    return p;
}
static id<MTLTexture> layered_root(NSUInteger layers) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = MTLTextureType2DArray; d.width = d.height = W; d.arrayLength = layers; d.mipmapLevelCount = LEVELS;
    d.pixelFormat = MTLPixelFormatRGBA8Unorm; d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> root = [g_dev newTextureWithDescriptor:d];
    for (NSUInteger lv = 0; lv < LEVELS; lv++) for (NSUInteger l = 0; l < layers; l++) {
        NSUInteger w = W >> lv; uint8_t px[W * W * 4];
        for (NSUInteger i = 0; i < w * w; i++) for (int c = 0; c < 4; c++) px[4 * i + c] = texel(lv, l, c);
        [root replaceRegion:MTLRegionMake2D(0, 0, w, w) mipmapLevel:lv slice:l withBytes:px bytesPerRow:4 * w bytesPerImage:4 * w * w];
    }
    return root;
}
// Cubes and cube arrays in the slots of the other layered types: every face and layer the shader asks for, from the view's levels and layers (Splatoon Raiders: a cube in a cube array slot)
static void cube_views(id<MTLLibrary> library) {
    id<MTLComputePipelineState> rd_ca = pipeline_of(library, @"rd_ca"), rd_c = pipeline_of(library, @"rd_c"), rd_2a = pipeline_of(library, @"rd_2a"), rd_2d = pipeline_of(library, @"rd");
    id<MTLTexture> root = layered_root(CUBE_LAYERS);
    id<MTLTexture> cube = [root newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureTypeCube levels:NSMakeRange(1, 2) slices:NSMakeRange(6, 6)];   // the second cube, from level 1
    id<MTLTexture> array = [root newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureTypeCubeArray levels:NSMakeRange(1, 2) slices:NSMakeRange(0, CUBE_LAYERS)];
    uint64_t rid = 0; const int plain[4] = {0, 1, 2, 3}; float v[4];
    static const float dirs[6][4] = {{1, 0, 0, 0}, {-1, 0, 0, 0}, {0, 1, 0, 0}, {0, -1, 0, 0}, {0, 0, 1, 0}, {0, 0, -1, 0}};
    os_unfair_lock_lock(&g_rmu);
    id<MTLTexture> c_ca = tex_fit(CUBE_CA, cube, MTLTextureTypeCubeArray, &rid), c_2a = tex_fit(CUBE_2A, cube, MTLTextureType2DArray, &rid);
    id<MTLTexture> ca_2d = tex_fit(CA_2D, array, MTLTextureType2D, &rid), ca_2a = tex_fit(CA_2A, array, MTLTextureType2DArray, &rid), ca_c = tex_fit(CA_CUBE, array, MTLTextureTypeCube, &rid);
    os_unfair_lock_unlock(&g_rmu);
    CHECK(c_ca && c_ca.textureType == MTLTextureTypeCubeArray && c_ca.arrayLength == 1, "a cube in a cube array slot: an array of that one cube");
    CHECK(c_ca.parentTexture == root && c_ca.parentRelativeSlice == 6 && c_ca.parentRelativeLevel == 1 && c_ca.mipmapLevelCount == 2, "from the root, with the cube's layers and levels");
    for (int f = 0; f < 6; f++) { read_args(rd_ca, c_ca, dirs[f], sizeof dirs[f], v); CHECK(same(v, 1, 6 + f, plain), "every face of the cube through the cube array view"); }
    CHECK(c_2a && c_2a.textureType == MTLTextureType2DArray && c_2a.arrayLength == 6 && c_2a.parentRelativeSlice == 6, "a cube in a 2D array slot: its six faces as layers");
    for (uint32_t l = 0; l < 6; l++) { uint32_t a[2] = {l, 1}; read_args(rd_2a, c_2a, a, sizeof a, v); CHECK(same(v, 2, 6 + l, plain), "every layer of the cube through the 2D array view"); }
    CHECK(ca_2d && ca_2d.textureType == MTLTextureType2D && ca_2d.parentRelativeSlice == 0, "a cube array in a 2D slot: its first face");
    uint32_t level0 = 0; read_args(rd_2d, ca_2d, &level0, sizeof level0, v); CHECK(same(v, 1, 0, plain), "the first face through the 2D view");
    CHECK(ca_2a && ca_2a.textureType == MTLTextureType2DArray && ca_2a.arrayLength == CUBE_LAYERS, "a cube array in a 2D array slot: all its faces as layers");
    uint32_t last[2] = {CUBE_LAYERS - 1, 0}; read_args(rd_2a, ca_2a, last, sizeof last, v); CHECK(same(v, 1, CUBE_LAYERS - 1, plain), "the last face through the 2D array view");
    CHECK(ca_c && ca_c.textureType == MTLTextureTypeCube, "a cube array in a cube slot: its first cube");
    read_args(rd_c, ca_c, dirs[5], sizeof dirs[5], v); CHECK(same(v, 1, 5, plain), "the first cube's last face through the cube view");
}
int main(void) { @autoreleasepool {
    mtl_init(); NSError *error = nil;
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void rd(texture2d<float> t [[texture(0)]], constant uint& level [[buffer(0)]], device float4* out [[buffer(1)]]) { out[0] = t.read(uint2(0, 0), level); }\n"
        "kernel void rd_2a(texture2d_array<float> t [[texture(0)]], constant uint2& a [[buffer(0)]], device float4* out [[buffer(1)]]) { out[0] = t.read(uint2(0, 0), a.x, a.y); }\n"
        "kernel void rd_ca(texturecube_array<float> t [[texture(0)]], constant float4& a [[buffer(0)]], device float4* out [[buffer(1)]]) { constexpr sampler s(filter::nearest); out[0] = t.sample(s, a.xyz, 0, level(a.w)); }\n"
        "kernel void rd_c(texturecube<float> t [[texture(0)]], constant float4& a [[buffer(0)]], device float4* out [[buffer(1)]]) { constexpr sampler s(filter::nearest); out[0] = t.sample(s, a.xyz, level(a.w)); }\n";
    id<MTLLibrary> library = [g_dev newLibraryWithSource:source options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    id<MTLComputePipelineState> pipeline = pipeline_of(library, @"rd");
    id<MTLTexture> root = layered_root(LAYERS);
    // the view cache's view: levels 1-2, layers 2-3, red and blue swapped
    MTLTextureSwizzleChannels sw = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed, MTLTextureSwizzleAlpha);
    id<MTLTexture> view = [root newTextureViewWithPixelFormat:root.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(1, 2) slices:NSMakeRange(2, 2) swizzle:sw];
    os_unfair_lock_lock(&g_rmu);
    g_treg = calloc(NTID, sizeof *g_treg); g_ntreg = NTID;
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
    cube_views(library);
    printf("tex_fit views from the root: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
