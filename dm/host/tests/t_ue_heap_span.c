// Wwise finds its span header by masking an interior pointer returned from Unreal's heap (#996).
#include "../hle_uemalloc.c"
#include <assert.h>
#include <pthread.h>

module_t *g_mod;
int g_nmod;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
int gate_obs_present(int id) { (void)id; return 0; }
void gate_obs_set(int id, void (*callback)(uint64_t *, int)) { (void)id; (void)callback; }

// A game can read a small block it has just freed: TMap::Add(Key, *Found) copies from its own storage after growing it (#2345). Unreal's binned allocator leaves such a block intact.
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define COUNTS_ZONE_BLOCKS 0   // a sanitizer's zones report its whole heap
#else
#define COUNTS_ZONE_BLOCKS 1
#endif
static size_t blocks_in_zone(void) { malloc_statistics_t s; malloc_zone_statistics(g_zone, &s); return s.blocks_in_use; }
static void assert_filled(const uint8_t *p, size_t n, uint8_t v) { for (size_t i = 0; i < n; i++) assert(p[i] == v); }
static void *free_small_blocks(void *arg) {
    uintptr_t seed = (uintptr_t)arg;
    for (unsigned i = 0; i < 20000; i++) {
        size_t n = 16 + ((seed + i) * 37) % 2048;
        uint8_t *p = m_Malloc(NULL, n, 0);
        memset(p, (int)(seed + 1), n);
        p = m_Realloc(NULL, p, n + 64, 0);
        assert_filled(p, n, (uint8_t)(seed + 1));
        m_Free(NULL, p);
    }
    return NULL;
}
static void test_freed_small_blocks(void) {
    enum { kept = 64, grown = 4096, limit = FREED_MAX, more = FREED_RING * 4 };
    uint8_t *freed = m_Malloc(NULL, kept, 0);
    memset(freed, 0xa5, kept);
    m_Free(NULL, freed);
    assert_filled(freed, kept, 0xa5);
    // A growing Realloc moves the block and leaves the old one readable, as the map's copy expects
    uint8_t *old = m_Malloc(NULL, kept, 0);
    memset(old, 0x3c, kept);
    uint8_t *moved = m_Realloc(NULL, old, grown, 0);
    assert(moved != old);
    assert_filled(old, kept, 0x3c);
    assert_filled(moved, kept, 0x3c);
    // A size the block still holds stays put, and a shrink past half gives the larger block back while leaving it readable
    size_t usable = malloc_size(moved);
    assert(m_Realloc(NULL, moved, usable - 1, 0) == moved);
    uint8_t *less = m_Realloc(NULL, moved, kept, 0);
    assert(less != moved && malloc_size(less) < usable);
    assert_filled(moved, kept, 0x3c);
    assert_filled(less, kept, 0x3c);
    m_Free(NULL, less);
    // Shrinking a minimum-size block that cannot get smaller stays in place rather than copying into the same size class
    uint8_t *tiny = m_Malloc(NULL, 16, 0);
    size_t tiny_usable = malloc_size(tiny);
    if (malloc_good_size(8) >= tiny_usable) assert(m_Realloc(NULL, tiny, 8, 0) == tiny);
    // Realloc to 0 also keeps the small block intact briefly
    memset(tiny, 0x6d, 8);
    assert(m_Realloc(NULL, tiny, 0, 0) == NULL);
    assert_filled(tiny, 8, 0x6d);
    // The per-thread ring holds a bounded number of blocks: the oldest is really freed
    for (unsigned i = 0; i < more; i++) m_Free(NULL, m_Malloc(NULL, 128, 0));
    assert(!COUNTS_ZONE_BLOCKS || blocks_in_zone() <= FREED_RING);
    // A block over the limit is not kept
    size_t before = blocks_in_zone();
    m_Free(NULL, m_Malloc(NULL, limit + 4096, 0));
    assert(!COUNTS_ZONE_BLOCKS || blocks_in_zone() == before);
    // Frees from other threads neither evict this thread's just-freed block nor leak after those threads exit
    uint8_t *pinned = m_Malloc(NULL, kept, 0);
    memset(pinned, 0x9b, kept);
    m_Free(NULL, pinned);
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, free_small_blocks, (void *)i));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    assert_filled(pinned, kept, 0x9b);
    assert(!COUNTS_ZONE_BLOCKS || blocks_in_zone() <= FREED_RING);
    // Trim drains the calling thread's ring back to the zone
    m_Trim(NULL);
    assert(!COUNTS_ZONE_BLOCKS || blocks_in_zone() == 0);
}

int main(void) {
    enum { span = 65536, count = 16 };
    uint8_t *blocks[count];
    g_zone = malloc_create_zone(0, 0);
    assert(g_zone);
    for (unsigned i = 0; i < count; ++i) {
        blocks[i] = m_Malloc(NULL, span + (i % 2) * 4096, (i % 4 < 2) ? 4096 : 0);
        assert(((uintptr_t)(blocks[i] + 0x2a0) & ~(uintptr_t)(span - 1)) == (uintptr_t)blocks[i]);
        blocks[i][0x2a0] = (uint8_t)(i + 1);
    }
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *grown = m_Realloc(NULL, blocks[i], 3 * span + 4096, 0);
        assert(((uintptr_t)grown & (span - 1)) == 0 && grown[0x2a0] == i + 1);
        uint8_t *shrunk = m_Realloc(NULL, grown, span, 2 * span);
        assert(((uintptr_t)shrunk & (2 * span - 1)) == 0 && shrunk[0x2a0] == i + 1);
        m_Free(NULL, shrunk);
    }
    uint8_t *small = m_Malloc(NULL, 48, 0);
    small[47] = 0x5a;
    uint8_t *span_block = m_Realloc(NULL, small, span + 4096, 0);
    assert(((uintptr_t)span_block & (span - 1)) == 0 && span_block[47] == 0x5a);
    m_Free(NULL, span_block);
    // A block just under the threshold can grow across it in place, off the boundary: it must be moved with its contents
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *under = m_Malloc(NULL, span - 4096, 0);
        memset(under, (int)(i + 1), span - 4096);
        uint8_t *over = m_Realloc(NULL, under, span + 4096, 0);
        assert(((uintptr_t)over & (span - 1)) == 0 && over[0] == i + 1 && over[span - 4097] == i + 1);
        m_Free(NULL, over);
    }
    // A shrink gives the memory back, at either alignment (keeping the old block would hold its pages for the block's life)
    for (uint32_t alignment = 0; alignment <= 4096; alignment += 4096) {
        enum { big = 8 << 20, kept = 2 * span };
        uint8_t *large = m_Malloc(NULL, big, alignment);
        large[kept - 1] = 0x77;
        uint8_t *less = m_Realloc(NULL, large, kept, alignment);
        assert(((uintptr_t)less & (span - 1)) == 0 && less[kept - 1] == 0x77);
        assert(malloc_size(less) < big);
        m_Free(NULL, less);
    }
    test_freed_small_blocks();
    assert(atomic_load(&g_used) == 0);
    malloc_destroy_zone(g_zone);
    puts("Unreal virtual-span alignment and realloc contents passed");
    return 0;
}
