// How long a short timed wait that nobody ends really takes: a condition variable's timed wait (as a guest's svcWaitSynchronization with a timeout waits: hle_thread.c
// block()), plat_sema_wait and plat_sleep_ns, each 20 times for 2 ms. Horizon's timeouts are precise; Windows serves a wait's timeout at its system timer's tick, 15.625 ms
// by default, which the platform layer sets to 1 ms for the process (plat_win32.c timer_period_1ms). The median of each must stay under 10 ms: back-to-back waits on
// the default tick end at the tick, about 15.6 ms each, while a loaded machine under Wine, where every wait is a round trip to wineserver, took up to 7 ms. Under Wine
// the waits are precise either way; a Windows PC (dm/host/tests/run_win64_tests.ps1) is where the tick shows.
#include "plat/plat.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { N = 20, WAIT_NS = 2000000, LIMIT_NS = 10000000 };
static int cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static uint64_t median(uint64_t *v) { qsort(v, N, sizeof *v, cmp); return v[N / 2]; }

int main(void) {
    uint64_t t[N]; int fails = 0;
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    for (int i = 0; i < N; i++) {   // a condition variable nobody signals, with a deadline 2 ms ahead (the realtime clock, as block() computes it)
        struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += WAIT_NS; if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        uint64_t a = plat_now_ns();
        pthread_mutex_lock(&mu); while (pthread_cond_timedwait(&cv, &mu, &dl) == 0) {} pthread_mutex_unlock(&mu);
        t[i] = plat_now_ns() - a;
    }
    uint64_t m = median(t); fails += m > LIMIT_NS;
    printf("%-5s a condition variable's timed wait of 2 ms takes %.2f ms (median of %d)\n", m > LIMIT_NS ? "FAIL" : "ok", m / 1e6, N);
    plat_sema *s = plat_sema_create(0);
    for (int i = 0; i < N; i++) { uint64_t a = plat_now_ns(); plat_sema_wait(s, WAIT_NS); t[i] = plat_now_ns() - a; }
    plat_sema_destroy(s);
    m = median(t); fails += m > LIMIT_NS;
    printf("%-5s plat_sema_wait of 2 ms takes %.2f ms (median of %d)\n", m > LIMIT_NS ? "FAIL" : "ok", m / 1e6, N);
    for (int i = 0; i < N; i++) { uint64_t a = plat_now_ns(); plat_sleep_ns(WAIT_NS); t[i] = plat_now_ns() - a; }
    m = median(t); fails += m > LIMIT_NS;
    printf("%-5s plat_sleep_ns of 2 ms takes %.2f ms (median of %d)\n", m > LIMIT_NS ? "FAIL" : "ok", m / 1e6, N);
    printf("t_timed_wait: 3 checks, %d failed\n", fails);
    return fails != 0;
}
