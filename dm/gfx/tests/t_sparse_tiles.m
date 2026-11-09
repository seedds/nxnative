// Large sparse textures as placement sparse textures (nvn_mtl.m sparse_make, sparse_sync, sparse_look) through the real NVN entry points: a tile the game maps holds what its physical pages hold, in
// level 0 and in the mip tail; nothing is uploaded or mapped without a request; an unmapped tile gives its heap tile back; a tile rewritten in place is uploaded again; more requests than the pool's
// log holds still end right; a GPU-written texture gets tiles and no uploads; a small sparse texture stays whole. Shared tiles: one physical page mapped at two places is one heap tile, a GPU
// write through one place shows at the other, a drawn tile keeps its content for the next place until the CPU writes its memory or its pool's Finalize gives it back, an undrawn one goes back with
// its last place and each new place reads the memory (the CPU's fill behind a GPU-written place, the GPU's write to the memory as a buffer), a tile cut by the level's edge shares nothing, and a
// new pool over the same memory never finds the old pool's tile.
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
int nvn_ft_tracked(uint64_t serial);
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
// The texels of [x, y, w, h] of level 0 equal w × h packed ones
static int same_tile(id<MTLTexture> m, unsigned x, unsigned y, unsigned w, unsigned h, const uint32_t *want) {
    uint32_t *full = calloc((size_t)(x + w) * (y + h), 4); if (!full) return 0;   // where same() reads them
    for (unsigned r = 0; r < h; r++) memcpy(full + (size_t)(y + r) * (x + w) + x, want + (size_t)r * w, (size_t)w * 4);
    int ok = same(m, 0, x, y, w, h, full, x + w); free(full); return ok;
}
// The texels of [x, y, w, h] of level 0 equal those at [lx, ly] of the expected level (a tile at another place)
static int same_at(id<MTLTexture> m, unsigned x, unsigned y, unsigned w, unsigned h, const uint32_t *lin, unsigned stride, unsigned lx, unsigned ly) {
    uint32_t *part = malloc((size_t)w * h * 4); if (!part) return 0;
    for (unsigned r = 0; r < h; r++) memcpy(part + (size_t)r * w, lin + (size_t)(ly + r) * stride + lx, (size_t)w * 4);
    int ok = same_tile(m, x, y, w, h, part); free(part); return ok;
}
static uint64_t kept_for(const uint8_t *lo, size_t n) {   // shared slots of [lo, lo+n) no place maps (their memory's content kept)
    uint64_t k = 0; pthread_mutex_lock(&g_sp_mu);
    for (size_t i = 0; i < g_sp_ntl; i++) if (g_sp_tl[i].p >= (uintptr_t)lo && g_sp_tl[i].p < (uintptr_t)lo + n && !g_sp_tl[i].rc) k++;
    pthread_mutex_unlock(&g_sp_mu); return k;
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
    map_at(0, TILE, 0, 0); (void)resolve(&tx); CHECK("an unmapped tile gives its heap tile back", !ps->slot[0] && g_sp_live == live0 + 2 && !kept_for(phys, plen));
    // That memory at two other places: one heap tile
    map_at(1 * TILE, TILE, 0, 1); map_at(3 * TILE, TILE, 0, 1); (void)resolve(&tx);
    CHECK("one heap tile for one physical page", ps->slot[1] && ps->slot[1] == ps->slot[3] && g_sp_live == live0 + 3);
    CHECK("tile (1, 0) holds that page", same_at(m, TW, 0, TW, TW, lin[0], W, 0, 0));
    CHECK("tile (3, 0) holds that page", same_at(m, 3 * TW, 0, TW, TW, lin[0], W, 0, 0));
    {   // The GPU draws through one place: it shows at the other, and the slot holds what the memory does not
        uint32_t *wp = malloc((size_t)TW * TW * 4); for (size_t i = 0; i < (size_t)TW * TW; i++) wp[i] = 0xFF0000FFu;
        CHECK("not drawn yet", !sp_tl_of(ps->slot[1] - 1)->drawn);
        p_clear_tex pc = {.tex = T, .r = {TW, 0, 0, TW, TW, 1}, .mask = 0xF, .c.f = {1, 0, 0, 1}}; mtl_sub_begin(); clear_tex(&pc); finish();
        CHECK("a clear marks it drawn", sp_tl_of(ps->slot[1] - 1)->drawn);
        CHECK("a GPU write through tile (1, 0) shows in tile (3, 0)", same_tile(m, 3 * TW, 0, TW, TW, wp));
        map_at(1 * TILE, TILE, 0, 0); map_at(3 * TILE, TILE, 0, 0); (void)resolve(&tx);
        CHECK("a drawn tile is kept with no place", kept_for(phys, plen) == 1 && g_sp_live == live0 + 3);
        uint64_t nb0 = g_bp_up_bytes;
        map_at(6 * TILE, TILE, 0, 1); (void)resolve(&tx);
        CHECK("the GPU's content is there at a new place, nothing uploaded", ps->slot[6] && same_tile(m, 6 * TW, 0, TW, TW, wp) && g_bp_up_bytes == nb0);
        map_at(6 * TILE, TILE, 0, 0); (void)resolve(&tx);
        dm_guest_write_prepare(phys, TILE); memcpy(phys, store, TILE);   // the CPU writes that memory
        map_at(7 * TILE, TILE, 0, 1); (void)resolve(&tx);
        CHECK("the CPU's later write wins at a new place", same_at(m, 7 * TW, 0, TW, TW, lin[0], W, 0, 0));
        map_at(7 * TILE, TILE, 0, 0); (void)resolve(&tx);
        free(wp);
    }
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
    (void)resolve(&tx); CHECK("after an overflow the tile is mapped", ps->slot[5]);
    CHECK("and holds its pages", same(m, 0, 5 * TW, 0, TW, TW, lin[0], W));
    // GPU-written: tiles and no uploads
    NVNtexture gx; h_nvnTextureBuilderSetLevels(&tb, 1); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE | NVN_TEXTURE_FLAGS_COMPRESSIBLE); h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&g_sparse, 1ull << 30);
    CHECK("GPU-written sparse texture", h_nvnTextureInitialize(&gx, &tb));
    CHECK("a virtual pool's addresses are not on the flush list, where they would read as written at once", !nvn_ft_tracked(((const tex_t *)&gx)->serial));
    id<MTLTexture> g = resolve(&gx);
    CHECK("placement sparse too", g.isSparse);
    uint64_t live1 = g_sp_live, nblit = g_bp_up_bytes;
    map_at((1ull << 30) + 3 * TILE, TILE, 20 * TILE, 1); (void)resolve(&gx);
    CHECK("its tile is mapped", g_sp_live == live1 + 1);
    CHECK("nothing uploaded", g_bp_up_bytes == nblit);
    map_at((1ull << 30) + 3 * TILE, TILE, 20 * TILE, 0); (void)resolve(&gx);
    NvSparse *gs = objc_getAssociatedObject(g, &g_sp_key);
    CHECK("and unmapped, not drawn: its heap tile goes back", gs && !gs->slot[3] && g_sp_live == live1);
    // The CPU filled a page; a GPU-written place of it is looked at first, then a packaged one, which reads the memory
    dm_guest_write_prepare(phys + 24 * TILE, TILE); memcpy(phys + 24 * TILE, store, TILE);
    map_at((1ull << 30) + 5 * TILE, TILE, 24 * TILE, 1); (void)resolve(&gx);
    map_at(8 * TILE, TILE, 24 * TILE, 1); (void)resolve(&tx);
    CHECK("one heap tile across the two", ps->slot[8] && ps->slot[8] == gs->slot[5]);
    CHECK("a packaged place behind a GPU-written one reads the CPU's fill", same_at(m, 8 * TW, 0, TW, TW, lin[0], W, 0, 0));
    // The GPU writes a page as a buffer, which no fault shows: a new place reads it, and so does the place already there, as they are one heap tile
    dm_guest_write_prepare(phys + 28 * TILE, TILE); memcpy(phys + 28 * TILE, store, TILE);
    map_at(9 * TILE, TILE, 28 * TILE, 1); (void)resolve(&tx);
    {   void *bu = NULL; uint64_t off = 0; CHECK("the pool's buffer", mtl_addr((uint64_t)(uintptr_t)(phys + 28 * TILE), TILE, &bu, &off));
        id<MTLBuffer> dst = (__bridge id<MTLBuffer>)bu, src = [g_dev newBufferWithBytes:store + TILE length:TILE options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> ce = [cb blitCommandEncoder]; [ce copyFromBuffer:src sourceOffset:0 toBuffer:dst destinationOffset:off size:TILE]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted]; }
    map_at(10 * TILE, TILE, 28 * TILE, 1); (void)resolve(&tx);
    CHECK("a new place reads what the GPU wrote to the memory", same_at(m, 10 * TW, 0, TW, TW, lin[0], W, TW, 0));
    CHECK("and the place already there", same_at(m, 9 * TW, 0, TW, TW, lin[0], W, TW, 0));
    // A tile cut by the level's edge (4032 wide: tile 31 holds 64 texels) is uploaded only in part, so it shares nothing: the whole tile of the same page gets all of it
    NVNtexture ex; h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE); h_nvnTextureBuilderSetWidth(&tb, W - TW / 2); h_nvnTextureBuilderSetHeight(&tb, 2 * W); h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&g_sparse, 3ull << 30);
    CHECK("a texture of cut tiles", h_nvnTextureInitialize(&ex, &tb));
    const tex_t *EX = (const tex_t *)&ex; id<MTLTexture> em = resolve(&ex); CHECK("placement sparse too", em.isSparse);
    {   size_t ew = W - TW / 2, eh = 2 * W; uint32_t *el = malloc(ew * eh * 4); uint8_t *es = malloc(EX->L.size);
        for (size_t i = 0; i < ew * eh; i++) el[i] = (uint32_t)(i + 7) * 2654435761u;
        CHECK("swizzle", !nvn_swizzle(&EX->d, 0, 0, el, ew * 4, es));
        dm_guest_write_prepare(phys + 12 * TILE, TILE); memcpy(phys + 12 * TILE, es, TILE);
        uint64_t E = 3ull << 30;
        map_at(E + 31 * TILE, TILE, 12 * TILE, 1); (void)resolve(&ex);
        map_at(E, TILE, 12 * TILE, 1); (void)resolve(&ex);
        NvPkg *epk = tex_get(EX->serial); NvSparse *eps = epk->ps;
        CHECK("the cut tile has a slot of its own", eps->slot[31] && eps->slot[0] && eps->slot[31] != eps->slot[0]);
        CHECK("the whole tile holds all of the page", same(em, 0, 0, 0, TW, TW, el, (unsigned)ew));
        free(el); free(es); }
    // Small: the whole upload, as before
    NVNtexture sx; h_nvnTextureBuilderSetWidth(&tb, 512); h_nvnTextureBuilderSetHeight(&tb, 512); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE); h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&g_sparse, 2ull << 30);
    CHECK("small sparse texture", h_nvnTextureInitialize(&sx, &tb));
    CHECK("stays whole", !resolve(&sx).isSparse);
    // The physical pool's Finalize gives back the heap tiles kept for its memory
    uint64_t kept = kept_for(phys, plen), live2 = g_sp_live;
    CHECK("tiles kept for the pool's memory", kept >= 1);
    // ... and a new pool over the same memory never finds a tile of the old one, even one a texture not looked at since still holds
    dm_guest_write_prepare(phys + 32 * TILE, TILE); memcpy(phys + 32 * TILE, store, TILE);
    map_at((3ull << 30) + 2 * TILE, TILE, 32 * TILE, 1); (void)resolve(&ex);
    map_at((3ull << 30) + 2 * TILE, TILE, 32 * TILE, 0);   // unmapped, and that texture is not looked at again
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&g_phys);
    CHECK("its Finalize gives them back", !kept_for(phys, plen) && g_sp_live == live2 + 1 - kept);
    memcpy(phys + 32 * TILE, store + TILE, TILE);   // the game fills the memory, then makes a pool of it
    pool_builder_t again = {.mem = phys, .size = plen, .flags = 0x24};
    CHECK("the pool again", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_phys, (const NVNmemoryPoolBuilder *)&again));
    map_at(11 * TILE, TILE, 32 * TILE, 1); (void)resolve(&tx);
    CHECK("the new pool's content, not the old one's", same_at(m, 11 * TW, 0, TW, TW, lin[0], W, TW, 0));
    printf("t_sparse_tiles: %d checks, %d failed\n", checks, failed); return failed != 0;
}}
