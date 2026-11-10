// Smaller textures on small devices (nvn_mtl.m DM_NVN_TEX_DROP_MIP, docs/reference/memory.md): where Metal's working set is under 6 GB, a large sampled texture made from the game's
// data leaves out its largest level, so Metal level k holds the game's level k+1. Only sampling sees it: any other use makes it whole again. Checks: the device rule; level k of a
// dropped texture equals the guest's level k+1, by the CPU and by the GPU deswizzle (the direct path writes every level, so a dropped texture is staged instead); a re-upload after a
// guest write stays dropped and shows the new data; small, single-level, render-target and linear textures are not dropped; a use other than sampling makes the texture whole with
// every level and it stays whole; a sampled view's levels shift by one. Forced on (g_drop_on) wherever it runs
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
static void finish(void) { CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK("GPU has no submission error", !mtl_last_commit_error()); }

typedef struct { tex_t t; pool_t pool; void *mem; size_t size; uint8_t *ref[16]; size_t ref_n[16]; unsigned bpp; } gtex;
// A swizzled texture in a CPU-visible pool, each level and layer filled with its own pattern (ref holds the linear bytes)
static void gtex_new(gtex *g, uint64_t serial, int target, int format, unsigned bpp, unsigned w, unsigned h, unsigned layers, unsigned levels, int flags) {
    memset(g, 0, sizeof *g);
    g->t = (tex_t){.magic = MAGIC('T', 'E', 'X', 0), .has_layout = 1, .serial = serial};
    g->t.d = (nvn_texdesc){.target = target, .format = format, .flags = flags, .width = w, .height = h, .depth = layers, .levels = levels, .samples = 1};
    nvn_tex_layout(&g->t.d, &g->t.L); g->bpp = bpp;
    g->size = (g->t.L.size + 4096 + getpagesize() - 1) & ~(size_t)(getpagesize() - 1);
    if (posix_memalign(&g->mem, getpagesize(), g->size)) abort();
    memset(g->mem, 0, g->size);
    g->pool = (pool_t){.magic = MAGIC('P', 'O', 'O', 'L'), .mem = g->mem, .size = g->size, .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    g->t.pool = &g->pool; g->t.mem = (uint8_t *)g->mem + 4096;
    mtl_pool_add(g->mem, g->size); nvn_dirty_add_range(g->mem, g->size);
}
static void gtex_fill(gtex *g, uint32_t seed) {
    dm_guest_write_prepare(g->mem, g->size);
    for (unsigned l = 0; l < g->t.d.levels; l++) {
        unsigned w = MAX(g->t.d.width >> l, 1u), h = MAX(g->t.d.height >> l, 1u), layers = g->t.d.target == NVN_TEXTURE_TARGET_2D ? 1 : g->t.d.depth;
        size_t n = (size_t)w * h * g->bpp; free(g->ref[l]); g->ref[l] = malloc(n * layers); g->ref_n[l] = n;
        for (unsigned a = 0; a < layers; a++) {
            uint8_t *p = g->ref[l] + n * a;
            for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 131u + l * 61u + a * 17u + seed * 7u + (i >> 9));
            CHECK("reference swizzle succeeds", !nvn_swizzle(&g->t.d, l, a, p, w * g->bpp, g->t.mem));
        }
    }
}
// Does level `ml` of the Metal texture hold the guest's level `gl` (every layer)?
static int level_is(id<MTLTexture> m, unsigned ml, gtex *g, unsigned gl) {
    unsigned w = MAX(g->t.d.width >> gl, 1u), h = MAX(g->t.d.height >> gl, 1u), layers = (unsigned)m.arrayLength;
    if (MAX(m.width >> ml, 1u) != w || MAX(m.height >> ml, 1u) != h) return 0;
    size_t n = (size_t)w * h * g->bpp;
    id<MTLBuffer> b = [g_dev newBufferWithLength:n * layers options:MTLResourceStorageModeShared]; res_add(b);
    for (unsigned a = 0; a < layers; a++) enc_b_copy_texture_buffer(blit(), m, a, ml, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, 1), b, n * a, w * g->bpp, n);
    finish(); mtl_sub_begin();
    int ok = !memcmp(b.contents, g->ref[gl], n * layers);
    res_remove(b); return ok;
}
static id<MTLTexture> sampled(tex_t *t) { t_drop_ok++; id<MTLTexture> m = tex_mtl(t); t_drop_ok--; finish(); mtl_sub_begin(); return m; }
static id<MTLTexture> used(tex_t *t) { id<MTLTexture> m = tex_mtl(t); finish(); mtl_sub_begin(); return m; }

