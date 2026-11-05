// Differential checks against registration-order lookup, including overlaps,
// negative results, changing span lengths and repeated retirement/slot reuse.
#include "../nvn/nvn_dirty.c"
#include <stdio.h>
module_t *g_mod; int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
static uint32_t rng = 0x6512734u;
static uint32_t random_word(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static range_t *reference(uintptr_t a, size_t len) {
    for (int i = 0, n = atomic_load(&g_nr); i < n; i++) {
        range_t *r = rg(i);
        if (live_at(i) && r->base <= a && len <= r->end - r->base && a - r->base <= r->end - r->base - len) return r;
    }
    return NULL;
}
static void check(uintptr_t a, size_t len) {
    const void *want = reference(a, len), *got = nvn_dirty_span((void *)a, len);
    if (got != want) { fprintf(stderr, "lookup mismatch at %p + %zu: %p != %p\n", (void *)a, len, got, want); exit(1); }
}
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int phase;
static uintptr_t gap_addr;
static size_t gap_size;
static void *gap_reader(void *unused) {
    (void)unused;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < 3; i++) {
        while (phase != i * 2) pthread_cond_wait(&cv, &mu);
        check(gap_addr, gap_size); check(gap_addr, gap_size);
        phase++; pthread_cond_broadcast(&cv);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}
int main(void) {
    enum { POOLS = 5000, QUERIES = 100000 };
    size_t pg = (size_t)getpagesize(), unit = pg / 4, size = (POOLS * 3 + 64) * unit;
    uint8_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 2; }
    uintptr_t mem[POOLS];
    for (int i = 0; i < POOLS; i++) {
        // An odd permutation gives registration order unrelated to addresses.
        mem[i] = (uintptr_t)p + ((unsigned)i * 3571u % POOLS) * 3 * unit;
        nvn_dirty_add_range((void *)mem[i], (1 + random_word() % 24) * unit);
    }
    for (int i = 0; i < QUERIES; i++) {
        uintptr_t a = (uintptr_t)p + random_word() % size;
        size_t len = 1 + random_word() % (32 * unit);
        check(a, len); check(a, 16); check(a + 17, len); check(a, len);
    }
    for (int i = 0; i < 200; i++) {
        unsigned slot = random_word() % POOLS;
        uintptr_t a = mem[slot];
        check(a, pg); nvn_dirty_remove_range((void *)a); check(a, pg);
        nvn_dirty_add_range((void *)a, (1 + random_word() % 24) * unit);
        check(a, pg); check(a, 16); check(a, 8 * pg);
    }
    // Warm an untracked gap, then add a range covering it. Negative entries
    // must be invalidated too, including entries held in another thread.
    uintptr_t gap = (uintptr_t)p + size - 4 * pg;
    gap_addr = gap; gap_size = pg;
    pthread_t th; if (pthread_create(&th, NULL, gap_reader, NULL)) return 2;
    pthread_mutex_lock(&mu);
    while (phase != 1) pthread_cond_wait(&cv, &mu);
    nvn_dirty_add_range((void *)gap, pg); check(gap, pg);
    phase = 2; pthread_cond_broadcast(&cv);
    while (phase != 3) pthread_cond_wait(&cv, &mu);
    nvn_dirty_remove_range((void *)gap); check(gap, pg);
    phase = 4; pthread_cond_broadcast(&cv);
    while (phase != 5) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu); pthread_join(th, NULL);
    if (nvn_dirty_span((void *)(UINTPTR_MAX - 7), 16)) return 1;
    printf("PASS: %d mixed queries, cache reuse, 200 retire/re-adds; shared index %zu KiB\n", QUERIES * 4, 2 * g_span_cap * sizeof *g_span_tree / 1024);
    for (int i = 0; i < POOLS; i++) nvn_dirty_remove_range((void *)mem[i]);
    munmap(p, size);
    return 0;
}
