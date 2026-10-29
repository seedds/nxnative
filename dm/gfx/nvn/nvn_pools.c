// Memory pool table of the NVN replay (see nvn_pools.h). The code is nvn_mtl.m's as it was before the move (docs/design/vulkan-backend.md step M2b): the record keeps the backend's
// object as an opaque pointer and the pool's NVN flags, and what the Metal backend did with its own objects inside the table's loops (the residency set, the deferred release of a
// removed pool's buffer) became the hooks of nvn_pools.h. No lock here, as before: the caller holds the backend's (nvn_mtl.m: g_mu)
#include "nvn_pools.h"
#include <stdlib.h>
#include <string.h>

static nvn_pool *g_pools; static int g_npool, g_cap;
static int g_pidx_ok, g_plast = -1;   // whether the address→pool index is valid, the last hit (pool_find)
_Atomic uint64_t g_pool_gen = 1;
__thread nvn_pool_cache t_pc;

void nvn_pool_add(const void *mem, uint64_t size, uint32_t nvn_flags, uintptr_t base, void *backend, uint64_t gpu) {
    uintptr_t lo = (uintptr_t)mem, hi = lo + size;
    if (g_npool == g_cap) { g_cap = g_cap ? g_cap * 2 : 64; g_pools = realloc(g_pools, g_cap * sizeof *g_pools); }
    g_pidx_ok = 0; g_plast = -1; atomic_fetch_add(&g_pool_gen, 1);   // the address index and the per-thread caches are invalid (pool_find, nvn_pool_cached)
    g_pools[g_npool++] = (nvn_pool){lo, hi, base, backend, gpu, mem, 0, nvn_flags};   // res 0: the backend's mark starts clear (Metal: not in the residency set yet, put in the first time it is addressed)
}

void nvn_pool_del(const void *mem) {
    for (int i = 0; i < g_npool; i++)
        if (g_pools[i].key == mem) { nvn_pool_hook_del(&g_pools[i]); g_pools[i] = g_pools[--g_npool]; g_pidx_ok = 0; g_plast = -1; atomic_fetch_add(&g_pool_gen, 1); break; }
}

// Address → pool: the last hit first, then a binary search over indices sorted by start address, and a full linear scan only when that fails (pools overlapping).
// The Legend of Zelda: Breath of the Wild keeps about 1760 memory pools alive and every buffer binding of every draw looks one up; the old full scan took about 4% of the submit thread's samples (mtl_gpu_addr 222 / 6 s). Adding / removing pools invalidates the index, which is re-sorted when next used. Caller holds the backend's lock
static int *g_pidx; static int g_pidx_cap;
static int pidx_cmp(const void *a, const void *b) { uintptr_t x = g_pools[*(const int *)a].lo, y = g_pools[*(const int *)b].lo; return x < y ? -1 : x > y; }
static int pool_find(uint64_t addr, uint64_t len) {
    if (g_plast >= 0 && g_plast < g_npool && addr >= g_pools[g_plast].lo && addr + len <= g_pools[g_plast].hi) return g_plast;
    if (!g_pidx_ok) {
        if (g_pidx_cap < g_npool) { g_pidx_cap = g_npool * 2; g_pidx = realloc(g_pidx, (size_t)g_pidx_cap * sizeof *g_pidx); }
        for (int i = 0; i < g_npool; i++) g_pidx[i] = i;
        qsort(g_pidx, (size_t)g_npool, sizeof *g_pidx, pidx_cmp); g_pidx_ok = 1;
    }
    int lo = 0, hi = g_npool - 1, hit = -1;   // the last one whose start ≤ addr
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_pools[g_pidx[m]].lo <= addr) { hit = m; lo = m + 1; } else hi = m - 1; }
    if (hit >= 0 && addr + len <= g_pools[g_pidx[hit]].hi) return g_plast = g_pidx[hit];
    for (int i = 0; i < g_npool; i++) if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) return g_plast = i;
    return -1;
}
static void pc_put(int i, uint64_t addr) {   // caller holds the backend's lock, pool i holds addr (and on Metal is resident)
    uint64_t gen = atomic_load_explicit(&g_pool_gen, memory_order_relaxed);
    if (t_pc.gen != gen) { memset(t_pc.r, 0, sizeof t_pc.r); memset(t_pc.e, 0, sizeof t_pc.e); t_pc.gen = gen; }
    nvn_pool_ref c = {g_pools[i].lo, g_pools[i].hi, g_pools[i].base, g_pools[i].backend, g_pools[i].gpu};
    t_pc.e[nvn_pool_cache_slot(addr)] = c; t_pc.r[t_pc.next++ % PC_RECENT] = c;
}
int nvn_pool_lookup(uint64_t addr, uint64_t len, nvn_pool_ref *out) {
    int i = pool_find(addr, len);
    if (i < 0) return 0;
    nvn_pool_hook_use(&g_pools[i]); pc_put(i, addr);
    *out = (nvn_pool_ref){g_pools[i].lo, g_pools[i].hi, g_pools[i].base, g_pools[i].backend, g_pools[i].gpu};
    return 1;
}
int nvn_pool_count(void) { return g_npool; }
