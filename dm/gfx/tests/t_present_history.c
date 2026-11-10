// Concurrent callbacks/drop writers and readers must never combine the sequence
// number of one present with the timing fields of another. No GPU or game data.
#include "../nvn/nvn_present_history.h"
#include <stdatomic.h>
#include <stdio.h>
#include <sched.h>

enum { WRITERS = 4, READERS = 4, ITERATIONS = 12000 };
static nvn_present_history history = NVN_PRESENT_HISTORY_INIT;
static _Atomic int start, done, errors;
static _Atomic uint64_t heads[WRITERS];
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static double stamp(uint64_t n) { return (double)n / 8 + 0.25; }
static void *writer(void *arg) {
    uintptr_t id = (uintptr_t)arg;
    while (!atomic_load(&start)) sched_yield();
    for (uint64_t i = 1; i <= ITERATIONS; i++) {
        // Deliberately collide writers on the same slot while generations advance.
        uint64_t n = (i * WRITERS + id) * NVN_PRESENT_HISTORY_CAP + 7;
        nvn_present_history_put(&history, n, stamp(n));
        nvn_link_history_put(&history, (nvn_link_time){n, stamp(n), (double)(n % 127), (double)(n % 17), (int)(n % 7)});
        atomic_store(&heads[id], n);
    }
    atomic_fetch_add(&done, 1);
    return NULL;
}
static void *reader(void *unused) {
    (void)unused;
    while (!atomic_load(&start)) sched_yield();
    do {
        for (unsigned i = 0; i < WRITERS; i++) {
            uint64_t n = atomic_load(&heads[i]);
            if (!n) continue;
            double t = nvn_present_history_get(&history, n);
            if (t && t != stamp(n)) atomic_fetch_add(&errors, 1);
            nvn_link_time value;
            if (nvn_link_history_get(&history, n, &value)
                && (value.n != n || value.target != stamp(n) || value.lead_ms != n % 127
                    || value.gap_ms != n % 17 || value.ticks != (int)(n % 7))) atomic_fetch_add(&errors, 1);
        }
    } while (atomic_load(&done) != WRITERS);
    return NULL;
}

int main(void) {
    CHECK(nvn_present_history_get(&history, 1) == 0, "unpublished present stays unknown");
    nvn_link_time out = {.n = 999};
    CHECK(!nvn_link_history_get(&history, 0, &out) && out.n == 999, "unpublished link history does not write outputs");
    nvn_present_history_put(&history, 1, -1);
    CHECK(nvn_present_history_get(&history, 1) == -1, "an explicit drop is preserved");
    nvn_present_history_put(&history, 1, 2.5);
    CHECK(nvn_present_history_get(&history, 1) == 2.5, "a completion for that same generation is visible");
    uint64_t newer = 1 + NVN_PRESENT_HISTORY_CAP;
    nvn_present_history_put(&history, newer, 3.5);
    nvn_present_history_put(&history, 1, 2.5);
    CHECK(nvn_present_history_get(&history, 1) == 0 && nvn_present_history_get(&history, newer) == 3.5,
          "a delayed old completion cannot displace a newer wrap");
    nvn_link_history_put(&history, (nvn_link_time){newer, 3.5, 2, 3, 4});
    nvn_link_history_put(&history, (nvn_link_time){1, 2.5, 5, 6, 7});
    CHECK(nvn_link_history_get(&history, newer, &out) && out.target == 3.5 && out.ticks == 4,
          "link timing fields retain their generation too");
    CHECK(!nvn_link_history_get(&history, 1, &out), "overwritten link history stays unknown");
    pthread_t threads[WRITERS + READERS];
    for (uintptr_t i = 0; i < WRITERS; i++) if (pthread_create(&threads[i], NULL, writer, (void *)i)) return 2;
    for (unsigned i = 0; i < READERS; i++) if (pthread_create(&threads[WRITERS + i], NULL, reader, NULL)) return 2;
    atomic_store(&start, 1);
    for (unsigned i = 0; i < WRITERS + READERS; i++) if (pthread_join(threads[i], NULL)) return 2;
    CHECK(!atomic_load(&errors), "all concurrent timing snapshots are coherent");
    CHECK(atomic_load(&done) == WRITERS, "all publishers completed");
    CHECK(!pthread_mutex_destroy(&history.mutex), "history mutex can retire after all users join");
    printf("t_present_history: %d checks, %d failed; %d concurrent publications\n", checks, failed, WRITERS * ITERATIONS);
    return failed != 0;
}
