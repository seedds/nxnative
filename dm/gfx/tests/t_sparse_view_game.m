// A game's level range on a sparse texture (nvn_mtl.m sv_game_range, tex_by_id): a sparse texture whose level 0 is not mapped is bound by sparse_view from level 1; a texture ID whose view the
// game set to start at level 1 (PRAGMATA hides its unmapped levels this way) must sample level 1 of the texture, not level 1 of sparse_view's view (level 2). A view from level 0 starts at level 1
// while level 0 is unmapped and at level 0 once it is mapped and uploaded. Also the level arithmetic: the later of the game's base and sparse_view's, the game's end kept, a game range wholly
// unmapped falling back to its last level.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
static pool_t g_phys, g_sparse;
static void map_at(uint64_t voff, uint64_t size, uint64_t poff) {
    map_req r = {.phys = (NVNmemoryPool *)&g_phys, .poff = (int64_t)poff, .voff = (int64_t)voff, .size = size};
    CHECK("mapping request", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, 1, (const NVNmappingRequest *)&r));
}
static void range(unsigned gb, unsigned gn, unsigned vb, unsigned n, unsigned wl0, unsigned wnl, const char *label) {
    unsigned a, c; sv_game_range(gb, gn, vb, n, &a, &c); CHECK(label, a == wl0 && c == wnl);
}
int main(void) { @autoreleasepool {
    // the arithmetic
    range(1, 0, 1, 7, 1, 6, "game base 1 = sparse_view base 1: level 1, to the end");
    range(1, 0, 2, 7, 2, 5, "sparse_view's later base wins");
    range(2, 0, 1, 7, 2, 5, "the game's later base wins");
    range(0, 3, 1, 7, 1, 2, "the game's end is kept");
    range(0, 2, 4, 7, 1, 1, "nothing the game asked for is mapped: its last level");
    range(3, 9, 0, 7, 3, 4, "a count past the texture is cut");
    // through tex_by_id
    mtl_init();
    size_t plen = 16u << 20;
    uint8_t *phys = mmap(NULL, plen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (phys == MAP_FAILED) abort();
    for (size_t i = 0; i < plen; i++) phys[i] = (uint8_t)(i * 7 + 1);
    pool_builder_t pb = {.mem = phys, .size = plen, .flags = 0x24};
    CHECK("physical pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_phys, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.size = DM_GAME_APP_POOL + (1ull << 32), .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    CHECK("sparse pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_sparse, (const NVNmemoryPoolBuilder *)&pb));
    NVNtextureBuilder tb; NVNtexture tx; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetWidth(&tb, 512); h_nvnTextureBuilderSetHeight(&tb, 512);
    h_nvnTextureBuilderSetLevels(&tb, 7); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE); h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&g_sparse, 0);
    CHECK("sparse texture", h_nvnTextureInitialize(&tx, &tb));
    const tex_t *T = (const tex_t *)&tx;
    uint64_t l1 = nvn_tex_level_offset(&T->d, 1), end = (T->L.size + 0xFFFF) & ~0xFFFFull;
    CHECK("level 1 starts on a 64 KB page", !(l1 & 0xFFFF));
    map_at(l1, end - l1, 0);   // every level but 0
    view_t v; memset(&v, 0, sizeof v); v.set_levels = 1; v.base_level = 1; v.nlevels = 0;
    mtl_reg_tex(1, T, &v);   // the game's view: from level 1
    mtl_reg_tex(2, T, NULL);   // the same texture bound whole
    view_t v0 = v; v0.base_level = 0;
    mtl_reg_tex(3, T, &v0);   // a view from level 0, which is not mapped yet
    mtl_sub_begin();
    os_unfair_lock_lock(&g_rmu);
    id<MTLTexture> whole = tex_by_id(2), game = tex_by_id(1), from0 = tex_by_id(3);
    os_unfair_lock_unlock(&g_rmu);
    mtl_wait(mtl_sub_commit(), 5000000000ull);
    id h = tex_get(T->serial); NvPkg *pk = [h isKindOfClass:[NvPkg class]] ? h : nil;
    CHECK("a packaged sparse texture", pk && pk->sparse);
    CHECK("sparse_view starts at level 1", pk && pk->vbase == 1 && pk->view && pk->view.parentRelativeLevel == 1);
    CHECK("bound whole: sparse_view's view", whole && pk && whole == pk->view);
    CHECK("the game's view is made from the whole texture", game && pk && game.parentTexture == pk->tex);
    CHECK("the game's view starts at level 1 (256 wide), not 2", game && game.parentRelativeLevel == 1 && game.width == 256 && game.mipmapLevelCount == 6);
    os_unfair_lock_lock(&g_rmu); id<MTLTexture> again = tex_by_id(1); os_unfair_lock_unlock(&g_rmu);
    CHECK("the cached view is kept", again == game);
    CHECK("a view from level 0 starts at sparse_view's level 1 while level 0 is unmapped", from0 && pk && from0.parentTexture == pk->tex && from0.parentRelativeLevel == 1);
    map_at(0, l1, 8u << 20);   // level 0 mapped too: the next use uploads the texture again, and sparse_view binds it whole
    mtl_sub_begin();
    os_unfair_lock_lock(&g_rmu); id<MTLTexture> whole2 = tex_by_id(2), from0b = tex_by_id(3), game2 = tex_by_id(1); os_unfair_lock_unlock(&g_rmu);
    mtl_wait(mtl_sub_commit(), 5000000000ull);
    h = tex_get(T->serial); pk = [h isKindOfClass:[NvPkg class]] ? h : nil;
    CHECK("all mapped: sparse_view starts at level 0", pk && pk->vbase == 0 && whole2 == pk->tex);
    CHECK("the view from level 0 now starts at level 0", from0b && pk && (from0b == pk->tex || from0b.parentTexture == pk->tex) && (from0b == pk->tex || from0b.parentRelativeLevel == 0) && from0b.width == 512);
    CHECK("the game's view from level 1 stays at level 1", game2 && game2.width == 256);
    printf("t_sparse_view_game: %d checks, %d failed\n", checks, failed); return failed != 0;
}}
