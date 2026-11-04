// Large sparse textures as placement sparse textures (nvn_mtl.m sparse_make, sparse_sync, sparse_look) through the real NVN entry points: a tile the game maps holds what its physical pages hold, in
// level 0 and in the mip tail; nothing is uploaded or mapped without a request; an unmapped tile gives its heap tile back; a tile rewritten in place is uploaded again; more requests than the pool's
// log holds still end right; a GPU-written texture gets tiles and no uploads; a small sparse texture stays whole.
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
static void finish(void) { CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK("GPU has no submission error", !mtl_last_commit_error()); }
static id<MTLTexture> resolve(const NVNtexture *x) { mtl_sub_begin(); id<MTLTexture> m = tex_mtl((const tex_t *)x); finish(); return m; }
static pool_t g_phys, g_sparse;
static void map_at(uint64_t voff, uint64_t size, uint64_t poff, int mapped) {
    map_req r = {.phys = mapped ? (NVNmemoryPool *)&g_phys : NULL, .poff = (int64_t)poff, .voff = (int64_t)voff, .size = size};
    CHECK("mapping request", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, 1, (const NVNmappingRequest *)&r));
}
// The texels of [x, y, w, h] of a level read back from the Metal texture equal the expected linear level
static int same(id<MTLTexture> m, unsigned level, unsigned x, unsigned y, unsigned w, unsigned h, const uint32_t *lin, unsigned stride) {
    id<MTLBuffer> rb = [g_dev newBufferWithLength:(size_t)w * h * 4 options:MTLResourceStorageModeShared];
    if (!rb) { fprintf(stderr, "readback buffer did not allocate\n"); return 0; }
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> ce = [cb blitCommandEncoder];
    [ce copyFromTexture:m sourceSlice:0 sourceLevel:level sourceOrigin:MTLOriginMake(x, y, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:(NSUInteger)w * h * 4];
    [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const uint32_t *got = rb.contents;
    for (unsigned r = 0; r < h; r++) if (memcmp(got + (size_t)r * w, lin + (size_t)(y + r) * stride + x, (size_t)w * 4)) return 0;
    return 1;
}
int main(void) { @autoreleasepool {
    mtl_init();
    enum { W = 4096, LEVELS = 13, TILE = 0x10000, TW = 128 };
    size_t plen = 64u << 20;
    uint8_t *phys = mmap(NULL, plen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (phys == MAP_FAILED) abort();
    pool_builder_t pb = {.mem = phys, .size = plen, .flags = 0x24};
    CHECK("physical pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_phys, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.size = DM_GAME_APP_POOL + (1ull << 32), .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    CHECK("sparse pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_sparse, (const NVNmemoryPoolBuilder *)&pb));
    NVNtextureBuilder tb; NVNtexture tx; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetWidth(&tb, W); h_nvnTextureBuilderSetHeight(&tb, W);
    h_nvnTextureBuilderSetLevels(&tb, LEVELS); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE); h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&g_sparse, 0);
    CHECK("sparse texture", h_nvnTextureInitialize(&tx, &tb));
    const tex_t *T = (const tex_t *)&tx;
    uint32_t tw, th, td; int nt = nvn_tex_sparse_tiles(&T->d, &tw, &th, &td); CHECK("tiled levels", nt == 6 && tw == TW && th == TW);
    // What each level holds, and its block-linear storage
    uint32_t *lin[LEVELS]; uint8_t *store = malloc(T->L.size);
    for (unsigned l = 0; l < LEVELS; l++) { unsigned w = MAX((unsigned)W >> l, 1u); lin[l] = malloc((size_t)w * w * 4);
        for (size_t i = 0; i < (size_t)w * w; i++) lin[l][i] = (uint32_t)(i + 1 + (l << 24)) * 2654435761u;
        CHECK("swizzle", !nvn_swizzle(&T->d, l, 0, lin[l], (size_t)w * 4, store)); }
    uint64_t tail = nvn_tex_level_offset(&T->d, (unsigned)nt), tail_n = T->L.layer_size - tail;

    id<MTLTexture> m = resolve(&tx);
    CHECK("placement sparse", m.isSparse);
    CHECK("no upgrade will be needed for a view or a clear", (m.usage & MTLTextureUsagePixelFormatView) && (m.usage & MTLTextureUsageRenderTarget));
    CHECK("an upgrade keeps it", tex_upgrade(T, m, MTLTextureUsageShaderWrite, m.pixelFormat) == m);
    uint64_t live0 = g_sp_live;
    // Tiles (0, 0) and (2, 1) of level 0, and the tail, each from pages of its own
    uint64_t t1 = 32 + 2;
    memcpy(phys, store, TILE); memcpy(phys + 2 * TILE, store + t1 * TILE, TILE); memcpy(phys + 4 * TILE, store + tail, tail_n);
    map_at(0, TILE, 0, 1); map_at(t1 * TILE, TILE, 2 * TILE, 1); map_at(tail, tail_n, 4 * TILE, 1);
    CHECK("the same texture", resolve(&tx) == m);
    CHECK("three tiles mapped", g_sp_live == live0 + 3);
    CHECK("tile (0, 0) holds its pages", same(m, 0, 0, 0, TW, TW, lin[0], W));
    CHECK("tile (2, 1) holds its pages", same(m, 0, 2 * TW, TW, TW, TW, lin[0], W));
    for (unsigned l = nt; l < LEVELS; l++) { unsigned w = MAX((unsigned)W >> l, 1u); CHECK("a tail level holds its pages", same(m, l, 0, 0, w, w, lin[l], w)); }
    NvPkg *pk = tex_get(T->serial); NvSparse *ps = pk->ps; uint64_t seq = ps->seq;
    (void)resolve(&tx); CHECK("no request: nothing looked at", ps->seq == seq && g_sp_live == live0 + 3);
    // A mapping elsewhere in the pool leaves the texture's tiles alone
    map_at(T->L.size + (1u << 20), TILE, 8 * TILE, 1); (void)resolve(&tx); CHECK("a request past the texture", g_sp_live == live0 + 3 && ps->seq != seq);
    // Unmapped
    map_at(0, TILE, 0, 0); (void)resolve(&tx); CHECK("an unmapped tile gives its heap tile back", g_sp_live == live0 + 2 && !ps->slot[0]);
    // Rewritten in place while mapped: uploaded again at the next look a second on
    uint64_t w0 = nvn_dirty_sparse_writes();
    uint32_t *fresh = malloc((size_t)W * W * 4); memcpy(fresh, lin[0], (size_t)W * W * 4);
    for (unsigned y = TW; y < 2 * TW; y++) for (unsigned x = 2 * TW; x < 3 * TW; x++) fresh[(size_t)y * W + x] = x ^ (y << 12);
    nvn_texdesc one = T->d; uint8_t *st2 = malloc(T->L.size); CHECK("swizzle", !nvn_swizzle(&one, 0, 0, fresh, (size_t)W * 4, st2));
    dm_guest_write_prepare(phys + 2 * TILE, TILE); memcpy(phys + 2 * TILE, st2 + t1 * TILE, TILE);   // as a host write; a game's write faults into the same path
    CHECK("the write shows", nvn_dirty_sparse_writes() != w0);
    atomic_fetch_add(&g_cur_frame, 60); (void)resolve(&tx);
    CHECK("the rewritten tile is uploaded again", same(m, 0, 2 * TW, TW, TW, TW, fresh, W));
    // More requests than the log holds: the texture is looked at whole
    for (int i = 0; i < VLOG_N + 8; i++) map_at(5 * TILE, TILE, 16 * TILE, i & 1);   // ends mapped when VLOG_N + 8 is odd, unmapped when even
    dm_guest_write_prepare(phys + 16 * TILE, TILE); memcpy(phys + 16 * TILE, store + 5 * TILE, TILE); map_at(5 * TILE, TILE, 16 * TILE, 1);
    (void)resolve(&tx); CHECK("after an overflow the tile is mapped", g_sp_live == live0 + 3);
    CHECK("and holds its pages", same(m, 0, 5 * TW, 0, TW, TW, lin[0], W));
    // GPU-written: tiles and no uploads
    NVNtexture gx; h_nvnTextureBuilderSetLevels(&tb, 1); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE | NVN_TEXTURE_FLAGS_COMPRESSIBLE); h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&g_sparse, 1ull << 30);
    CHECK("GPU-written sparse texture", h_nvnTextureInitialize(&gx, &tb));
    id<MTLTexture> g = resolve(&gx);
    CHECK("placement sparse too", g.isSparse);
    uint64_t live1 = g_sp_live, nblit = g_bp_up_bytes;
    map_at((1ull << 30) + 3 * TILE, TILE, 20 * TILE, 1); (void)resolve(&gx);
    CHECK("its tile is mapped", g_sp_live == live1 + 1);
    CHECK("nothing uploaded", g_bp_up_bytes == nblit);
    map_at((1ull << 30) + 3 * TILE, TILE, 20 * TILE, 0); (void)resolve(&gx);
    CHECK("and unmapped", g_sp_live == live1);
    // Small: the whole upload, as before
    NVNtexture sx; h_nvnTextureBuilderSetWidth(&tb, 512); h_nvnTextureBuilderSetHeight(&tb, 512); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE); h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&g_sparse, 2ull << 30);
    CHECK("small sparse texture", h_nvnTextureInitialize(&sx, &tb));
    CHECK("stays whole", !resolve(&sx).isSparse);
    printf("t_sparse_tiles: %d checks, %d failed\n", checks, failed); return failed != 0;
}}
