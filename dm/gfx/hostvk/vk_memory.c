// The Vulkan backend's memory (docs/design/vulkan-backend.md 4.1-4.3): memory pools as imports of the guest's own pages (vk_import.c), so a guest address is a buffer and an
// offset as on Apple's unified memory; the address lookup through the frontend's pool table (nvn_pools.c) and its per-thread cache; the guest memory the host commits and
// decommits (be_guest_map / be_guest_unmap); the ring of the open submission (staging for uploads and readbacks, the driver banks of each draw, query results, converted indices:
// host-visible, coherent, recycled when the submission completes); the backend's own buffers; and the deferred release of what the GPU may still be using.
// Device memory: the backend's own images and buffers are ranges of blocks made by the allocator of decision D2 (vk_alloc.c, step D2); imports stay one allocation each, but where
// the live allocations near the device's limit, the pools inside one 64 MiB granule of committed guest memory share an import of that granule's committed part (design 4.2's
// coalesced imports, "Pools" below). hvk_buffer_make remains for the GPU tests (dm/gfx/tests/vk/t_vk_harness.h's readbacks, t_vk_deswizzle), which declare it themselves:
// one VkDeviceMemory per buffer, as before D2; nothing of the backend or the guest layers calls it.
// Not here yet: device-local mirrors on discrete GPUs (design 4.2, phase P6)
#include "plat/plat.h"
#include "vk_backend.h"
#include "vk_alloc.h"
#include "../nvn/nvn_pools.h"
#include <stdlib.h>
#include <string.h>

