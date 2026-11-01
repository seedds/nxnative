// A sparse texture (flag SPARSE) in a sparse virtual pool resolves to a Metal placement sparse texture (sp_make) with the format-view and, for an uncompressed format, the
// render-target usage from the start, so no usage upgrade makes it whole; and the one upgrade left (image writes) drops its tile record and keeps the texture whole from then on,
// instead of leaving the record on a texture that is not sparse (the next sync called updateTextureMappings on it). Needs the Metal 4 host (macOS 26); without placement sparse
// textures the resolve takes the whole-texture path and the checks that need the sparse one are skipped
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
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

int main(void) { @autoreleasepool {
    mtl_init();
    // A 2 MB physical pool and a 4 MB virtual pool (flags 0x2a1) whose first mapping is not the whole pool: sparse, with a page table
    const size_t plen = 2u << 20, vlen = 4u << 20;
    uint8_t *pmem = mmap(NULL, plen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (pmem == MAP_FAILED) abort();
    for (size_t i = 0; i < plen; i++) pmem[i] = (uint8_t)(i * 7 + 3);
    pool_t phys = {0}, virt = {0};
    pool_builder_t ppb = {.mem = pmem, .size = plen, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    pool_builder_t vpb = {.mem = NULL, .size = vlen, .flags = 0x2a1};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&phys, (const NVNmemoryPoolBuilder *)&ppb), "physical pool initialized");
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&virt, (const NVNmemoryPoolBuilder *)&vpb), "virtual pool initialized");
    // A 512x512 RGBA8 sparse texture of 3 levels at offset 0: level 0 is 16 tiles of 128x128 (1 MB), the rest its mip tail. Map level 0's first half (8 tiles) and the tail
    NVNtextureBuilder tb; NVNtexture tex = {0}; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetWidth(&tb, 512); h_nvnTextureBuilderSetHeight(&tb, 512); h_nvnTextureBuilderSetDepth(&tb, 1); h_nvnTextureBuilderSetLevels(&tb, 3); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&virt, 0);
    CHECK(h_nvnTextureInitialize(&tex, &tb), "sparse texture initialized in the virtual pool");
    tex_t *t = (tex_t *)&tex;
    CHECK(t->L.size > (1u << 20) && t->L.size <= (2u << 20), "the texture's storage is level 0's 16 tiles and a tail");
    map_req rq[2] = {{(NVNmemoryPool *)&phys, 0, 0, 0x80000, 0, 0}, {(NVNmemoryPool *)&phys, 0x80000, 0x100000, (int64_t)(t->L.size - 0x100000), 0, 0}};
    CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&virt, 2, (const NVNmappingRequest *)rq), "level 0's first 8 tiles and the mip tail mapped");
    CHECK(nvn_sparse_pool(&virt), "the pool is sparse");
    mtl_tex_init(t);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    id<MTLTexture> mt = tex_mtl(t);
    CHECK(mt != nil, "the sparse texture resolves");
    NvPkg *pk = tex_get(t->serial);
    CHECK([pk isKindOfClass:[NvPkg class]] && pk->sparse, "resolved through the page table (packaged, sparse)");
    if (pk->sp) {
        CHECK(pk->tex.usage & MTLTextureUsageRenderTarget, "an uncompressed placement sparse texture has render-target use from the start");
        CHECK(pk->tex.usage & MTLTextureUsagePixelFormatView, "and format-view use");
        CHECK(pk->sp->nunit > 0 && pk->sp->map[0] && !pk->sp->map[15], "tile 0 is placed, the unmapped tile 15 is not");
        // An upgrade that sp_make cannot give up front (image writes): the texture becomes whole, the tile record goes, and the next resolve after a mapping change stays whole
        id<MTLTexture> up = tex_upgrade(t, pk->tex, MTLTextureUsageShaderWrite, pk->tex.pixelFormat);
        CHECK(up != mt && pk->tex == up, "the upgrade replaced the holder's texture");
        CHECK(pk->sp == nil, "the upgrade dropped the tile record");
        CHECK(!up.isSparse, "the upgraded texture is whole");
        map_req more = {(NVNmemoryPool *)&phys, 0x80000, 0x80000, 0x40000, 0, 0};   // tiles 8..11 mapped now: the next resolve re-uploads through the whole path
        CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&virt, 1, (const NVNmappingRequest *)&more), "four more tiles mapped");
        id<MTLTexture> again = tex_mtl(t);
        CHECK(again != nil && !again.isSparse && pk->sp == nil, "after the upgrade the texture resolves whole, with no tile record");
    } else printf("no placement sparse texture on this host: the upgrade checks are skipped\n");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission completes");
    CHECK(!mtl_last_commit_error(), "no GPU error");
    h_nvnTextureFinalize(&tex); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&virt); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&phys);
    defer_drain(); munmap(pmem, plen);
    printf("t_sparse_upgrade: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
