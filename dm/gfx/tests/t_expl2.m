// expl2 (nvn_expl2.inc, 2026-11-07): DM_NVN_VA_AUDIT (hle_nvn.c nvn_sparse_va_audit: pieces mapped out of order, a buffer made before a remap, a hole read through a physical
// pool), DM_NVN_REMAP_REFRESH (a remap of mapped pages is noted once, bumps the pool / registration generations, the resolved bindings over it are marked and counted; a fresh
// mapping is not a remap) and DM_NVN_ALIAS_FENCE's conflict rules (write then access / access then write through a different buffer; same buffer and read-read are not; a conflict
// between encoders waits at the next encoder's begin). CPU only: no command buffer is committed (mtl_finish with nothing submitted returns at once); the encoders are plain objects.
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
static int map_at(uint64_t voff, uint64_t size, int64_t poff) {   // poff < 0: unmap
    map_req r = {.phys = poff < 0 ? NULL : (NVNmemoryPool *)&g_phys, .poff = poff < 0 ? 0 : poff, .voff = (int64_t)voff, .size = size};
    return h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, 1, (const NVNmappingRequest *)&r);
}
int main(void) { @autoreleasepool {
    setenv("DM_NVN_VA_AUDIT", "1", 1); setenv("DM_NVN_REMAP_REFRESH", "1", 1); setenv("DM_NVN_ALIAS_FENCE", "1", 1);
    mtl_init();
    mtl_pool_addressed((const void *)(uintptr_t)0x1000, 1ull << 47);
    CHECK("switches on", x2_any() && va_audit_on() && rr_on() && af_on() && mtl_remap_wanted());
    size_t plen = 16u << 20;
    uint8_t *phys = mmap(NULL, plen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (phys == MAP_FAILED) abort();
    for (size_t i = 0; i < plen / 4; i++) ((uint32_t *)phys)[i] = (uint32_t)i * 2654435761u;
    pool_builder_t pb = {.mem = phys, .size = plen, .flags = 0x24};
    CHECK("physical pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_phys, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.size = 1ull << 30, .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    CHECK("sparse pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_sparse, (const NVNmemoryPoolBuilder *)&pb));
    CHECK("first mapping", map_at(0, 1u << 20, 0));
    const struct vpool *v = vpool_of(&g_sparse); CHECK("pool is sparse", v != NULL);
    if (!v) { fprintf(stderr, "%d checks, %d failed\n", checks, failed); return 1; }
    uint8_t *base = v->base; const uint64_t V = 8u << 20, P = 0x10000;
    uint64_t remaps0 = atomic_load(&g_rr_remaps);
    // 1. VA_AUDIT: three 64 KB pieces whose physical places DESCEND (citron-neo #449's 4 MB chunks), as fresh mappings (not remaps)
    uint64_t gen0 = nvn_sparse_gen();
    CHECK("map piece 0", map_at(V, P, 3u << 20)); CHECK("map piece 1", map_at(V + P, P, 2u << 20)); CHECK("map piece 2", map_at(V + 2 * P, P, 1u << 20));
    CHECK("fresh mappings are not remaps", atomic_load(&g_rr_remaps) == remaps0);
    uint64_t a = (uint64_t)(uintptr_t)(base + V); nvn_va_audit_t r;
    int bad = nvn_sparse_va_audit(a, 3 * P, (uint64_t)(uintptr_t)(phys + (3u << 20)), UINT64_MAX, &r);   // the first piece's physical pool buffer, read as if contiguous
    fprintf(stderr, "order bad=%d\n", bad); CHECK("order: bad pages", bad == 32); CHECK("order: kind", r.kind == 'o'); CHECK("order: first break at the second piece", r.first == P);
    CHECK("order: expected host", r.expect == (uint64_t)(uintptr_t)(phys + (2u << 20))); CHECK("order: binding reads", r.got == (uint64_t)(uintptr_t)(phys + (3u << 20) + P));
    CHECK("order: 3 pieces, 2 descending", r.pieces == 3 && r.desc == 2 && r.holes == 0);
    bad = nvn_sparse_va_audit(a + 0x100, P - 0x200, (uint64_t)(uintptr_t)(phys + (3u << 20) + 0x100), UINT64_MAX, &r);
    CHECK("one piece through its physical buffer is fine", bad == 0 && r.pieces == 1);
    bad = nvn_sparse_va_audit(a, 3 * P, a, nvn_sparse_gen(), &r);
    CHECK("a buffer over the virtual range made now is fine", bad == 0 && r.pieces == 3 && r.desc == 2);
    bad = nvn_sparse_va_audit(a, 3 * P, a, gen0, &r);
    fprintf(stderr, "stale bad=%d kind=%c first=%llx\n", bad, r.kind, (unsigned long long)r.first); CHECK("a buffer made before the pages were mapped is stale", bad == 48 && r.kind == 's' && r.first == 0);
    bad = nvn_sparse_va_audit(a, 3 * P, a, UINT64_MAX, &r);
    CHECK("generation not known: no stale check", bad == 0);
    CHECK("map piece 4 after a hole", map_at(V + 4 * P, P, 4u << 20));
    bad = nvn_sparse_va_audit(a + 2 * P, 3 * P, (uint64_t)(uintptr_t)(phys + (1u << 20)), UINT64_MAX, &r);
    CHECK("a hole read through a physical buffer", bad > 0 && r.kind == 'h' && r.first == P && r.holes == 16);
    CHECK("outside sparse pools", nvn_sparse_va_audit((uint64_t)(uintptr_t)phys, 4096, (uint64_t)(uintptr_t)phys, UINT64_MAX, &r) == -1);
    // va_audit itself (the log path) on a resolved binding: no crash, counted
    g_sub_thread = pthread_self(); atomic_store(&g_sub_thread_set, 1);
    void *mb = NULL; uint64_t mo = 0, gpu = 0;
    t_lk_what = "test"; int ok = mtl_gpu_addr(a, 3 * P, &mb, &mo, &gpu);
    CHECK("lookup of the pieces", ok && mb);
    CHECK("audited", g_va_seen >= 1 && g_va_sparse >= 1);
    fprintf(stderr, "va-audit: seen %llu walked %llu sparse %llu multi %llu desc %llu bad o/s/h %llu/%llu/%llu\n", (unsigned long long)g_va_seen, (unsigned long long)g_va_walked, (unsigned long long)g_va_sparse,
            (unsigned long long)g_va_multi, (unsigned long long)g_va_desc, (unsigned long long)g_va_bad[0], (unsigned long long)g_va_bad[1], (unsigned long long)g_va_bad[2]);
    // 2. REMAP_REFRESH: remap piece 1 (mapped) to other pages: noted once, generations bumped, the binding over it is covered; mapping the hole is not a remap
    uint64_t pg0 = atomic_load(&g_pool_gen), rg0 = atomic_load(&g_reg_gen), seq0 = atomic_load(&g_rr_seq);
    CHECK("remap piece 1", map_at(V + P, P, 6u << 20));
    CHECK("noted as a remap", atomic_load(&g_rr_remaps) == remaps0 + 1 && atomic_load(&g_rr_pages) >= 16 && atomic_load(&g_rr_seq) == seq0 + 1);
    CHECK("pool generation bumped", atomic_load(&g_pool_gen) > pg0); CHECK("registration generation bumped", atomic_load(&g_reg_gen) > rg0);
    uint64_t cov0 = g_rr_covered, inf0 = g_rr_inflight;
    ok = mtl_gpu_addr(a, 3 * P, &mb, &mo, &gpu);   // the next lookup drains the note, then records the new resolution
    CHECK("lookup again", ok);
    CHECK("the binding over the remapped page was covered", g_rr_covered == cov0 + 1);
    CHECK("it was resolved in this (open) submission: in flight", g_rr_inflight == inf0 + 1);
    CHECK("re-resolved (changed or same) counted", g_rr_changed + g_rr_same == 1);
    fprintf(stderr, "remap-refresh: covered %llu changed %llu same %llu inflight %llu\n", (unsigned long long)g_rr_covered, (unsigned long long)g_rr_changed, (unsigned long long)g_rr_same, (unsigned long long)g_rr_inflight);
    uint64_t rm1 = atomic_load(&g_rr_remaps);
    CHECK("map the hole", map_at(V + 3 * P, P, 7u << 20));
    CHECK("mapping a hole is not a remap", atomic_load(&g_rr_remaps) == rm1);
    CHECK("same pages again", map_at(V + 3 * P, P, 7u << 20));
    CHECK("mapping the same pages again is not a remap", atomic_load(&g_rr_remaps) == rm1);
    uint64_t um0 = atomic_load(&g_rr_unmaps);
    CHECK("unmap piece 4", map_at(V + 4 * P, P, -1));
    CHECK("an unmap of mapped pages is noted", atomic_load(&g_rr_unmaps) == um0 + 1);
    // 3. ALIAS_FENCE rules (plain objects as encoders: no fence is encoded, the conflicts are counted)
    NSObject *e1 = [NSObject new], *e2 = [NSObject new], *e3 = [NSObject new], *e4 = [NSObject new], *e5 = [NSObject new];
    const void *A = (const void *)0xA000, *B = (const void *)0xB000, *C = (const void *)0xC000, *D = (const void *)0xD000, *E = (const void *)0xE000;
    uint64_t X = 0x500000000ull, Y = 0x600000000ull;
    af_reset();
    af_begin(e1, 'C'); af_access(X, P, A); af_write(X, P); af_end(e1);
    CHECK("one encoder in the history", g_af_nhist == 1 && g_af_hist[0].n == 1 && g_af_hist[0].a[0].w == 1 && g_af_hist[0].fence >= 0);
    af_begin(e2, 'R'); af_access(X + 0x1000, 0x1000, B);
    CHECK("write then access through another buffer: a conflict", g_af_conf_raw == 1);
    af_access(X + 0x2000, 0x1000, A); CHECK("the same buffer: none", g_af_conf_raw == 1 && g_af_conf_war == 0);
    af_access(Y, 0x1000, B); CHECK("another range: none", g_af_conf_raw == 1);
    af_access(X + 0x3000, 0x1000, C); CHECK("the same producer is waited for once", g_af_conf_raw == 1 || g_af_conf_raw == 2);
    uint64_t raw2 = g_af_conf_raw;
    af_end(e2);
    af_begin(e3, 'C'); af_access(Y, 0x1000, C); CHECK("read then read through another buffer: none", g_af_conf_raw == raw2 && g_af_conf_war == 0);
    af_end(e3);
    af_begin(e4, 'C'); af_access(Y, 0x1000, D); af_write(Y, 0x1000);
    CHECK("access then write through another buffer: a conflict", g_af_conf_war >= 1);
    af_end(e4);
    CHECK("between encoders: nothing pending", g_af_npend == 0 && g_af_cur == nil);
    af_access(Y, 0x1000, E); af_write(Y, 0x1000);
    CHECK("a conflict found between encoders waits for the next one", g_af_npend >= 1);
    af_begin(e5, 'R'); CHECK("applied at its begin", g_af_npend == 0);
    af_end(e5);
    af_reset(); CHECK("reset per submission", g_af_nhist == 0);
    fprintf(stderr, "alias-fence: raw %llu war %llu waits %llu at-begin %llu updates %llu\n", (unsigned long long)g_af_conf_raw, (unsigned long long)g_af_conf_war, (unsigned long long)g_af_waits, (unsigned long long)g_af_waits_begin, (unsigned long long)g_af_updates);
    atomic_store(&g_sub_thread_set, 0);
    fprintf(stderr, "%d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