// ---------- Memory types and buffers ----------
uint32_t hvk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback) {
    const VkPhysicalDeviceMemoryProperties *m = &hvk.dev.mem;
    for (int pass = 0; pass < 2; pass++) {
        VkMemoryPropertyFlags w = pass ? fallback : want;
        for (uint32_t i = 0; i < m->memoryTypeCount; i++) if ((bits >> i & 1) && (m->memoryTypes[i].propertyFlags & w) == w) return i;
    }
    return UINT32_MAX;
}
static VkResult alloc_mem(VkMemoryRequirements req, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback, int device_address, VkDeviceMemory *out) {
    uint32_t type = hvk_mem_type(req.memoryTypeBits, want, fallback);
    if (type == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (!hvk_alloc_count_take("a buffer of hvk_buffer_make", req.size)) return VK_ERROR_TOO_MANY_OBJECTS;
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, device_address ? &fl : NULL, req.size, type};
    VkResult r = vkAllocateMemory(hvk.d, &ai, NULL, out);
    if (r) { hvk_alloc_count_give(); *out = VK_NULL_HANDLE; }
    return r;
}
static void free_mem(VkDeviceMemory m) { if (m) { vkFreeMemory(hvk.d, m, NULL); hvk_alloc_count_give(); } }
VkResult hvk_buffer_make(VkDeviceSize n, VkBufferUsageFlags usage, int host_visible, VkBuffer *buf, VkDeviceMemory *mem, void **map) {
    *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; if (map) *map = NULL;
    if (!hvk.dev.caps.buffer_device_address) usage &= ~VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n ? n : 16, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkResult r = vkCreateBuffer(hvk.d, &bi, NULL, buf);
    if (r) { *buf = VK_NULL_HANDLE; return r; }
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(hvk.d, *buf, &req);
    VkMemoryPropertyFlags want = host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    r = alloc_mem(req, want, host_visible ? want : 0, (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0, mem);
    if (!r) r = vkBindBufferMemory(hvk.d, *buf, *mem, 0);
    if (!r && host_visible && map) r = vkMapMemory(hvk.d, *mem, 0, VK_WHOLE_SIZE, 0, map);
    if (r) { vkDestroyBuffer(hvk.d, *buf, NULL); free_mem(*mem); *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; if (map) *map = NULL; }
    return r;
}
void hvk_buffer_free(VkBuffer buf, VkDeviceMemory mem) { if (buf) vkDestroyBuffer(hvk.d, buf, NULL); free_mem(mem); }
VkBufferUsageFlags hvk_ring_usage(void) {
    VkBufferUsageFlags u = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (hvk.dev.caps.cond_render) u |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
    return u;
}

// ---------- Committed guest memory ----------
// What the host committed (be_guest_map) and has not decommitted (be_guest_unmap), as sorted, joined ranges; recorded from the first call, before the device is up (a game sizes
// its heap first). Under hvk.mu
typedef struct { uintptr_t lo, hi; } span_t;
static span_t *g_rg; static int g_nrg, g_crg;
static void region_add(uintptr_t lo, uintptr_t hi) {
    int i = 0;
    while (i < g_nrg && g_rg[i].hi < lo) i++;   // the first range that ends at or after lo: it and those after it that start at or before hi join
    int j = i;
    while (j < g_nrg && g_rg[j].lo <= hi) { if (g_rg[j].lo < lo) lo = g_rg[j].lo; if (g_rg[j].hi > hi) hi = g_rg[j].hi; j++; }
    if (j == i) {   // a new range at i
        if (g_nrg == g_crg) { int c = g_crg ? 2 * g_crg : 16; span_t *n = realloc(g_rg, (size_t)c * sizeof *n); if (!n) return; g_rg = n; g_crg = c; }
        memmove(g_rg + i + 1, g_rg + i, (size_t)(g_nrg - i) * sizeof *g_rg); g_nrg++;
    } else { memmove(g_rg + i + 1, g_rg + j, (size_t)(g_nrg - j) * sizeof *g_rg); g_nrg -= j - i - 1; }
    g_rg[i] = (span_t){lo, hi};
}
static void region_sub(uintptr_t lo, uintptr_t hi) {
    for (int i = 0; i < g_nrg; i++) {
        span_t r = g_rg[i];
        if (r.hi <= lo || r.lo >= hi) continue;
        if (r.lo < lo && r.hi > hi) {   // split in two
            if (g_nrg == g_crg) { int c = 2 * g_crg; span_t *n = realloc(g_rg, (size_t)c * sizeof *n); if (!n) return; g_rg = n; g_crg = c; }
            memmove(g_rg + i + 2, g_rg + i + 1, (size_t)(g_nrg - i - 1) * sizeof *g_rg); g_nrg++;
            g_rg[i] = (span_t){r.lo, lo}; g_rg[i + 1] = (span_t){hi, r.hi};
            return;
        }
        if (r.lo < lo) g_rg[i].hi = lo;
        else if (r.hi > hi) g_rg[i].lo = hi;
        else { memmove(g_rg + i, g_rg + i + 1, (size_t)(g_nrg - i - 1) * sizeof *g_rg); g_nrg--; i--; }
    }
}
static int region_of(uintptr_t lo, uintptr_t hi, span_t *out) {   // the committed range holding [lo, hi)
    int a = 0, b = g_nrg - 1;
    while (a <= b) { int m = (a + b) / 2; if (g_rg[m].hi <= lo) a = m + 1; else if (g_rg[m].lo > lo) b = m - 1; else { if (hi > g_rg[m].hi) return 0; *out = g_rg[m]; return 1; } }
    return 0;
}
void hvk_region_map(void *mem, uint64_t size, int committed) {   // be_guest_map (1) and, while the device is not up, be_guest_unmap (0)
    if (!size) return;
    pthread_mutex_lock(&hvk.mu);
    if (committed) region_add((uintptr_t)mem, (uintptr_t)mem + size); else region_sub((uintptr_t)mem, (uintptr_t)mem + size);
    pthread_mutex_unlock(&hvk.mu);
}

// ---------- Pools ----------
// A pool's import is its own (the pool rounded out to whole import pages: neighbouring pools may share a page, which the drivers allow: README.md), or, where the live device
// memory allocations have reached half the device's limit (maxMemoryAllocationCount: 4,096 on NVIDIA and AMD under Windows), a shared one: a pool that lies inside one granule
// (an aligned 64 MiB) of committed guest memory joins the import of that granule's committed range around it (design 4.2's coalesced imports), made by the first such pool and
// released when its last pool goes. A granule committed further after its import was made gets a new one, for the pools that fall outside the old (not grown: the old one's pools
// keep it until they go); a granule with two committed ranges has one import for each. A pool's import changes only when guest memory under a shared import is decommitted
// (hvk_guest_unmap): its other pools move to new imports. hvk_buf reads a pool's import pointer without the lock; an import a pool leaves goes through hvk_defer, after the
// submissions that may use it
struct hvk_imp {
    hostvk_import_t im;
    int refs;          // pools on it (hvk.mu)
    uint8_t shared;    // a granule's (counted in g_imp_shared), not a pool's own
    int64_t granule;   // its granule while new pools of that granule may join it (it is in g_join), -1 otherwise
    uint64_t serial;   // never reused: what a view over its buffer was made for (vk_texture.c's texel buffer views)
};
enum { GRANULE_SHIFT = 26 };
static hvk_imp **g_join; static int g_njoin, g_cjoin;   // the shared imports new pools may join
static _Atomic uint32_t g_imp_own, g_imp_shared, g_pools_shared;
static _Atomic uint64_t g_imp_serial;
void hvk_import_counts(uint32_t *own, uint32_t *shared, uint32_t *pools_shared) {
    *own = atomic_load(&g_imp_own); *shared = atomic_load(&g_imp_shared); *pools_shared = atomic_load(&g_pools_shared);
}
// The live pools of the backend, for be_guest_unmap's scan (the frontend's table has no iteration); under hvk.mu
static hvk_pool **g_live; static int g_nlive, g_clive;

static hvk_imp *imp_new(uintptr_t lo, uintptr_t hi, int64_t granule) {   // an import of [lo, hi); NULL when it is refused (hostvk_import logs why)
    hvk_imp *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    if (atomic_load(&hvk.dev.allocations) >= hvk.dev.caps.max_allocations) hvk_alloc_reclaim();   // empty blocks first, at the limit
    if (hostvk_import(&hvk.dev, (void *)lo, hi - lo, &m->im) != VK_SUCCESS) { free(m); return NULL; }
    hvk_alloc_note_live();
    m->shared = granule >= 0; m->granule = granule; m->serial = atomic_fetch_add(&g_imp_serial, 1) + 1;
    atomic_fetch_add(m->shared ? &g_imp_shared : &g_imp_own, 1);
    return m;
}
static void imp_release(hvk_imp *m) {   // now: the GPU is done with it
    hostvk_import_release(&hvk.dev, &m->im);
    atomic_fetch_sub(m->shared ? &g_imp_shared : &g_imp_own, 1);
    free(m);
}
static void retire(hvk_imp *m) { for (int i = 0; i < g_njoin; i++) if (g_join[i] == m) { g_join[i] = g_join[--g_njoin]; break; } m->granule = -1; }   // new pools no longer join m
// The shared import a pool over [plo, phi) joins (hvk.mu held): NULL when it takes one of its own (no pressure on the count, not inside one granule of committed memory, or
// the granule's import refused)
static hvk_imp *shared_attach(uintptr_t plo, uintptr_t phi) {
    uint64_t a = hvk.dev.caps.min_import_alignment;
    if (!a || atomic_load(&hvk.dev.allocations) + 1 <= hvk.dev.caps.max_allocations / 2) return NULL;
    uintptr_t lo = plo & ~(uintptr_t)(a - 1), hi = (phi + a - 1) & ~(uintptr_t)(a - 1);
    int64_t gi = (int64_t)(lo >> GRANULE_SHIFT);
    span_t rg;
    if ((int64_t)((hi - 1) >> GRANULE_SHIFT) != gi || !region_of(lo, hi, &rg)) return NULL;
    for (int i = 0; i < g_njoin; i++) {
        hvk_imp *m = g_join[i];
        if (m->granule == gi && (uintptr_t)m->im.base <= lo && hi <= (uintptr_t)m->im.base + m->im.bytes) { m->refs++; atomic_fetch_add(&g_pools_shared, 1); return m; }
    }
    if (g_njoin == g_cjoin) { int c = g_cjoin ? 2 * g_cjoin : 16; hvk_imp **n = realloc(g_join, (size_t)c * sizeof *n); if (!n) return NULL; g_join = n; g_cjoin = c; }
    uintptr_t glo = (uintptr_t)gi << GRANULE_SHIFT, ghi = glo + ((uintptr_t)1 << GRANULE_SHIFT);
    if (rg.lo > glo) glo = (rg.lo + a - 1) & ~(uintptr_t)(a - 1);
    if (rg.hi < ghi) ghi = rg.hi & ~(uintptr_t)(a - 1);
    if (glo > lo || ghi < hi) return NULL;   // the committed range's ends are not on import pages
    hvk_imp *m = imp_new(glo, ghi, gi);
    if (!m) return NULL;
    for (int i = g_njoin - 1; i >= 0; i--)   // the granule's imports inside the new one (made before the range grew) take no new pool: theirs keep them until they go
        if (g_join[i]->granule == gi && glo <= (uintptr_t)g_join[i]->im.base && (uintptr_t)g_join[i]->im.base + g_join[i]->im.bytes <= ghi) retire(g_join[i]);
    g_join[g_njoin++] = m;
    m->refs = 1; atomic_fetch_add(&g_pools_shared, 1);
    return m;
}
static hvk_imp *own_attach(uintptr_t plo, uintptr_t phi) {
    uint64_t a = hvk.dev.caps.min_import_alignment;
    if (!a) return NULL;
    hvk_imp *m = imp_new(plo & ~(uintptr_t)(a - 1), (phi + a - 1) & ~(uintptr_t)(a - 1), -1);
    if (m) m->refs = 1;
    return m;
}
static void detach(hvk_imp *m) {   // hvk.mu held: a pool leaves m, which goes after the GPU when it was the last
    if (!m) return;
    if (m->shared) atomic_fetch_sub(&g_pools_shared, 1);
    if (--m->refs) return;
    if (m->granule >= 0) retire(m);
    hvk_defer(HVK_DEL_IMPORT, m, 0, 0);
}
void hvk_pool_add(const void *mem, uint64_t size, uint32_t nvn_flags);
void hvk_pool_add(const void *mem, uint64_t size, uint32_t nvn_flags) {
    hvk_pool *p = NULL; hvk_imp *m = NULL;
    uintptr_t lo = (uintptr_t)mem, hi = lo + size;
    int locked = 0;
    if (mem && size && hvk.dev.caps.min_import_alignment && (p = calloc(1, sizeof *p))) {
        pthread_mutex_lock(&hvk.mu);
        if ((m = shared_attach(lo, hi))) locked = 1;   // into the table under the same lock: a decommit beside it sees it
        else { pthread_mutex_unlock(&hvk.mu); m = own_attach(lo, hi); }   // outside the lock: an import takes tens of microseconds
        if (!m) { free(p); p = NULL; }   // hostvk_import logs the refusal
        else { p->lo = lo; p->hi = hi; p->nvn_flags = nvn_flags; atomic_store(&p->imp, m); }
    }
    if (!p && size) HVK_RED_ONCE("memory pool %p + 0x%llx has no Vulkan import (first case): the GPU cannot address it, and its draws and copies are dropped", mem, (unsigned long long)size);
    if (!locked) pthread_mutex_lock(&hvk.mu);
    nvn_pool_add(mem, size, nvn_flags, m ? (uintptr_t)m->im.base : (uintptr_t)mem, p, m ? (uint64_t)m->im.addr : 0);
    if (p) {
        if (g_nlive == g_clive) { g_clive = g_clive ? 2 * g_clive : 64; g_live = realloc(g_live, (size_t)g_clive * sizeof *g_live); }
        if (g_live) g_live[g_nlive++] = p;
    }
    pthread_mutex_unlock(&hvk.mu);
}
void hvk_pool_del(const void *mem);
void hvk_pool_del(const void *mem) { pthread_mutex_lock(&hvk.mu); nvn_pool_del(mem); pthread_mutex_unlock(&hvk.mu); }
// nvn_pools.h's hooks, under hvk.mu: nothing to make resident; a removed pool's import goes when the GPU is done with it (when it was its last pool), and the pool object with it
void hvk_pool_hook_use(nvn_pool *p);
void hvk_pool_hook_use(nvn_pool *p) { (void)p; }
void hvk_pool_hook_del(nvn_pool *p);
void hvk_pool_hook_del(nvn_pool *p) {
    hvk_pool *b = p->backend;
    if (!b) return;
    for (int i = 0; i < g_nlive; i++) if (g_live[i] == b) { g_live[i] = g_live[--g_nlive]; break; }
    detach(atomic_exchange(&b->imp, NULL));
    hvk_defer(HVK_DEL_FREE, b, 0, 0);   // a thread's cached lookup may still hold it until the submission ends
}
// The import a guest address is in: its serial (vk_texture.c), 0 when no pool holds [addr, addr + len)
uint64_t hvk_buf_import(uint64_t addr, uint64_t len) {
    const nvn_pool_ref *c = nvn_pool_cached(addr, len ? len : 1); nvn_pool_ref r;
    if (!c) { pthread_mutex_lock(&hvk.mu); int ok = nvn_pool_lookup(addr, len ? len : 1, &r); pthread_mutex_unlock(&hvk.mu); if (!ok) return 0; c = &r; }
    const hvk_pool *p = c->backend;
    hvk_imp *m = p ? atomic_load(&p->imp) : NULL;
    return m && m->im.buf ? m->serial : 0;
}
int hvk_buf(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off, VkDeviceAddress *dev) {
    const nvn_pool_ref *c = nvn_pool_cached(addr, len ? len : 1); nvn_pool_ref r;
    if (!c) { pthread_mutex_lock(&hvk.mu); int ok = nvn_pool_lookup(addr, len ? len : 1, &r); pthread_mutex_unlock(&hvk.mu); if (!ok) return 0; c = &r; }
    const hvk_pool *p = c->backend;
    hvk_imp *m = p ? atomic_load(&p->imp) : NULL;   // the import itself, not the table's base: a pool's import can change (hvk_guest_unmap)
    if (!m || !m->im.buf) return 0;
    *buf = m->im.buf; *off = addr - (uintptr_t)m->im.base;
    if (dev) *dev = m->im.addr ? m->im.addr + *off : 0;
    return 1;
}
int hvk_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off);
int hvk_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {   // be_addr: the pool's backend object and the offset in it
    const nvn_pool_ref *c = nvn_pool_cached(addr, len); nvn_pool_ref r;
    if (c) { *buf = c->backend; *off = addr - c->base; return 1; }
    int ok = 0;
    pthread_mutex_lock(&hvk.mu);
    if (nvn_pool_lookup(addr, len, &r)) { *buf = r.backend; *off = addr - r.base; ok = 1; }
    pthread_mutex_unlock(&hvk.mu);
    return ok;
}
// Guest memory about to be decommitted (be_guest_unmap, design 1.5 and 1.6): an import over any of it must go first. A game finalizes its pools before it frees their memory, so a
// pool still live there is a guest error: it loses its import (said once), and the GPU can no longer address it. A shared import also covers committed memory between its pools:
// its other pools move to new imports (of the granule's part still committed, or their own) and the old one goes after the submission that may still use it (none is in flight
// after be_finish but the one being recorded, if any); its decommitted pages are none of a pool's
static int pool_cmp(const void *a, const void *b) { uintptr_t x = (*(hvk_pool *const *)a)->lo, y = (*(hvk_pool *const *)b)->lo; return x < y ? -1 : x > y; }
static int imp_meets(const hvk_imp *m, uintptr_t lo, uintptr_t hi) { return m && m->im.buf && (uintptr_t)m->im.base < hi && lo < (uintptr_t)m->im.base + m->im.bytes; }
void hvk_guest_unmap(void *mem, uint64_t size);
void hvk_guest_unmap(void *mem, uint64_t size) {
    uintptr_t lo = (uintptr_t)mem, hi = lo + size; int n = 0;
    pthread_mutex_lock(&hvk.mu);
    region_sub(lo, hi);
    for (int i = 0; i < g_nlive; i++) n += imp_meets(atomic_load(&g_live[i]->imp), lo, hi);
    pthread_mutex_unlock(&hvk.mu);
    if (!n) return;
    extern void be_finish(void);
    be_finish();
    pthread_mutex_lock(&hvk.mu);
    for (int i = 0; i < g_nlive; i++) {   // the shared imports over it take no new pool
        hvk_imp *m = atomic_load(&g_live[i]->imp);
        if (imp_meets(m, lo, hi) && m->granule >= 0) retire(m);
    }
    qsort(g_live, (size_t)g_nlive, sizeof *g_live, pool_cmp);   // in address order, each range's pools one after another: one new import per granule and range
    int moved = 0;
    for (int i = 0; i < g_nlive; i++) {
        hvk_pool *p = g_live[i];
        hvk_imp *m = atomic_load(&p->imp);
        if (!imp_meets(m, lo, hi)) continue;
        hvk_imp *to = NULL;
        if (p->lo < hi && lo < p->hi)
            HVK_RED_ONCE("guest memory %p + 0x%llx decommitted under a live memory pool %p + 0x%llx (first case): its import is released, the GPU no longer addresses it",
                         mem, (unsigned long long)size, (void *)p->lo, (unsigned long long)(p->hi - p->lo));
        else if (!(to = shared_attach(p->lo, p->hi)) && !(to = own_attach(p->lo, p->hi)))
            HVK_RED_ONCE("memory pool %p + 0x%llx lost its shared import to a decommit beside it and no other import could be made (first case): the GPU no longer addresses it",
                         (void *)p->lo, (unsigned long long)(p->hi - p->lo));
        atomic_store(&p->imp, to);
        detach(m);
        moved++;
    }
    if (moved) atomic_fetch_add(&g_pool_gen, 1);   // the per-thread caches hold the old bases
    pthread_mutex_unlock(&hvk.mu);
}

