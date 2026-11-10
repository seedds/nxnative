// Concurrent first use of the real yield/spin settings, with a thread-local test
// clock: no guest code, real sleeps, or GPU work. Each invocation is a fresh process.
#include "../dm.h"
#include <assert.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

static __thread uint64_t tick = 1000000000ull;
static uint64_t test_now_ns(void) { return tick; }
static int test_nanosleep(const struct timespec *ts, struct timespec *rem) {
    (void)rem; tick += (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec; return 0;
}
static int test_sched_yield(void) { tick += 500; return 0; }
static pthread_mutex_t init_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t init_cv = PTHREAD_COND_INITIALIZER;
static int controlled, parsing_start, release_parse, second_waiting, second_done;
static __thread int init_role;
static long long test_atoll(const char *);
static int test_once(pthread_once_t *, void (*)(void));
#define now_ns test_now_ns
#define nanosleep test_nanosleep
#define sched_yield test_sched_yield
#define atoll test_atoll
#define pthread_once test_once
#include "../hle_thread.c"
#undef now_ns
#undef nanosleep
#undef sched_yield
#undef atoll
#undef pthread_once

static long long test_atoll(const char *text) {
    if (controlled && init_role == 1 && !strcmp(text, "500")) {
        pthread_mutex_lock(&init_mu); parsing_start = 1; pthread_cond_broadcast(&init_cv);
        while (!release_parse) pthread_cond_wait(&init_cv, &init_mu);
        pthread_mutex_unlock(&init_mu);
    }
    return atoll(text);
}
static int test_once(pthread_once_t *once, void (*fn)(void)) {
    if (controlled && init_role == 2) {
        pthread_mutex_lock(&init_mu); second_waiting = 1; pthread_cond_broadcast(&init_cv); pthread_mutex_unlock(&init_mu);
    }
    return pthread_once(once, fn);
}
static uint64_t partial_result[2];
static void *partial_reader(void *arg) {
    init_role = (int)(uintptr_t)arg;
    partial_result[init_role - 1] = init_role == 1 ? yield_backoff_cap() : yield_backoff_ns(1000000, 0);
    if (init_role == 2) {
        pthread_mutex_lock(&init_mu); second_done = 1; pthread_cond_broadcast(&init_cv); pthread_mutex_unlock(&init_mu);
    }
    return NULL;
}
static int partial_publication(void) {
    controlled = 1; setenv("DM_YIELD_BACKOFF", "80:500", 1);
    pthread_t first, second;
    assert(!pthread_create(&first, NULL, partial_reader, (void *)1));
    pthread_mutex_lock(&init_mu);
    while (!parsing_start) pthread_cond_wait(&init_cv, &init_mu);
    pthread_mutex_unlock(&init_mu);
    assert(!pthread_create(&second, NULL, partial_reader, (void *)2));
    pthread_mutex_lock(&init_mu);
    while (!second_waiting && !second_done) pthread_cond_wait(&init_cv, &init_mu);
    int returned_early = second_done; release_parse = 1; pthread_cond_broadcast(&init_cv); pthread_mutex_unlock(&init_mu);
    assert(!pthread_join(first, NULL)); assert(!pthread_join(second, NULL));
    int ok = !returned_early && partial_result[0] == 80000 && partial_result[1] == 80000;
    printf("%s: cap and duration getters publish both parsed fields before returning\n", ok ? "PASS" : "FAIL");
    return !ok;
}

enum { WORKERS = 8 };
static pthread_mutex_t start_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cv = PTHREAD_COND_INITIALIZER;
static unsigned arrived, generation;
static uint64_t want_short, want_long;
static int want_clock, want_spin;
static void rendezvous(void) {
    pthread_mutex_lock(&start_mu);
    unsigned seen = generation;
    if (++arrived == WORKERS) { arrived = 0; generation++; pthread_cond_broadcast(&start_cv); }
    else while (seen == generation) pthread_cond_wait(&start_cv, &start_mu);
    pthread_mutex_unlock(&start_mu);
}
static void *worker(void *arg) {
    thr_t me = {.handle = 0x2000}; uint64_t regs[32] = {0};
    assert(!pthread_setspecific(g_keythr, &me));
    rendezvous();
    if ((uintptr_t)arg & 1) assert(yield_backoff_cap() == want_long);
    else assert(yield_backoff_ns(4000000, 0) == want_long);
    rendezvous();
    thr_yield(&me, regs); // clock mode and backoff first use on every thread
    me.nsvc++; dm_clock_reads++;
    thr_yield(&me, regs);
    assert(me.nclkyield == (uint64_t)want_clock);
    assert(yield_backoff_ns(1000000, 0) == want_short);
    assert(yield_backoff_ns(4000000, 0) == want_long);
    rendezvous();
    me.srun_t0 = tick - 5000000; me.spin_ret = tick; me.srun_svc = me.nsvc;
    uint64_t before = me.nyield;
    dm_spin_wait(0, 1);
    assert(me.nspin == 1 && me.nyield - before == (uint64_t)want_spin);
    assert(!pthread_setspecific(g_keythr, NULL));
    return NULL;
}
int main(int argc, char **argv) {
    alarm(20);
    assert(argc == 2);
    if (!strcmp(argv[1], "partial")) return partial_publication();
    unsetenv("DM_YIELD_BACKOFF"); unsetenv("DM_YIELD_CLOCK"); unsetenv("DM_SPIN"); unsetenv("DM_SPIN_POLL");
    want_short = 0; want_long = 250000; want_clock = want_spin = 1;
    if (!strcmp(argv[1], "custom")) {
        setenv("DM_YIELD_BACKOFF", "80:500", 1); setenv("DM_YIELD_CLOCK", "1", 1);
        setenv("DM_SPIN_POLL", "50", 1); want_short = want_long = 80000;
    } else if (!strcmp(argv[1], "off")) {
        setenv("DM_YIELD_BACKOFF", "0", 1); setenv("DM_YIELD_CLOCK", "0", 1); setenv("DM_SPIN", "0", 1);
        want_long = 0; want_clock = want_spin = 0;
    } else if (!strcmp(argv[1], "empty")) {
        setenv("DM_YIELD_BACKOFF", "", 1); setenv("DM_YIELD_CLOCK", "", 1);
        setenv("DM_SPIN", "", 1); setenv("DM_SPIN_POLL", "", 1);
    } else assert(!strcmp(argv[1], "default"));
    assert(!pthread_key_create(&g_keythr, NULL));
    pthread_t threads[WORKERS];
    for (int i = 0; i < WORKERS; i++) assert(!pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i));
    for (int i = 0; i < WORKERS; i++) assert(!pthread_join(threads[i], NULL));
    assert(!pthread_key_delete(g_keythr));
    printf("PASS: concurrent scheduler first use (%s), %d workers\n", argv[1], WORKERS);
    return 0;
}
