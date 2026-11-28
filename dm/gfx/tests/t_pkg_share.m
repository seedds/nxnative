// The packaged-texture sharing table (g_pkg_share, pkg_shared, pkg_share_put, mtl_alias_drop_pool): partitioned by pool so a pool Finalize drops only that pool's map in O(1)
// for unrelated pools and frees its memory; dead NvPkgRef entries are swept and capacity resized before a pool's map grows, and a lookup that meets a dead entry drops it.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

static uint8_t mem[4];
static pool_t pools[4] = {{.mem = &mem[0]}, {.mem = &mem[1]}, {.mem = &mem[2]}, {.mem = &mem[3]}};
static tex_t at(int pool, int64_t off) {
    tex_t t = {0};
    t.pool = &pools[pool]; t.off = off;
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 4, .height = 4, .depth = 1, .levels = 1, .samples = 1};
    return t;
}
static void pool_stats(int pool, uint32_t *n, uint32_t *cap) {
    uint64_t k = (uint64_t)(uintptr_t)&pools[pool];
    pthread_mutex_lock(&g_smu);
    const nvn_hmap *pm = nvn_hm_get(&g_pkg_share, &k);
    *n = pm ? pm->n : 0; *cap = pm ? pm->cap : 0;
    pthread_mutex_unlock(&g_smu);
}
static void put_dead(int pool, int first, int count) {   // entries whose package is gone at once
    for (int i = 0; i < count; i++) @autoreleasepool { tex_t t = at(pool, (int64_t)(first + i) * 64); NvPkg *p = [NvPkg new]; pkg_share_put(&t, p); }
}

int main(void) { @autoreleasepool {
    // 1. Dead entries in a permanent pool stay bounded O(live) without any pool Finalize
    NSMutableArray<NvPkg *> *live = [NSMutableArray array];
    for (int i = 0; i < 40; i++) { tex_t t = at(1, (int64_t)i * 64); NvPkg *p = [NvPkg new]; pkg_share_put(&t, p); [live addObject:p]; }
    put_dead(1, 100, 3000);
    uint32_t n1 = 0, cap1 = 0; pool_stats(1, &n1, &cap1);
    CHECK(n1 >= 40 && n1 <= 128 && cap1 == 128, "3,000 dead entries beside 40 live ones stay bounded without any pool Finalize");
    int found = 0; for (int i = 0; i < 40; i++) { tex_t t = at(1, (int64_t)i * 64); found += pkg_shared(&t) == live[i]; }
    CHECK(found == 40, "the 40 live entries are all still found");

    // 2. A temporary live spike grows the pool's map, and once those packages die the next compaction shrinks capacity back down
    {
        NSMutableArray<NvPkg *> *burst = [NSMutableArray array];
        for (int i = 0; i < 1000; i++) { tex_t t = at(1, (int64_t)(10000 + i) * 64); NvPkg *p = [NvPkg new]; pkg_share_put(&t, p); [burst addObject:p]; }
        pool_stats(1, &n1, &cap1);
        CHECK(cap1 >= 2048, "1,040 live entries grow the pool's map");
        [burst removeAllObjects];
    }
    put_dead(1, 20000, 2000);
    pool_stats(1, &n1, &cap1);
    CHECK(cap1 == 128, "compaction shrinks capacity once the spike's packages die");
    found = 0; for (int i = 0; i < 40; i++) { tex_t t = at(1, (int64_t)i * 64); found += pkg_shared(&t) == live[i]; }
    CHECK(found == 40, "the original live entries survive shrink");

    // 3. A lookup that meets a dead entry drops it and reports a miss
    put_dead(1, 9000, 1);
    tex_t d = at(1, 9000 * 64);
    uint32_t before = 0, cap_before = 0; pool_stats(1, &before, &cap_before);
    CHECK(pkg_shared(&d) == nil, "a lookup of a dead entry reports none");
    uint32_t after = 0, cap_after = 0; pool_stats(1, &after, &cap_after);
    CHECK(after == before - 1, "a lookup of a dead entry drops it");

    // 4. Finalizing an unrelated pool is O(1) even beside thousands of live entries in another pool
    NSMutableArray<NvPkg *> *big = [NSMutableArray array];
    for (int i = 0; i < 5000; i++) { tex_t t = at(3, (int64_t)i * 64); NvPkg *p = [NvPkg new]; pkg_share_put(&t, p); [big addObject:p]; }
    uint64_t t0 = bp_now();
    for (int i = 0; i < 1000; i++) mtl_alias_drop_pool(&pools[2]);
    double ms = (bp_now() - t0) / 1e6;
    printf("1,000 unrelated pool Finalizes beside 5,000 live packages: %.2f ms\n", ms);
    CHECK(ms < 40, "unrelated pool Finalizes do not walk other pools' sharing tables");

    // 5. A pool's Finalize removes all its entries, frees its table, drops finalized ghost serials holding its NvPkg, and keeps other pools' entries
    put_dead(0, 0, 10);
    NvPkg *own = [NvPkg new]; tex_t o = at(0, 4096); pkg_share_put(&o, own);
    uint64_t ghost_ser = 777001;
    tex_put(ghost_ser, own);   // finalized serial (not in g_tex_live) resolves through g_ghost
    CHECK(tex_get(ghost_ser) == own, "ghost serial holds the pool's package before Finalize");
    mtl_alias_drop_pool(&pools[0]);
    uint32_t n0 = 0, cap0 = 0; pool_stats(0, &n0, &cap0);
    CHECK(n0 == 0 && cap0 == 0 && pkg_shared(&o) == nil, "a pool's Finalize removes and frees its sharing table");
    CHECK(tex_get(ghost_ser) == nil, "a pool's Finalize drops ghost serial entries naming its live packages");
    tex_t k = at(1, 0);
    CHECK(pkg_shared(&k) == live[0], "other pools' live entries stay");

    mtl_alias_drop_pool(&pools[1]);
    mtl_alias_drop_pool(&pools[3]);
    pthread_mutex_lock(&g_smu); uint32_t leftover = g_pkg_share.n; pthread_mutex_unlock(&g_smu);
    CHECK(leftover == 0, "no pool map is left after finalizing every pool");
    printf("t_pkg_share: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
