// Two NVN textures over the same storage that differ only by the formats of an sRGB / linear pair share their bytes: what is written through one reads back through the other
// (Mario Kart World's Grand Prix movie: an RGBA8 render target sampled through an RGBA8_SRGB texture).
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
int main(void) { @autoreleasepool {
    mtl_init(); mtl_sub_begin();
    tex_t lin = {.magic=MAGIC('T','E','X',0), .serial=99501};
    lin.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8, .width=4, .height=4, .depth=1, .levels=1, .samples=1};
    nvn_tex_layout(&lin.d, &lin.L);
    tex_t srgb = lin; srgb.serial++; srgb.d.format = NVN_FORMAT_RGBA8_SRGB;
    tex_t other = lin; other.serial += 2; other.d.format = NVN_FORMAT_R32F;   // not an sRGB / linear pair: storage of its own
    mtl_tex_init(&lin); mtl_tex_init(&srgb); mtl_tex_init(&other);
    id<MTLTexture> base = tex_mtl(&lin), view = tex_mtl(&srgb);
    CHECK(base && view && view.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB, "the sRGB texture has its own format");
    CHECK(view.parentTexture == base, "the sRGB texture is a view of the linear texture's storage");
    CHECK(tex_mtl(&other) != base && tex_mtl(&other).parentTexture == nil, "another format family keeps its own texture");
    uint32_t pixels[16]; for (int i = 0; i < 16; i++) pixels[i] = 0x44332211u + (uint32_t)i;
    id<MTLBuffer> src = [g_dev newBufferWithBytes:pixels length:sizeof pixels options:MTLResourceStorageModeShared]; res_add(src);
    id<MTLBuffer> dst = [g_dev newBufferWithLength:sizeof pixels options:MTLResourceStorageModeShared]; res_add(dst);
    id<MTL4ComputeCommandEncoder> encoder = blit();
    enc_b_copy_buffer_texture(encoder, src, 0, 16, sizeof pixels, MTLSizeMake(4,4,1), base, 0, 0, MTLOriginMake(0,0,0));
    encoder = blit(); enc_b_copy_texture_buffer(encoder, view, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(4,4,1), dst, 0, 16, sizeof pixels);
    mtl_sub_commit(); mtl_drain();
    CHECK(!memcmp(dst.contents, pixels, sizeof pixels), "pixels written through the linear texture read back through the sRGB one");
    // The linear texture upgraded after the view was made (image writes): the view follows the upgraded storage
    id<MTLTexture> up = tex_upgrade(&lin, base, MTLTextureUsageShaderWrite, base.pixelFormat), view2 = tex_mtl(&srgb);
    CHECK(up != base && tex_mtl(&lin) == up, "the linear texture is upgraded");
    CHECK(view2.parentTexture == up && view2.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB, "the sRGB texture is a view of the upgraded storage");
    for (int i = 0; i < 16; i++) ((uint32_t *)src.contents)[i] = 0x88776655u + (uint32_t)i;
    mtl_sub_begin(); encoder = blit();
    enc_b_copy_buffer_texture(encoder, src, 0, 16, sizeof pixels, MTLSizeMake(4,4,1), up, 0, 0, MTLOriginMake(0,0,0));
    encoder = blit(); enc_b_copy_texture_buffer(encoder, view2, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(4,4,1), dst, 0, 16, sizeof pixels);
    mtl_sub_commit(); mtl_drain();
    CHECK(!memcmp(dst.contents, src.contents, sizeof pixels), "pixels written through the upgraded linear texture read back through the sRGB view");
    // An upgrade asked through the view upgrades the parent; the view is made again on it
    id<MTLTexture> up2 = tex_upgrade(&srgb, view2, MTLTextureUsageRenderTarget, view2.pixelFormat);
    CHECK(up2.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB && up2.parentTexture == tex_mtl(&lin) && tex_mtl(&lin) != up && tex_mtl(&srgb) == up2, "an upgrade through the view upgrades the parent and keeps the pair sharing");
    printf("sRGB / linear aliases: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
