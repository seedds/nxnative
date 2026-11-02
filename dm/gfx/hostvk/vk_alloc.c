// The Vulkan backend's device memory allocator (vk_alloc.h says what it does and why; decision D2, step D2)
#include "plat/plat.h"
#include "vk_alloc.h"
#include "vk_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { K_FREE, K_LINEAR, K_OPTIMAL };   // a range's kind: optimal-tiling images and linear resources (buffers, linear images) keep a page of bufferImageGranularity apart
typedef struct { VkDeviceSize off, size; uint8_t kind; } range_t;
typedef struct block {
    VkDeviceMemory mem;
    VkDeviceSize size, used;   // used: the bytes of its used ranges (padded to the atom size where the type needs it)
    uint32_t type;
    uint8_t *map;              // the block mapped (a host-visible type), NULL otherwise
    range_t *r; int n, cap;    // sorted by offset, covering [0, size); two free ranges are never neighbours
    int live;                  // used ranges
    int empty; uint64_t empty_since;   // left empty at that fence stamp (hvk_alloc_trim)
} block_t;
struct hvk_mem {
    block_t *b;                // NULL: a VkDeviceMemory of its own
    VkDeviceMemory mem;
    VkDeviceSize off, size, span;   // size: the resource's; span: what it takes in the block (size padded to the atom size)
    uint8_t *map;
    uint32_t type;
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   // everything below; a leaf lock
typedef struct { block_t **b; int n, cap; VkDeviceSize block_size; VkMemoryPropertyFlags flags; } type_t;   // a memory type's blocks
static type_t g_type[VK_MAX_MEMORY_TYPES];
static VkDeviceSize g_gran = 1, g_atom = 1, g_test_gran;
static hvk_alloc_stats g_st;
static _Atomic int g_nempty;      // empty blocks waiting for their grace (hvk_alloc_trim's quick exit)
static _Atomic uint64_t g_refused;
static _Atomic uint32_t g_live_peak;
extern _Atomic uint64_t hvk_last_fence;   // vk_commands.c

static VkDeviceSize up(VkDeviceSize v, VkDeviceSize a) { return a > 1 ? (v + a - 1) / a * a : v; }
static VkDeviceSize down(VkDeviceSize v, VkDeviceSize a) { return a > 1 ? v / a * a : v; }
static int conflict(uint8_t a, uint8_t b) { return (a == K_LINEAR && b == K_OPTIMAL) || (a == K_OPTIMAL && b == K_LINEAR); }
static int non_coherent(uint32_t type) { VkMemoryPropertyFlags f = g_type[type].flags; return (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); }
static uint64_t stamp(void) { uint64_t f = hvk_open_fence(); return f ? f : atomic_load(&hvk_last_fence); }

// ---------- The count ----------
static int reclaim_locked(void);
static void count_red(const char *what, VkDeviceSize bytes, int locked) {
    uint64_t n = atomic_fetch_add(&g_refused, 1);
    if (n >= 8) { if (n == 8) hostvk_log("[RED] [hostvk] device memory: more allocations refused for the count; the shutdown line gives how many"); return; }
    uint32_t own = 0, shared = 0, pools = 0; hvk_import_counts(&own, &shared, &pools);
    if (!locked) pthread_mutex_lock(&g_mu);
    uint32_t blocks = g_st.blocks, owns = g_st.own;
    if (!locked) pthread_mutex_unlock(&g_mu);
    uint32_t live = atomic_load(&hvk.dev.allocations), mine = blocks + owns, imp = own + shared;
    hostvk_log("[RED] [hostvk] device memory: %s of %llu bytes refused: %u of %u allocations live (maxMemoryAllocationCount %u%s): %u blocks, %u own allocations, "
               "%u pool imports (%u shared by %u pools), %u other", what, (unsigned long long)bytes, live, hvk.dev.caps.max_allocations, hvk.dev.caps.device_max_allocations,
               hvk.dev.caps.max_allocations != hvk.dev.caps.device_max_allocations ? ", lowered by DM_VK_MAX_ALLOCATIONS" : "", blocks, owns, imp, shared, pools,
               live > mine + imp ? live - mine - imp : 0);
}
static void note_live(uint32_t live) { uint32_t p = atomic_load(&g_live_peak); while (live > p && !atomic_compare_exchange_weak(&g_live_peak, &p, live)) {} }
void hvk_alloc_note_live(void) { note_live(atomic_load(&hvk.dev.allocations)); }
static int take(const char *what, VkDeviceSize bytes, int locked, int quiet) {   // quiet: the caller has another way (no red line, no refusal counted)
    for (int pass = 0; pass < 2; pass++) {
        uint32_t live = atomic_fetch_add(&hvk.dev.allocations, 1) + 1;
        if (live <= hvk.dev.caps.max_allocations) { note_live(live); return 1; }
        atomic_fetch_sub(&hvk.dev.allocations, 1);
        if (pass) break;
        int freed;
        if (locked) freed = reclaim_locked(); else { pthread_mutex_lock(&g_mu); freed = reclaim_locked(); pthread_mutex_unlock(&g_mu); }
        if (!freed) break;
    }
    if (!quiet) count_red(what, bytes, locked);
    return 0;
}
int hvk_alloc_count_take(const char *what, VkDeviceSize bytes) { return take(what, bytes, 0, 0); }
void hvk_alloc_count_give(void) { atomic_fetch_sub(&hvk.dev.allocations, 1); }

// ---------- Blocks ----------
static void peaks(void) {
    if (g_st.blocks > g_st.blocks_peak) g_st.blocks_peak = g_st.blocks;
    if (g_st.own > g_st.own_peak) g_st.own_peak = g_st.own;
    if (g_st.own_bytes > g_st.own_bytes_peak) g_st.own_bytes_peak = g_st.own_bytes;
    if (g_st.block_bytes > g_st.block_bytes_peak) { g_st.block_bytes_peak = g_st.block_bytes; g_st.used_at_peak = g_st.used_bytes; }
    else if (g_st.block_bytes == g_st.block_bytes_peak && g_st.used_bytes > g_st.used_at_peak) g_st.used_at_peak = g_st.used_bytes;
}
static void block_free(block_t *b) {   // g_mu held: out of its type's list, back to the driver
    type_t *t = &g_type[b->type];
    for (int i = 0; i < t->n; i++) if (t->b[i] == b) { t->b[i] = t->b[--t->n]; break; }
    if (b->empty) { b->empty = 0; atomic_fetch_sub(&g_nempty, 1); g_st.empty--; }
    if (b->map) vkUnmapMemory(hvk.d, b->mem);
    vkFreeMemory(hvk.d, b->mem, NULL);
    hvk_alloc_count_give();
    g_st.blocks--; g_st.block_bytes -= b->size;
    free(b->r); free(b);
}
static int reclaim_locked(void) {
    int n = 0;
    for (uint32_t ty = 0; ty < VK_MAX_MEMORY_TYPES; ty++)
        for (int i = g_type[ty].n - 1; i >= 0; i--) if (g_type[ty].b[i]->empty) { block_free(g_type[ty].b[i]); n++; }
    return n;
}
int hvk_alloc_reclaim(void) { pthread_mutex_lock(&g_mu); int n = reclaim_locked(); pthread_mutex_unlock(&g_mu); return n; }
void hvk_alloc_trim(uint64_t done) {
    if (!atomic_load(&g_nempty)) return;
    pthread_mutex_lock(&g_mu);
    for (uint32_t ty = 0; ty < VK_MAX_MEMORY_TYPES; ty++)
        for (int i = g_type[ty].n - 1; i >= 0; i--) {
            block_t *b = g_type[ty].b[i];
            if (b->empty && (done == UINT64_MAX || done >= b->empty_since + HVK_ALLOC_GRACE)) block_free(b);
        }
    pthread_mutex_unlock(&g_mu);
}
static VkMemoryAllocateFlagsInfo *device_address_flags(VkMemoryAllocateFlagsInfo *fl) {   // every allocation can back a buffer with a device address
    *fl = (VkMemoryAllocateFlagsInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    return hvk.dev.caps.buffer_device_address ? fl : NULL;
}
static VkResult block_new(uint32_t type, VkDeviceSize need, block_t **out) {   // g_mu held. A refusal of the count or of the driver logs a red line
    *out = NULL;
    type_t *t = &g_type[type];
    if (t->n == t->cap) {
        int c = t->cap ? 2 * t->cap : 8; block_t **nb = realloc(t->b, (size_t)c * sizeof *nb);
        if (!nb) return VK_ERROR_OUT_OF_HOST_MEMORY;
        t->b = nb; t->cap = c;
    }
    block_t *b = calloc(1, sizeof *b);
    range_t *r0 = malloc(16 * sizeof *r0);
    if (!b || !r0) { free(b); free(r0); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    if (!take("a block", t->block_size, 1, 0)) { free(b); free(r0); return VK_ERROR_TOO_MANY_OBJECTS; }
    VkMemoryAllocateFlagsInfo fl;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, device_address_flags(&fl), t->block_size, type};
    VkResult r;
    for (;;) {   // out of memory: half the size, down to what the resource needs (the heap may be nearly full)
        r = vkAllocateMemory(hvk.d, &ai, NULL, &b->mem);
        if (r != VK_ERROR_OUT_OF_DEVICE_MEMORY && r != VK_ERROR_OUT_OF_HOST_MEMORY) break;
        if (ai.allocationSize / 2 < need || ai.allocationSize / 2 < (1u << 20)) break;
        ai.allocationSize /= 2;
    }
    if (!r && (t->flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (r = vkMapMemory(hvk.d, b->mem, 0, VK_WHOLE_SIZE, 0, (void **)&b->map))) vkFreeMemory(hvk.d, b->mem, NULL);
    if (r) {
        hvk_alloc_count_give(); free(b); free(r0);
        hostvk_log("[RED] [hostvk] device memory: a block of %llu bytes in memory type %u could not be made (VkResult %d)", (unsigned long long)ai.allocationSize, type, r);
        return r;
    }
    b->size = ai.allocationSize; b->type = type;
    b->r = r0; b->cap = 16; b->r[0] = (range_t){0, b->size, K_FREE}; b->n = 1;
    t->b[t->n++] = b;
    g_st.blocks++; g_st.block_bytes += b->size;
    *out = b;
    return VK_SUCCESS;
}
// The best fit in b for span bytes aligned to align of kind: the index of the free range and the offset, or -1
static int fit(const block_t *b, VkDeviceSize span, VkDeviceSize align, uint8_t kind, VkDeviceSize *at) {
    int best = -1; VkDeviceSize best_size = ~(VkDeviceSize)0;
    if (b->size - b->used < span) return -1;
    for (int i = 0; i < b->n; i++) {
        const range_t *r = &b->r[i];
        if (r->kind != K_FREE || r->size < span || r->size >= best_size) continue;
        VkDeviceSize o = up(r->off, align);
        // a neighbour of the other kind on the same page of bufferImageGranularity: start on the next page
        if (g_gran > 1 && i > 0 && conflict(b->r[i - 1].kind, kind) && (b->r[i - 1].off + b->r[i - 1].size - 1) / g_gran == o / g_gran) o = up(up(o, g_gran), align);
        if (o + span > r->off + r->size) continue;
        if (g_gran > 1 && i + 1 < b->n && conflict(kind, b->r[i + 1].kind) && (o + span - 1) / g_gran == b->r[i + 1].off / g_gran) continue;
        best = i; best_size = r->size; *at = o;
        if (r->size == span) break;
    }
    return best;
}
static int take_range(block_t *b, int i, VkDeviceSize off, VkDeviceSize span, uint8_t kind) {   // split free range i around [off, off + span)
    range_t r = b->r[i], parts[3]; int k = 0;
    if (off > r.off) parts[k++] = (range_t){r.off, off - r.off, K_FREE};
    parts[k++] = (range_t){off, span, kind};
    if (off + span < r.off + r.size) parts[k++] = (range_t){off + span, r.off + r.size - off - span, K_FREE};
    if (b->n + k - 1 > b->cap) {
        int c = 2 * b->cap; range_t *nr = realloc(b->r, (size_t)c * sizeof *nr);
        if (!nr) return 0;
        b->r = nr; b->cap = c;
    }
    memmove(b->r + i + k, b->r + i + 1, (size_t)(b->n - i - 1) * sizeof *b->r);
    memcpy(b->r + i, parts, (size_t)k * sizeof *parts);
    b->n += k - 1; b->used += span; b->live++;
    if (b->empty) { b->empty = 0; atomic_fetch_sub(&g_nempty, 1); g_st.empty--; }
    return 1;
}
static void give_range(block_t *b, VkDeviceSize off) {   // g_mu held: the used range at off freed and joined with its free neighbours
    int lo = 0, hi = b->n - 1, i = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (b->r[m].off == off) { i = m; break; } if (b->r[m].off < off) lo = m + 1; else hi = m - 1; }
    if (i < 0 || b->r[i].kind == K_FREE) { HVK_RED_ONCE("device memory: a range given back twice, or never taken (offset %llu): ignored", (unsigned long long)off); return; }
    b->used -= b->r[i].size; b->live--; b->r[i].kind = K_FREE;
    if (i + 1 < b->n && b->r[i + 1].kind == K_FREE) { b->r[i].size += b->r[i + 1].size; memmove(b->r + i + 1, b->r + i + 2, (size_t)(b->n - i - 2) * sizeof *b->r); b->n--; }
    if (i > 0 && b->r[i - 1].kind == K_FREE) { b->r[i - 1].size += b->r[i].size; memmove(b->r + i, b->r + i + 1, (size_t)(b->n - i - 1) * sizeof *b->r); b->n--; }
    if (!b->live) { b->empty = 1; b->empty_since = stamp(); atomic_fetch_add(&g_nempty, 1); g_st.empty++; }
}

// ---------- Allocations ----------
static VkResult own_alloc(VkMemoryRequirements req, uint32_t type, VkBuffer dbuf, VkImage dimg, int quiet, hvk_mem *m) {   // g_mu held; with VkMemoryDedicatedAllocateInfo
    if (!take(dimg ? "an image's own allocation" : "a buffer's own allocation", req.size, 1, quiet)) return VK_ERROR_TOO_MANY_OBJECTS;
    VkMemoryAllocateFlagsInfo fl;
    VkMemoryDedicatedAllocateInfo di = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, device_address_flags(&fl), dimg, dbuf};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &di, req.size, type};
    VkResult r = vkAllocateMemory(hvk.d, &ai, NULL, &m->mem);
    if (r) { hvk_alloc_count_give(); m->mem = VK_NULL_HANDLE; return r; }
    if ((g_type[type].flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && vkMapMemory(hvk.d, m->mem, 0, VK_WHOLE_SIZE, 0, (void **)&m->map)) {
        vkFreeMemory(hvk.d, m->mem, NULL); hvk_alloc_count_give(); m->mem = VK_NULL_HANDLE; return VK_ERROR_MEMORY_MAP_FAILED;
    }
    m->b = NULL; m->off = 0; m->size = m->span = req.size; m->type = type;
    g_st.own++; g_st.own_bytes += req.size; g_st.allocs++;
    return VK_SUCCESS;
}
static VkResult block_alloc(VkMemoryRequirements req, uint32_t type, uint8_t kind, hvk_mem *m) {   // g_mu held
    VkDeviceSize align = req.alignment ? req.alignment : 1, span = req.size ? req.size : 1;
    if (non_coherent(type)) { if (align < g_atom) align = g_atom; span = up(span, g_atom); }
    type_t *t = &g_type[type];
    block_t *b = NULL; int at = -1; VkDeviceSize off = 0;
    for (int pass = 0; pass < 2 && at < 0; pass++)   // the blocks in use first, then the empty ones (which would go back to the driver)
        for (int i = 0; i < t->n && at < 0; i++) if (t->b[i]->empty == pass && (at = fit(t->b[i], span, align, kind, &off)) >= 0) b = t->b[i];
    if (at < 0) {
        VkResult r = block_new(type, up(span, align), &b);
        if (r) return r;
        if ((at = fit(b, span, align, kind, &off)) < 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;   // a smaller block than asked for (out of memory) that still cannot hold it
    }
    if (!take_range(b, at, off, span, kind)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    m->b = b; m->mem = b->mem; m->off = off; m->size = req.size; m->span = span; m->type = type; m->map = b->map ? b->map + off : NULL;
    g_st.used_bytes += req.size; g_st.allocs++;
    return VK_SUCCESS;
}
// Memory for a resource with these requirements: its own allocation where it is large, the driver prefers it or the caller asks for it, else a range of a block
static VkResult alloc(const VkMemoryRequirements2 *rq, const VkMemoryDedicatedRequirements *dr, VkBuffer dbuf, VkImage dimg, uint8_t kind, unsigned flags, hvk_mem **out) {
    *out = NULL;
    VkMemoryRequirements req = rq->memoryRequirements;
    VkMemoryPropertyFlags want = flags & HVK_MEM_HOST ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = hvk_mem_type(req.memoryTypeBits, want, flags & HVK_MEM_HOST ? want : 0);
    if (type == UINT32_MAX || !g_type[type].block_size) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    hvk_mem *m = calloc(1, sizeof *m);
    if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
    int required = dr->requiresDedicatedAllocation, own = required || dr->prefersDedicatedAllocation || (flags & HVK_MEM_OWN) || req.size > g_type[type].block_size / 2;
    VkResult r;
    pthread_mutex_lock(&g_mu);
    if (!own) r = block_alloc(req, type, kind, m);
    else {
        // an allocation of its own that was only preferred, refused for the count: a range of a block, where one can hold it
        int fallback = !required && !(flags & HVK_MEM_OWN) && req.size <= g_type[type].block_size;
        r = own_alloc(req, type, dbuf, dimg, fallback, m);
        if (r == VK_ERROR_TOO_MANY_OBJECTS && fallback) r = block_alloc(req, type, kind, m);
    }
    if (!r) peaks();
    pthread_mutex_unlock(&g_mu);
    if (r) { free(m); return r; }
    *out = m;
    return VK_SUCCESS;
}
void hvk_mem_free(hvk_mem *m) {
    if (!m) return;
    pthread_mutex_lock(&g_mu);
    if (m->b) { give_range(m->b, m->off); g_st.used_bytes -= m->size; }
    else {
        if (m->map) vkUnmapMemory(hvk.d, m->mem);
        vkFreeMemory(hvk.d, m->mem, NULL);
        hvk_alloc_count_give();
        g_st.own--; g_st.own_bytes -= m->size;
    }
    g_st.frees++;
    pthread_mutex_unlock(&g_mu);
    free(m);
}
VkResult hvk_mem_buffer(VkDeviceSize n, VkBufferUsageFlags usage, unsigned flags, VkBuffer *buf, hvk_mem **mem, void **map) {
    *buf = VK_NULL_HANDLE; *mem = NULL; if (map) *map = NULL;
    if (!hvk.dev.caps.buffer_device_address) usage &= ~VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n ? n : 16, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkResult r = vkCreateBuffer(hvk.d, &bi, NULL, buf);
    if (r) { *buf = VK_NULL_HANDLE; return r; }
    VkMemoryDedicatedRequirements dr = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 rq = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dr};
    VkBufferMemoryRequirementsInfo2 ri = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, NULL, *buf};
    vkGetBufferMemoryRequirements2(hvk.d, &ri, &rq);
    r = alloc(&rq, &dr, *buf, VK_NULL_HANDLE, K_LINEAR, flags, mem);
    if (!r) r = vkBindBufferMemory(hvk.d, *buf, (*mem)->mem, (*mem)->off);
    if (!r && (flags & HVK_MEM_HOST) && !(*mem)->map) r = VK_ERROR_MEMORY_MAP_FAILED;
    if (r) { vkDestroyBuffer(hvk.d, *buf, NULL); hvk_mem_free(*mem); *buf = VK_NULL_HANDLE; *mem = NULL; return r; }
    if (map) *map = (*mem)->map;
    return VK_SUCCESS;
}
void hvk_mem_buffer_free(VkBuffer buf, hvk_mem *mem) { if (buf) vkDestroyBuffer(hvk.d, buf, NULL); hvk_mem_free(mem); }
VkResult hvk_mem_image(VkImage img, int linear, unsigned flags, hvk_mem **mem) {
    *mem = NULL;
    VkMemoryDedicatedRequirements dr = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 rq = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dr};
    VkImageMemoryRequirementsInfo2 ri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, NULL, img};
    vkGetImageMemoryRequirements2(hvk.d, &ri, &rq);
    VkResult r = alloc(&rq, &dr, VK_NULL_HANDLE, img, linear ? K_LINEAR : K_OPTIMAL, flags, mem);
    if (!r) r = vkBindImageMemory(hvk.d, img, (*mem)->mem, (*mem)->off);
    if (r) { hvk_mem_free(*mem); *mem = NULL; }
    return r;
}
void *hvk_mem_map(const hvk_mem *m) { return m ? m->map : NULL; }
VkDeviceMemory hvk_mem_handle(const hvk_mem *m) { return m ? m->mem : VK_NULL_HANDLE; }
VkDeviceSize hvk_mem_offset(const hvk_mem *m) { return m ? m->off : 0; }
VkDeviceSize hvk_mem_size(const hvk_mem *m) { return m ? m->size : 0; }
int hvk_mem_is_own(const hvk_mem *m) { return m && !m->b; }
void hvk_mem_atom_range(const hvk_mem *m, VkDeviceSize off, VkDeviceSize n, VkDeviceSize *mem_off, VkDeviceSize *mem_n) {
    VkDeviceSize whole = m->b ? m->b->size : m->size;   // the VkDeviceMemory's size
    if (off > m->size) off = m->size;
    if (n == VK_WHOLE_SIZE || off + n > m->size) n = m->size - off;
    VkDeviceSize lo = down(m->off + off, g_atom), hi = up(m->off + off + n, g_atom);
    if (hi > whole) hi = whole;   // the end of the allocation needs no multiple of the atom size (VUID-VkMappedMemoryRange-size-01390)
    *mem_off = lo; *mem_n = hi - lo;
}
static void sync_range(const hvk_mem *m, VkDeviceSize off, VkDeviceSize n, int flush) {
    if (!m || !m->map || !non_coherent(m->type)) return;
    VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, m->mem, 0, 0};
    hvk_mem_atom_range(m, off, n, &r.offset, &r.size);
    if (flush) vkFlushMappedMemoryRanges(hvk.d, 1, &r); else vkInvalidateMappedMemoryRanges(hvk.d, 1, &r);
}
void hvk_mem_flush(const hvk_mem *m, VkDeviceSize off, VkDeviceSize n) { sync_range(m, off, n, 1); }
void hvk_mem_invalidate(const hvk_mem *m, VkDeviceSize off, VkDeviceSize n) { sync_range(m, off, n, 0); }

