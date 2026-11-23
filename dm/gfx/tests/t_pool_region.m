// The per-thread address → pool cache (pc_get / pc_put, behind mtl_addr and mtl_gpu_addr) has a table direct-mapped by 64 KB region beside the one by 4 KB page: an address in
// another page of a region already seen is found there, with no lock and no walk of the pool index, when the page table has not seen the page and the last pools found are others
// (Donkey Kong Bananza binds from about 3,500 pools). An entry answers only for addresses its pool holds, and neither table outlives a change of the pools (g_pool_gen): a pool
// removed or replaced is never found through a region it used to cover
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
static uint64_t slow_lookups(uint64_t a) { pthread_mutex_lock(&g_mu); int i = pool_find(a, 1); uint64_t n = i >= 0 ? g_pools[i].n_other : 0; pthread_mutex_unlock(&g_mu); return n; }   // lookups of a's pool that took g_mu (mtl_gpu_addr)
static int in_table(const pc_ent *c, const pc_ent *t) { return c >= t && c < t + (1u << PC_BITS); }
int main(void) { @autoreleasepool {
    mtl_init();
    enum { REGION = 1 << 16, PAGE = 1 << 12 };
    const size_t n = 8u << 20;
    uint8_t *mem = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (mem == MAP_FAILED) abort();
    uint64_t A = ((uint64_t)(uintptr_t)mem + REGION - 1) & ~(uint64_t)(REGION - 1);   // pool A: two whole regions and a quarter of a third, so it ends inside a region
    const uint64_t a_size = 2 * REGION + 0x4000, a_end = A + a_size;
    uint64_t B = A + (2u << 20); enum { OTHERS = 16 };   // 16 other pools of one region each: eight of them fill the list of the last pools found
    mtl_pool_add((void *)(uintptr_t)A, a_size);
    for (int r = 0; r < OTHERS; r++) mtl_pool_add((void *)(uintptr_t)(B + (uint64_t)r * REGION), REGION);
    void *mb = NULL; uint64_t mo = 0, gpu = 0;

    // The first lookup of a region takes the lock and fills the tables
    uint64_t first = A + 0x100;
    CHECK("an address in no table is not answered by the cache", pc_get(first, 16) == NULL);
    uint64_t slow0 = slow_lookups(A);
    CHECK("the first lookup finds the pool", mtl_gpu_addr(first, 16, &mb, &mo, &gpu) && mo == 0x100);
    CHECK("through the pool index", slow_lookups(A) == slow0 + 1);
    CHECK("and the cache answers it afterwards", pc_get(first, 16) != NULL);
    // Another 4 KB page of the same region, one whose page slot is not the first page's, after eight lookups of another pool (PC_RECENT) have pushed pool A out of the list of the
    // last pools found: only the region table can answer it. Before the region table this lookup took g_mu and searched the pool index again
    uint64_t other = 0;
    for (uint64_t p = 1; p < REGION / PAGE && !other; p++) if (pc_slot(A + p * PAGE) != pc_slot(first)) other = A + p * PAGE + 0x40;
    CHECK("a page of the region with a page slot of its own", other != 0 && pc_slot_w(other) == pc_slot_w(first));
    int pushed = 0;
    for (uint64_t r = 0; r < OTHERS && pushed < PC_RECENT; r++) { uint64_t q = B + r * REGION + 0x80;
        if (pc_slot_w(q) == pc_slot_w(first) || pc_slot(q) == pc_slot(other) || pc_slot(q) == pc_slot(first)) continue;   // would replace the entries this test looks at
        CHECK("another pool is not in the cache before its first lookup", pc_get(q, 16) == NULL);
        CHECK("a lookup of another pool", mtl_gpu_addr(q, 16, &mb, &mo, &gpu) && mo == 0x80); pushed++; }
    CHECK("the last pools found are eight other pools", pushed == PC_RECENT);
    { int recent = 0; for (unsigned k = 0; k < PC_RECENT; k++) recent |= t_pc.r[k].lo == A; CHECK("pool A is in the recent list no more", !recent); }
    { const pc_ent *e = &t_pc.e[pc_slot(other)]; CHECK("the page table has not seen the other page", !(other >= e->lo && other < e->hi)); }
    slow0 = slow_lookups(A);
    const pc_ent *c = pc_get(other, 16);
    CHECK("another page of a region already seen is found in the cache", c != NULL);
    CHECK("through the region table", c && in_table(c, t_pc.w) && c == &t_pc.w[pc_slot_w(other)]);
    CHECK("as pool A", c && c->lo == A && c->hi == a_end && c->base == A);
    CHECK("mtl_gpu_addr answers it", mtl_gpu_addr(other, 16, &mb, &mo, &gpu) && mo == other - A);
    CHECK("without the lock and the pool index", slow_lookups(A) == slow0);
    { void *mb2 = NULL; uint64_t mo2 = 0; CHECK("mtl_addr finds the same buffer", mtl_addr(other, 16, &mb2, &mo2) && mb2 == mb && mo2 == mo && slow_lookups(A) == slow0); }

    // An entry answers only for what its pool holds: pool A ends a quarter into its third region
    uint64_t last = a_end - 0x1000;
    CHECK("the pool's last region is looked up", mtl_gpu_addr(last, 16, &mb, &mo, &gpu) && mo == last - A);
    CHECK("its entry is in the region table", pc_get(last + 0x40, 16) != NULL && pc_slot_w(last) == pc_slot_w(a_end + 0x100));
    CHECK("an address past the pool's end in the same region is not answered", pc_get(a_end + 0x100, 16) == NULL);
    CHECK("nor found at all", !mtl_gpu_addr(a_end + 0x100, 16, &mb, &mo, &gpu));
    CHECK("a range that runs past the pool's end is not answered", pc_get(a_end - 8, 16) == NULL && !mtl_gpu_addr(a_end - 8, 16, &mb, &mo, &gpu));
    CHECK("a range that ends with the pool is", pc_get(a_end - 16, 16) != NULL);
    CHECK("an address before the pool is not answered", pc_get(A - 0x100, 16) == NULL && !mtl_gpu_addr(A - 0x100, 16, &mb, &mo, &gpu));
    // and a pool that begins half way into a region does not answer for the half before it
    uint64_t C = B + 20ull * REGION + 0x8000; mtl_pool_add((void *)(uintptr_t)C, 0x8000);
    CHECK("a pool inside one region is looked up", mtl_gpu_addr(C + 0x2000, 16, &mb, &mo, &gpu) && mo == 0x2000 && pc_get(C + 0x3000, 16) != NULL);
    CHECK("an address before it in the same region is not answered", pc_slot_w(C - 0x7000) == pc_slot_w(C) && pc_get(C - 0x7000, 16) == NULL && pc_get(C - 8, 16) == NULL);
    CHECK("nor found", !mtl_gpu_addr(C - 0x7000, 16, &mb, &mo, &gpu) && !mtl_gpu_addr(C - 8, 16, &mb, &mo, &gpu));

    // The pools change (g_pool_gen): no table answers until it is filled again, and filling one clears them all
    uint64_t gen0 = atomic_load(&g_pool_gen);
    mtl_pool_del((void *)(uintptr_t)A);
    CHECK("removing a pool changes the pool generation", atomic_load(&g_pool_gen) != gen0);
    CHECK("the removed pool is not answered from its region entry", pc_get(other, 16) == NULL && pc_get(first, 16) == NULL);
    CHECK("nor found", !mtl_gpu_addr(other, 16, &mb, &mo, &gpu) && !mtl_gpu_addr(first, 16, &mb, &mo, &gpu));
    CHECK("the other pool is not answered from the old tables either", pc_get(B + 0x80, 16) == NULL);
    // A smaller pool replaces it in the same regions: its second region and the quarter. One lookup of the new pool fills the tables at the new generation; the old pool's
    // entries, which covered the first region too, must be gone from both tables, not answer under the new generation
    uint64_t A2 = A + REGION; const uint64_t a2_size = REGION + 0x4000;
    mtl_pool_add((void *)(uintptr_t)A2, a2_size);
    uint64_t in2 = A2 + 0x5000;
    CHECK("the replacing pool is found", mtl_gpu_addr(in2, 16, &mb, &mo, &gpu) && mo == 0x5000);
    CHECK("the cache has taken the new generation", t_pc.gen == atomic_load(&g_pool_gen));
    CHECK("the first region, which the old pool held and the new one does not, is not answered", pc_get(first, 16) == NULL && pc_get(other, 16) == NULL);
    { const pc_ent *w = &t_pc.w[pc_slot_w(first)], *e = &t_pc.e[pc_slot(first)]; CHECK("its old entries are cleared from both tables", !(first >= w->lo && first < w->hi) && !(first >= e->lo && first < e->hi)); }
    CHECK("nor found", !mtl_gpu_addr(first, 16, &mb, &mo, &gpu) && !mtl_gpu_addr(other, 16, &mb, &mo, &gpu));
    c = pc_get(A2 + 0x9000, 16);
    CHECK("the new pool's region answers with the new pool", c && c->lo == A2 && c->base == A2 && c->hi == A2 + a2_size);
    CHECK("at the new offset", mtl_gpu_addr(A2 + 0x9000, 16, &mb, &mo, &gpu) && mo == 0x9000);
    mtl_pool_del((void *)(uintptr_t)A2); mtl_pool_del((void *)(uintptr_t)C); for (int r = 0; r < OTHERS; r++) mtl_pool_del((void *)(uintptr_t)(B + (uint64_t)r * REGION)); mtl_drain(); munmap(mem, n);
    printf("t_pool_region: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
