// A slot reused while lock-free readers use it: one thread keeps registering, protecting and retiring pools over one arena, of changing sizes so a slot's page arrays are
// both reused and grown, while other threads write the arena directly (the fault hook), prepare host writes into it (the pre-write hook) and ask whether it was written
// (nvn_dirty_since_cached with a kept handle). Every write must land without a hang or a crash, and every pool, once retired, leaves the arena writable. Under ThreadSanitizer
// (tsan-dirty-reuse) the run must also be free of races: a reader that found a slot live read its start, end and arrays while add_range rewrote them for the next pool.
#include "../nvn/nvn_dirty.h"
#include "../../host/dm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

module_t *g_mod;
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

enum { PAGES = 64, LIVE = 4 };
static uint8_t *g_arena;
static size_t g_pg;
static _Atomic int g_stop;
static _Atomic uint64_t g_writes, g_preps, g_checks;

static uint32_t rnd(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }
static void *churn(void *arg) {
    int rounds = (int)(intptr_t)arg; uint32_t s = 0x9e3779b9; uint8_t *live[LIVE] = {0};
    for (int k = 0; k < rounds; k++) {
        int j = k % LIVE;
        if (live[j]) nvn_dirty_remove_range(live[j]);
        // 4 KB aligned starts and sizes from 1 to 32 host pages: neighbours share pages, and a slot gets a larger pool (new arrays) or a smaller one (its arrays reused)
        size_t first = rnd(&s) % (PAGES / 2), len = (1 + rnd(&s) % (PAGES / 2)) * g_pg - (rnd(&s) % 4) * 4096;
        live[j] = g_arena + first * g_pg + (rnd(&s) % 4) * 4096;
        nvn_dirty_add_range(live[j], len);
        nvn_dirty_protect(live[j], len);
    }
    for (int j = 0; j < LIVE; j++) if (live[j]) nvn_dirty_remove_range(live[j]);
    atomic_store(&g_stop, 1);
    return NULL;
}
static void *writer(void *arg) {   // direct writes: a protected page faults
    uint32_t s = (uint32_t)(intptr_t)arg;
    while (!atomic_load(&g_stop)) { ((volatile uint8_t *)g_arena)[rnd(&s) % (PAGES * g_pg)] = (uint8_t)s; atomic_fetch_add(&g_writes, 1); }
    return NULL;
}
static void *preparer(void *arg) {   // host writes: prepared, then written
    uint32_t s = (uint32_t)(intptr_t)arg;
    while (!atomic_load(&g_stop)) {
        size_t o = rnd(&s) % (PAGES * g_pg), len = 1 + rnd(&s) % (2 * g_pg); if (o + len > PAGES * g_pg) len = PAGES * g_pg - o;
        dm_guest_write_prepare(g_arena + o, len); memset(g_arena + o, (int)(s & 0xff), len); atomic_fetch_add(&g_preps, 1);
    }
    return NULL;
}
static void *checker(void *arg) {   // dirty checks through a kept handle, as packaged textures make them
    uint32_t s = (uint32_t)(intptr_t)arg; const void *span = NULL;
    while (!atomic_load(&g_stop)) { (void)nvn_dirty_since_cached(&span, g_arena + rnd(&s) % (PAGES * g_pg - 4096), 4096, nvn_dirty_now() / 2); atomic_fetch_add(&g_checks, 1); }
    return NULL;
}
int main(int argc, char **argv) {
    crash_install();
    alarm(60);
    int rounds = argc > 1 ? atoi(argv[1]) : 20000;
    g_pg = (size_t)getpagesize();
    g_arena = mmap(NULL, PAGES * g_pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_arena == MAP_FAILED) return 2;
    pthread_t c, t[5];
    pthread_create(&t[0], NULL, writer, (void *)(intptr_t)0x1234567);
    pthread_create(&t[1], NULL, preparer, (void *)(intptr_t)0x2345678);
    pthread_create(&t[2], NULL, preparer, (void *)(intptr_t)0x3456789);
    pthread_create(&t[3], NULL, checker, (void *)(intptr_t)0x456789a);
    pthread_create(&t[4], NULL, checker, (void *)(intptr_t)0x56789ab);
    pthread_create(&c, NULL, churn, (void *)(intptr_t)rounds);
    pthread_join(c, NULL);
    for (int i = 0; i < 5; i++) pthread_join(t[i], NULL);
    int fail = 0, live, peak, cap; nvn_dirty_stat(&live, &peak, &cap);
    if (live) { printf("pools left registered: %d\n", live); fail++; }
    for (size_t o = 0; o < PAGES * g_pg; o += g_pg) g_arena[o] = 1;   // every page writable again: no fault reaches the hook now, so a protected one would crash here
    printf("t_dirty_reuse: %d rounds, %llu writes, %llu host writes, %llu checks, %d failed\n", rounds, (unsigned long long)g_writes, (unsigned long long)g_preps,
           (unsigned long long)g_checks, fail);
    return fail != 0;
}
