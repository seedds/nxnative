// Memory pool table of the NVN replay, shared by every graphics backend (pure C, every host): each pool's storage range with an opaque handle of the backend's object over it, the
// address → pool lookup and a per-thread cache of lookups. Moved out of the Metal replay layer (nvn_mtl.m: pool_ent, pool_find, pc_get, pc_put, g_pool_gen and the table part of
// mtl_pool_add / mtl_pool_del / mtl_addr / mtl_gpu_addr) with no change of logic (docs/design/vulkan-backend.md, sections 1.3 and 1.7, step M2b); the backend creates and releases its
// objects, and keeps what is its own per pool (Metal: the residency set, the buffer references, the table of dead pools) through the hooks below.
// It has no lock of its own, as before the move: every function but nvn_pool_cached is called with the backend's lock held, the one that also guards what the backend keeps per pool
// (nvn_mtl.m: g_mu); nvn_pool_cached reads only the calling thread's cache and the atomic generation
#pragma once
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

// A pool: lo, hi, base and gpu are as the backend gave them at nvn_pool_add (mem, mem + size, where its object starts, that object's GPU address); an address's offset in the object is addr - base
typedef struct {
    uintptr_t lo, hi;      // the pool's storage, [mem, mem + size)
    uintptr_t base;        // where the backend's object over it starts (Metal: lo rounded down to the host page)
    void *backend;         // the backend's object, not retained by the table (Metal: the zero-copy id<MTLBuffer>; the backend holds the reference, g_keep)
    uint64_t gpu;          // the backend's GPU address of base (Metal: the buffer's gpuAddress)
    const void *key;       // mem as nvn_pool_add was given it: what nvn_pool_del finds the pool by
    int res;               // the backend's own mark, 0 when added (Metal: already in the residency set, on demand, see pool_res)
    uint32_t nvn_flags;    // the pool's NVN flags as be_pool_add gives them (the Metal backend records them since step M6, mtl_pool_add_flags, and does not read them)
} nvn_pool;
// What a lookup gives, and the per-thread cache keeps (an empty entry has hi 0)
typedef struct { uintptr_t lo, hi, base; void *backend; uint64_t gpu; } nvn_pool_ref;

// Bumped by every pool add / removal: the per-thread caches made before it are not used (nvn_pool_cached), nor the backend's own caches of lookups (nvn_mtl.m: the vertex buffers')
extern _Atomic uint64_t g_pool_gen;

// A pool over [mem, mem + size): the address index and the per-thread caches are invalid. Pools may overlap (not checked, as before)
void nvn_pool_add(const void *mem, uint64_t size, uint32_t nvn_flags, uintptr_t base, void *backend, uint64_t gpu);
// The pool nvn_pool_add was given mem for: nvn_pool_hook_del sees it, then it goes (the last pool takes its place in the table). None: nothing happens
void nvn_pool_del(const void *mem);
// Address → pool: the last hit first, then a binary search over indices sorted by start address, and a full linear scan only when that fails (pools overlapping). The pool found holds all
// of [addr, addr + len): nvn_pool_hook_use sees it, the calling thread's cache takes it, and *out is its record → 1; none → 0
int nvn_pool_lookup(uint64_t addr, uint64_t len, nvn_pool_ref *out);
int nvn_pool_count(void);   // pools in the table

// ---------- The per-thread cache of lookups (nvn_pool_cached needs no lock) ----------
// Valid while g_pool_gen is unchanged (every pool add / removal bumps it under the backend's lock), holding only pools a lookup found (on Metal: already in the residency set).
// Every buffer binding of every draw used to take g_mu, which game threads creating pools and the GPU completion handler also take, and search the pool index. Two levels: a table direct-mapped
// by 4 KB page, then the last 8 pools found, which also fill the table. Breath of the Wild binds from more pools a frame than 8 entries alone could hold (about 2300 of its 21000 lookups a frame
// took the lock), and its uniform rings move to a new page every few draws (the table alone missed as often)
enum { PC_BITS = 10, PC_RECENT = 8 };
typedef struct { uint64_t gen; unsigned next; nvn_pool_ref r[PC_RECENT], e[1u << PC_BITS]; } nvn_pool_cache;
extern __thread nvn_pool_cache t_pc;
static inline unsigned nvn_pool_cache_slot(uint64_t addr) { return (unsigned)(((addr >> 12) * 0x9E3779B97F4A7C15ull) >> (64 - PC_BITS)); }
// The pool the calling thread's cache holds for [addr, addr + len), NULL when it holds none (then nvn_pool_lookup, under the lock)
static inline const nvn_pool_ref *nvn_pool_cached(uint64_t addr, uint64_t len) {
    if (t_pc.gen != atomic_load_explicit(&g_pool_gen, memory_order_acquire)) return NULL;
    nvn_pool_ref *c = &t_pc.e[nvn_pool_cache_slot(addr)];
    if (addr >= c->lo && addr + len <= c->hi) return c;
    for (unsigned k = 0; k < PC_RECENT; k++) { const nvn_pool_ref *x = &t_pc.r[k]; if (addr >= x->lo && addr + len <= x->hi) { *c = *x; return c; } }
    return NULL;
}

// ---------- Hooks the backend defines (nvn_mtl.m), called with its lock held ----------
// A lookup found pool p, before the calling thread's cache takes it (Metal: pool_res puts its buffer in the residency set the first time, p->res)
void nvn_pool_hook_use(nvn_pool *p);
// Pool p is about to leave the table (nvn_pool_del): the backend lets go of its object (Metal: deferred release, the table of dead pools)
void nvn_pool_hook_del(nvn_pool *p);
