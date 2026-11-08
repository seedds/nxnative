// A new depth texture is zeroed in the upload batch (tex_zero_fill): large ones by copies from the zero buffer, not a clear pass (which fails with a GPU out-of-memory error on big attachments).
// Every texel of every level and layer reads 0 afterwards, from a texture first filled with a non-zero depth.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

int main(void) { @autoreleasepool {
    mtl_init();
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void count_nz(depth2d<float,access::read> d [[texture(0)]], device atomic_uint *o [[buffer(0)]], uint2 p [[thread_position_in_grid]]) { if (p.x < d.get_width() && p.y < d.get_height() && d.read(p) != 0.0f) atomic_fetch_add_explicit(o, 1u, memory_order_relaxed); }\n" options:nil error:&error];
    id<MTLComputePipelineState> counter = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"count_nz"] error:&error];
    CHECK(counter != nil, "synthetic non-zero counter compiles");
    if (!counter) return 1;
    id<MTLCommandQueue> queue = [g_dev newCommandQueue];
    struct { int target, format; unsigned w, h, depth, levels; const char *label; } cases[] = {
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH16, 4096, 4096, 1, 1, "Depth16 4096x4096 (copies)"},
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH32F, 2048, 2048, 1, 1, "Depth32F 2048x2048, the smallest size that is copied"},
        {NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_DEPTH16, 3072, 3072, 2, 3, "Depth16 3072x3072 two layers, three levels (copies)"},
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH32F, 16384, 257, 1, 1, "Depth32F 16384x257: the rows go in pieces and the last piece is one row"},
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH16, 1024, 1024, 1, 1, "Depth16 1024x1024 (clear pass)"},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 99601 + i};
        t.d = (nvn_texdesc){.target = cases[i].target, .format = cases[i].format, .width = cases[i].w, .height = cases[i].h, .depth = cases[i].depth, .levels = cases[i].levels, .samples = 1};
        MTLTextureDescriptor *descriptor = tex_desc(&t, 0);
        id<MTLTexture> base = descriptor ? [g_dev newTextureWithDescriptor:descriptor] : nil;
        CHECK(base != nil, "synthetic depth texture allocates");
        if (!base) continue;
        size_t bpp = base.pixelFormat == MTLPixelFormatDepth16Unorm ? 2 : 4, poison_bytes = (size_t)cases[i].w * cases[i].h * bpp;
        id<MTLBuffer> poison = [g_dev newBufferWithLength:poison_bytes options:MTLResourceStorageModeShared];
        if (bpp == 2) for (size_t k = 0; k < poison_bytes / 2; k++) ((uint16_t *)poison.contents)[k] = 0xbfff;   // 0.75
        else for (size_t k = 0; k < poison_bytes / 4; k++) ((float *)poison.contents)[k] = .75f;
        id<MTLCommandBuffer> cb = [queue commandBuffer]; id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        for (NSUInteger l = 0; l < base.mipmapLevelCount; l++) for (NSUInteger s = 0; s < base.arrayLength; s++) {
            NSUInteger w = MAX(base.width >> l, 1u), h = MAX(base.height >> l, 1u);
            [blit copyFromBuffer:poison sourceOffset:0 sourceBytesPerRow:w * bpp sourceBytesPerImage:w * h * bpp sourceSize:MTLSizeMake(w, h, 1) toTexture:base destinationSlice:s destinationLevel:l destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
        [blit endEncoding]; [cb commit]; [cb waitUntilCompleted];
        CHECK(cb.status == MTLCommandBufferStatusCompleted, "the texture takes its non-zero depth");
        res_add(base);
        state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
        tex_zero_fill(base, 1);
        mtl_sub_commit(); mtl_drain();
        CHECK(!mtl_last_commit_error(), "zero fill completes without GPU error");
        id<MTLBuffer> count = [g_dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
        int clean = 1;
        for (NSUInteger l = 0; l < base.mipmapLevelCount; l++) for (NSUInteger s = 0; s < base.arrayLength; s++) {
            id<MTLTexture> v = [base newTextureViewWithPixelFormat:base.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(l, 1) slices:NSMakeRange(s, 1)];
            *(uint32_t *)count.contents = 0;
            cb = [queue commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:counter]; [e setTexture:v atIndex:0]; [e setBuffer:count offset:0 atIndex:0];
            [e dispatchThreads:MTLSizeMake(v.width, v.height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
            CHECK(cb.status == MTLCommandBufferStatusCompleted, "the texels are read on the GPU");
            if (*(uint32_t *)count.contents) { clean = 0; fprintf(stderr, "  level %lu layer %lu: %u texels not zero\n", (unsigned long)l, (unsigned long)s, *(uint32_t *)count.contents); }
        }
        CHECK(clean, cases[i].label);
        printf("case %u: %s %s\n", i, cases[i].label, clean ? "PASS" : "FAIL");
        defer_obj_release(base);
    }
    printf("depth zero fill: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
