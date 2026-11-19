// A new colour texture is zeroed in the upload batch by copies from the zero buffer in pieces of rows (tex_zero_fill), as depth is (t_zero_depth): the zero buffer stays at its 4 MB
// instead of growing to the largest texture zeroed and staying resident at that size (Ocarina of Time's reached 64 MB on the iPhone 17). Every byte of every level, layer and depth slice
// reads 0 afterwards, from a texture first filled with non-zero bytes
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
    id<MTLCommandQueue> queue = [g_dev newCommandQueue];
    struct { MTLTextureType type; MTLPixelFormat pf; unsigned w, h, depth, layers, levels, bpp, pieces; const char *label; } cases[] = {   // pieces: copies into level 0 of layer 0
        {MTLTextureType2D, MTLPixelFormatRGBA8Unorm, 4096, 4096, 1, 1, 1, 4, 16, "RGBA8 4096x4096 (64 MB)"},
        {MTLTextureType2D, MTLPixelFormatRGBA16Float, 2048, 1500, 1, 1, 2, 8, 6, "RGBA16F 2048x1500, two levels: the last piece is short"},
        {MTLTextureType2DArray, MTLPixelFormatR32Float, 1024, 1024, 1, 3, 3, 4, 1, "R32F 1024x1024, three layers, three levels"},
        {MTLTextureType3D, MTLPixelFormatRGBA8Unorm, 512, 512, 8, 1, 1, 4, 8, "RGBA8 3D 512x512x8: a piece per depth slice"},
        {MTLTextureType2D, MTLPixelFormatRGBA32Float, 16384, 70, 1, 1, 1, 16, 5, "RGBA32F 16384x70: 16 rows a piece"},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor new]; d.textureType = cases[i].type; d.pixelFormat = cases[i].pf; d.width = cases[i].w; d.height = cases[i].h;
        d.depth = cases[i].depth; d.arrayLength = cases[i].layers; d.mipmapLevelCount = cases[i].levels; d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> base = [g_dev newTextureWithDescriptor:d];
        CHECK(base != nil, "synthetic colour texture allocates");
        if (!base) continue;
        size_t bpp = cases[i].bpp, img0 = (size_t)cases[i].w * cases[i].h * bpp;
        id<MTLBuffer> io = [g_dev newBufferWithLength:img0 options:MTLResourceStorageModeShared];
        memset(io.contents, 0x5a, img0);
        id<MTLCommandBuffer> cb = [queue commandBuffer]; id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        for (NSUInteger l = 0; l < base.mipmapLevelCount; l++) for (NSUInteger s = 0; s < base.arrayLength; s++) {
            NSUInteger w = MAX(base.width >> l, 1u), h = MAX(base.height >> l, 1u), z = MAX(base.depth >> l, 1u);
            for (NSUInteger k = 0; k < z; k++)
                [blit copyFromBuffer:io sourceOffset:0 sourceBytesPerRow:w * bpp sourceBytesPerImage:w * h * bpp sourceSize:MTLSizeMake(w, h, 1) toTexture:base destinationSlice:s destinationLevel:l destinationOrigin:MTLOriginMake(0, 0, k)];
        }
        [blit endEncoding]; [cb commit]; [cb waitUntilCompleted];
        CHECK(cb.status == MTLCommandBufferStatusCompleted, "the texture takes its non-zero bytes");
        res_add(base);
        state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
        tex_zero_fill(base);
        int pieces0 = 0; for (int k = 0; k < g_nup_blits; k++) pieces0 += g_up_blits[k].dst == base && g_up_blits[k].slice == 0 && g_up_blits[k].level == 0 && !g_up_blits[k].tsrc;
        CHECK(pieces0 == (int)cases[i].pieces, "level 0 of layer 0 is zeroed by the expected number of copies");
        mtl_sub_commit(); mtl_drain();
        CHECK(!mtl_last_commit_error(), "zero fill completes without GPU error");
        CHECK(zero_buf(1).length == (4u << 20), "the zero buffer stays at 4 MB");
        int clean = 1;
        for (NSUInteger l = 0; l < base.mipmapLevelCount; l++) for (NSUInteger s = 0; s < base.arrayLength; s++) {
            NSUInteger w = MAX(base.width >> l, 1u), h = MAX(base.height >> l, 1u), z = MAX(base.depth >> l, 1u);
            memset(io.contents, 0x5a, img0);
            for (NSUInteger k = 0; k < z && clean; k++) {
                cb = [queue commandBuffer]; blit = [cb blitCommandEncoder];
                [blit copyFromTexture:base sourceSlice:s sourceLevel:l sourceOrigin:MTLOriginMake(0, 0, k) sourceSize:MTLSizeMake(w, h, 1) toBuffer:io destinationOffset:0 destinationBytesPerRow:w * bpp destinationBytesPerImage:w * h * bpp];
                [blit endEncoding]; [cb commit]; [cb waitUntilCompleted];
                const uint8_t *b = io.contents; for (size_t j = 0; j < (size_t)w * h * bpp; j++) if (b[j]) { clean = 0; fprintf(stderr, "  level %lu layer %lu slice %lu: byte %zu not zero\n", (unsigned long)l, (unsigned long)s, (unsigned long)k, j); break; }
            }
        }
        CHECK(clean, cases[i].label);
        printf("case %u: %s: %d copies into level 0, %s\n", i, cases[i].label, pieces0, clean ? "PASS" : "FAIL");
        defer_obj_release(base);
    }
    printf("colour zero fill: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
