// The Vulkan backend's memory (docs/design/vulkan-backend.md 4.1-4.3): memory pools as imports of the guest's own pages (vk_import.c), so a guest address is a buffer and an
// offset as on Apple's unified memory; the address lookup through the frontend's pool table (nvn_pools.c) and its per-thread cache; the ring of the open submission (staging for
// uploads and readbacks, the driver banks of each draw, query results, converted indices: host-visible, coherent, recycled when the submission completes); the backend's own
// buffers; and the deferred release of what the GPU may still be using.
// Not here yet (design 4.2): coalesced imports where a driver limits the allocation count (lavapipe has no practical limit, README.md), device-local mirrors on discrete GPUs
// (phase P6), AMD's memory allocator (decision D2: one VkDeviceMemory per image and per chunk for now)
#include "plat/plat.h"
#include "vk_backend.h"
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
    if (++hvk.dev.allocations > hvk.dev.caps.max_allocations) { hvk.dev.allocations--; return VK_ERROR_TOO_MANY_OBJECTS; }
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, device_address ? &fl : NULL, req.size, type};
    VkResult r = vkAllocateMemory(hvk.d, &ai, NULL, out);
    if (r) { hvk.dev.allocations--; *out = VK_NULL_HANDLE; }
    return r;
}
static void free_mem(VkDeviceMemory m) { if (m) { vkFreeMemory(hvk.d, m, NULL); hvk.dev.allocations--; } }
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

// ---------- Pools ----------
// The live pools of the backend, for be_guest_unmap's scan (the frontend's table has no iteration); under hvk.mu
static hvk_pool **g_live; static int g_nlive, g_clive;
void hvk_pool_add(const void *mem, uint64_t size, uint32_t nvn_flags);
void hvk_pool_add(const void *mem, uint64_t size, uint32_t nvn_flags) {
    hvk_pool *p = NULL;
    uint64_t a = hvk.dev.caps.min_import_alignment;
    if (mem && size && a) {
        uintptr_t lo = (uintptr_t)mem & ~(uintptr_t)(a - 1), hi = ((uintptr_t)mem + size + a - 1) & ~(uintptr_t)(a - 1);   // whole import pages: neighbouring pools may share one
        p = calloc(1, sizeof *p);
        if (p && hostvk_import(&hvk.dev, (void *)lo, hi - lo, &p->im) != VK_SUCCESS) { free(p); p = NULL; }   // hostvk_import logs the refusal
        if (p) { p->lo = (uintptr_t)mem; p->hi = (uintptr_t)mem + size; p->nvn_flags = nvn_flags; }
    }
    if (!p && size) HVK_RED_ONCE("memory pool %p + 0x%llx has no Vulkan import (first case): the GPU cannot address it, and its draws and copies are dropped", mem, (unsigned long long)size);
    pthread_mutex_lock(&hvk.mu);
    nvn_pool_add(mem, size, nvn_flags, p ? (uintptr_t)p->im.base : (uintptr_t)mem, p, p ? (uint64_t)p->im.addr : 0);
    if (p) {
        if (g_nlive == g_clive) { g_clive = g_clive ? 2 * g_clive : 64; g_live = realloc(g_live, (size_t)g_clive * sizeof *g_live); }
        if (g_live) g_live[g_nlive++] = p;
    }
    pthread_mutex_unlock(&hvk.mu);
}
void hvk_pool_del(const void *mem);
void hvk_pool_del(const void *mem) { pthread_mutex_lock(&hvk.mu); nvn_pool_del(mem); pthread_mutex_unlock(&hvk.mu); }
// nvn_pools.h's hooks, under hvk.mu: nothing to make resident; a removed pool's import goes when the GPU is done with it
void hvk_pool_hook_use(nvn_pool *p);
void hvk_pool_hook_use(nvn_pool *p) { (void)p; }
void hvk_pool_hook_del(nvn_pool *p);
void hvk_pool_hook_del(nvn_pool *p) {
    hvk_pool *b = p->backend;
    if (!b) return;
    for (int i = 0; i < g_nlive; i++) if (g_live[i] == b) { g_live[i] = g_live[--g_nlive]; break; }
    hvk_defer(HVK_DEL_IMPORT, b, 0, 0);
}
int hvk_buf(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off, VkDeviceAddress *dev) {
    const nvn_pool_ref *c = nvn_pool_cached(addr, len ? len : 1); nvn_pool_ref r;
    if (!c) { pthread_mutex_lock(&hvk.mu); int ok = nvn_pool_lookup(addr, len ? len : 1, &r); pthread_mutex_unlock(&hvk.mu); if (!ok) return 0; c = &r; }
    const hvk_pool *p = c->backend;
    if (!p || !p->im.buf) return 0;
    *buf = p->im.buf; *off = addr - c->base;
    if (dev) *dev = p->im.addr ? p->im.addr + *off : 0;
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
// Guest memory about to be decommitted (be_guest_unmap, design 1.5 and 1.6): an import under it must go first. A game finalizes its pools before it frees their memory, so a pool
// still live there is a guest error: its import is released after the GPU has finished, and the GPU can no longer address the pool (said once)
void hvk_guest_unmap(void *mem, uint64_t size);
void hvk_guest_unmap(void *mem, uint64_t size) {
    uintptr_t lo = (uintptr_t)mem, hi = lo + size; int n = 0;
    pthread_mutex_lock(&hvk.mu);
    for (int i = 0; i < g_nlive; i++) if (g_live[i]->im.buf && (uintptr_t)g_live[i]->im.base < hi && lo < (uintptr_t)g_live[i]->im.base + g_live[i]->im.bytes) n++;
    pthread_mutex_unlock(&hvk.mu);
    if (!n) return;
    extern void be_finish(void);
    be_finish();
    pthread_mutex_lock(&hvk.mu);
    for (int i = 0; i < g_nlive; i++) {
        hvk_pool *p = g_live[i];
        if (!p->im.buf || (uintptr_t)p->im.base >= hi || lo >= (uintptr_t)p->im.base + p->im.bytes) continue;
        HVK_RED_ONCE("guest memory %p + 0x%llx decommitted under a live memory pool %p + 0x%llx (first case): its import is released, the GPU no longer addresses it",
                     mem, (unsigned long long)size, (void *)p->lo, (unsigned long long)(p->hi - p->lo));
        hostvk_import_release(&hvk.dev, &p->im);   // p->im zeroed: lookups find the pool and no buffer
        atomic_fetch_add(&g_pool_gen, 1);         // the per-thread caches hold the old buffer
    }
    pthread_mutex_unlock(&hvk.mu);
}

// ---------- The ring of the open submission ----------
struct hvk_chunk { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; VkDeviceSize size, used; VkDeviceAddress dev; hvk_chunk *next; };
enum { CHUNK = 4u << 20 };
static hvk_chunk *g_free_chunks;   // submit thread only
static hvk_chunk **g_cur;          // the open submission's list
static void chunk_destroy(hvk_chunk *c) {
    if (c->mem) vkUnmapMemory(hvk.d, c->mem);
    vkDestroyBuffer(hvk.d, c->buf, NULL); free_mem(c->mem); free(c);
}
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
            if (hvk_buffer_make(c->size, hvk_ring_usage(), 1, &c->buf, &c->mem, &map)) { free(c); hostvk_log("[RED] [hostvk] ring chunk of %llu bytes: allocation failed", (unsigned long long)n); return 0; }
            c->map = map;
            if (hvk.dev.caps.buffer_device_address) { VkBufferDeviceAddressInfo ba = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, c->buf}; c->dev = vkGetBufferDeviceAddress(hvk.d, &ba); }
        }
        c->used = 0; c->next = *g_cur; *g_cur = c; at = 0;
    }
    c->used = at + n;
    *out = (hvk_alloc){c->buf, at, c->map + at, c->dev ? c->dev + at : 0};
    return 1;
}

