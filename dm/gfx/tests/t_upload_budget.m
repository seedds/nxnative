// A streaming burst inside one still-open rendering submission must not retain
// all staging until its rendering fence. Exercise the real Metal upload and
// completion-retirement path.
#define DM_NVN_LEGACY_RENDERER_TEST 1
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
int front_screen_max_fps(void) { return 60; }
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
    // The legacy Metal path uses a compute encoder for deswizzle and a blit
    // encoder for copies. Temporarily switch encoders so the regression can
    // initialize this private staging buffer on the GPU.
    if (g_up_ce) { m4_end(g_up_ce); g_up_ce = nil; }
    id<MTLBlitCommandEncoder> fill = [g_up_cb blitCommandEncoder];
    [fill fillBuffer:b range:NSMakeRange(off, 256) value:value];
    [fill endEncoding];
    g_up_ce = [g_up_cb computeCommandEncoder];
    up_blit_add(b, off, 256, 256, MTLSizeMake(1, 1, 1), dst, 0, 0);
    [g_up_texs addObject:dst];
}
int main(void) { @autoreleasepool {
    mtl_init();
    tex_t cube = {.magic=MAGIC('T','E','X',0), .serial=96999};
    cube.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_CUBEMAP_ARRAY,
                          .format=NVN_FORMAT_RGBA8, .width=64, .height=64,
                          .depth=12, .levels=1, .samples=1};
    MTLTextureDescriptor *cube_desc = tex_desc(&cube, 0);
    CHECK(cube_desc.textureType == MTLTextureType2DArray,
          "NVN cube array uses a 2D-array Metal base");
    CHECK(cube_desc.arrayLength == 12,
          "NVN cube-array depth remains its twelve layer-faces");
    id<MTLTexture> cube_base = [g_dev newTextureWithDescriptor:cube_desc];
    id<MTLTexture> cube_view = [cube_base newTextureViewWithPixelFormat:cube_base.pixelFormat
        textureType:MTLTextureTypeCubeArray levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 12)];
    CHECK(cube_view && cube_view.textureType == MTLTextureTypeCubeArray && cube_view.arrayLength == 2,
          "twelve face layers form a two-cube Metal view");
    NSHashTable *buffers = [NSHashTable weakObjectsHashTable];
    NSMutableArray *textures = [NSMutableArray new];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:d]; res_add(target);
    mtl_sub_begin();

    // A temporary decompression view belongs to its upload command buffer,
    // not to the terminal replay fence.  This is the ownership pattern used
    // by the large BC scene-load burst on tvOS.
    up_begin();
    id<MTLTexture> temporary_view = [target newTextureViewWithPixelFormat:target.pixelFormat];
    __weak id<MTLTexture> temporary_view_weak = temporary_view;
    [g_up_texs addObject:temporary_view];
    temporary_view = nil;
    CHECK(temporary_view_weak != nil, "upload batch retains its temporary decompression view");
    NSUInteger deferred_before_view_flush = g_deferred.count;
    up_flush(); up_wait_zero(&g_up_pending);
    CHECK(temporary_view_weak == nil, "upload completion releases its temporary decompression view before the replay fence");
    CHECK(g_deferred.count == deferred_before_view_flush,
          "temporary upload views never enter the whole-submission deferred queue");

    // Decompressed-target accounting splits only when the next texture would
    // exceed the budget.  Equality stays in the current batch.
    NSUInteger deferred_before_budget = g_deferred.count;
    size_t quarter = (size_t)UP_BATCH_TARGET_BYTES / 4;
    up_target_prepare(quarter * 2); up_target_charge(quarter * 2);
    unsigned seq_at_half = g_up_seq;
    up_target_prepare(quarter * 2); up_target_charge(quarter * 2);
    CHECK(g_up_seq == seq_at_half && g_up_target_bytes == (size_t)UP_BATCH_TARGET_BYTES,
          "target budget accepts an exact-boundary texture in the current batch");
    up_target_prepare(1);
    CHECK(g_up_seq == seq_at_half + 1 && g_up_target_bytes == 0,
          "the first byte beyond the target budget starts one new batch");
    up_target_charge(1); up_flush(); up_wait_zero(&g_up_pending);
    CHECK(g_up_target_bytes == 0 && g_up_pending == 0,
          "target accounting clears when its upload batch completes");

    // The independent staging limit may flush after target preparation.  A
    // successful texture must be charged to the replacement batch, not the
    // batch that up_alloc just committed.
    up_target_prepare(quarter); size_t staging_off;
    (void)up_alloc(96u << 20, &staging_off); up_target_charge(quarter);
    unsigned seq_before_staging_split = g_up_seq;
    up_target_prepare(quarter);
    (void)up_alloc(64u << 20, &staging_off);
    CHECK(g_up_seq == seq_before_staging_split + 1 && g_up_target_bytes == 0,
          "staging-budget flush resets decompressed-target accounting");
    up_target_charge(quarter);
    CHECK(g_up_target_bytes == quarter,
          "post-encode charge belongs to the batch created by up_alloc");
    up_flush(); up_wait_zero(&g_up_pending);

    // An indivisible texture may exceed the limit alone.  It must not loop
    // while entering an empty batch, and the following texture must split.
    up_target_prepare(1); up_target_charge(1);
    unsigned seq_before_oversize = g_up_seq;
    up_target_prepare((size_t)UP_BATCH_TARGET_BYTES + 1);
    CHECK(g_up_seq == seq_before_oversize + 1 && g_up_target_bytes == 0,
          "an oversized texture first retires the preceding non-empty batch");
    up_target_charge((size_t)UP_BATCH_TARGET_BYTES + 1);
    unsigned seq_with_oversize = g_up_seq;
    up_target_prepare(1);
    CHECK(g_up_seq == seq_with_oversize + 1 && g_up_target_bytes == 0,
          "the texture following an oversized batch starts a new batch");
    up_target_charge(1); up_flush(); up_wait_zero(&g_up_pending);
    CHECK(g_deferred.count == deferred_before_budget,
          "target-budget splitting creates no whole-submission retirements");

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
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
    up_flush(); up_wait_zero(&g_up_pending);
    size_t idle = live_bytes(buffers);
    CHECK(peak <= (512u << 20), "streaming burst staging stays within 512 MiB");
    CHECK(idle <= (96u << 20), "only three reusable staging blocks survive upload completion before the rendering fence");
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
    printf("upload budget: peak %zu MiB, completed %zu MiB, %d checks, %d failed\n", peak >> 20, idle >> 20, checks, fails);
    return fails != 0;
} }
