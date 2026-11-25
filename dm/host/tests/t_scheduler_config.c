// Concurrent first use of the yield, clock-wait and spin-wait settings after
// svc_thread_main_init, with a thread-local test clock (no guest code or real sleeps).
// Each invocation runs one environment configuration in a fresh process.
#include "../dm.h"
#include <assert.h>
#include <time.h>
#include <unistd.h>

static __thread uint64_t tick = 1000000000ull;
static uint64_t test_now_ns(void) { return tick; }
static int test_nanosleep(const struct timespec *ts, struct timespec *rem) {
    (void)rem; tick += (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec; return 0;
}
static int test_sched_yield(void) { tick += 500; return 0; }
#define now_ns test_now_ns
#define nanosleep test_nanosleep
#define sched_yield test_sched_yield
#include "../hle_thread.c"
#undef now_ns
#undef nanosleep
#undef sched_yield

enum { WORKERS = 8 };
static pthread_mutex_t start_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cv = PTHREAD_COND_INITIALIZER;
static unsigned arrived, generation;
static uint64_t want_short, want_long;
static int want_clock, want_spin_mid, want_spin_long;

static void rendezvous(void) {
    pthread_mutex_lock(&start_mu);
    unsigned seen = generation;
    if (++arrived == WORKERS) { arrived = 0; generation++; pthread_cond_broadcast(&start_cv); }
    else while (seen == generation) pthread_cond_wait(&start_cv, &start_mu);
    pthread_mutex_unlock(&start_mu);
}

static void *worker(void *arg) {
    thr_t me = {.handle = 0x2000 + (uint32_t)(uintptr_t)arg};
    uint64_t regs[32] = {0};
    assert(!pthread_setspecific(g_keythr, &me));

    rendezvous();
    if ((uintptr_t)arg & 1) assert(yield_backoff_cap() == want_long);
    else assert(yield_backoff_ns(4000000, 0) == want_long);
    assert(yield_backoff_cap() == want_long);
    assert(yield_backoff_ns(1000000, 0) == want_short);
    assert(yield_backoff_ns(4000000, 0) == want_long);

    rendezvous();
    thr_yield(&me, regs);
    me.nsvc++; dm_clock_reads++;
    thr_yield(&me, regs);
    assert(me.nclkyield == (uint64_t)want_clock);

    rendezvous();
    // 20 us: below both custom (50 us) and default (2000 us) spin-poll windows
    me.srun_t0 = tick - 20000; me.spin_ret = tick; me.srun_svc = me.nsvc;
    uint64_t before = me.nyield;
    dm_spin_wait(0, 1);
    assert(me.nspin == 1 && me.nyield == before);

    // 100 us: past custom 50 us poll window, still below default 2000 us window
    me.srun_t0 = tick - 100000; me.spin_ret = tick; me.srun_svc = me.nsvc;
    before = me.nyield;
    dm_spin_wait(0, 1);
    assert(me.nspin == 2 && me.nyield - before == (uint64_t)want_spin_mid);

    // 5 ms: past both poll windows (yields unless DM_SPIN=0)
    me.srun_t0 = tick - 5000000; me.spin_ret = tick; me.srun_svc = me.nsvc;
    before = me.nyield;
    dm_spin_wait(0, 1);
    assert(me.nspin == 3 && me.nyield - before == (uint64_t)want_spin_long);

    assert(!pthread_setspecific(g_keythr, NULL));
    return NULL;
}

int main(int argc, char **argv) {
    alarm(20);
    assert(argc == 2);
    unsetenv("DM_YIELD_BACKOFF"); unsetenv("DM_YIELD_CLOCK");
    unsetenv("DM_SPIN"); unsetenv("DM_SPIN_POLL");
    want_short = 0; want_long = 250000; want_clock = 1; want_spin_mid = 0; want_spin_long = 1;
    if (!strcmp(argv[1], "custom")) {
        setenv("DM_YIELD_BACKOFF", "80:500", 1); setenv("DM_YIELD_CLOCK", "1", 1);
        setenv("DM_SPIN_POLL", "50", 1);
        want_short = want_long = 80000; want_spin_mid = 1;
    } else if (!strcmp(argv[1], "off")) {
        setenv("DM_YIELD_BACKOFF", "0", 1); setenv("DM_YIELD_CLOCK", "0", 1);
        setenv("DM_SPIN", "0", 1);
        want_long = 0; want_clock = 0; want_spin_long = 0;
    } else if (!strcmp(argv[1], "empty")) {
        setenv("DM_YIELD_BACKOFF", "", 1); setenv("DM_YIELD_CLOCK", "", 1);
        setenv("DM_SPIN", "", 1); setenv("DM_SPIN_POLL", "", 1);
    } else {
        assert(!strcmp(argv[1], "default"));
    }

    svc_thread_main_init();

    pthread_t threads[WORKERS];
    for (int i = 0; i < WORKERS; i++) assert(!pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i));
    for (int i = 0; i < WORKERS; i++) assert(!pthread_join(threads[i], NULL));
    printf("PASS: concurrent scheduler settings (%s), %d workers\n", argv[1], WORKERS);
    return 0;
}
