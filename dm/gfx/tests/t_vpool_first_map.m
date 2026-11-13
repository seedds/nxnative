// A virtual pool's first mapping (nvnMemoryPoolMapVirtual): a whole-pool mapping aliases the pool onto its physical pool only while no address of the pool's own range has left the host.
// Once a texture, buffer, event or address was made on it, the pool stays sparse and those addresses read the physical pages (issue #2513). Sampler IDs past the sampler pool are empty slots.
// Based on PR #2519's t_vpool_alias.
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

enum { POOL_SZ = 128u << 10 };
static pool_t g_phys; static uint8_t *g_phys_mem;

static void vpool_init(pool_t *v) {
    pool_builder_t pb = {.size = POOL_SZ, .flags = 0x2a1};   // VIRTUAL | COMPRESSIBLE | GPU_CACHED | CPU_NO_ACCESS (Ocarina of Time's)
    CHECK("virtual pool initialize", h_nvnMemoryPoolInitialize((NVNmemoryPool *)v, (const NVNmemoryPoolBuilder *)&pb));
    CHECK("virtual pool has a range of its own, mode 0", v->vp && v->mem && v->vmode == 0);
}
static int map_whole(pool_t *v) {
    map_req r = {.phys = (NVNmemoryPool *)&g_phys, .poff = 0, .voff = 0, .size = POOL_SZ};
    return h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)v, 1, (const NVNmappingRequest *)&r);
}

int main(void) { @autoreleasepool {
    mtl_init();
    g_phys_mem = mmap(NULL, POOL_SZ, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK("physical storage", g_phys_mem != MAP_FAILED);
    for (uint32_t i = 0; i < POOL_SZ / 4; i++) ((uint32_t *)g_phys_mem)[i] = 0x9e3779b9u * (i + 1);
    pool_builder_t pb = {.mem = g_phys_mem, .size = POOL_SZ, .flags = 0x18c};   // PHYSICAL | COMPRESSIBLE | GPU_NO_ACCESS | CPU_CACHED
    CHECK("physical pool initialize", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_phys, (const NVNmemoryPoolBuilder *)&pb));

    // Nothing made on the pool before its first mapping: aliased, its storage the physical pool's
    pool_t va; vpool_init(&va);
    CHECK("map untouched pool", map_whole(&va));
    CHECK("untouched pool aliased", va.vmode == 1 && !va.vp && va.mem == g_phys_mem);
    CHECK("aliased pool keeps its own flags", va.flags == 0x2a1 && h_nvnMemoryPoolGetFlags((const NVNmemoryPool *)&va) == 0x2a1);

    // A pool address asked for first: the address stays the pool's, and reads the physical pages once mapped
    pool_t vb; vpool_init(&vb);
    NVNbufferAddress ab = h_nvnMemoryPoolGetBufferAddress((const NVNmemoryPool *)&vb);
    CHECK("address noted", vb.vp && vb.vp->taken);
    CHECK("map addressed pool", map_whole(&vb));
    CHECK("addressed pool sparse", vb.vmode == 2 && vb.vp && (NVNbufferAddress)vb.mem == ab);
    CHECK("its address reads the physical pages", !memcmp((const void *)ab, g_phys_mem, POOL_SZ));

    // A texture made first: its storage pointer stays valid
    pool_t vc; vpool_init(&vc);
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetWidth(&tb, 32); h_nvnTextureBuilderSetHeight(&tb, 32);
    h_nvnTextureBuilderSetLevels(&tb, 1); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&vc, 0x1000);
    NVNtexture tx; CHECK("texture initialize", h_nvnTextureInitialize(&tx, &tb));
    const uint8_t *tmem = ((const tex_t *)&tx)->mem;
    CHECK("texture noted", vc.vp && vc.vp->taken);
    CHECK("map pool with a texture", map_whole(&vc));
    CHECK("pool with a texture sparse", vc.vmode == 2 && vc.mem + 0x1000 == tmem);
    CHECK("texture storage reads the physical pages", !memcmp(tmem, g_phys_mem + 0x1000, 32 * 32 * 4));

    // A buffer made first (BufferInitialize notes it through pool_mark_addressed)
    pool_t vd; vpool_init(&vd);
    NVNbufferBuilder bb; h_nvnBufferBuilderSetDefaults(&bb); h_nvnBufferBuilderSetStorage(&bb, (NVNmemoryPool *)&vd, 0x2000, 256);
    NVNbuffer buf; CHECK("buffer initialize", h_nvnBufferInitialize(&buf, &bb));
    CHECK("buffer noted", vd.vp && vd.vp->taken);
    CHECK("map pool with a buffer", map_whole(&vd));
    CHECK("pool with a buffer sparse", vd.vmode == 2 && (const uint8_t *)h_nvnBufferGetAddress(&buf) == vd.mem + 0x2000);
    CHECK("buffer reads the physical pages", !memcmp((const void *)h_nvnBufferGetAddress(&buf), g_phys_mem + 0x2000, 256));

    // After the first mapping a pool is decided: later objects change nothing
    NVNbufferBuilder bb2; h_nvnBufferBuilderSetDefaults(&bb2); h_nvnBufferBuilderSetStorage(&bb2, (NVNmemoryPool *)&va, 0, 256);
    NVNbuffer buf2; CHECK("buffer on aliased pool", h_nvnBufferInitialize(&buf2, &bb2));
    CHECK("aliased pool stays aliased", va.vmode == 1 && (const uint8_t *)h_nvnBufferGetAddress(&buf2) == g_phys_mem);

    // Sampler IDs past the pool are empty slots; unregistered IDs inside it are still reported
    // Super Mario Party's pool holds 4000: the end is 4000, not the next numbering's 4096
    NVNsamplerPool sp;
    CHECK("sampler pool n=4000", h_nvnSamplerPoolInitialize(&sp, (const NVNmemoryPool *)&g_phys, 0, 4000));
    CHECK("sampler pool end is its size", g_spool_n == 4000);
    os_unfair_lock_lock(&g_rmu);
    id<MTLSamplerState> s_oob = samp_by_id(4095), s_in = samp_by_id(300);
    os_unfair_lock_unlock(&g_rmu);
    CHECK("sampler ID 4095 an empty slot, not red", s_oob == nil && g_stat_samp_oob == 1);
    CHECK("unregistered sampler ID 300 inside the pool red", s_in == nil && g_red_once[12] == 1 && g_stat_samp_oob == 1);

    h_nvnTextureFinalize(&tx);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&vd); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&vc);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&vb); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&va);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&g_phys); munmap(g_phys_mem, POOL_SZ);
    if (failed) { fprintf(stderr, "t_vpool_first_map: %d/%d failed\n", failed, checks); return 1; }
    printf("t_vpool_first_map: ok (%d checks)\n", checks);
    return 0;
}}
