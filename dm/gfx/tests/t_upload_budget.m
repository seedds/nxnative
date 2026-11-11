// A streaming burst inside one still-open rendering submission must not retain all
// staging until its rendering fence. Exercise the real Metal upload/retirement path.
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

static size_t live_bytes(NSHashTable *buffers) { @autoreleasepool {
    size_t n = 0;
    for (id<MTLBuffer> b in buffers.allObjects) n += b.length;
    return n;
} }
static void upload_one(id<MTLTexture> dst, NSHashTable *buffers, uint8_t value) {
    up_begin();
    size_t off;
    id<MTLBuffer> b = up_alloc(32u << 20, &off);
    @autoreleasepool { [buffers addObject:b]; }
    [g_up_ce fillBuffer:b range:NSMakeRange(off, 256) value:value];
    up_blit_add(b, off, 256, 256, MTLSizeMake(1, 1, 1), dst, 0, 0);
    [g_up_texs addObject:dst];
}
static void cpu_burst(void) {
    enum { N = 20, W = 1024, H = 2048 };
    tex_t t = {0};
    t.d = (nvn_texdesc){.target=1,.format=NVN_FORMAT_RGBA8,.width=W,.height=H,.depth=1,.levels=1,.samples=1,.flags=NVN_TEXTURE_FLAGS_LINEAR};
    nvn_tex_layout(&t.d, &t.L);
    t.mem = malloc(t.L.size);
    NSMutableArray *images = [NSMutableArray array];
    uint64_t start = g_up_seq;
    mtl_sub_begin();
    for (int i=0; i<N; i++) {
        memset((void *)t.mem, i+71, t.L.size);
        id<MTLTexture> image = pkg_upload_(&t, NULL, nil, 0);
        CHECK(image != nil, "CPU fallback texture is created");
        if (image) [images addObject:image];
        CHECK(g_up_staging_bytes <= UP_BATCH_BYTES, "CPU fallback staging respects batch budget");
    }
    CHECK(g_up_seq-start < N/2, "CPU burst groups uploads instead of committing each texture");
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "CPU burst submission completes");
    CHECK(!mtl_last_commit_error(), "CPU burst has no GPU error");
    id<MTLBuffer> result=[g_dev newBufferWithLength:(size_t)W*H*4 options:MTLResourceStorageModeShared];
    for (int i=0; i<(int)images.count; i++) {
        id<MTLCommandBuffer> cb=q_cb();id<MTLBlitCommandEncoder> e=[cb blitCommandEncoder];
        [e copyFromTexture:images[i] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(W,H,1) toBuffer:result destinationOffset:0 destinationBytesPerRow:W*4 destinationBytesPerImage:(size_t)W*H*4];
        [e endEncoding];[cb commit];[cb waitUntilCompleted];
        CHECK(cb.status==MTLCommandBufferStatusCompleted,"CPU burst readback completes");
        const uint8_t *bytes=result.contents;int valid=1;
        for(size_t j=0;j<(size_t)W*H*4;j++)if(bytes[j]!=(uint8_t)(i+71)){valid=0;break;}
        CHECK(valid,"all CPU-uploaded pixels survive later uploads and budget splitting");
        res_remove(images[i]);
    }
    free((void *)t.mem);
}
int main(void) { @autoreleasepool {
    mtl_init();
    NSHashTable *buffers = [NSHashTable weakObjectsHashTable];
    NSMutableArray *textures = [NSMutableArray new];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:d]; res_add(target);
    mtl_sub_begin();
    MTL4RenderPassDescriptor *pass = [MTL4RenderPassDescriptor new];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
    id encoder = [g_cb renderCommandEncoderWithDescriptor:pass];
    size_t peak = 0;
    for (int i = 0; i < 64; i++) {
        id<MTLTexture> t = [g_dev newTextureWithDescriptor:d];
        [textures addObject:t]; res_add(t);
        upload_one(t, buffers, (uint8_t)(i + 1));
        peak = MAX(peak, live_bytes(buffers));
    }
    up_flush(); rec_publish(); up_wait_zero(&g_up_pending, WS_DIAG);
    size_t idle = live_bytes(buffers);
    CHECK(peak <= (512u << 20), "streaming burst staging stays within 512 MiB");
    // Demand reuse retains a primary and one bounded overflow block per slot.
    // Everything beyond those six blocks must retire before the rendering fence.
    CHECK(idle <= (up_reuse() ? (192u << 20) : (96u << 20)),
          "completed uploads retain only the bounded slot staging cache");
    m4_end(encoder);
    uint64_t fence = mtl_sub_commit();
    CHECK(mtl_wait(fence, 5000000000ull), "rendering submission completes");
    CHECK(!mtl_last_commit_error(), "no rendering GPU error");
    uint8_t clear[4] = {0};
    [target getBytes:clear bytesPerRow:4 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
    CHECK(clear[0] == 255 && clear[1] == 0 && clear[2] == 0 && clear[3] == 255,
          "the open rendering pass survives upload commits");
    for (int i = 0; i < 64; i++) {
        uint8_t pixel[4] = {0};
        [textures[i] getBytes:pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
        for (int c = 0; c < 4; c++) CHECK(pixel[c] == i + 1, "upload pixels survive batch splitting and staging retirement");
    }
    cpu_burst();
    printf("upload budget: peak %zu MiB, completed %zu MiB, %d checks, %d failed\n", peak >> 20, idle >> 20, checks, fails);
    return fails != 0;
} }
