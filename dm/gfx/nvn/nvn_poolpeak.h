// DM_NVN_POOLPEAK (diagnostic, hle_nvn.c nvnTextureInitialize): which 64 KB pages of a memory pool textures have ever been placed on. A game that sub-allocates its render targets
// from one large pool (Breath of the Wild: gsys's 115 MB render buffer pool) shows here how much of it a scene, a menu or a larger output takes, also when it allocates from both ends
// of the pool, where the highest offset alone says nothing. Pure C, no host state: t_poolpeak.c tests it
#pragma once
#include <stdint.h>
#include <stdlib.h>

typedef struct { uint8_t *map; uint64_t pages, used; } nvn_poolpeak;   // one byte per 64 KB page: 1 = a texture was placed on it

static inline int nvn_poolpeak_init(nvn_poolpeak *p, uint64_t pool_size) {
    p->pages = (pool_size + 0xffff) >> 16; p->used = 0; p->map = calloc(p->pages ? p->pages : 1, 1);
    return p->map != NULL;
}
// Marks the pages [off, off + size) holds; returns how many were not marked before (0: the set did not grow)
static inline uint64_t nvn_poolpeak_add(nvn_poolpeak *p, uint64_t off, uint64_t size) {
    if (!p->map || !size) return 0;
    uint64_t p0 = off >> 16, p1 = (off + size + 0xffff) >> 16, add = 0;
    for (uint64_t q = p0; q < p1 && q < p->pages; q++) if (!p->map[q]) { p->map[q] = 1; add++; }
    p->used += add; return add;
}
// The largest run of pages no texture was ever placed on (pages)
static inline uint64_t nvn_poolpeak_gap(const nvn_poolpeak *p) {
    uint64_t gap = 0, run = 0;
    for (uint64_t q = 0; q < p->pages; q++) { run = p->map[q] ? 0 : run + 1; if (run > gap) gap = run; }
    return gap;
}
