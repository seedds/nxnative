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

// Adjacent pools can both contain a short span after host-page rounding, but
// only the protected descriptor receives a generation on the write fault.
// Warming the other descriptor must not change the canonical first match.
static void *cache_shared_page(void *arg) {
    int reverse = (int)(intptr_t)arg;
    size_t pg = (size_t)getpagesize();
    uint8_t *p = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    uint8_t *a = reverse ? p + 2 * pg - 0x1000 : p;
    uint8_t *b = reverse ? p : p + pg - 0x1000;
    size_t na = reverse ? 0x1000 : pg - 0x1000, nb = reverse ? 2 * pg - 0x1000 : 2 * pg;
    nvn_dirty_add_range(a, na); nvn_dirty_add_range(b, nb);
    const void *first = nvn_dirty_span(a, 16);
    uint64_t stamp = nvn_dirty_now();
    nvn_dirty_protect(a, 16);
    const void *other = nvn_dirty_span(reverse ? p : p + pg, pg);
    a[0] = 7;
    printf("lookup cache, shared page (%s):\n", reverse ? "earlier range starts later" : "earlier range ends sooner");
    EXPECT("warmed a different containing descriptor", 1, first != other);
    EXPECT("cached lookup still detects the write", 1, nvn_dirty_since(a, 16, stamp));
    EXPECT("cached lookup preserves the first containing range", 1, nvn_dirty_span(a, 16) == first);
    nvn_dirty_remove_range(a); nvn_dirty_remove_range(b); munmap(p, 4 * pg);
    return NULL;
}

typedef struct { const void *addr, *expected; _Atomic int phase; int matched; } cache_reuse;
static void *cache_reuse_reader(void *arg) {
    cache_reuse *t = arg;
    nvn_dirty_span(t->addr, 16);   // warm this thread's cache while only the later slot contains the span
    atomic_store(&t->phase, 1);
    while (atomic_load(&t->phase) != 2) usleep(100);
    t->matched = nvn_dirty_span(t->addr, 16) == t->expected;
    return NULL;
}
static void cache_slot_reuse(void) {
    size_t pg = (size_t)getpagesize();
    uint8_t *p = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    nvn_dirty_add_range(p, pg); nvn_dirty_add_range(p + 2 * pg, 2 * pg);
    const void *first_slot = nvn_dirty_span(p, 16), *later_slot = nvn_dirty_span(p + 2 * pg, 2 * pg);
    nvn_dirty_remove_range(p);
    uint8_t *c = p + 2 * pg + 0x1000;
    cache_reuse t = {.addr = c, .expected = first_slot}; pthread_t reader;
    if (pthread_create(&reader, NULL, cache_reuse_reader, &t)) exit(2);
    while (atomic_load(&t.phase) != 1) usleep(100);
    nvn_dirty_add_range(c, pg - 0x1000);   // reuse an earlier slot, now overlapping the cached range
    atomic_store(&t.phase, 2); pthread_join(reader, NULL);
    EXPECT("slot reuse invalidates another thread's cached selection", 1, t.matched);
    EXPECT("reused earlier slot is the canonical match", 1, nvn_dirty_span(c, 16) == first_slot);
    nvn_dirty_remove_range(c);
    EXPECT("retiring a cached match falls back to the remaining range", 1, nvn_dirty_span(c, 16) == later_slot);
    nvn_dirty_remove_range(p + 2 * pg); munmap(p, 4 * pg);
}

// A page key alone cannot select a descriptor: queries starting on the same
// page but covering different lengths can have different canonical matches.
static void cache_span_lengths(void) {
    size_t pg = (size_t)getpagesize();
    uint8_t *p = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    nvn_dirty_add_range(p, pg);
    const void *short_span = nvn_dirty_span(p, 16);
    nvn_dirty_add_range(p + 0x1000, 2 * pg);
    const void *long_span = nvn_dirty_span(p, 2 * pg);
    EXPECT("different query lengths select distinct overlapping pools", 1, short_span != long_span);
    int bad = 0;
    for (int i = 0; i < 1000; i++) {
        bad |= nvn_dirty_span(p + 32, 2 * pg) != long_span;
        bad |= nvn_dirty_span(p + 32, 16) != short_span;
    }
    EXPECT("page-cache collisions preserve span-dependent first match", 0, bad);
    nvn_dirty_remove_range(p);
    EXPECT("retired short range no longer wins a cached page", 1, nvn_dirty_span(p + 32, 16) == long_span);
    nvn_dirty_remove_range(p + 0x1000);
    EXPECT("retired long range is not returned from either cache", 0, nvn_dirty_tracked(p + 32, 16));
    munmap(p, 4 * pg);
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
    for (int reverse = 0; reverse < 2; reverse++) {
        pthread_t reader;   // each case starts with an empty TLS cache
        if (pthread_create(&reader, NULL, cache_shared_page, (void *)(intptr_t)reverse)) exit(2);
        pthread_join(reader, NULL);
    }
    cache_slot_reuse();
    cache_span_lengths();
    return g_fail != 0;
}
