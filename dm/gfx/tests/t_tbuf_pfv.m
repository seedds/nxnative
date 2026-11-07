// 32-bit integer buffer textures and linear textures get the format-view usage at creation, as tex_desc gives it to other R32 textures: shaders read and write R32 images in
// the other signedness (img_fit), and a texture made without the usage was upgraded (copied into a new texture) before the first pass that bound it. Orbitals re-creates a
// 128-element R32I image buffer every frame: about 8,000 upgrades a session. Checks R32I and R32UI buffer and linear textures (with the usage) and R32F ones (without)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static uint8_t *g_mem;
static id<MTLTexture> make_tbuf(uint32_t format, uint64_t serial, int image) {
    tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .serial = serial};
    t.d = (nvn_texdesc){.target = 10, .format = format, .flags = image ? NVN_TEXTURE_FLAGS_IMAGE : 0, .width = 128, .height = 1, .depth = 1, .levels = 1, .samples = 1};
    t.mem = g_mem + (serial % 16) * 4096;
    nvn_tex_layout(&t.d, &t.L); mtl_tex_init(&t);
    return tex_mtl(&t);
}
static id<MTLTexture> make_linear(uint32_t format, uint64_t serial) {
    tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .serial = serial, .pool = (pool_t *)(uintptr_t)1};   // tex_linear only asks whether the texture has a pool
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .flags = NVN_TEXTURE_FLAGS_LINEAR, .width = 64, .height = 4, .depth = 1, .levels = 1, .samples = 1};
    t.mem = g_mem + 65536 + (serial % 16) * 4096;
    nvn_tex_layout(&t.d, &t.L);
    return tex_linear(&t);
}
int main(void) { @autoreleasepool {
    mtl_init();
    if (posix_memalign((void **)&g_mem, getpagesize(), 1 << 17)) abort(); memset(g_mem, 0, 1 << 17); mtl_pool_add(g_mem, 1 << 17);
    id<MTLTexture> si = make_tbuf(NVN_FORMAT_R32I, 98701, 1), ui = make_tbuf(NVN_FORMAT_R32UI, 98702, 0), fl = make_tbuf(NVN_FORMAT_R32F, 98703, 1);
    CHECK(si && si.textureType == MTLTextureTypeTextureBuffer, "an R32I image buffer texture is a texture buffer");
    CHECK(si && (si.usage & MTLTextureUsagePixelFormatView), "an R32I buffer texture has the format-view usage at creation");
    CHECK(ui && (ui.usage & MTLTextureUsagePixelFormatView), "an R32UI buffer texture has the format-view usage at creation");
    CHECK(fl && !(fl.usage & MTLTextureUsagePixelFormatView), "an R32F buffer texture does not");
    CHECK(si && [si newTextureViewWithPixelFormat:MTLPixelFormatR32Uint] != nil, "the R32I buffer texture takes an R32Uint view without an upgrade");
    id<MTLTexture> li = make_linear(NVN_FORMAT_R32I, 98704), lf = make_linear(NVN_FORMAT_R32F, 98705);
    CHECK(li && (li.usage & MTLTextureUsagePixelFormatView), "a linear R32I texture has the format-view usage at creation");
    CHECK(lf && !(lf.usage & MTLTextureUsagePixelFormatView), "a linear R32F texture does not");
    printf("t_tbuf_pfv: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
