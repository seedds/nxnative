// Texture views by the Metal texture they are made of (vb_get / vb_put in nvn_mtl.m), behind the cache by texture object, through the production lookup (tex_by_id): a view asked
// for again through a new texture object over the same storage, or through an ID whose object was finalized, is the one made before; an entry goes when the host retires the Metal
// texture (here: another description takes its storage, alias_take_over) and when it has gone VB_AGE frames unused; a texture uploaded from guest memory stays out of the cache.
// Ocarina of Time made 77 views a frame of 39 Metal textures that stayed (docs/reference/frame-rate.md)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

static tex_t tex(uint64_t serial, NVNformat format, int w) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = w, .height = w, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t.d, &t.L);
    return t;
}
static view_t swizzle(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { view_t v = {0}; v.set_swizzle = 1; v.swz[0] = r; v.swz[1] = g; v.swz[2] = b; v.swz[3] = a; return v; }
static id<MTLTexture> view_of(int tid) { os_unfair_lock_lock(&g_rmu); id<MTLTexture> v = tex_by_id(tid); os_unfair_lock_unlock(&g_rmu); return v; }
static int cached(id<MTLTexture> base) { const void *b = (__bridge const void *)base; os_unfair_lock_lock(&g_vb_lk); int r = nvn_hm_get(&g_vb, &b) != NULL; os_unfair_lock_unlock(&g_vb_lk); return r; }
static int shared(id<MTLTexture> t) { __block int r = 0; pthread_mutex_lock(&g_smu); own_alias_each_locked(^(const alias_key *k, id o) { (void)k; r |= o == t; }); pthread_mutex_unlock(&g_smu); return r; }
static uint64_t reused(void) { unsigned n; uint64_t hit, made; mtl_vc_stats(&n, &hit, &made); return hit; }
static uint64_t made(void) { unsigned n; uint64_t hit, m; mtl_vc_stats(&n, &hit, &m); return m; }
static void next_submission(void) { CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "submission completes"); mtl_sub_begin(); }
enum { SW_R = MTLTextureSwizzleRed, SW_1 = MTLTextureSwizzleOne, SW_0 = MTLTextureSwizzleZero };

