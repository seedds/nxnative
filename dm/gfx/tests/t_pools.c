// Unit tests of the replay's memory pool table (nvn_pools.c), CPU only, on every host: lookups inside a pool, at its end, across two adjacent pools, in a gap and after a deletion; a
// pool's base below its start (page rounding) and the GPU address of an address; the opaque backend handle and the NVN flags carried to the hooks and back; the backend's mark
// (Metal: in the residency set) the use hook sets; overlapping pools as today's lookup takes them (the last hit first, else the last start at or below the address, else the first in the
// table); hundreds of pools (the table grows, the sorted index); the generation every add and removal bumps; and the per-thread cache (filled by a lookup, emptied by a change, one per
// thread). Addresses are only compared, never dereferenced; the hooks are stubs that record their calls. Expected values are written out by hand
#include "../nvn/nvn_pools.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int fail, total;
static void t(const char *name, int ok) {
    total++; fail += !ok;
    printf("%-80s %s\n", name, ok ? "✓" : "✗");
}

// ---------- Stubs: the backend's hooks ----------
static int n_use, n_del, use_res_before; static nvn_pool use_seen, del_seen;
void nvn_pool_hook_use(nvn_pool *p) { n_use++; use_res_before = p->res; use_seen = *p; p->res = 1; }   // as Metal's pool_res: the mark set the first time
void nvn_pool_hook_del(nvn_pool *p) { n_del++; del_seen = *p; }

#define H(x) ((void *)(uintptr_t)(x))         // a backend handle: only compared
#define M(x) ((const void *)(uintptr_t)(x))   // a pool's storage (its key and its start): only compared
static int found(uint64_t addr, uint64_t len, void *backend) { nvn_pool_ref r; return nvn_pool_lookup(addr, len, &r) && r.backend == backend; }

static int th_cached_before, th_found;
static void *other_thread(void *arg) {
    (void)arg; th_cached_before = nvn_pool_cached(0x30000100, 16) != NULL;
    nvn_pool_ref r; th_found = nvn_pool_lookup(0x30000100, 16, &r) && nvn_pool_cached(0x30000100, 16) != NULL;
    return NULL;
}