// ---------- The ring of the open submission ----------
struct hvk_chunk { VkBuffer buf; hvk_mem *mem; uint8_t *map; VkDeviceSize size, used; VkDeviceAddress dev; hvk_chunk *next; };
enum { CHUNK = 4u << 20 };
static hvk_chunk *g_free_chunks;   // submit thread only
static hvk_chunk **g_cur;          // the open submission's list
static void chunk_destroy(hvk_chunk *c) { hvk_mem_buffer_free(c->buf, c->mem); free(c); }
void hvk_ring_bind(hvk_chunk **chunks) { g_cur = chunks; }
void hvk_ring_slot_release(hvk_chunk **chunks) {
    while (*chunks) {
        hvk_chunk *c = *chunks; *chunks = c->next;
        if (c->size > CHUNK) { chunk_destroy(c); continue; }   // a large one-off: not kept
        c->used = 0; c->next = g_free_chunks; g_free_chunks = c;
    }
}
int hvk_ring_alloc(VkDeviceSize n, VkDeviceSize align, hvk_alloc *out) {
    memset(out, 0, sizeof *out);
    if (!g_cur) { HVK_RED_ONCE("ring allocation outside a submission"); return 0; }
    if (align < 16) align = 16;
    hvk_chunk *c = *g_cur;
    VkDeviceSize at = c ? (c->used + align - 1) & ~(align - 1) : 0;
    if (!c || at + n > c->size) {
        c = NULL;
        if (n <= CHUNK && g_free_chunks) { c = g_free_chunks; g_free_chunks = c->next; }
        else {
            c = calloc(1, sizeof *c);
            if (!c) return 0;
            c->size = n > CHUNK ? (n + 0xffff) & ~(VkDeviceSize)0xffff : CHUNK;
            void *map;
            if (hvk_mem_buffer(c->size, hvk_ring_usage(), HVK_MEM_HOST, &c->buf, &c->mem, &map)) { free(c); hostvk_log("[RED] [hostvk] ring chunk of %llu bytes: allocation failed", (unsigned long long)n); return 0; }
            c->map = map;
            if (hvk.dev.caps.buffer_device_address) { VkBufferDeviceAddressInfo ba = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, c->buf}; c->dev = vkGetBufferDeviceAddress(hvk.d, &ba); }
        }
        c->used = 0; c->next = *g_cur; *g_cur = c; at = 0;
    }
    c->used = at + n;
    *out = (hvk_alloc){c->buf, at, c->map + at, c->dev ? c->dev + at : 0};
    return 1;
}

