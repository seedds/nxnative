// Stand-ins (pool_stand, t_pool_standin) for textures in a sparse pool, through the real NVN entry points: past the walk's budget, a sparse pool's texture no shader has read holds a
// stand-in of its smallest levels, gathered through the pool's page table (stand_fill), not the dummy. One whose small levels are not mapped yet holds the dummy, and gets its stand-in
// at a later walk once the game maps them (PRAGMATA maps a streamed texture's small levels after registering it). Without sparse stand-ins every check of a stand-in here fails
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
static NVNtexture g_tx[4];
// A 256x256 RGBA8 sparse texture of 5 levels at voff in the sparse pool; the first map bytes of it mapped to the same offset of the physical pool (0: all of it)
static const tex_t *tex_at(int k, uint64_t voff, uint64_t map) {
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetWidth(&tb, 256); h_nvnTextureBuilderSetHeight(&tb, 256);
    h_nvnTextureBuilderSetLevels(&tb, 5); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE); h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&g_sparse, (int64_t)voff);
    CHECK("texture", h_nvnTextureInitialize(&g_tx[k], &tb));
    const tex_t *T = (const tex_t *)&g_tx[k];
    map_req r = {.phys = (NVNmemoryPool *)&g_phys, .poff = (int64_t)voff, .voff = (int64_t)voff, .size = map ? map : (T->L.size + 0xFFFF) & ~0xFFFFull};
    CHECK("mapping request", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, 1, (const NVNmappingRequest *)&r));
    return T;
}
// One submission that looks at the 2D float texture table: the entry of ID id_, and the dummy's in *dummy
static uint64_t entry(int id_, uint64_t *dummy) {
    mtl_sub_begin();
    os_unfair_lock_lock(&g_rmu);
    __unsafe_unretained id<MTLBuffer> b = nil; pool_gpu(0, (int)MTLTextureType2D, NVN_DPF_FLOAT, 0, &b);
    const uint8_t *c = b.contents; uint32_t n = *(const uint32_t *)c; const uint64_t *e = (const uint64_t *)(c + DMSH_POOL_HEAD);
    uint64_t r = (uint32_t)id_ < n ? e[id_] : 0; *dummy = e[n];
    os_unfair_lock_unlock(&g_rmu);
    CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
    return r;
}
int main(void) { @autoreleasepool {
    mtl_init();
    size_t plen = 16u << 20;
    uint8_t *phys = mmap(NULL, plen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (phys == MAP_FAILED) abort();
    for (size_t i = 0; i < plen; i++) phys[i] = (uint8_t)(i * 7 + 1);
    pool_builder_t pb = {.mem = phys, .size = plen, .flags = 0x24};
    CHECK("physical pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_phys, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.size = DM_GAME_APP_POOL + (1ull << 32), .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    CHECK("sparse pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_sparse, (const NVNmemoryPoolBuilder *)&pb));
    id<MTLBuffer> hog = [g_dev newBufferWithLength:pool_budget() + (1u << 20) options:MTLResourceStorageModePrivate];   // past the walk's budget, as PRAGMATA is from its first scene
    CHECK("over the budget", hog && pool_budget_full());
    const tex_t *B = tex_at(0, 1u << 20, 0);
    atomic_store(&g_cur_frame, 100);

    // 1. a sparse pool's texture no shader has read: its stand-in, level 4 (16x16, 1 KB) gathered through the page table
    mtl_reg_tex(6, B, NULL);
    uint64_t dummy, e = entry(6, &dummy);
    atomic_store(&g_cur_frame, 101); e = entry(6, &dummy);   // the walk past the budget: what it left out gets its stand-in
    CHECK("the walk makes no unread texture", !tex_get(B->serial));
    id<MTLTexture> st = (__bridge id<MTLTexture>)g_treg[6].stand;
    CHECK("a sparse pool's texture left out holds its stand-in, not the dummy", st && e == g_treg[6].stand_rid && e != dummy);
    if (st) {
        uint8_t want[16 * 16 * 4], got[16 * 16 * 4];
        CHECK("level 4 decodes", !nvn_deswizzle(&B->d, 4, 0, phys + (1u << 20), want, 16 * 4));
        [st getBytes:got bytesPerRow:16 * 4 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0];
        CHECK("the stand-in holds the texture's level 4, read through the page table", st.width == 16 && st.mipmapLevelCount == 1 && !memcmp(want, got, sizeof want));
    }
    CHECK("counted as made in a sparse pool", g_stand_sparse >= 1);

    // 2. one whose small levels are not mapped yet (level 0's first 64 KB only): the dummy, not a stand-in that would read 0
    const tex_t *U = tex_at(1, 6u << 20, 0x10000);
    uint64_t un0 = g_stand_sparse_unmapped;
    mtl_reg_tex(9, U, NULL); atomic_store(&g_cur_frame, 200);
    e = entry(9, &dummy);
    CHECK("a sparse texture whose small levels are unmapped holds the dummy, no stand-in", e == dummy && !g_treg[9].stand && g_stand_sparse_unmapped > un0);
    CHECK("the refusal counts toward the frame's stand-in budget", g_stand_frame == 200 && g_stand_frame_n >= 1);

    // 2b. the game maps the rest within the same frame: no walk, but the mapping notes the texture (treg_sparse_notes), the table entry is patched, and the ID that waited (stand_no
    //     still 0) gets its stand-in then
    map_req r2 = {.phys = (NVNmemoryPool *)&g_phys, .poff = (6u << 20) + 0x10000, .voff = (6u << 20) + 0x10000, .size = ((U->L.size + 0xFFFF) & ~0xFFFFull) - 0x10000};
    CHECK("rest mapped", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, 1, (const NVNmappingRequest *)&r2));
    e = entry(9, &dummy);
    CHECK("mapped in the same frame: its stand-in when the mapping notes it", g_treg[9].stand && e == g_treg[9].stand_rid && e != dummy);
    mtl_reg_tex(9, NULL, NULL); mtl_reg_tex(9, U, NULL);   // registered again: looked at afresh in step 3

    // 3. registered again with every page mapped: the next walk makes its stand-in
    atomic_store(&g_cur_frame, 201);
    e = entry(9, &dummy);
    CHECK("mapped later: its stand-in at the next walk", g_treg[9].stand && e == g_treg[9].stand_rid && e != dummy);

    mtl_reg_tex(6, NULL, NULL); mtl_reg_tex(9, NULL, NULL);
    CHECK("no stand-in is left when the IDs are emptied", !g_stand_live);
    hog = nil;
    printf("t_pool_standin_sparse: %d checks, %d failed\n", checks, failed); return failed != 0;
}}