static void new_object_reuses_view(void) {
    tex_t a = tex(93001, NVN_FORMAT_R32F, 64), b = tex(93002, NVN_FORMAT_R32F, 64);
    view_t rrr1 = swizzle(SW_R, SW_R, SW_R, SW_1), r001 = swizzle(SW_R, SW_0, SW_0, SW_0);
    mtl_tex_init(&a); mtl_reg_tex(1, &a, &rrr1);
    id<MTLTexture> ta = tex_mtl(&a);
    uint64_t hit0 = reused(), made0 = made(), vb0 = atomic_load(&g_vb_hit);
    id<MTLTexture> v1 = view_of(1);
    CHECK(ta && v1 && v1 != ta && v1.parentTexture == ta, "a swizzled descriptor gets a view of the object's Metal texture");
    CHECK(cached(ta) && made() == made0 + 1 && reused() == hit0, "the first ask makes the view and enters it by Metal texture");
    mtl_tex_release(&a);
    CHECK(!g_treg[1].view && !nvn_hm_get(&g_vc, &a.serial), "finalizing the object drops its descriptor's view and its entry by texture object");
    CHECK(view_of(1) == v1 && atomic_load(&g_vb_hit) == vb0 + 1, "the ID of a finalized object gets the view made before");
    mtl_tex_init(&b); mtl_reg_tex(1, &b, &rrr1);
    CHECK(tex_mtl(&b) == ta, "a new object over the same storage resolves to the same Metal texture");
    CHECK(view_of(1) == v1 && atomic_load(&g_vb_hit) == vb0 + 2, "and gets the view made before");
    CHECK(made() == made0 + 1 && reused() == hit0 + 2, "no view made for either, and both counted as reused");
    mtl_reg_tex(2, &b, &r001);
    id<MTLTexture> v2 = view_of(2);
    CHECK(v2 && v2 != v1 && v2.parentTexture == ta && made() == made0 + 2 && atomic_load(&g_vb_hit) == vb0 + 2, "another swizzle of the same texture is another view");
    mtl_tex_release(&b); tex_t c = tex(93003, NVN_FORMAT_R32F, 64); mtl_tex_init(&c);
    mtl_reg_tex(1, &c, &rrr1); mtl_reg_tex(2, &c, &r001);
    CHECK(view_of(1) == v1 && view_of(2) == v2 && atomic_load(&g_vb_hit) == vb0 + 4, "a texture holds several views, each found by its own parameters");
    defer_obj_unref(ta);
    CHECK(cached(ta), "dropping one reference to the texture (defer_obj_unref) leaves its views");
    mtl_reg_tex(1, NULL, NULL); mtl_reg_tex(2, NULL, NULL); mtl_tex_release(&c);
}
static void retired_texture_leaves(void) {
    tex_t a = tex(93011, NVN_FORMAT_R32F, 128), other = tex(93012, NVN_FORMAT_RG16F, 128), again = tex(93013, NVN_FORMAT_R32F, 128);
    view_t rrr1 = swizzle(SW_R, SW_R, SW_R, SW_1);
    mtl_tex_init(&a); mtl_reg_tex(3, &a, &rrr1);
    id<MTLTexture> ta = tex_mtl(&a), v1 = view_of(3);
    CHECK(ta && v1.parentTexture == ta && cached(ta), "a view entered by Metal texture");
    mtl_reg_tex(3, NULL, NULL); mtl_tex_release(&a);
    for (int i = 0; i <= ALIAS_IDLE; i++) next_submission();
    CHECK(cached(ta) && shared(ta), "the entry stays while the host keeps the texture for its storage");
    mtl_tex_init(&other);
    CHECK(tex_mtl(&other) && !shared(ta), "another description takes the storage and the host retires the texture");
    CHECK(!cached(ta), "a retired texture's views go with it");
    uint64_t vb0 = atomic_load(&g_vb_hit), made0 = made();
    mtl_tex_init(&again); mtl_reg_tex(3, &again, &rrr1);
    id<MTLTexture> tn = tex_mtl(&again), v2 = view_of(3);
    CHECK(tn && tn != ta && v2 && v2 != v1 && v2.parentTexture == tn, "the next object over the storage gets a view of its own Metal texture");
    CHECK(atomic_load(&g_vb_hit) == vb0 && made() == made0 + 1, "made, not reused");
    mtl_reg_tex(3, NULL, NULL); mtl_tex_release(&other); mtl_tex_release(&again);
}
static void unused_entry_ages_out(uint64_t f0) {
    tex_t used = tex(93021, NVN_FORMAT_R32F, 32), used2 = tex(93022, NVN_FORMAT_R32F, 32), idle = tex(93023, NVN_FORMAT_R32F, 16), idle2 = tex(93024, NVN_FORMAT_R32F, 16), late = tex(93025, NVN_FORMAT_R32F, 8);
    view_t rrr1 = swizzle(SW_R, SW_R, SW_R, SW_1);
    mtl_set_frame(f0);
    mtl_tex_init(&used); mtl_tex_init(&idle); mtl_reg_tex(4, &used, &rrr1); mtl_reg_tex(5, &idle, &rrr1);
    id<MTLTexture> tu = tex_mtl(&used), ti = tex_mtl(&idle), vu = view_of(4), vi = view_of(5);
    CHECK(tu && ti && tu != ti && cached(tu) && cached(ti), "two textures' views entered in the same frame");
    mtl_tex_release(&used); mtl_tex_release(&idle);
    mtl_set_frame(f0 + VB_AGE);
    mtl_tex_init(&late); mtl_reg_tex(6, &late, &rrr1);
    CHECK(view_of(6) && cached(ti), "an entry unused for exactly VB_AGE frames stays");
    mtl_tex_init(&used2); mtl_reg_tex(4, &used2, &rrr1);
    CHECK(view_of(4) == vu, "one of the two is asked for again in that frame, through a new object");
    mtl_set_frame(f0 + 2 * VB_AGE);
    view_t r001 = swizzle(SW_R, SW_0, SW_0, SW_0); mtl_reg_tex(6, &late, &r001);
    id<MTLTexture> tl = tex_mtl(&late);
    CHECK(view_of(6) && cached(tl), "a view made later enters the cache");
    CHECK(!cached(ti), "the entry unused for more than VB_AGE frames is gone");
    CHECK(cached(tu), "the one used since stays");
    uint64_t vb0 = atomic_load(&g_vb_hit);
    mtl_tex_init(&idle2); mtl_reg_tex(5, &idle2, &rrr1);
    id<MTLTexture> vi2 = view_of(5);
    CHECK(tex_mtl(&idle2) == ti && vi2 && vi2 != vi && vi2.parentTexture == ti && atomic_load(&g_vb_hit) == vb0, "its texture's next view is made again");
    mtl_reg_tex(4, NULL, NULL); mtl_reg_tex(5, NULL, NULL); mtl_reg_tex(6, NULL, NULL);
    mtl_tex_release(&used2); mtl_tex_release(&idle2); mtl_tex_release(&late);
}
// As tex_mtl leaves a texture uploaded from guest memory: an NvPkg in the serial table, clean since its upload, which two texture objects over the same bytes share (pkg_shared)
static void uploaded_stays_out(void) {
    static uint8_t pixels[64];
    tex_t a = tex(93031, NVN_FORMAT_RGBA8, 4), b = tex(93032, NVN_FORMAT_RGBA8, 4);
    a.has_layout = b.has_layout = 1; a.mem = b.mem = pixels;
    MTLTextureDescriptor *d = tex_desc(&a, 1); d.storageMode = MTLStorageModeShared;
    id<MTLTexture> base = [g_dev newTextureWithDescriptor:d];
    [base replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:pixels bytesPerRow:16];
    NvPkg *pk = [NvPkg new]; pk->tex = base; pk->span = pixels; pk->seen = nvn_dirty_now();
    serial_put(a.serial, pk); serial_put(b.serial, pk);
    view_t bgra = swizzle(MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed, MTLTextureSwizzleAlpha);
    os_unfair_lock_lock(&g_vb_lk); uint32_t n0 = g_vb.n; os_unfair_lock_unlock(&g_vb_lk);
    uint64_t vb0 = atomic_load(&g_vb_hit);
    mtl_reg_tex(7, &a, &bgra); mtl_reg_tex(8, &b, &bgra);
    id<MTLTexture> va = view_of(7), vb = view_of(8);
    CHECK(base && va && va != base && va.parentTexture == base && g_treg[7].view_pkg, "an uploaded texture's swizzled descriptor gets a view of the upload");
    CHECK(vb && vb.parentTexture == base && vb != va, "another object over the same upload gets a view of its own");
    os_unfair_lock_lock(&g_vb_lk); uint32_t n1 = g_vb.n; os_unfair_lock_unlock(&g_vb_lk);
    CHECK(!cached(base) && n1 == n0 && atomic_load(&g_vb_hit) == vb0, "an uploaded texture's views are neither entered by Metal texture nor found there");
    mtl_reg_tex(7, NULL, NULL); mtl_reg_tex(8, NULL, NULL); serial_del(a.serial); serial_del(b.serial);
}

int main(void) { @autoreleasepool {
    mtl_init(); mtl_sub_begin();
    mtl_set_frame(1000);
    new_object_reuses_view();
    retired_texture_leaves();
    unused_entry_ages_out(2000);
    uploaded_stays_out();
    next_submission(); defer_drain();
    printf("t_view_cache: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