// ---------- The upload scratch (vk_texture.c's GPU paths, design 4.3) ----------
// One device-local buffer with a device address, which the GPU deswizzle and the ASTC decode write band after band and the image copies read; the caller orders the uses with
// barriers (a band's kernel waits for the previous band's copy). It grows to the largest band asked for, at most HVK_SCRATCH_MAX; the old one goes when the GPU is done with it
static VkBuffer g_scr; static hvk_mem *g_scr_mem; static VkDeviceSize g_scr_size; static VkDeviceAddress g_scr_addr;
int hvk_scratch(VkDeviceSize n, VkBuffer *buf, VkDeviceAddress *addr) {
    if (!hvk.dev.caps.buffer_device_address || !n || n > HVK_SCRATCH_MAX) return 0;
    if (n > g_scr_size) {
        if (g_scr) hvk_defer(HVK_DEL_BUFFER, g_scr, (uint64_t)(uintptr_t)g_scr_mem, 0);
        g_scr = VK_NULL_HANDLE; g_scr_mem = NULL; g_scr_size = 0; g_scr_addr = 0;
        VkDeviceSize want = n < (4u << 20) ? (4u << 20) : (n + 0xfffff) & ~(VkDeviceSize)0xfffff;
        if (want > HVK_SCRATCH_MAX) want = HVK_SCRATCH_MAX;
        VkBufferUsageFlags u = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        if (hvk_mem_buffer(want, u, 0, &g_scr, &g_scr_mem, NULL)) { HVK_RED_ONCE("the upload scratch of %llu bytes could not be made: uploads go through the CPU", (unsigned long long)want); return 0; }
        VkBufferDeviceAddressInfo ba = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, g_scr};
        g_scr_addr = vkGetBufferDeviceAddress(hvk.d, &ba); g_scr_size = want;
    }
    *buf = g_scr; *addr = g_scr_addr;
    return 1;
}

