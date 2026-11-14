// GPU copies into a texture of a sparse pool that the host uploads whole (tex_from_storage): the copy reaches the physical memory mapped behind the texture, as the hardware's does
// (gsb_swz through the page table); a copy into a level of a texture bound from that level (sparse_view) lands in that level (tex_mtl_all); and an upload after a mapping change keeps
// what earlier copies wrote, also when the copy's command buffer is still running (the upload batch's GPU gather, or on the CPU sp_store_wait). Densha de GO!! Hashiro Yamanote Line
// streams the finer levels of its sparse ASTC textures in by buffer-to-texture copies, and every upload after a mapping change put back zeros (GitHub #2350). Run twice: with the GPU
// deswizzle (the gather on the GPU) and without it (the gather on the CPU). Texel values are literal colours
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
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } else printf("PASS: %s\n", label); } while (0)
static void next_sub(void) { mtl_sub_commit(); mtl_drain(); mtl_sub_begin(); }
static int map(pool_t *virt, pool_t *phys, uint64_t voff, uint64_t poff, uint64_t size) {
    map_req req = {.phys = (NVNmemoryPool *)phys, .voff = (int64_t)voff, .poff = (int64_t)poff, .size = size};
    return h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)virt, 1, (const NVNmappingRequest *)&req);
}
// A buffer-to-texture copy of the whole of level `level` (w x w) from src, through a view of that level, replayed in the open submission
static void copy_in(pool_t *cpool, NVNtexture *tx, unsigned level, unsigned w, const void *src) {
    NVNcommandBuffer cmd; h_nvnCommandBufferInitialize(&cmd, NULL);
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)cpool, 0, 65536);
    h_nvnCommandBufferBeginRecording(&cmd);
    NVNtextureView v; h_nvnTextureViewSetDefaults(&v); h_nvnTextureViewSetLevels(&v, (int)level, 1);
    NVNcopyRegion r = {0, 0, 0, (int)w, (int)w, 1};
    h_nvnCommandBufferCopyBufferToTexture(&cmd, (NVNbufferAddress)src, tx, &v, &r, 0);
    NVNcommandHandle h = h_nvnCommandBufferEndRecording(&cmd);
    state_reset(); memset(&R, 0, sizeof R); mtl_exec(nvn_rec_linear(h, NULL));
}
static int words_not(const uint8_t *p, uint64_t n, uint32_t want) { int bad = 0; for (uint64_t i = 0; i < n; i += 4) bad += *(const uint32_t *)(p + i) != want; return bad; }
// Texels of level `level` (w x w) of the host texture other than want: the texture is looked up (and uploaded if its pages changed) in this submission, and read after it
static int host_not(const tex_t *t, unsigned level, unsigned w, uint32_t want) {
    id<MTLTexture> mt = tex_mtl_all(t);
    next_sub();
    if (!mt || level >= mt.mipmapLevelCount || mt.width != t->d.width) return -1;
    id<MTLBuffer> b = [g_dev newBufferWithLength:(NSUInteger)w * w * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> c = q_cb(); id<MTLBlitCommandEncoder> e = [c blitCommandEncoder];
    [e copyFromTexture:mt sourceSlice:0 sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, w, 1) toBuffer:b destinationOffset:0
        destinationBytesPerRow:w * 4 destinationBytesPerImage:(NSUInteger)w * w * 4];
    [e endEncoding]; [c commit]; [c waitUntilCompleted];
    return words_not(b.contents, (uint64_t)w * w * 4, want);
}
static void run(int gpu, pool_t *virt, pool_t *phys, uint8_t *pmem, pool_t *cpool, uint8_t *cmem, uint64_t VO, uint64_t P0, uint64_t P1) {
    const uint32_t A = 0xff112233, B = 0xff445566, C = 0xff778899;
    const char *how = gpu ? "GPU gather" : "CPU gather";
    char n[200];
    g_nvn_deswz_gpu = gpu;
    NVNtextureBuilder tb; NVNtexture tx; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetWidth(&tb, 256); h_nvnTextureBuilderSetHeight(&tb, 256); h_nvnTextureBuilderSetLevels(&tb, 2);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)virt, VO);
    CHECK(h_nvnTextureInitialize(&tx, &tb), "sparse texture of two levels");
    const tex_t *t = (const tex_t *)&tx;
    uint64_t l1 = nvn_tex_level_offset(&t->d, 1), lend = (t->L.size + 0xFFF) & ~0xFFFull;
    CHECK(l1 == 0x40000 && lend - l1 <= 0x10000, "level 0 is 256 KB of pages, level 1 at most 64 KB");
    CHECK(tex_storage_dst(t), "a texture of a sparse pool uploaded whole takes GPU writes into its storage");

    // Level 1 mapped over memory the CPU wrote (A), level 0 not: the texture is bound from level 1. A copy of B into level 1 lands there, and in the memory behind it
    for (uint64_t o = 0; o < lend - l1; o += 4) *(uint32_t *)(pmem + P1 + o) = A;
    CHECK(map(virt, phys, VO + l1, P1, lend - l1), "level 1 mapped, level 0 not");
    for (unsigned i = 0; i < 128 * 128; i++) ((uint32_t *)(cmem + 0x10000))[i] = B;
    copy_in(cpool, &tx, 1, 128, cmem + 0x10000);
    next_sub();
    NvPkg *pk = tex_get(t->serial);
    snprintf(n, sizeof n, "%s: the texture is bound from level 1 (sparse_view)", how); CHECK([pk isKindOfClass:[NvPkg class]] && pk->vbase == 1, n);
    snprintf(n, sizeof n, "%s: the copy into level 1 reaches the physical memory mapped behind it", how); CHECK(words_not(pmem + P1, 128 * 128 * 4, B) == 0, n);
    snprintf(n, sizeof n, "%s: the copy lands in level 1 of the host texture, not past its view", how); CHECK(host_not(t, 1, 128, B) == 0, n);

    // Level 0 mapped (zeros): an upload, and a copy of C into level 0. The copy's command buffer is committed and not waited for; a CPU write behind level 1 (B, what the copy
    // stored there) then uploads the texture again, which reads both copies from the physical memory. (A mapping request over pages mapped before waits for the GPU: not that)
    CHECK(map(virt, phys, VO, P0, l1), "level 0 mapped");
    for (unsigned i = 0; i < 256 * 256; i++) ((uint32_t *)(cmem + 0x20000))[i] = C;
    copy_in(cpool, &tx, 0, 256, cmem + 0x20000);
    mtl_sub_commit(); mtl_sub_begin();
    uint64_t reups = g_pkg_reuploads, done = g_done, sent = g_fence;
    *(volatile uint32_t *)(pmem + P1) = B;
    printf("%s: copy into level 0 committed as fence %llu, %llu completed when the CPU wrote\n", how, (unsigned long long)sent, (unsigned long long)done);
    snprintf(n, sizeof n, "%s: the upload after the CPU write keeps the copy into level 0", how); CHECK(host_not(t, 0, 256, C) == 0, n);
    snprintf(n, sizeof n, "%s: the CPU write uploaded the texture again", how); CHECK(g_pkg_reuploads > reups, n);
    snprintf(n, sizeof n, "%s: and the copy into level 1", how); CHECK(host_not(t, 1, 128, B) == 0, n);
    pk = tex_get(t->serial);
    snprintf(n, sizeof n, "%s: the texture is bound from level 0", how); CHECK([pk isKindOfClass:[NvPkg class]] && pk->vbase == 0, n);
    snprintf(n, sizeof n, "%s: the copy into level 0 reaches the physical memory mapped behind it", how); CHECK(words_not(pmem + P0, 256 * 256 * 4, C) == 0, n);
    uint32_t got[4] = {0}; NVNtextureView v; h_nvnTextureViewSetDefaults(&v); h_nvnTextureViewSetLevels(&v, 0, 1); NVNcopyRegion r = {0, 0, 0, 2, 2, 1};
    h_nvnTextureReadTexels(&tx, &v, &r, got);
    snprintf(n, sizeof n, "%s: ReadTexels of level 0, from the storage, reads the copy", how); CHECK(got[0] == C && got[3] == C, n);
    h_nvnTextureFinalize(&tx);
}
void crash_install(void);
int main(void) { @autoreleasepool {
    alarm(120); if (setvbuf(stdout, NULL, _IONBF, 0)) abort();
    crash_install();   // the CPU's write into protected physical memory goes through the page-fault hook, as in production
    mtl_init(); mtl_sub_begin();
    int gpu = g_nvn_deswz_gpu;
    CHECK(gpu, "the GPU deswizzle is available");
    uint8_t *pmem = NULL, *cmem = NULL; posix_memalign((void **)&pmem, 16384, 2 << 20); memset(pmem, 0, 2 << 20);
    posix_memalign((void **)&cmem, 16384, 1 << 20); memset(cmem, 0, 1 << 20);
    pool_t phys = {0}, virt = {0}, cpool = {0}; pool_builder_t pb = {.mem = pmem, .size = 2 << 20, .flags = 0x24};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&phys, (NVNmemoryPoolBuilder *)&pb), "physical pool");
    pb = (pool_builder_t){.mem = cmem, .size = 1 << 20, .flags = 0x24};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&cpool, (NVNmemoryPoolBuilder *)&pb), "command and source pool");
    pb = (pool_builder_t){.size = DM_GAME_APP_POOL + (1ull << 32), .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};   // larger than the application pool: sparse from the start
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&virt, (NVNmemoryPoolBuilder *)&pb), "sparse virtual pool");
    // A mapping nowhere near the textures makes the pool sparse; level 1's 4 KB-aligned offsets inside 16 KB pages keep the textures read through the page table
    CHECK(map(&virt, &phys, 0x10000000, 0x1F0000, 0x1000), "a first mapping");
    CHECK(nvn_sparse_pool(&virt), "the pool reads through its page table");
    run(1, &virt, &phys, pmem, &cpool, cmem, 0x100000, 0x000000, 0x040000);
    run(0, &virt, &phys, pmem, &cpool, cmem, 0x200000, 0x080000, 0x0C0000);
    g_nvn_deswz_gpu = gpu;
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&virt); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&cpool); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&phys);
    mtl_sub_commit(); mtl_drain(); free(pmem); free(cmem);
    printf("sparse copies: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
