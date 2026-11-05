// Reduced texture quality keeps the original mip contents and remaps guest levels.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, s) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", s); } } while (0)
static uint16_t first_colour(id<MTLTexture> texture, unsigned level) {
    NSUInteger w = texture.width >> level, h = texture.height >> level, row = ((w + 3) / 4) * 8;
    id<MTLBuffer> buffer = [g_dev newBufferWithLength:row * ((h + 3) / 4) options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
        toBuffer:buffer destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:buffer.length];
    [blit endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted, "compressed readback completed");
    return *(uint16_t *)buffer.contents;
}
int main(void) { @autoreleasepool {
    mtl_init();
    tex_t t = {.serial=81001, .d={.target=NVN_TEXTURE_TARGET_2D, .flags=NVN_TEXTURE_FLAGS_IMAGE, .format=NVN_FORMAT_RGBA_DXT1, .width=512, .height=512, .depth=1, .levels=5}};
    nvn_tex_layout(&t.d, &t.L);
    CHECK(t.L.size > 0, "texture storage layout is nonempty"); if (!t.L.size) return 1;
    void *mem = mmap(NULL, t.L.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(mem != MAP_FAILED, "guest texture storage allocated"); if (mem == MAP_FAILED) return 1;
    t.mem = mem; const uint16_t colours[5] = {0xf800, 0x07e0, 0x001f, 0xffff, 0xffe0};
    for (unsigned l = 0; l < 5; l++) {
        uint64_t lo = nvn_tex_level_offset(&t.d, l), hi = l + 1 < 5 ? nvn_tex_level_offset(&t.d, l + 1) : t.L.size;
        for (uint64_t off = lo; off + 8 <= hi; off += 8) { uint16_t *block = (uint16_t *)((uint8_t *)mem + off); block[0] = block[1] = colours[l]; }
    }
    mtl_pool_add(mem, t.L.size); uint64_t stamp = 0;
    mtl_sub_begin(); id<MTLTexture> image = pkg_upload_(&t, &stamp, nil, 0);
    CHECK(image != nil, "compressed CPU texture uploaded");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "upload submission completed");
    unsigned skipped = mip_skip_px() ? mip_skip_levels() : 0;
    CHECK(image.width == (512u >> skipped) && image.height == (512u >> skipped), "requested dimensions");
    CHECK(image.mipmapLevelCount == 5 - skipped, "only the requested finest levels were removed");
    CHECK(first_colour(image, 0) == colours[skipped], "host mip zero contains the correct original mip");
    CHECK(first_colour(image, 1) == colours[1 + skipped], "the following mip retains its original content");
    unsigned level = skipped + 1; CHECK(lv_host(&t, image, &level) && level == 1, "guest mip maps to its host level");
    level = 0; CHECK(lv_host(&t, image, &level) == !skipped, "copies of a removed mip are refused");
    MTLTextureDescriptor *d = tex_desc(&t, 1); d.usage |= MTLTextureUsageShaderWrite;
    CHECK(!mip_skip(&t, d), "shader-written textures keep their storage");
    tex_t rgba = t; rgba.d.format = NVN_FORMAT_RGBA8;
    CHECK(!mip_skip(&rgba, tex_desc(&rgba, 1)), "uncompressed render formats are unchanged");
    CHECK(!tex_mip_skipped(&rgba, image), "an unrelated size difference does not remap levels");
    tex_t few = t; few.d.levels = 2;
    CHECK(mip_skip(&few, tex_desc(&few, 1)) == MIN(skipped, 1u), "at least one mip is always retained");
    tex_t small = t; small.d.width = small.d.height = 256;
    CHECK(!mip_skip(&small, tex_desc(&small, 1)), "textures below the pixel threshold are unchanged");
    tex_t volume = t; volume.d.target = NVN_TEXTURE_TARGET_3D;
    CHECK(!mip_skip(&volume, tex_desc(&volume, 1)), "volume textures are unchanged");
    mtl_finish(); mtl_pool_del(mem); mtl_finish();
    printf("texture quality: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
