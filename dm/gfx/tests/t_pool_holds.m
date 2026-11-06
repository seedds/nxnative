// mtl_pool_holds answers whether a range lies in one memory pool as mtl_addr finds it, without making the pool resident: hle_nvn.c keys_hash asks it whether a shader's code is
// mapped, and through mtl_addr every pool holding shader code went into the residency set though the GPU never reads it (Ocarina of Time: 222 pools, 334 MB)
#include "../nvn/nvn_mtl.m"
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
    CHECK("a range inside the pool is held", mtl_pool_holds(base + 4096, 2048));
    CHECK("the pool's last bytes are held", mtl_pool_holds(base + n - 16, 16));
    CHECK("a range past the pool's end is not", !mtl_pool_holds(base + n - 16, 32));
    CHECK("an address outside every pool is not", !mtl_pool_holds(base + n + 4096, 16));
    CHECK("asking makes no pool resident", g_pool_res_n == res0 && !pool_resident(base));
    void *mb = NULL; uint64_t mo = 0;
    CHECK("mtl_addr finds the same range", mtl_addr(base + 4096, 2048, &mb, &mo) && mo == 4096);
    CHECK("mtl_addr makes the pool resident (what keys_hash did)", g_pool_res_n == res0 + 1 && pool_resident(base));
    mtl_pool_del(mem);
    CHECK("a removed pool holds nothing", !mtl_pool_holds(base + 4096, 2048));
    printf("t_pool_holds: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
