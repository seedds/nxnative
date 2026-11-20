// Clock waits (hle_thread.c clock_wait), through real yield SVCs on one guest thread: a frame limiter that yields and reads the clock until its deadline, on a test clock that
// only sleeps and yields advance (nanosleep moves it by the time asked, sched_yield by 0.5 µs), so where each wait ends is exact.
#include "../dm.h"
#include <assert.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

static _Atomic uint64_t clk = 1000000000ull;
static uint64_t test_now_ns(void) { return atomic_load(&clk); }
static int test_nanosleep(const struct timespec *ts, struct timespec *rem) { (void)rem; atomic_fetch_add(&clk, (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec); return 0; }
static int test_sched_yield(void) { atomic_fetch_add(&clk, 500); return 0; }
#define now_ns test_now_ns
#define nanosleep test_nanosleep
#define sched_yield test_sched_yield
#include "../hle_thread.c"
#undef now_ns
#undef nanosleep
#undef sched_yield

static thr_t *self(void) { thr_t *me = pthread_getspecific(g_keythr); assert(me); return me; }
static void other_svc(void) { uint64_t r[32] = {0, self()->handle}; svc_thread(0x0C, r); }   // GetThreadPriority: ends a run of yields
// One yield of a loop that keeps x19–x28 (a poll); reads = clock reads before it
static void yield_poll(int reads) { dm_clock_reads += (uint64_t)reads; uint64_t r[32] = {0}; for (int i = 19; i <= 28; i++) r[i] = 0x1000 + (uint64_t)i; svc_thread(0x0B, r); }
// A frame limiter's wait: yield, read the clock, until dl has passed; returns where it ended
static uint64_t limiter(uint64_t dl) {
    other_svc();
    do yield_poll(1); while (test_now_ns() < dl);
    return test_now_ns();
}
int main(void) {
    alarm(20);
    svc_thread_main_init();

    // A run of yields with no clock read: plain yields for its first 2 ms; with a read in each pass: back-off sleeps from its second yield (its first follows another SVC)
    other_svc(); uint64_t b0 = self()->nbackoff, t0 = test_now_ns();
    while (test_now_ns() - t0 < 1900000) yield_poll(0);
    assert(self()->nbackoff == b0);
    other_svc(); b0 = self()->nbackoff;
    yield_poll(1); assert(self()->nbackoff == b0);
    yield_poll(1); assert(self()->nbackoff == b0 + 1);
    puts("PASS: a yield loop that reads the clock backs off from its start, one that does not after 2 ms");

    // A limiter at a steady period (its deadline is its last end + 16.68 ms, after 3 ms of work a frame): the period is learned, and the waits then sleep to their predicted end
    const uint64_t P60 = 16683333, P30 = 2 * P60, WORK = 3000000;
    uint64_t end = limiter(test_now_ns() + P60), late_max = 0, p0 = self()->ncwpred;
    for (int f = 0; f < 20; f++) {
        uint64_t dl = end + P60; atomic_fetch_add(&clk, WORK); end = limiter(dl);
        if (f >= 5 && end - dl > late_max) late_max = end - dl;
    }
    printf("  steady 60: predicted sleeps %llu of 20 waits, latest end %llu ns after the deadline\n", (unsigned long long)(self()->ncwpred - p0), (unsigned long long)late_max);
    assert(self()->ncwpred - p0 >= 15);
    assert(late_max < 2000);   // the polls after the sleep end it within a few yields (the back-off alone ended up to 250 µs late)
    puts("PASS: a limiter at a steady period sleeps to its predicted end and ends on time");

    // The period gets longer (60 → 30): a prediction that comes early costs polls, not lateness
    late_max = 0;
    for (int f = 0; f < 10; f++) { uint64_t dl = end + P30; atomic_fetch_add(&clk, WORK); end = limiter(dl); if (end - dl > late_max) late_max = end - dl; }
    printf("  60 → 30: latest end %llu ns after the deadline\n", (unsigned long long)late_max);
    assert(late_max < 2000 + 250000 / 4 * 5);   // never later than the back-off's own lateness (its sleep, with slack)
    puts("PASS: a longer period never makes a wait end late beyond the back-off's own lateness");

    // The period gets shorter (30 → 60): two waits end late (the first is taken for lateness the learned period carried, the second relearns it), then the waits end on time once more
    int late_frames = 0; late_max = 0;
    for (int f = 0; f < 20; f++) {
        uint64_t dl = end + P60; atomic_fetch_add(&clk, WORK); end = limiter(dl);
        if (end - dl > 1000000) late_frames++;
        if (f >= 10 && end - dl > late_max) late_max = end - dl;
    }
    printf("  30 → 60: %d waits ended over 1 ms late, then latest end %llu ns after the deadline\n", late_frames, (unsigned long long)late_max);
    assert(late_frames == 2);
    assert(late_max < 2000);
    puts("PASS: a shorter period costs two late waits, then the waits end on time again");
    return 0;
}