static void dropped_case(const char *name, int target, int format, unsigned bpp, unsigned w, unsigned h, unsigned layers, unsigned levels, int direct, uint64_t serial) {
    g_direct_upload_on = direct;
    gtex g; gtex_new(&g, serial, target, format, bpp, w, h, layers, levels, 0); gtex_fill(&g, 1);
    uint64_t n0 = g_drop_n, d0 = g_direct_uploads;
    id<MTLTexture> m = sampled(&g.t);
    char msg[160];
    snprintf(msg, sizeof msg, "%s: dropped, half size with one level fewer", name);
    CHECK(msg, m && g_drop_n == n0 + 1 && m.width == w / 2 && m.height == h / 2 && m.mipmapLevelCount == levels - 1);
    snprintf(msg, sizeof msg, "%s: the direct GPU deswizzle is not used for a dropped texture", name);
    CHECK(msg, g_direct_uploads == d0);
    int shift = 1; for (unsigned l = 0; m && l + 1 < levels; l++) shift &= level_is(m, l, &g, l + 1);
    snprintf(msg, sizeof msg, "%s: Metal level k holds the guest's level k+1", name);
    CHECK(msg, shift);
    // A guest write: re-uploaded, still dropped, with the new data
    gtex_fill(&g, 2);
    id<MTLTexture> r = sampled(&g.t);
    snprintf(msg, sizeof msg, "%s: a re-upload after a guest write stays dropped and shows the new data", name);
    CHECK(msg, r && r.width == w / 2 && r.mipmapLevelCount == levels - 1 && level_is(r, 0, &g, 1));
    // A use other than sampling: made whole, every level, and it stays whole for sampling too
    uint64_t f0 = g_drop_full_n;
    id<MTLTexture> u = used(&g.t);
    int whole = u && u.width == w && u.height == h && u.mipmapLevelCount == levels && g_drop_full_n == f0 + 1;
    for (unsigned l = 0; whole && l < levels; l++) whole &= level_is(u, l, &g, l);
    snprintf(msg, sizeof msg, "%s: a use other than sampling makes it whole with every level", name);
    CHECK(msg, whole);
    id<MTLTexture> s = sampled(&g.t);
    snprintf(msg, sizeof msg, "%s: once made whole, it stays whole when sampled and after a guest write", name);
    gtex_fill(&g, 3); id<MTLTexture> s2 = sampled(&g.t);
    CHECK(msg, s && s.width == w && s.mipmapLevelCount == levels && s2 && s2.width == w && level_is(s2, 0, &g, 0));
}
static void kept_case(const char *name, int target, int format, unsigned bpp, unsigned w, unsigned h, unsigned levels, int flags, uint64_t serial) {
    gtex g; gtex_new(&g, serial, target, format, bpp, w, h, 1, levels, flags); gtex_fill(&g, 1);
    uint64_t n0 = g_drop_n;
    id<MTLTexture> m = sampled(&g.t);
    char msg[160]; snprintf(msg, sizeof msg, "%s: not dropped", name);
    CHECK(msg, m && g_drop_n == n0 && m.width == w && m.mipmapLevelCount == levels);
}
int main(void) { @autoreleasepool {
    CHECK("the M1 iPad's and the iPhone 17's working set (5,461 MB) is in", drop_wants(5461ull << 20));
    CHECK("6 GB of working set or more, or none known, keeps every level", !drop_wants(6144ull << 20) && !drop_wants(12124ull << 20) && !drop_wants(0));
    // A sampled view's levels: from level 0 → from Metal level 0, one fewer; from level k > 0 → from Metal level k-1, the same count
    { NSUInteger l0 = 0, nl = 4; drop_levels(&l0, &nl); CHECK("a view from level 0 starts at Metal level 0 with one level fewer", l0 == 0 && nl == 3); }
    { NSUInteger l0 = 2, nl = 3; drop_levels(&l0, &nl); CHECK("a view from level 2 starts at Metal level 1 with as many levels", l0 == 1 && nl == 3); }
    { NSUInteger l0 = 0, nl = 1; drop_levels(&l0, &nl); CHECK("a view of level 0 alone reads Metal level 0 (the guest's level 1)", l0 == 0 && nl == 1); }
    mtl_init(); mtl_sub_begin();
    g_drop_on = 1;
    dropped_case("RGBA8 2D 512x512, 4 levels, CPU path", NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 4, 512, 512, 1, 4, 0, 991000);
    dropped_case("RGBA8 2D 1024x512, 3 levels, GPU deswizzle", NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 4, 1024, 512, 1, 3, 1, 991100);
    dropped_case("RGBA16F 2D array 512x512x3, 3 levels, direct upload asked", NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA16F, 8, 512, 512, 3, 3, 1, 991200);
    kept_case("RGBA8 2D 256x256, 4 levels (under the threshold)", NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 4, 256, 256, 4, 0, 991300);
    kept_case("RGBA8 2D 512x512, 1 level", NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 4, 512, 512, 1, 0, 991400);
    kept_case("RGBA8 2D 512x512, 3 levels, compressible (a render target)", NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 4, 512, 512, 3, NVN_TEXTURE_FLAGS_COMPRESSIBLE, 991500);
    kept_case("RGBA8 2D 512x512, 3 levels, image", NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 4, 512, 512, 3, NVN_TEXTURE_FLAGS_IMAGE, 991600);
    g_drop_on = 0;
    kept_case("RGBA8 2D 512x512, 4 levels, switch off", NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 4, 512, 512, 4, 0, 991700);
    printf("t_tex_drop_mip: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
