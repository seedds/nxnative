// A lookup redirected to a sparse run's own entry (sparse_redirect → run_lookup) when that run is 512 MB or more. Such a run is a large pool (mtl_pool_add, BIG_MIN):
// no entry over the run, only 16 MB stride chunks made on demand, so pool_run found no entry for a range crossing a stride or in a stride not looked up yet, and the lookup fell back to
// pool_find with "[RED] sparse pool run ...: no pool entry of its own holds ...; looked up as before" (PRAGMATA hg0: a 16 MB SBUFU span at +0xb60000 into the 523 MB run 0x381f8c000).
// Checks that run_lookup itself resolves such ranges to a buffer that holds them and whose CPU view is the guest address, with no [RED] line (g_run_red).
// CPU only: no command buffer is committed. Run: ./t_sparse_run_big
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
static void map_many(uint64_t voff, uint64_t n, const uint64_t *poffs, uint64_t piece) {
    map_req *r = calloc(n, sizeof *r);
    for (uint64_t i = 0; i < n; i++) r[i] = (map_req){.phys = (NVNmemoryPool *)&g_phys, .poff = (int64_t)poffs[i], .voff = (int64_t)(voff + i * piece), .size = piece};
    CHECK("mapping requests", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, (int)n, (const NVNmappingRequest *)r));
    free(r);
}
static uint8_t *g_base, *pm;
static void one(const char *name, uint64_t voff, uint64_t len, uint64_t want_run) {
    uint64_t a = (uint64_t)(uintptr_t)g_base + voff, p = 0, rk = 0, vr = nvn_sparse_vrun(a, len, &p, &rk);
    char lab[200];
    snprintf(lab, sizeof lab, "%s: one virtual run of several physical pieces, its key", name); CHECK(lab, vr == len && !p && rk == (uint64_t)(uintptr_t)g_base + want_run);
    void *mb = NULL; uint64_t mo = 0, gpu = 0; int red0 = g_run_red;
    int ok = rk && run_lookup(rk, a, len, &mb, &mo, &gpu);
    uint64_t have = ok ? (uint64_t)[(__bridge id<MTLBuffer>)mb length] - mo : 0;
    printf("  %-48s run lookup %s (buffer holds %#llx of %#llx)\n", name, ok ? "ok" : "FAILED", (unsigned long long)have, (unsigned long long)len);
    snprintf(lab, sizeof lab, "%s: the run's own lookup resolves it", name); CHECK(lab, ok);
    snprintf(lab, sizeof lab, "%s: no [RED] line", name); CHECK(lab, g_run_red == red0);
    if (!ok) return;
    snprintf(lab, sizeof lab, "%s: the buffer holds the range", name); CHECK(lab, have >= len);
    snprintf(lab, sizeof lab, "%s: CPU view is the guest address", name); CHECK(lab, (const uint8_t *)[(__bridge id<MTLBuffer>)mb contents] + mo == (const uint8_t *)(uintptr_t)a);
    snprintf(lab, sizeof lab, "%s: GPU address matches the buffer", name); CHECK(lab, gpu == [(__bridge id<MTLBuffer>)mb gpuAddress] + mo);
    // and the full lookup (sparse_redirect → run_lookup, else pool_find) agrees with it
    void *mb2 = NULL; uint64_t mo2 = 0, gpu2 = 0; int ok2 = mtl_gpu_addr_(a, len, &mb2, &mo2, &gpu2);
    snprintf(lab, sizeof lab, "%s: mtl_gpu_addr_ holds the range at the guest address", name);
    CHECK(lab, ok2 && (uint64_t)[(__bridge id<MTLBuffer>)mb2 length] - mo2 >= len && (const uint8_t *)[(__bridge id<MTLBuffer>)mb2 contents] + mo2 == (const uint8_t *)(uintptr_t)a);
}
int main(void) { @autoreleasepool {
    mtl_init();
    size_t plen = 640u << 20;   // unique physical pages inside every run (Metal refuses a zero-copy buffer that aliases its own pages)
    pm = mmap(NULL, plen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (pm == MAP_FAILED) abort();
    for (size_t i = 0; i < plen / 4; i++) ((uint32_t *)pm)[i] = (uint32_t)i * 2654435761u | 1u;
    pool_builder_t pb = {.mem = pm, .size = plen, .flags = 0x24};
    CHECK("physical pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_phys, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.size = 2ull << 30, .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    CHECK("sparse pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_sparse, (const NVNmemoryPoolBuilder *)&pb));
    enum { Q = 1u << 20 };   // 1 MB pieces from scattered physical offsets: the run is one virtual run in many physical pieces, so lookups go to its own entry (sparse_redirect)
    uint64_t nq = (64u << 20) / Q, nqq = plen / Q, *perm = malloc(nqq * sizeof *perm);
    for (uint64_t i = 0; i < nqq; i++) perm[i] = i * Q;
    for (uint64_t i = nqq - 1; i > 0; i--) { uint64_t j = (i * 40503u + 7) % (i + 1), x = perm[i]; perm[i] = perm[j]; perm[j] = x; }
    map_req first = {.phys = (NVNmemoryPool *)&g_phys, .poff = 0, .voff = 0, .size = 0x10000};
    CHECK("first mapping", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, 1, (const NVNmappingRequest *)&first));
    const struct vpool *v = vpool_of(&g_sparse); if (!v) { printf("FAIL: no sparse pool\n"); return 1; }
    g_base = v->base;
    uint64_t vc = 256ull << 20;   // a 576 MB run (BIG_MIN 512 MB: a large pool, kept in 16 MB chunks), then a 64 MB run that is a plain pool
    for (uint64_t c = 0; c < 9; c++) map_many(vc + c * (64u << 20), nq, perm + c * nq, Q);
    int big = -1; for (int k = 0; k < g_nbig; k++) if (g_big[k].mem == g_base + vc) big = k;
    CHECK("the run is a large pool", big >= 0);
    one("A crossing a 16 MB stride (hg0's shape)", vc + 0xb60000, 0x1000000, vc);
    one("B inside a stride not looked up yet", vc + (300ull << 20) + 0x10000, 2u << 20, vc);
    one("C the same range again (its chunk exists)", vc + (300ull << 20) + 0x10000, 2u << 20, vc);
    one("D crossing two strides, 40 MB", vc + (100ull << 20) - 0x3000, 40u << 20, vc);
    one("E at the run's end", vc + (576ull << 20) - (3u << 20), 3u << 20, vc);
    uint64_t vs = 1200ull << 20; map_many(vs, nq, perm + 9 * nq, Q);   // not a large pool: the run's own entry, as before
    one("F a plain run (64 MB)", vs + 0x30000, 32u << 20, vs);
    printf("%s: %d checks, %d failed ([RED] sparse pool run lines: %d)\n", failed ? "FAIL" : "ok", checks, failed, g_run_red);
    return failed != 0;
} }
