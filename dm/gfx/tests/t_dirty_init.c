// First use of dirty-page tracking from several threads at once, in a fresh process (PR #2225): eight threads register their first pools together while another
// thread keeps doing host pre-writes (a file read into guest memory). Every pool must be tracked and see a direct write (through the fault hook) and a host write
// (through the pre-write hook). Under ThreadSanitizer (tsan-dirty-init) the run must also be free of races: setup used to run at the first registration behind a plain
// flag, and the pre-write thread read crash.c's hook table while that registration grew it.
#include "../nvn/nvn_dirty.h"
#include "../../host/dm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

module_t *g_mod;
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

enum { N = 8 };
static int g_fail;
#define EXPECT(name, i, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    if (want_ != got_) { printf("pool %d: %s: expected %lld, got %lld\n", i, name, want_, got_); g_fail++; } \
} while (0)

static _Atomic int g_ready;   // macOS has no pthread barrier: every thread counts in, then all start together
static void go(void) { atomic_fetch_add(&g_ready, 1); while (atomic_load(&g_ready) < N + 1) {} }
static _Atomic int g_stop;
static uint8_t *g_pages[N];
static size_t g_pg;

static void *pre_writer(void *arg) {
    uint8_t *spare = arg;
    go();
    while (!atomic_load(&g_stop)) { dm_guest_write_prepare(spare, 64); spare[0]++; }
    return NULL;
}
static void *first_pool(void *arg) {
    int i = (int)(intptr_t)arg; uint8_t *p = g_pages[i];
    go();
    nvn_dirty_add_range(p, g_pg);
    EXPECT("tracked", i, 1, nvn_dirty_tracked(p, g_pg));
    uint64_t stamp = nvn_dirty_now();
    nvn_dirty_protect(p, g_pg);
    EXPECT("clean once protected", i, 0, nvn_dirty_since(p, g_pg, stamp));
    ((volatile uint8_t *)p)[g_pg - 1] = 0x5a;   // faults: the fault hook unprotects and stamps the page
    EXPECT("direct write dirty", i, 1, nvn_dirty_since(p, g_pg, stamp));
    stamp = nvn_dirty_now(); nvn_dirty_protect(p, g_pg);
    dm_guest_write_prepare(p, 1); p[0] = 0x63;   // a host write: the pre-write hook unprotects it, no fault
    EXPECT("host write dirty", i, 1, nvn_dirty_since(p, g_pg, stamp));
    return NULL;
}
int main(void) {
    crash_install();
    alarm(30);
    g_pg = (size_t)getpagesize();
    uint8_t *spare = mmap(NULL, g_pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (spare == MAP_FAILED) return 2;
    for (int i = 0; i < N; i++) { g_pages[i] = mmap(NULL, g_pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); if (g_pages[i] == MAP_FAILED) return 2; }
    pthread_t w, t[N];
    pthread_create(&w, NULL, pre_writer, spare);
    for (int i = 0; i < N; i++) pthread_create(&t[i], NULL, first_pool, (void *)(intptr_t)i);
    for (int i = 0; i < N; i++) pthread_join(t[i], NULL);
    atomic_store(&g_stop, 1); pthread_join(w, NULL);
    for (int i = 0; i < N; i++) nvn_dirty_remove_range(g_pages[i]);   // after the threads: retiring a slot while others scan is t_dirty_retire_race's case, not this one
    int live, peak, cap; nvn_dirty_stat(&live, &peak, &cap);
    EXPECT("pools left registered", -1, 0, live);
    printf("t_dirty_init: %d pools, %d failed\n", N, g_fail);
    return g_fail != 0;
}
