// The raw pool table (gmem_map) and storage the CPU cannot reach: a block-linear texture there keeps its content in a Metal texture of its own, so its address
// (nvnTextureGetTextureAddress) and a virtual pool's mapping over a physical pool put nothing into the table, and nothing wires that pool for the table's sake. A buffer, the pool's own
// address (before or after the mapping), a linear texture's address and a texture address in CPU-visible storage still put the pool in (docs/reference/memory.md)
#define nvn_game_fps nvn_game_fps_mtl_default
#define nvn_fstep_fine nvn_fstep_fine_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#undef nvn_fstep_fine
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
static int pool_resident(uint64_t a) { pthread_mutex_lock(&g_mu); int i = pool_find(a, 1), r = i >= 0 && g_pools[i].res; pthread_mutex_unlock(&g_mu); return r; }
static int in_table(uint64_t b) {
    id<MTLBuffer> t = gmem_map(); const uint8_t *e = (const uint8_t *)t.contents;
    for (int k = 1; k < DMSH_GMEM_MAP_ENTRIES; k++) { uint64_t r[2]; memcpy(r, e + (size_t)k * DMSH_GMEM_MAP_ENTRY, sizeof r); if (r[0] <= b && b < r[1]) return 1; }
    return 0;
}
enum { LEN = 4u << 20, PHYS = 0x18c, VIRT_GPU = 0x2a1, CPU_POOL = 0x24, CPU_NONE = 0xa1 };
static uint8_t *pages(void) { uint8_t *m = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (m == MAP_FAILED) abort(); return m; }
static void pool_init(pool_t *p, uint8_t *mem, uint32_t flags) { pool_builder_t pb = {.mem = mem, .size = LEN, .flags = flags}; if (!h_nvnMemoryPoolInitialize((NVNmemoryPool *)p, (const NVNmemoryPoolBuilder *)&pb)) abort(); }
static void map_whole(pool_t *v, pool_t *phys) { map_req m = {.phys = (NVNmemoryPool *)phys, .size = LEN}; if (!h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)v, 1, (const NVNmappingRequest *)&m)) abort(); }
static uint64_t tex_addr(pool_t *pool, int64_t off, uint32_t tflags) {
    tex_t t; memset(&t, 0, sizeof t); t.magic = MAGIC('T', 'E', 'X', 0); t.pool = pool; t.off = off; t.mem = pool->mem + off;
    t.d.target = NVN_TEXTURE_TARGET_2D; t.d.width = t.d.height = 256; t.d.flags = tflags; t.L.size = 256 * 256 * 4;
    return (uint64_t)h_nvnTextureGetTextureAddress((const NVNtexture *)&t);
}
int main(void) { @autoreleasepool {
    mtl_init();
    {   // render targets in a GPU-only virtual pool over a physical pool, as Ocarina of Time keeps them
        uint8_t *m = pages(); pool_t phys, virt; pool_init(&phys, m, PHYS); pool_init(&virt, NULL, VIRT_GPU); map_whole(&virt, &phys);
        CHECK("the virtual pool aliases the physical pool's pages", virt.mem == m);
        uint64_t a = tex_addr(&virt, 65536, 0);
        CHECK("the texture address is pool address + offset", a == (uint64_t)(uintptr_t)m + 65536);
        CHECK("neither the mapping nor a block-linear target's address puts the physical pool in the raw pool table", !in_table((uint64_t)(uintptr_t)m + 65536));
        CHECK("so nothing makes it resident", !pool_resident((uint64_t)(uintptr_t)m));
        tex_addr(&virt, 131072, NVN_TEXTURE_FLAGS_LINEAR);
        CHECK("a linear texture's address does: its bytes are its content", in_table((uint64_t)(uintptr_t)m + 65536) && pool_resident((uint64_t)(uintptr_t)m));
        h_nvnMemoryPoolFinalize((NVNmemoryPool *)&virt); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&phys);
    }
    {   // a buffer in the GPU-only virtual pool
        uint8_t *m = pages(); pool_t phys, virt; pool_init(&phys, m, PHYS); pool_init(&virt, NULL, VIRT_GPU); map_whole(&virt, &phys);
        CHECK("mapped alone, the pool stays out", !in_table((uint64_t)(uintptr_t)m + 4096));
        buf_builder_t bb = {.pool = &virt, .off = 8192, .size = 4096}; NVNbuffer buf;
        CHECK("a buffer initializes in it", h_nvnBufferInitialize(&buf, (const NVNbufferBuilder *)&bb));
        CHECK("a buffer puts the pool in the table", in_table((uint64_t)(uintptr_t)m + 4096) && pool_resident((uint64_t)(uintptr_t)m));
        h_nvnMemoryPoolFinalize((NVNmemoryPool *)&virt); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&phys);
    }
    {   // the virtual pool's own address asked before its first mapping: the mapping must keep it reachable
        uint8_t *m = pages(); pool_t phys, virt; pool_init(&phys, m, PHYS); pool_init(&virt, NULL, VIRT_GPU);
        h_nvnMemoryPoolGetBufferAddress((const NVNmemoryPool *)&virt);
        map_whole(&virt, &phys);
        CHECK("an address given out before the mapping puts the physical pool in", in_table((uint64_t)(uintptr_t)m + 4096));
        h_nvnMemoryPoolFinalize((NVNmemoryPool *)&virt); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&phys);
    }
    {   // plain pools
        uint8_t *a = pages(), *b = pages(); pool_t cpu, none; pool_init(&cpu, a, CPU_POOL); pool_init(&none, b, CPU_NONE);
        tex_addr(&cpu, 4096, 0);
        CHECK("a texture address in CPU-visible storage puts its pool in, as before", in_table((uint64_t)(uintptr_t)a + 4096));
        tex_addr(&none, 4096, 0);
        CHECK("a block-linear texture's address in a CPU_NO_ACCESS pool does not", !in_table((uint64_t)(uintptr_t)b + 4096) && !pool_resident((uint64_t)(uintptr_t)b));
        h_nvnMemoryPoolGetBufferAddress((const NVNmemoryPool *)&none);
        CHECK("the pool's own address does", in_table((uint64_t)(uintptr_t)b + 4096));
        h_nvnMemoryPoolFinalize((NVNmemoryPool *)&cpu); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&none);
    }
    printf("t_gpu_only_addr: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