// ---------- Deferred release ----------
typedef struct { hvk_del_kind kind; void *obj; uint64_t a, b, fence; } del_ent;
static del_ent *g_del; static size_t g_ndel, g_cdel; static pthread_mutex_t g_del_mu = PTHREAD_MUTEX_INITIALIZER;
extern _Atomic uint64_t hvk_last_fence;   // vk_commands.c: the last fence committed
static void del_now(const del_ent *e) {
    switch (e->kind) {
        case HVK_DEL_IMPORT: imp_release(e->obj); break;
        case HVK_DEL_BUFFER: vkDestroyBuffer(hvk.d, (VkBuffer)e->obj, NULL); hvk_mem_free((hvk_mem *)(uintptr_t)e->a); break;
        case HVK_DEL_IMAGE: vkDestroyImage(hvk.d, (VkImage)e->obj, NULL); hvk_mem_free((hvk_mem *)(uintptr_t)e->a); break;
        case HVK_DEL_VIEW: vkDestroyImageView(hvk.d, (VkImageView)e->obj, NULL); break;
        case HVK_DEL_BUFVIEW: vkDestroyBufferView(hvk.d, (VkBufferView)e->obj, NULL); break;
        case HVK_DEL_FREE: free(e->obj); break;
        case HVK_DEL_PIPELINE: vkDestroyPipeline(hvk.d, (VkPipeline)e->obj, NULL); break;   // a fast-linked pipeline its optimized one replaced (vk_gpl.c)
    }
}
void hvk_defer(hvk_del_kind kind, void *obj, uint64_t a, uint64_t b) {
    if (!obj) return;
    uint64_t f = hvk_open_fence(); if (!f) f = atomic_load(&hvk_last_fence);   // the open submission may use it, else the last one committed
    del_ent e = {kind, obj, a, b, f};
    if (f <= hvk_done_fence()) { del_now(&e); return; }
    pthread_mutex_lock(&g_del_mu);
    if (g_ndel == g_cdel) { g_cdel = g_cdel ? 2 * g_cdel : 256; g_del = realloc(g_del, g_cdel * sizeof *g_del); }
    if (g_del) g_del[g_ndel++] = e;
    pthread_mutex_unlock(&g_del_mu);
}
void hvk_deferred_run(uint64_t done) {
    for (;;) {
        del_ent e; int have = 0;
        pthread_mutex_lock(&g_del_mu);
        for (size_t i = 0; i < g_ndel; i++) if (g_del[i].fence <= done) { e = g_del[i]; g_del[i] = g_del[--g_ndel]; have = 1; break; }
        pthread_mutex_unlock(&g_del_mu);
        if (!have) break;
        del_now(&e);
    }
    hvk_alloc_trim(done);   // empty blocks past their grace go back to the driver
}
void hvk_mem_shutdown(void) {
    if (g_scr) { hvk_mem_buffer_free(g_scr, g_scr_mem); g_scr = VK_NULL_HANDLE; g_scr_mem = NULL; g_scr_size = 0; g_scr_addr = 0; }
    hvk_deferred_run(UINT64_MAX);
    while (g_free_chunks) { hvk_chunk *c = g_free_chunks; g_free_chunks = c->next; chunk_destroy(c); }
    pthread_mutex_lock(&hvk.mu);
    for (int i = 0; i < g_nlive; i++) {   // the pools the game did not finalize: their table entries stay, with no buffer
        hvk_imp *m = atomic_exchange(&g_live[i]->imp, NULL);
        if (!m) continue;
        if (m->shared) atomic_fetch_sub(&g_pools_shared, 1);
        if (!--m->refs) imp_release(m);
    }
    g_nlive = 0; g_njoin = 0;
    pthread_mutex_unlock(&hvk.mu);
    hvk_alloc_shutdown();
}
