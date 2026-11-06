#include "../nvn/nvn_dirty.h"
#include "../../host/dm.h"
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>

module_t *g_mod;
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static int g_fail;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-52s expected %-4lld → got %-4lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)

static void *page_map(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    return p;
}

enum { GUEST_PAGE_SIZE = 0x1000, CACHE_PROBE_SIZE = 16 };
enum overlap_layout {
    EARLIER_RANGE_ENDS_SOONER,
    EARLIER_RANGE_STARTS_LATER,
};

// Adjacent pools can share a host page after rounding, but only a protected
// descriptor receives a generation on the write fault. Warming the other
// descriptor must not change which range a subsequent lookup selects.
static void *test_shared_page_cache(void *arg) {
    enum overlap_layout layout = (enum overlap_layout)(intptr_t)arg;
    size_t page_size = (size_t)getpagesize();
    size_t mapping_size = 4 * page_size;
    uint8_t *mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) {
        perror("mmap");
        exit(2);
    }

    uint8_t *first_pool;
    uint8_t *second_pool;
    const void *second_pool_only_span;
    size_t first_pool_size;
    size_t second_pool_size;
    const char *description;
    if (layout == EARLIER_RANGE_STARTS_LATER) {
        first_pool = mapping + 2 * page_size - GUEST_PAGE_SIZE;
        first_pool_size = GUEST_PAGE_SIZE;
        second_pool = mapping;
        second_pool_size = 2 * page_size - GUEST_PAGE_SIZE;
        second_pool_only_span = mapping;
        description = "earlier range starts later";
    } else {
        first_pool = mapping;
        first_pool_size = page_size - GUEST_PAGE_SIZE;
        second_pool = mapping + page_size - GUEST_PAGE_SIZE;
        second_pool_size = 2 * page_size;
        second_pool_only_span = mapping + page_size;
        description = "earlier range ends sooner";
    }

    nvn_dirty_add_range(first_pool, first_pool_size);
    nvn_dirty_add_range(second_pool, second_pool_size);
    const void *first_range = nvn_dirty_span(first_pool, CACHE_PROBE_SIZE);
    uint64_t stamp = nvn_dirty_now();
    nvn_dirty_protect(first_pool, CACHE_PROBE_SIZE);
    const void *other_range = nvn_dirty_span(second_pool_only_span, page_size);
    first_pool[0] = 7;

    printf("lookup cache, shared page (%s):\n", description);
    EXPECT("warmed a different containing descriptor", 1, first_range != other_range);
    EXPECT("cached lookup still detects the write", 1,
           nvn_dirty_since(first_pool, CACHE_PROBE_SIZE, stamp));
    EXPECT("cached lookup preserves the first containing range", 1,
           nvn_dirty_span(first_pool, CACHE_PROBE_SIZE) == first_range);

    nvn_dirty_remove_range(first_pool);
    nvn_dirty_remove_range(second_pool);
    munmap(mapping, mapping_size);
    return NULL;
}

enum cache_reuse_phase {
    CACHE_NOT_WARMED,
    CACHE_WARMED,
    REGISTRY_UPDATED,
};

typedef struct {
    const void *probe_address;
    const void *expected_range;
    _Atomic int phase;
    int matched;
} cache_reuse_test_t;

static void *cache_reuse_reader(void *arg) {
    cache_reuse_test_t *test = arg;
    // Warm this thread's cache while only the later slot contains the span.
    nvn_dirty_span(test->probe_address, CACHE_PROBE_SIZE);
    atomic_store(&test->phase, CACHE_WARMED);
    while (atomic_load(&test->phase) != REGISTRY_UPDATED) {
        usleep(100);
    }
    test->matched = nvn_dirty_span(test->probe_address, CACHE_PROBE_SIZE) == test->expected_range;
    return NULL;
}

static void test_cache_slot_reuse(void) {
    size_t page_size = (size_t)getpagesize();
    size_t mapping_size = 4 * page_size;
    uint8_t *mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) {
        perror("mmap");
        exit(2);
    }

    uint8_t *later_pool = mapping + 2 * page_size;
    nvn_dirty_add_range(mapping, page_size);
    nvn_dirty_add_range(later_pool, 2 * page_size);
    const void *first_slot = nvn_dirty_span(mapping, CACHE_PROBE_SIZE);
    const void *later_slot = nvn_dirty_span(later_pool, 2 * page_size);
    nvn_dirty_remove_range(mapping);

    uint8_t *replacement_pool = later_pool + GUEST_PAGE_SIZE;
    cache_reuse_test_t test = {
        .probe_address = replacement_pool,
        .expected_range = first_slot,
        .phase = CACHE_NOT_WARMED,
    };
    pthread_t reader;
    if (pthread_create(&reader, NULL, cache_reuse_reader, &test)) {
        exit(2);
    }
    while (atomic_load(&test.phase) != CACHE_WARMED) {
        usleep(100);
    }

    // Reuse the earlier slot with a pool overlapping the reader's cached range.
    nvn_dirty_add_range(replacement_pool, page_size - GUEST_PAGE_SIZE);
    atomic_store(&test.phase, REGISTRY_UPDATED);
    pthread_join(reader, NULL);
    EXPECT("slot reuse invalidates another thread's cached selection", 1, test.matched);
    EXPECT("reused earlier slot is the canonical match", 1,
           nvn_dirty_span(replacement_pool, CACHE_PROBE_SIZE) == first_slot);

    nvn_dirty_remove_range(replacement_pool);
    EXPECT("retiring a cached match falls back to the remaining range", 1,
           nvn_dirty_span(replacement_pool, CACHE_PROBE_SIZE) == later_slot);
    nvn_dirty_remove_range(later_pool);
    munmap(mapping, mapping_size);
}

