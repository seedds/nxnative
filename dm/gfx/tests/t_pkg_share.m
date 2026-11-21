// The packaged-texture sharing table (g_pkg_share, pkg_shared, pkg_share_put): an entry's package is weak, so the entry outlives it, and only a Finalize of the entry's own pool took it
// out. A pool that lives on while its textures come and go at other offsets kept every dead entry (Ocarina of Time: 357,000 after 200 loads from the pause menu, 99 % dead), and every
// pool Finalize walks the table under g_smu. A Finalize now also sweeps the dead entries once the table has doubled since its last sweep, and a lookup that meets one drops it.
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
static tex_t at(int pool, int64_t off) { tex_t t = {0}; t.pool = &pools[pool]; t.off = off; t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 4, .height = 4, .depth = 1, .levels = 1, .samples = 1}; return t; }
static uint32_t entries(void) { pthread_mutex_lock(&g_smu); uint32_t n = g_pkg_share.n; pthread_mutex_unlock(&g_smu); return n; }
static void put_dead(int pool, int first, int count) {   // entries whose package is gone at once
    for (int i = 0; i < count; i++) @autoreleasepool { tex_t t = at(pool, (int64_t)(first + i) * 64); NvPkg *p = [NvPkg new]; pkg_share_put(&t, p); }
}

int main(void) { @autoreleasepool {
    mtl_init(); mtl_sub_begin();
    // 1. Pool 1 keeps 40 packages alive and has 3,000 dead entries beside them; finalizing the unrelated pool 3 sweeps the dead ones, once the table is past 1,024
    NSMutableArray<NvPkg *> *live = [NSMutableArray array];
    for (int i = 0; i < 40; i++) { tex_t t = at(1, (int64_t)i * 64); NvPkg *p = [NvPkg new]; pkg_share_put(&t, p); [live addObject:p]; }
    put_dead(1, 100, 3000);
    CHECK(entries() == 3040, "3,040 entries: 40 alive and 3,000 dead");
    tex_t other = at(3, 0); NvPkg *op = [NvPkg new]; pkg_share_put(&other, op);
    mtl_alias_drop_pool(&pools[2]);   // a pool with no entry
    CHECK(entries() == 41, "a Finalize sweeps the dead entries of other pools");
    int found = 0; for (int i = 0; i < 40; i++) { tex_t t = at(1, (int64_t)i * 64); found += pkg_shared(&t) == live[i]; }
    CHECK(found == 40, "the live entries are all still found");
    // 2. A sweep is not made at each Finalize: 500 dead entries are under the doubling bound and stay; 700 more pass it
    put_dead(1, 5000, 500);
    mtl_alias_drop_pool(&pools[2]);
    CHECK(entries() == 541, "500 dead entries stay at the next Finalize");
    put_dead(1, 6000, 700);
    mtl_alias_drop_pool(&pools[2]);
    CHECK(entries() == 41, "past the bound the next Finalize sweeps them");
    // 3. A lookup that meets a dead entry takes it out and reports a miss
    put_dead(1, 9000, 1);
    tex_t d = at(1, 9000 * 64);
    uint32_t before = entries();
    CHECK(pkg_shared(&d) == nil && entries() == before - 1, "a lookup of a dead entry reports none and drops it");
    // 4. A Finalize of the entry's own pool takes out its entries, live or dead, and keeps the others
    put_dead(0, 0, 10);
    NvPkg *own = [NvPkg new]; tex_t o = at(0, 4096); pkg_share_put(&o, own);
    before = entries();
    mtl_alias_drop_pool(&pools[0]);
    CHECK(entries() == before - 11 && pkg_shared(&o) == nil, "a pool's Finalize takes out all of its entries");
    tex_t k = at(1, 0);
    CHECK(pkg_shared(&k) == live[0], "the other pools' live entries stay");
    printf("t_pkg_share: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
