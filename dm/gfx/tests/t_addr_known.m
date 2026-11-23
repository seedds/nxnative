// mtl_addr_known answers whether a range lies in one memory pool as mtl_addr finds it, with no side effect: hle_nvn.c keys_hash asks it whether a shader's code is mapped, and
// through mtl_addr every pool holding shader code went into the residency set though the GPU never reads it (docs/reference/memory.md). A large pool (BIG_MIN or more)
// is answered from its range: its lookup would make a chunk buffer over the range
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
static int pool_resident(uint64_t a) { pthread_mutex_lock(&g_mu); int i = pool_find(a, 1), r = i >= 0 && g_pools[i].res; pthread_mutex_unlock(&g_mu); return r; }
int main(void) { @autoreleasepool {
    mtl_init();
    const size_t n = 4u << 20;
    uint8_t *mem = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (mem == MAP_FAILED) abort();
    uint64_t base = (uint64_t)(uintptr_t)mem;
    mtl_pool_add(mem, n);
    int res0 = g_pool_res_n;
    CHECK("a range inside the pool is known", mtl_addr_known(base + 4096, 2048));
    CHECK("the pool's last bytes are known", mtl_addr_known(base + n - 16, 16));
    CHECK("a range past the pool's end is not", !mtl_addr_known(base + n - 16, 32));
    CHECK("an address outside every pool is not", !mtl_addr_known(base + n + 4096, 16));
    CHECK("asking makes no pool resident", g_pool_res_n == res0 && !pool_resident(base));
    void *mb = NULL; uint64_t mo = 0;
    CHECK("mtl_addr finds the same range", mtl_addr(base + 4096, 2048, &mb, &mo) && mo == 4096);
    CHECK("mtl_addr makes the pool resident (what keys_hash did)", g_pool_res_n == res0 + 1 && pool_resident(base));
    mtl_pool_del(mem);
    CHECK("a removed pool holds nothing", !mtl_addr_known(base + 4096, 2048));

    // A large pool: anonymous pages, untouched, so its size costs nothing
    const size_t nb = (size_t)BIG_MIN + (32u << 20);
    uint8_t *big = mmap(NULL, nb, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (big == MAP_FAILED) abort();
    uint64_t bb = (uint64_t)(uintptr_t)big;
    mtl_pool_add(big, nb);
    int np0 = g_npool, rb0 = g_pool_res_n;
    CHECK("the large pool is in chunks", big_of(bb) >= 0);
    CHECK("a range in the large pool is known", mtl_addr_known(bb + (100u << 20), 4096));
    CHECK("the large pool's last bytes are known", mtl_addr_known(bb + nb - 16, 16));
    CHECK("a range past the large pool's end is not", !mtl_addr_known(bb + nb - 16, 32));
    CHECK("asking makes no chunk and nothing resident", g_npool == np0 && g_pool_res_n == rb0);
    CHECK("mtl_addr makes a chunk for the same range", mtl_addr(bb + (100u << 20), 4096, &mb, &mo) && g_npool == np0 + 1);
    mtl_pool_del(big);

    // The raw pool table (gmem_map) and shader-code pools: nvnMemoryPoolGetBufferAddress on a non-virtual SHADER_CODE pool keeps it out of the table, so nothing makes it
    // resident and wires its pages for the table's sake; a buffer made in it (nvnBufferInitialize) puts it in, as does any other pool's own address
    PFNNVNDEVICEGETPROCADDRESSPROC get = (PFNNVNDEVICEGETPROCADDRESSPROC)h_nvnBootstrapLoader("nvnDeviceGetProcAddress");
    PFNNVNMEMORYPOOLBUILDERSETDEFAULTSPROC pb_def = (PFNNVNMEMORYPOOLBUILDERSETDEFAULTSPROC)get(NULL, "nvnMemoryPoolBuilderSetDefaults");
    PFNNVNMEMORYPOOLBUILDERSETSTORAGEPROC pb_sto = (PFNNVNMEMORYPOOLBUILDERSETSTORAGEPROC)get(NULL, "nvnMemoryPoolBuilderSetStorage");
    PFNNVNMEMORYPOOLBUILDERSETFLAGSPROC pb_flg = (PFNNVNMEMORYPOOLBUILDERSETFLAGSPROC)get(NULL, "nvnMemoryPoolBuilderSetFlags");
    PFNNVNMEMORYPOOLINITIALIZEPROC pool_init = (PFNNVNMEMORYPOOLINITIALIZEPROC)get(NULL, "nvnMemoryPoolInitialize");
    PFNNVNMEMORYPOOLFINALIZEPROC pool_fin = (PFNNVNMEMORYPOOLFINALIZEPROC)get(NULL, "nvnMemoryPoolFinalize");
    PFNNVNMEMORYPOOLGETBUFFERADDRESSPROC pool_addr = (PFNNVNMEMORYPOOLGETBUFFERADDRESSPROC)get(NULL, "nvnMemoryPoolGetBufferAddress");
    PFNNVNBUFFERBUILDERSETDEFAULTSPROC bb_def = (PFNNVNBUFFERBUILDERSETDEFAULTSPROC)get(NULL, "nvnBufferBuilderSetDefaults");
    PFNNVNBUFFERBUILDERSETSTORAGEPROC bb_sto = (PFNNVNBUFFERBUILDERSETSTORAGEPROC)get(NULL, "nvnBufferBuilderSetStorage");
    PFNNVNBUFFERINITIALIZEPROC buf_init = (PFNNVNBUFFERINITIALIZEPROC)get(NULL, "nvnBufferInitialize");
    PFNNVNBUFFERFINALIZEPROC buf_fin = (PFNNVNBUFFERFINALIZEPROC)get(NULL, "nvnBufferFinalize");
    uint8_t *code = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0), *data = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (code == MAP_FAILED || data == MAP_FAILED) abort();
    uint64_t cb = (uint64_t)(uintptr_t)code, db = (uint64_t)(uintptr_t)data;
    NVNmemoryPoolBuilder pb; NVNmemoryPool cp, dp;
    pb_def(&pb); pb_sto(&pb, code, n);
    pb_flg(&pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED | NVN_MEMORY_POOL_FLAGS_SHADER_CODE);
    CHECK("shader-code pool initializes", pool_init(&cp, &pb));
    pb_sto(&pb, data, n);
    pb_flg(&pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    CHECK("data pool initializes", pool_init(&dp, &pb));
    CHECK("shader-code pool returns its own address", pool_addr(&cp) == cb);
    CHECK("data pool returns its own address", pool_addr(&dp) == db);
    #define IN_TABLE(b) ({ id<MTLBuffer> t_ = gmem_map(); const uint8_t *e_ = (const uint8_t *)t_.contents; int in_ = 0; \
        for (int k = 1; k < DMSH_GMEM_MAP_ENTRIES && !in_; k++) { uint64_t r_[2]; memcpy(r_, e_ + (size_t)k * DMSH_GMEM_MAP_ENTRY, sizeof r_); in_ = r_[0] <= (b) && (b) < r_[1]; } in_; })
    CHECK("a shader-code pool's own address keeps it out of the raw pool table", !IN_TABLE(cb + 4096));
    CHECK("so the table makes it not resident", !pool_resident(cb));
    CHECK("another pool's address puts it in the table", IN_TABLE(db + 4096));
    CHECK("and makes it resident", pool_resident(db));
    NVNbufferBuilder bbld; NVNbuffer cbuf;
    bb_def(&bbld); bb_sto(&bbld, &cp, 8192, 4096);
    CHECK("buffer in shader-code pool initializes", buf_init(&cbuf, &bbld));
    CHECK("a buffer in the shader-code pool puts it in the table", IN_TABLE(cb + 4096));
    CHECK("and makes it resident", pool_resident(cb));
    buf_fin(&cbuf); pool_fin(&cp); pool_fin(&dp);
    munmap(code, n); munmap(data, n);
    printf("t_addr_known: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
