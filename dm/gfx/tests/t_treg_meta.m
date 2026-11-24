// Cached texture view type and pixel format on treg_t (tex_fast_rid_meta):
// tex_by_id_ref caches immutable view_tt and view_pf beside view_rid when rp->view is populated, and treg_drop_views /
// invalidation clears them. Checks base textures, format/target views, stencil-mode views (fb_depth_planes), buffer
// textures, ID re-registration with a different type/format, and view drop + rebuild.
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

int main(void) { @autoreleasepool {
    mtl_init();

    // 1. Plain 2D RGBA8Unorm texture: slow path populates view_tt/view_pf, fast path returns the same values
    tex_t t2d = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 99601};
    t2d.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 16, .height = 16, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t2d.d, &t2d.L);
    mtl_tex_init(&t2d);
    mtl_reg_tex(101, &t2d, NULL);

    os_unfair_lock_lock(&g_rmu);
    void *tv = NULL; uint8_t tt = 0; uint32_t pf = 0;
    uint64_t r1 = tex_fast_rid_meta(101, &tv, &tt, &pf);
    id<MTLTexture> o1 = (__bridge id<MTLTexture>)tv;
    CHECK("2D slow path resolves and caches type/format", o1 && r1 == o1.gpuResourceID._impl && tt == (uint8_t)o1.textureType && pf == (uint32_t)o1.pixelFormat);
    CHECK("2D cached fields match RGBA8Unorm", tt == MTLTextureType2D && pf == MTLPixelFormatRGBA8Unorm && g_treg[101].view_tt == tt && g_treg[101].view_pf == pf);

    tv = NULL; tt = 0xff; pf = 0xffffffffu;
    uint64_t r2 = tex_fast_rid_meta(101, &tv, &tt, &pf);
    CHECK("2D fast path matches slow path", tv == (__bridge void *)o1 && r2 == r1 && tt == MTLTextureType2D && pf == MTLPixelFormatRGBA8Unorm);
    os_unfair_lock_unlock(&g_rmu);

    // 2. View overriding both target (2DArray -> 2D) and format (RGBA8 -> RGBA8_SRGB): cached metadata must reflect the view, not the base
    tex_t tarr = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 99602, .off = 0x100000};
    tarr.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = NVN_FORMAT_RGBA8, .width = 16, .height = 16, .depth = 4, .levels = 1, .samples = 1};
    nvn_tex_layout(&tarr.d, &tarr.L);
    mtl_tex_init(&tarr);
    view_t v2d_srgb = {.set_target = 1, .target = NVN_TEXTURE_TARGET_2D, .set_format = 1, .format = NVN_FORMAT_RGBA8_SRGB, .set_layers = 1, .min_layer = 1, .nlayers = 1};
    mtl_reg_tex(102, &tarr, &v2d_srgb);

    os_unfair_lock_lock(&g_rmu);
    tv = NULL; tt = 0; pf = 0;
    uint64_t rv1 = tex_fast_rid_meta(102, &tv, &tt, &pf);
    id<MTLTexture> ov = (__bridge id<MTLTexture>)tv;
    CHECK("view slow path caches view type and view format", ov && rv1 == ov.gpuResourceID._impl && tt == MTLTextureType2D && pf == MTLPixelFormatRGBA8Unorm_sRGB);
    CHECK("view cached metadata matches Metal object", tt == (uint8_t)ov.textureType && pf == (uint32_t)ov.pixelFormat);
    tv = NULL; tt = 0xff; pf = 0xffffffffu;
    uint64_t rv2 = tex_fast_rid_meta(102, &tv, &tt, &pf);
    CHECK("view fast path returns view type and view format", tv == (__bridge void *)ov && rv2 == rv1 && tt == MTLTextureType2D && pf == MTLPixelFormatRGBA8Unorm_sRGB);
    os_unfair_lock_unlock(&g_rmu);

    // 3. Depth vs stencil-mode views over Depth32Float_Stencil8: cached format drives fb_depth_planes
    tex_t tds = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 99603, .off = 0x200000};
    tds.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_DEPTH32F_STENCIL8, .width = 8, .height = 8, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&tds.d, &tds.L);
    mtl_tex_init(&tds);
    view_t vst = {.set_dsmode = 1, .dsmode = NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL};
    mtl_reg_tex(103, &tds, NULL);
    mtl_reg_tex(104, &tds, &vst);

    CHECK("fb_depth_planes reads cached depth format", fb_depth_planes(103, tds.serial) == FB_DS_DEPTH);
    CHECK("fb_depth_planes reads cached stencil format", fb_depth_planes(104, tds.serial) == FB_DS_STENCIL);
    os_unfair_lock_lock(&g_rmu);
    CHECK("depth ID cached Depth32Float_Stencil8", g_treg[103].view_pf == MTLPixelFormatDepth32Float_Stencil8 && g_treg[103].view_tt == MTLTextureType2D);
    CHECK("stencil ID cached X32_Stencil8", g_treg[104].view_pf == MTLPixelFormatX32_Stencil8 && g_treg[104].view_tt == MTLTextureType2D);
    os_unfair_lock_unlock(&g_rmu);

    // 4. Re-register ID 101 with a different texture (2DArray R32Uint): cached fields clear and update
    tex_t tu32 = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 99604, .off = 0x300000};
    tu32.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = NVN_FORMAT_R32UI, .width = 8, .height = 8, .depth = 2, .levels = 1, .samples = 1};
    nvn_tex_layout(&tu32.d, &tu32.L);
    mtl_tex_init(&tu32);
    mtl_reg_tex(101, &tu32, NULL);

    os_unfair_lock_lock(&g_rmu);
    CHECK("re-registration clears old cached metadata", g_treg[101].view == NULL && g_treg[101].view_tt == 0 && g_treg[101].view_pf == 0);
    tv = NULL; tt = 0; pf = 0;
    uint64_t ru = tex_fast_rid_meta(101, &tv, &tt, &pf);
    id<MTLTexture> ou = (__bridge id<MTLTexture>)tv;
    CHECK("re-registered ID caches new type/format", ou && ru == ou.gpuResourceID._impl && tt == MTLTextureType2DArray && pf == MTLPixelFormatR32Uint);
    os_unfair_lock_unlock(&g_rmu);

    // 5. Dropping views clears cached fields, and a subsequent lookup repopulates them
    os_unfair_lock_lock(&g_rmu);
    treg_drop_views(&g_treg[101]);
    CHECK("treg_drop_views clears view_tt and view_pf", g_treg[101].view == NULL && g_treg[101].view_tt == 0 && g_treg[101].view_pf == 0);
    tv = NULL; tt = 0; pf = 0;
    uint64_t ru2 = tex_fast_rid_meta(101, &tv, &tt, &pf);
    CHECK("lookup after drop repopulates view_tt and view_pf", tv != NULL && ru2 != 0 && tt == MTLTextureType2DArray && pf == MTLPixelFormatR32Uint);
    os_unfair_lock_unlock(&g_rmu);

    // 6. Buffer texture caches MTLTextureTypeTextureBuffer and its Metal pixel format
    size_t bsz = 4096;
    uint8_t *bmem = mmap(NULL, bsz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (bmem == MAP_FAILED) abort();
    mtl_pool_add(bmem, bsz);
    tex_t tbuf = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 99605, .mem = bmem};
    tbuf.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_BUFFER, .format = NVN_FORMAT_RGBA32F, .width = 64, .height = 1, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&tbuf.d, &tbuf.L);
    mtl_tex_init(&tbuf);
    mtl_reg_tex(105, &tbuf, NULL);

    os_unfair_lock_lock(&g_rmu);
    tv = NULL; tt = 0; pf = 0;
    uint64_t rb = tex_fast_rid_meta(105, &tv, &tt, &pf);
    id<MTLTexture> ob = (__bridge id<MTLTexture>)tv;
    CHECK("buffer texture caches TextureBuffer and RGBA32Float", ob && rb == ob.gpuResourceID._impl && tt == MTLTextureTypeTextureBuffer && pf == MTLPixelFormatRGBA32Float);
    os_unfair_lock_unlock(&g_rmu);

    // 7. Unregistered ID returns 0 and leaves outputs untouched (no crash)
    os_unfair_lock_lock(&g_rmu);
    tv = (void *)0x1; tt = 0; pf = 0;
    uint64_t r0 = tex_fast_rid_meta(106, &tv, &tt, &pf);
    CHECK("unregistered ID returns 0 and NULL view", r0 == 0 && tv == NULL);
    os_unfair_lock_unlock(&g_rmu);

    mtl_pool_del(bmem);
    mtl_drain();
    munmap(bmem, bsz);
    printf("t_treg_meta: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
