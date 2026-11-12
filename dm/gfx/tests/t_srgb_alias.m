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
    {   // A finalized sRGB texture resolved again (a replayed record): its view goes to the ghost ring. Its parent then leaves the sharing table: the next resolution takes the stale
        // view's ghost entry (own_view_drop_stale) and makes the texture's own, and the held counts match the tables throughout
        tex_t l2 = lin; l2.serial = 99511; l2.d.width = l2.d.height = 8; nvn_tex_layout(&l2.d, &l2.L);
        tex_t s2 = l2; s2.serial = 99512; s2.d.format = NVN_FORMAT_RGBA8_SRGB;
        mtl_tex_init(&l2); mtl_tex_init(&s2);
        id<MTLTexture> p = tex_mtl(&l2), v = tex_mtl(&s2);
        CHECK(p && v.parentTexture == p && serial_get(s2.serial) == v, "a live sRGB texture is a view of the shared linear one");
        mtl_tex_release(&s2);
        CHECK(!tex_get(s2.serial), "finalized: its live entry goes");
        id<MTLTexture> gv = tex_mtl(&s2);
        CHECK(gv.parentTexture == p && tex_get(s2.serial) == gv && !serial_get(s2.serial), "resolved again after its finalize: a view of the shared texture, in the ghost ring");
        char why[256] = ""; pthread_mutex_lock(&g_smu); int ok = own_check_locked(why, sizeof why); pthread_mutex_unlock(&g_smu);
        CHECK(ok, why);
        alias_key ak = tex_alias_key(&l2);
        pthread_mutex_lock(&g_smu); int gone = own_alias_del_locked(&ak); pthread_mutex_unlock(&g_smu);   // as a pool's finalize evicts it
        CHECK(gone && tex_get(s2.serial) == gv, "the parent leaves the sharing table; the ghost entry still names the view");
        id<MTLTexture> own = tex_mtl(&s2);
        pthread_mutex_lock(&g_smu); int held = entry_holds((__bridge void *)gv); pthread_mutex_unlock(&g_smu);
        CHECK(own && own != gv && !own.parentTexture && tex_get(s2.serial) == own && !held, "the next resolution drops the stale view and makes the texture's own, in its ghost entry");
        pthread_mutex_lock(&g_smu); ok = own_check_locked(why, sizeof why); pthread_mutex_unlock(&g_smu);
        CHECK(ok, why);
        mtl_tex_release(&l2);
    }
    printf("sRGB / linear aliases: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
