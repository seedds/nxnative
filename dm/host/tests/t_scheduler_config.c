// Concurrent first use, validation, and sleep-duration normalization of the
// yield, clock-wait and spin-wait settings after svc_thread_main_init, with a
// thread-local test clock (no guest code or real sleeps).
#include "../dm.h"
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

static __thread uint64_t tick = 1000000000ull;
static __thread struct timespec last_sleep;
static uint64_t test_now_ns(void) { return tick; }
static int test_nanosleep(const struct timespec *ts, struct timespec *rem) {
    (void)rem;
    assert(ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000);
    last_sleep = *ts;
    tick += (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec;
    return 0;
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
static uint64_t want_cap, want_short, want_long, want_10s;
static int want_clock, want_spin_mid, want_spin_long;
static struct timespec want_sleep_10s;

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
    if ((uintptr_t)arg & 1) assert(yield_backoff_cap() == want_cap);
    else assert(yield_backoff_ns(4000000, 0) == want_long);
    assert(yield_backoff_cap() == want_cap);
    assert(yield_backoff_ns(1000000, 0) == want_short);
    assert(yield_backoff_ns(4000000, 0) == want_long);
    assert(yield_backoff_ns(10000000000ull, 1) == want_10s);
    if (want_10s) {
        uint64_t b0 = me.nbackoff;
        last_sleep = (struct timespec){-1, -1};
        yield_for(&me, 10000000000ull, 1);
        assert(me.nbackoff == b0 + 1);
        assert(last_sleep.tv_sec == want_sleep_10s.tv_sec && last_sleep.tv_nsec == want_sleep_10s.tv_nsec);
    }

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

typedef struct { const char *name, *env; uint64_t cap, at_2_5ms, at_8s; } backoff_case_t;
static const backoff_case_t backoff_cases[] = {
    {"default",           NULL,                        250000,               250000, 250000},
    {"empty",             "",                          250000,               250000, 250000},
    {"off",               "0",                         0,                    0,      0},
    {"custom",            "500:3000",                  500000,               0,      500000},
    {"small",             "5:0",                       5000,                 5000,   5000},
    {"zero-start",        "250:0",                     250000,               250000, 250000},
    {"large",             "1250000:3000",              1250000000,           0,      1000000000},
    {"max",               "9223372036854775:0",        9223372036854775000u, 312500, 1000000000},
    {"max-start",         "250:9223372036854775",      250000,               0,      250000},
    {"spaces",            "  +500 : 3000 ",            500000,               0,      500000},
    {"negative",          "-1",                        250000,               250000, 250000},
    {"negative-zero",     "-0",                        250000,               250000, 250000},
    {"negative-start",    "500:-1",                    250000,               250000, 250000},
    {"overflow",          "9999999999999999999999999", 250000,               250000, 250000},
    {"multiply-overflow", "9223372036854776",          250000,               250000, 250000},
    {"junk",              "500x",                      250000,               250000, 250000},
    {"empty-start",       "500:",                      250000,               250000, 250000},
    {"extra",             "500:3000:1",                250000,               250000, 250000},
    {"whitespace",        "   ",                       250000,               250000, 250000},
};

static int test_backoff_validation(void) {
    svc_thread_main_init();
    thr_t *me = pthread_getspecific(g_keythr);
    assert(me);

    for (size_t i = 0; i < sizeof backoff_cases / sizeof *backoff_cases; i++) {
        const backoff_case_t *c = &backoff_cases[i];
        if (c->env) setenv("DM_YIELD_BACKOFF", c->env, 1);
        else unsetenv("DM_YIELD_BACKOFF");
        errno = EAGAIN;
        yield_spin_init();
        assert(errno == EAGAIN);
        assert(yield_backoff_cap() == c->cap);
        assert(yield_backoff_ns(2500000, 0) == c->at_2_5ms);
        assert(yield_backoff_ns(8000000000ull, 1) == c->at_8s);
    }

    // Valid >= 1 s caps normalize timespec instead of passing tv_nsec >= 1e9 (EINVAL)
    setenv("DM_YIELD_BACKOFF", "1250000:3000", 1);
    yield_spin_init();
    uint64_t b0 = me->nbackoff;
    last_sleep = (struct timespec){-1, -1};
    yield_for(me, 10000000000ull, 1);
    assert(me->nbackoff == b0 + 1);
    assert(last_sleep.tv_sec == 1 && last_sleep.tv_nsec == 250000000);

    setenv("DM_YIELD_BACKOFF", "9223372036854775:0", 1);
    yield_spin_init();
    b0 = me->nbackoff;
    last_sleep = (struct timespec){-1, -1};
    yield_for(me, 80000000000ull, 1);
    assert(me->nbackoff == b0 + 1);
    assert(last_sleep.tv_sec == 10 && last_sleep.tv_nsec == 0);

    // Negative DM_YIELD_BACKOFF falls back to the 250 us cap rather than wrapping to
    // UINT64_MAX and misclassifying a 1 ms SleepThread as a polling wait.
    setenv("DM_YIELD_BACKOFF", "-1", 1);
    yield_spin_init();
    me->yrun_t0 = tick - 10000000; me->yrun_svc = me->nsvc; me->poll_ret = tick - 10000;
    b0 = me->nbackoff;
    uint64_t r[32] = {1000000};
    svc_thread(0x0B, r);
    assert(me->nbackoff == b0);

    printf("PASS: yield backoff validation (%zu cases) and long timespec normalization\n",
           sizeof backoff_cases / sizeof *backoff_cases);
    return 0;
}

int main(int argc, char **argv) {
    alarm(20);
    assert(argc == 2);
    unsetenv("DM_YIELD_BACKOFF"); unsetenv("DM_YIELD_CLOCK");
    unsetenv("DM_SPIN"); unsetenv("DM_SPIN_POLL");
    if (!strcmp(argv[1], "backoff")) return test_backoff_validation();

    want_cap = 250000; want_short = 0; want_long = 250000; want_10s = 250000;
    want_sleep_10s = (struct timespec){0, 250000};
    want_clock = 1; want_spin_mid = 0; want_spin_long = 1;
    if (!strcmp(argv[1], "custom")) {
        setenv("DM_YIELD_BACKOFF", "80:500", 1); setenv("DM_YIELD_CLOCK", "1", 1);
        setenv("DM_SPIN_POLL", "50", 1);
        want_cap = want_short = want_long = want_10s = 80000;
        want_sleep_10s = (struct timespec){0, 80000};
        want_spin_mid = 1;
    } else if (!strcmp(argv[1], "off")) {
        setenv("DM_YIELD_BACKOFF", "0", 1); setenv("DM_YIELD_CLOCK", "0", 1);
        setenv("DM_SPIN", "0", 1);
        want_cap = want_long = want_10s = 0; want_clock = 0; want_spin_long = 0;
    } else if (!strcmp(argv[1], "empty")) {
        setenv("DM_YIELD_BACKOFF", "", 1); setenv("DM_YIELD_CLOCK", "", 1);
        setenv("DM_SPIN", "", 1); setenv("DM_SPIN_POLL", "", 1);
    } else if (!strcmp(argv[1], "large")) {
        setenv("DM_YIELD_BACKOFF", "1250000:3000", 1);
        want_cap = 1250000000; want_short = 0; want_long = 500000; want_10s = 1250000000;
        want_sleep_10s = (struct timespec){1, 250000000};
    } else if (!strcmp(argv[1], "invalid")) {
        setenv("DM_YIELD_BACKOFF", "-1", 1);
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