int main(void) {
    nvn_pool_ref r;
    t("an empty table: no pool, nothing cached", !nvn_pool_lookup(0x10000000, 16, &r) && nvn_pool_count() == 0 && !nvn_pool_cached(0x10000000, 16));
    uint64_t g0 = atomic_load(&g_pool_gen);
    t("the generation starts at 1", g0 == 1);

    // ---------- One pool ----------
    nvn_pool_add(M(0x10000000), 0x10000, 0x24, 0x10000000, H(0xa1), 0x900000000ull);
    t("added: one pool, the generation bumped", nvn_pool_count() == 1 && atomic_load(&g_pool_gen) == g0 + 1);
    int ok = nvn_pool_lookup(0x10000100, 16, &r);
    t("a lookup inside it: its record", ok && r.lo == 0x10000000 && r.hi == 0x10010000 && r.base == 0x10000000 && r.backend == H(0xa1) && r.gpu == 0x900000000ull);
    t("the use hook saw it once: flags and key as given, the mark clear", n_use == 1 && use_seen.backend == H(0xa1) && use_seen.nvn_flags == 0x24 && use_seen.key == M(0x10000000)
      && !use_res_before);
    t("the GPU address of an address: gpu + (addr - base)", r.gpu + (0x10000100 - r.base) == 0x900000100ull);
    nvn_pool_lookup(0x10000200, 4, &r);
    t("a second lookup: the hook again, the mark it set kept (the record is the table's)", n_use == 2 && use_res_before == 1);
    t("a lookup ending at the pool's end: found", found(0x1000fff0, 16, H(0xa1)));
    t("one byte past the end: not found", !nvn_pool_lookup(0x1000fff0, 17, &r));
    t("starting at the end, or one byte before the start: not found", !nvn_pool_lookup(0x10010000, 1, &r) && !nvn_pool_lookup(0x0fffffff, 2, &r));
    t("a zero-length lookup at the end address: found (as today)", found(0x10010000, 0, H(0xa1)));

    // ---------- The per-thread cache ----------
    nvn_pool_lookup(0x10000100, 16, &r);
    const nvn_pool_ref *c = nvn_pool_cached(0x10000100, 16);
    t("after a lookup the thread's cache holds the pool", c && c->backend == H(0xa1) && c->base == 0x10000000 && c->gpu == 0x900000000ull);
    c = nvn_pool_cached(0x1000f000, 16);
    t("another page of it: from the recent list", c && c->backend == H(0xa1));
    t("an address outside: not cached", !nvn_pool_cached(0x20000000, 16));
    int n_use_before = n_use; c = nvn_pool_cached(0x10000300, 8);
    t("a cache hit does not call the use hook", c && n_use == n_use_before);

    // ---------- Base below the start; a change empties the caches ----------
    uint64_t g1 = atomic_load(&g_pool_gen);
    nvn_pool_add(M(0x20000123), 0x100, 0, 0x20000000, H(0xb2), 0x910000000ull);
    t("another pool: the generation bumped, the caches no longer used", atomic_load(&g_pool_gen) == g1 + 1 && !nvn_pool_cached(0x10000100, 16));
    ok = nvn_pool_lookup(0x20000123, 4, &r);
    t("a pool whose base is below its start: its start's offset is 0x123", ok && r.lo == 0x20000123 && r.hi == 0x20000223 && 0x20000123 - r.base == 0x123 && r.backend == H(0xb2));
    t("its page below its start is not the pool's", !nvn_pool_lookup(0x20000100, 4, &r));

    // ---------- Adjacent pools ----------
    nvn_pool_add(M(0x30000000), 0x1000, 0, 0x30000000, H(0xc3), 0);
    nvn_pool_add(M(0x30001000), 0x1000, 0, 0x30001000, H(0xd4), 0);
    t("adjacent pools: each found alone", found(0x30000ff0, 16, H(0xc3)) && found(0x30001000, 16, H(0xd4)));
    t("a range across both: in no pool", !nvn_pool_lookup(0x30000ff0, 0x20, &r));

    // ---------- Many pools: the table grows past its first 64 records, the sorted index finds each ----------
    for (int i = 0; i < 200; i++) nvn_pool_add(M(0x60000000 + 0x10000 * (uint64_t)i), 0x1000, (uint32_t)i, 0x60000000 + 0x10000 * (uintptr_t)i, H(0x1000 + i), 0);
    t("204 pools", nvn_pool_count() == 204);
    int all = 1; for (int i = 199; i >= 0; i--) all &= found(0x60000800 + 0x10000 * (uint64_t)i, 8, H(0x1000 + i));
    t("each of the 200 found, from its last to its first", all);
    t("a gap between two of them: in no pool", !nvn_pool_lookup(0x60002000, 4, &r) && !nvn_pool_lookup(0x60001000, 1, &r));
    t("the first pools still found after the table grew", found(0x10000010, 4, H(0xa1)) && found(0x30001010, 4, H(0xd4)));

    // ---------- Overlapping pools: one inside another ----------
    nvn_pool_add(M(0x50000000), 0x100000, 0, 0x50000000, H(0xe5), 0);
    nvn_pool_add(M(0x50040000), 0x10000, 0, 0x50040000, H(0xf6), 0);
    t("a pool inside another, fresh index: the inner one (the last start at or below)", found(0x50041000, 16, H(0xf6)));
    t("past the inner pool: the outer one, through the full scan", found(0x50080000, 16, H(0xe5)));
    t("the inner range again: the outer one, the last hit, holds it", found(0x50041000, 16, H(0xe5)));

    // ---------- Deletion ----------
    uint64_t g2 = atomic_load(&g_pool_gen); int n = nvn_pool_count();
    nvn_pool_lookup(0x30000100, 16, &r);   // the mark set, then the pool goes
    nvn_pool_del(M(0x30000000));
    t("deleted: the hook saw its handle and mark, one pool less, the generation bumped", n_del == 1 && del_seen.backend == H(0xc3) && del_seen.res == 1
      && nvn_pool_count() == n - 1 && atomic_load(&g_pool_gen) == g2 + 1);
    t("after deletion: in no pool, not cached", !nvn_pool_lookup(0x30000100, 16, &r) && !nvn_pool_cached(0x30000100, 16));
    t("its neighbour still found", found(0x30001010, 16, H(0xd4)));
    nvn_pool_del(M(0x12345));
    t("deleting an unknown key: nothing", n_del == 1 && nvn_pool_count() == n - 1 && atomic_load(&g_pool_gen) == g2 + 1);
    nvn_pool_del(M(0x10000000));
    t("the first pool deleted (the last takes its place): its flags to the hook", n_del == 2 && del_seen.nvn_flags == 0x24 && del_seen.backend == H(0xa1) && nvn_pool_count() == n - 2);
    all = 1; for (int i = 0; i < 200; i++) all &= found(0x60000ff8 + 0x10000 * (uint64_t)i, 8, H(0x1000 + i));
    t("every other pool still found", all && found(0x20000123, 4, H(0xb2)) && found(0x50080000, 16, H(0xe5)) && found(0x30001010, 16, H(0xd4)));
    t("the deleted first pool: in no pool", !nvn_pool_lookup(0x10000100, 4, &r));
    nvn_pool_add(M(0x30000000), 0x1000, 7, 0x30000000, H(0xc7), 0x920000000ull);
    ok = nvn_pool_lookup(0x30000100, 16, &r);
    t("a new pool at a deleted one's address: the new handle, flags, the mark clear again", ok && r.backend == H(0xc7) && r.gpu == 0x920000000ull && !use_res_before && use_seen.nvn_flags == 7);

    // ---------- One cache per thread ----------
    t("this thread's cache holds the pool", nvn_pool_cached(0x30000100, 16) != NULL);
    pthread_t th; pthread_create(&th, NULL, other_thread, NULL); pthread_join(th, NULL);
    t("another thread's cache starts empty, and fills from its own lookup", !th_cached_before && th_found);
    c = nvn_pool_cached(0x30000100, 16);
    t("this thread's cache is unchanged by the other's", c && c->backend == H(0xc7));

    printf("== t_pools: %d checks, %d failed ==\n", total, fail);
    return fail != 0;
}