// ---------- Deferred release ----------
typedef struct { hvk_del_kind kind; void *obj; uint64_t a, b, fence; } del_ent;
static del_ent *g_del; static size_t g_ndel, g_cdel; static pthread_mutex_t g_del_mu = PTHREAD_MUTEX_INITIALIZER;
extern _Atomic uint64_t hvk_last_fence;   // vk_commands.c: the last fence committed
static void del_now(const del_ent *e) {
    switch (e->kind) {
        case HVK_DEL_IMPORT: { hvk_pool *p = e->obj; hostvk_import_release(&hvk.dev, &p->im); free(p); break; }
        case HVK_DEL_BUFFER: vkDestroyBuffer(hvk.d, (VkBuffer)e->obj, NULL); if (e->a) free_mem((VkDeviceMemory)(uintptr_t)e->a); break;
        case HVK_DEL_IMAGE: vkDestroyImage(hvk.d, (VkImage)e->obj, NULL); if (e->a) free_mem((VkDeviceMemory)(uintptr_t)e->a); break;
        case HVK_DEL_VIEW: vkDestroyImageView(hvk.d, (VkImageView)e->obj, NULL); break;
        case HVK_DEL_BUFVIEW: vkDestroyBufferView(hvk.d, (VkBufferView)e->obj, NULL); break;
        case HVK_DEL_FREE: free(e->obj); break;
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
        if (!have) return;
        del_now(&e);
    }
}
void hvk_mem_shutdown(void) {
    hvk_deferred_run(UINT64_MAX);
    while (g_free_chunks) { hvk_chunk *c = g_free_chunks; g_free_chunks = c->next; chunk_destroy(c); }
    pthread_mutex_lock(&hvk.mu);
    for (int i = 0; i < g_nlive; i++) hostvk_import_release(&hvk.dev, &g_live[i]->im);   // the pools the game did not finalize: their table entries stay, with no buffer
    g_nlive = 0;
    pthread_mutex_unlock(&hvk.mu);
}