// ---------- Bring-up, statistics, shutdown ----------
void hvk_alloc_test_granularity(VkDeviceSize g) { g_test_gran = g; }
VkDeviceSize hvk_alloc_block_size(uint32_t type) { return type < VK_MAX_MEMORY_TYPES ? g_type[type].block_size : 0; }
static VkDeviceSize pow2_floor(VkDeviceSize v) { VkDeviceSize p = 1; while (p <= v / 2) p *= 2; return p; }
void hvk_alloc_init(void) {
    const VkPhysicalDeviceMemoryProperties *mp = &hvk.dev.mem;
    pthread_mutex_lock(&g_mu);
    memset(&g_st, 0, sizeof g_st); atomic_store(&g_refused, 0); atomic_store(&g_live_peak, 0); atomic_store(&g_nempty, 0);
    g_gran = hvk.lim.bufferImageGranularity ? hvk.lim.bufferImageGranularity : 1;
    if (g_test_gran > g_gran) g_gran = g_test_gran;
    g_atom = hvk.lim.nonCoherentAtomSize ? hvk.lim.nonCoherentAtomSize : 1;
    g_st.granularity = g_gran;
    char types[600]; int k = 0; types[0] = 0;
    for (uint32_t i = 0; i < mp->memoryTypeCount && i < VK_MAX_MEMORY_TYPES; i++) {
        VkMemoryPropertyFlags f = mp->memoryTypes[i].propertyFlags;
        VkDeviceSize heap = mp->memoryHeaps[mp->memoryTypes[i].heapIndex].size, s = f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? 64u << 20 : 16u << 20;
        if (heap / 8 < s) s = heap / 8 < (1u << 20) ? (1u << 20) : pow2_floor(heap / 8);
        g_type[i].block_size = s; g_type[i].flags = f;
        if (k >= 0 && (size_t)k < sizeof types)
            k += snprintf(types + k, sizeof types - (size_t)k, "%s%u %s%s%s%s%s(heap %u, %.0f MiB) %llu MiB", i ? ", " : "", i, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "D" : "",
                          f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "V" : "", f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? "C" : "", f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "H" : "",
                          f & (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? " " : "", mp->memoryTypes[i].heapIndex, (double)heap / 1048576.0,
                          (unsigned long long)(s >> 20));
    }
    pthread_mutex_unlock(&g_mu);
    const hostvk_caps *c = &hvk.dev.caps;
    hostvk_log("[hostvk] device memory: maxMemoryAllocationCount %u; blocks per memory type (D device-local, V host-visible, C coherent, H cached): %s; an allocation of its own "
               "above half a block or where the driver prefers one; bufferImageGranularity %llu%s, nonCoherentAtomSize %llu; an empty block goes back after %d submissions",
               c->device_max_allocations, types, (unsigned long long)g_gran,
               g_gran != hvk.lim.bufferImageGranularity ? " (a test's)" : "", (unsigned long long)g_atom, HVK_ALLOC_GRACE);
    if (c->max_allocations != c->device_max_allocations)
        hostvk_log("[hostvk] DM_VK_MAX_ALLOCATIONS=%u: the backend keeps under %u live allocations instead of the device's %u (a test switch: a limited driver on this device)",
                   c->max_allocations, c->max_allocations, c->device_max_allocations);
}
void hvk_alloc_get_stats(hvk_alloc_stats *s) {
    pthread_mutex_lock(&g_mu);
    *s = g_st; s->refused = atomic_load(&g_refused); s->live_peak = atomic_load(&g_live_peak);
    pthread_mutex_unlock(&g_mu);
}
void hvk_alloc_line(char *out, size_t n) {
    hvk_alloc_stats s; hvk_alloc_get_stats(&s);
    uint32_t own = 0, shared = 0, pools = 0; hvk_import_counts(&own, &shared, &pools);
    double mib = 1048576.0, waste = (double)(s.block_bytes_peak - s.used_at_peak);
    snprintf(out, n, "blocks %u (%.1f MiB, %.1f MiB in use, %u empty), peak %u blocks %.1f MiB with %.1f MiB in use: waste %.1f MiB (%.0f %%); own allocations %u (%.1f MiB), "
             "peak %u (%.1f MiB); %llu made, %llu given back; live allocations peak %u of %u; pool imports %u own, %u shared by %u pools; refused for the count %llu",
             s.blocks, (double)s.block_bytes / mib, (double)s.used_bytes / mib, s.empty, s.blocks_peak, (double)s.block_bytes_peak / mib, (double)s.used_at_peak / mib, waste / mib,
             s.block_bytes_peak ? 100.0 * waste / (double)s.block_bytes_peak : 0.0, s.own, (double)s.own_bytes / mib, s.own_peak, (double)s.own_bytes_peak / mib,
             (unsigned long long)s.allocs, (unsigned long long)s.frees, s.live_peak, hvk.dev.caps.max_allocations, own, shared, pools, (unsigned long long)s.refused);
}
void hvk_alloc_shutdown(void) {
    char line[600]; hvk_alloc_line(line, sizeof line);
    hostvk_log("[hostvk] device memory at shutdown: %s", line);
    pthread_mutex_lock(&g_mu);
    int leaked = 0;
    for (uint32_t ty = 0; ty < VK_MAX_MEMORY_TYPES; ty++) {
        while (g_type[ty].n) { block_t *b = g_type[ty].b[g_type[ty].n - 1]; leaked += b->live; block_free(b); }
        free(g_type[ty].b); g_type[ty].b = NULL; g_type[ty].n = g_type[ty].cap = 0; g_type[ty].block_size = 0;
    }
    uint32_t own = g_st.own;
    pthread_mutex_unlock(&g_mu);
    if (leaked || own) hostvk_log("[RED] [hostvk] device memory at shutdown: %d ranges of blocks and %u own allocations still held (not given back by their owners)", leaked, own);
}
