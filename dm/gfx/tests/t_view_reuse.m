// Texture views are kept by the Metal texture they view (tex_by_id, img_fit, vc_get), so they outlive the NVN texture that asked for them: a game that initializes its frame's render
// targets again every frame over the same storage (Ocarina of Time: about 40 a frame, each a new texture serial in a new texture ID) finds each one's Metal texture in the sharing table,
// and its view (the depth's (R, 0, 0, 1), RGBX's alpha 1) must be found again too, not made anew each frame. A texture's views go at the first commit after the owner lets it go.
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
static tex_t target(uint64_t serial) {   // an RGBX8 render target: its sampled view maps alpha to 1, so it always needs a view of the shared texture
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=serial};
    t.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBX8, .width=16, .height=8, .depth=1, .levels=1, .samples=1, .flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE};
    nvn_tex_layout(&t.d, &t.L);
    return t;
}
static id<MTLTexture> view_of(int tid) { os_unfair_lock_lock(&g_rmu); id<MTLTexture> v = tex_by_id(tid); os_unfair_lock_unlock(&g_rmu); return v; }
int main(void) { @autoreleasepool {
    mtl_init(); mtl_sub_begin();
    // Frame 1: the target is made, registered and sampled
    tex_t a = target(97001); mtl_tex_init(&a);
    id<MTLTexture> base = tex_mtl(&a);
    CHECK(base != nil, "the render target is made");
    mtl_reg_tex(301, &a, NULL);
    id<MTLTexture> v1 = view_of(301);
    CHECK(v1 && v1 != base && v1.parentTexture == base && v1.swizzle.alpha == MTLTextureSwizzleOne, "the sampled view maps alpha to 1");
    uint64_t made = g_vc_new;
    // The frame ends: the game finalizes the target. Frame 2: it initializes it again over the same storage (a new serial) and registers it in another ID
    mtl_tex_release(&a);
    tex_t b = target(97002); mtl_tex_init(&b);
    CHECK(tex_mtl(&b) == base, "the new texture finds the shared Metal texture");
    mtl_reg_tex(302, &b, NULL);
    id<MTLTexture> v2 = view_of(302);
    CHECK(v2 == v1, "its sampled view is the one made in frame 1");
    CHECK(g_vc_new == made, "no view is made again");
    CHECK(view_of(302) == v2, "the ID keeps it");
    // An image slot's writable view (img_fit: no channel mapping) of the shared texture is kept the same way
    {   tex_t i1 = target(97101); i1.d.flags |= NVN_TEXTURE_FLAGS_IMAGE; i1.d.width = 32; nvn_tex_layout(&i1.d, &i1.L); mtl_tex_init(&i1);
        id<MTLTexture> ib = tex_mtl(&i1); mtl_reg_tex(311, &i1, NULL);
        os_unfair_lock_lock(&g_rmu); uint64_t r1 = 0, r2 = 0; id<MTLTexture> s1 = tex_by_id(311), w1 = img_fit(311, s1, NVN_DPF_FLOAT, s1.textureType, &r1); os_unfair_lock_unlock(&g_rmu);
        CHECK(w1 && w1.parentTexture == ib && (w1.usage & MTLTextureUsageShaderWrite) && w1.swizzle.alpha == MTLTextureSwizzleAlpha, "the image slot gets a writable view without the mapping");
        mtl_tex_release(&i1);
        tex_t i2 = i1; i2.serial = 97102; mtl_tex_init(&i2); mtl_reg_tex(312, &i2, NULL);
        uint64_t made_views = g_vc_new;
        os_unfair_lock_lock(&g_rmu); id<MTLTexture> s2 = tex_by_id(312), w2 = img_fit(312, s2, NVN_DPF_FLOAT, s2.textureType, &r2); os_unfair_lock_unlock(&g_rmu);
        CHECK(s2 == s1 && g_vc_new == made_views, "the next frame's texture samples the same view");
        CHECK(w2 == w1 && r2 == r1, "and its image slot gets the same writable view");
        mtl_tex_release(&i2); }
    // Another view of the same storage (a view format) is a view of its own, and is kept as well
    mtl_tex_release(&b);
    tex_t c = target(97003); mtl_tex_init(&c);
    view_t fv = {.set_format=1, .format=NVN_FORMAT_RGBX8_SRGB};
    mtl_reg_tex(303, &c, &fv);
    id<MTLTexture> v3 = view_of(303);
    CHECK(v3 && v3 != v1 && v3.parentTexture == base && v3.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB, "a view format makes a view of its own");
    mtl_reg_tex(304, &c, &fv);
    CHECK(view_of(304) == v3, "found again from another ID");
    // The shared texture leaves the sharing table (its pool's finalize): its views stay while a live texture still holds it, go at the first commit after the last one lets it go,
    // and a texture over that storage then gets a texture and views of its own
    uint64_t ba = vc_addr(base);
    alias_key ak = tex_alias_key(&c);
    pthread_mutex_lock(&g_smu); int gone = own_alias_del_locked(&ak); pthread_mutex_unlock(&g_smu);
    CHECK(gone, "the shared texture leaves the sharing table");
    vc_commit();
    os_unfair_lock_lock(&g_rmu); int kept = nvn_hm_get(&g_vc, &ba) != NULL; os_unfair_lock_unlock(&g_rmu);
    CHECK(kept, "its views stay while the live texture over it holds it");
    mtl_tex_release(&c);
    vc_commit();
    os_unfair_lock_lock(&g_rmu); int left = nvn_hm_get(&g_vc, &ba) == NULL; os_unfair_lock_unlock(&g_rmu);
    CHECK(left, "the commit after its last holder lets it go drops its views");
    tex_t d = target(97004); mtl_tex_init(&d);
    id<MTLTexture> base2 = tex_mtl(&d);
    CHECK(base2 && base2 != base, "a texture over that storage is made again");
    mtl_reg_tex(305, &d, NULL);
    id<MTLTexture> v5 = view_of(305);
    CHECK(v5 && v5.parentTexture == base2, "its view is of the new texture");
    // A texture outside the sharing table (on a pool's buffer, read in place) keeps its views while its serial holds it
    id<MTLBuffer> buf = [g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    tex_t e = target(97005); e.d.flags = NVN_TEXTURE_FLAGS_LINEAR; e.d.stride = 256; nvn_tex_layout(&e.d, &e.L);
    MTLTextureDescriptor *td = tex_desc(&e, 1); td.storageMode = MTLStorageModeShared;
    id<MTLTexture> lin = [buf newTextureWithDescriptor:td offset:0 bytesPerRow:256];
    mtl_tex_init(&e); serial_put(e.serial, lin);
    mtl_reg_tex(306, &e, NULL);
    uint64_t la = vc_addr(lin);
    os_unfair_lock_lock(&g_rmu); id<MTLTexture> v6 = tex_by_id(306); int has = nvn_hm_get(&g_vc, &la) != NULL; os_unfair_lock_unlock(&g_rmu);
    CHECK(v6 && v6.parentTexture == lin && has, "a texture the sharing table does not hold keeps its views while its serial holds it");
    mtl_tex_release(&e); vc_commit();
    os_unfair_lock_lock(&g_rmu); int dropped = nvn_hm_get(&g_vc, &la) == NULL; os_unfair_lock_unlock(&g_rmu);
    CHECK(dropped, "and they go at the commit after its release");
    // A texture the owner does not hold gets a view but no entry: nothing would let it go
    {   id<MTLTexture> loose = [g_dev newTextureWithDescriptor:td]; vc_key k = {.pf = MTLPixelFormatRGBA8Unorm, .tt = MTLTextureType2D, .nl = 1, .ns = 1};
        os_unfair_lock_lock(&g_rmu); vc_put(loose, NULL, &k, [loose newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm]); uint64_t lo = vc_addr(loose); int none = nvn_hm_get(&g_vc, &lo) == NULL; os_unfair_lock_unlock(&g_rmu);
        CHECK(none, "a texture the owner does not hold is not kept"); }
    mtl_sub_commit(); mtl_drain();
    printf("t_view_reuse: %d checks, %d failed\n", checks, fails);
    return fails != 0;
} }