int main(void) {
    crash_install();

    for (int i = 0; i < 65; i++) {
        void *p = page_map();
        nvn_dirty_add_range(p, 4096);
        nvn_dirty_remove_range(p);
        munmap(p, 4096);
    }

    void *reused = page_map();
    nvn_dirty_add_range(reused, 4096);
    nvn_dirty_protect(reused, 4096);
    uint64_t stamp = nvn_dirty_now();
    ((volatile uint8_t *)reused)[0] = 1;
    EXPECT("after 65 pool destroys in total, a write to a new pool is dirty", 1, nvn_dirty_since(reused, 4096, stamp));
    nvn_dirty_remove_range(reused);
    munmap(reused, 4096);

    void *live[300];
    for (int i = 0; i < 300; i++) {
        live[i] = page_map();
        nvn_dirty_add_range(live[i], 4096);
    }
    void *overflow = live[299];
    nvn_dirty_protect(overflow, 4096);
    stamp = nvn_dirty_now();
    ((volatile uint8_t *)overflow)[0] = 2;
    EXPECT("pools beyond the live limit are conservatively dirty", 1, nvn_dirty_since(overflow, 4096, stamp));
    for (int i = 0; i < 300; i++) {
        nvn_dirty_remove_range(live[i]);
        munmap(live[i], 4096);
    }

    // More pools alive at once than the table's fixed 4,096 slots used to hold (Breath of the Wild reaches 4,006 in its first shrine): the table grows, and the last pool is still tracked
    enum { NMANY = 5000 };
    void **many = malloc(NMANY * sizeof *many);
    for (int i = 0; i < NMANY; i++) { many[i] = page_map(); nvn_dirty_add_range(many[i], 4096); }
    void *last = many[NMANY - 1];
    EXPECT("5000 live pools: the last one is tracked", 1, nvn_dirty_tracked(last, 4096));
    nvn_dirty_protect(last, 4096);
    stamp = nvn_dirty_now();
    EXPECT("pool 5000 not written after protect: clean", 0, nvn_dirty_since(last, 4096, stamp));
    ((volatile uint8_t *)last)[0] = 5;
    EXPECT("pool 5000 written by the game: dirty", 1, nvn_dirty_since(last, 4096, stamp));
    for (int i = 0; i < NMANY; i++) { nvn_dirty_remove_range(many[i]); munmap(many[i], 4096); }
    free(many);

    // Adjacent pools sharing a host page (pools are 4 KB aligned, host pages 16 KB): the first page of SMBW's ASTC texture pool is shared with a small preceding pool, and was judged untracked and re-uploaded on every use
    size_t pg = (size_t)getpagesize();
    uint8_t *blk = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (blk == MAP_FAILED) { perror("mmap"); exit(2); }
    uint8_t *pa = blk, *pb = blk + pg - 0x1000;           // pool A covers the start of page 0, pool B starts in the last 4 KB of page 0
    size_t na = pg - 0x1000, nb = 2 * pg, tex = pg + 0x800;   // the texture in B spans the shared page and the next one
    nvn_dirty_add_range(pa, na);
    nvn_dirty_add_range(pb, nb);
    EXPECT("texture in the later of two page-sharing pools is tracked", 1, nvn_dirty_tracked(pb, tex));
    nvn_dirty_protect(pb, tex);
    stamp = nvn_dirty_now();
    EXPECT("page-sharing texture not written after protect: clean", 0, nvn_dirty_since(pb, tex, stamp));
    pb[0x10] = 3;
    EXPECT("game writes B's part of the shared page: B's texture dirty", 1, nvn_dirty_since(pb, tex, stamp));
    nvn_dirty_protect(pb, tex);
    stamp = nvn_dirty_now();
    pa[0x10] = 4;   // same physical page, A's part: the page is read-only, the fault must be claimed through B's range or it is treated as a crash
    EXPECT("game writes A's part of the shared page: B's texture conservatively dirty", 1, nvn_dirty_since(pb, tex, stamp));
    nvn_dirty_protect(pb, tex);
    stamp = nvn_dirty_now();
    dm_guest_write_prepare(pa, 0x20); pa[0x20] = 5;   // the host writes A on behalf of the GPU: the shared page must also release B's protection
    EXPECT("host unprotects the shared page before writing: B's texture dirty", 1, nvn_dirty_since(pb, tex, stamp));
    nvn_dirty_remove_range(pb);
    EXPECT("A still tracked after destroying B (ranges keyed by pool start)", 1, nvn_dirty_tracked(pa, na));
    EXPECT("B no longer tracked after destroying B", 0, nvn_dirty_tracked(pb, tex));
    nvn_dirty_remove_range(pa);
    munmap(blk, 4 * pg);
    for (int layout = EARLIER_RANGE_ENDS_SOONER; layout <= EARLIER_RANGE_STARTS_LATER; layout++) {
        // Each case starts on a new thread with an empty lookup cache.
        pthread_t reader;
        if (pthread_create(&reader, NULL, test_shared_page_cache, (void *)(intptr_t)layout)) {
            exit(2);
        }
        pthread_join(reader, NULL);
    }
    test_cache_slot_reuse();
    return g_fail != 0;
}
