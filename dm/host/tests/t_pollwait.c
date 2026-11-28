// Short timed waits as polls, through real thread SVCs on one guest thread.
// Drive the run/gap clock explicitly: a CI deschedule longer than SPIN_GAP_NS
// legitimately resets the run, so 20 ms of wall time need not include a backoff.
// Host condition waits still use the real clock and pthread timed waits. Sleep
// requests are observed without waiting: the frozen poll clock would otherwise
// make sleep_precise repeat each real sleep until its iteration limit, which
// exhausted the watchdog on CI.
#include "../dm.h"
#include <assert.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

static _Atomic uint64_t poll_clock;
static uint64_t test_now_ns(void) { return atomic_load(&poll_clock); }
static unsigned sleep_requests;
static void (*sleep_hook)(void);
static int test_nanosleep(const struct timespec *request, struct timespec *remaining) {
    (void)remaining;
    assert(request->tv_sec == 0 && request->tv_nsec > 0 && request->tv_nsec < 1000000000);
    sleep_requests++;
    if (sleep_hook) sleep_hook();
    return 0;
}
static _Thread_local struct timespec wait_start;
static int test_clock_gettime(clockid_t clock, struct timespec *ts) {
    int r = clock_gettime(clock, ts);
    if (!r && clock == CLOCK_REALTIME) wait_start = *ts;
    return r;
}
static int signal_wait, waiter_ready;   // signal_wait is set before starting the signaller; waiter_ready is protected by G
static pthread_cond_t ready_cv = PTHREAD_COND_INITIALIZER;
static int test_timedwait(pthread_cond_t *cv, pthread_mutex_t *mutex, const struct timespec *deadline) {
    if (!signal_wait) return pthread_cond_timedwait(cv, mutex, deadline);
    int64_t ns = (deadline->tv_sec - wait_start.tv_sec) * 1000000000ll + deadline->tv_nsec - wait_start.tv_nsec;
    assert(ns == 250000);   // the production wait must have been lengthened, not merely counted as a backoff
    waiter_ready = 1; pthread_cond_signal(&ready_cv);
    // Only the signal case has no OS timeout: the signaller must run after this
    // waiter releases G, even when CI takes longer than 250 us to schedule it.
    // The real condition wait and SignalProcessWideKey still perform the wake.
    return pthread_cond_wait(cv, mutex);
}
#define now_ns test_now_ns
#define clock_gettime test_clock_gettime
#define pthread_cond_timedwait test_timedwait
#define nanosleep test_nanosleep
#include "../hle_thread.c"
#undef now_ns
#undef clock_gettime
#undef pthread_cond_timedwait
#undef nanosleep

static _Atomic uint32_t mutex_word, condition_key;
static uint32_t me_handle;
static uint64_t backoffs(void) { thr_t *me = pthread_getspecific(g_keythr); assert(me); return me->nbackoff; }
// One condition variable wait as the SDK's TimedWait makes it: take the lock in user mode, wait (the timeout releases it without taking it back)
static int64_t cv_wait(int64_t ns) {
    atomic_store(&mutex_word, me_handle);
    uint64_t r[32] = {(uint64_t)&mutex_word, (uint64_t)&condition_key, me_handle, (uint64_t)ns};
    svc_thread(0x1C, r);
    int64_t result = (int64_t)r[0];
    if (!result) {
        assert(atomic_load(&mutex_word) == me_handle);   // a signalled waiter owns the lock again
    }
    return result;
}
static void poll_step(int64_t wait_ns, int sleep_too, uint32_t event) {
    if (event) { uint64_t r[32] = {0, (uint64_t)&event, 1, (uint64_t)wait_ns}; svc_thread(0x18, r); assert(r[0] == R_TIMEOUT); }
    else assert(cv_wait(wait_ns) == R_TIMEOUT);
    if (sleep_too) { uint64_t r[32] = {1000}; svc_thread(0x0B, r); }
}
static void poll_run(int64_t wait_ns, int sleep_too, uint32_t event) {
    atomic_fetch_add(&poll_clock, 30000);   // guest work ends the previous run
    uint64_t b0 = backoffs();
    for (int i = 0; i < 200; i++) {   // 0 through 1.99 ms: no backoff yet
        unsigned s0 = sleep_requests;
        poll_step(wait_ns, sleep_too, event);
        assert(backoffs() == b0);
        if (!wait_ns && !sleep_too) assert(sleep_requests == s0);
        atomic_fetch_add(&poll_clock, 10000);
    }
    unsigned s0 = sleep_requests;
    poll_step(wait_ns, sleep_too, event);   // exactly 2 ms: the wait (and any yield) backs off
    assert(backoffs() == b0 + 1 + sleep_too);
    if (!wait_ns && !sleep_too) assert(sleep_requests == s0 + 1);
}
// WaitForAddress(wait if equal, timeout 0) on a word that holds the value: a poll, TimedOut at once as on Horizon (issue #1768)
static _Atomic uint32_t addr_word = 1;
static int64_t addr_poll_tag(uint32_t value, uint64_t tag) {
    uint64_t r[32] = {(uint64_t)&addr_word, 2, value, 0};
    r[19] = tag;
    svc_thread(0x34, r);
    return (int64_t)r[0];
}
static int64_t addr_poll(uint32_t value) { return addr_poll_tag(value, 0); }
static void addr_run(void) {
    atomic_store(&addr_word, 1);
    atomic_fetch_add(&poll_clock, 30000);   // guest work ends the previous run
    uint64_t b0 = backoffs();
    for (int i = 0; i < 200; i++) {   // 0 through 1.99 ms: no backoff yet
        unsigned s0 = sleep_requests;
        assert(addr_poll(1) == R_TIMEOUT);
        assert(backoffs() == b0 && sleep_requests == s0);
        atomic_fetch_add(&poll_clock, 10000);
    }
    unsigned s0 = sleep_requests;
    assert(addr_poll(1) == R_TIMEOUT);   // exactly 2 ms: the poll sleeps the back-off, and still times out
    assert(backoffs() == b0 + 1 && sleep_requests == s0 + 1);
    atomic_fetch_add(&poll_clock, 10000);
    assert(addr_poll(2) == R_BAD_STATE);   // the word moved on: the loop ends, and so does the run
    atomic_fetch_add(&poll_clock, 10000);
    assert(addr_poll(1) == R_TIMEOUT && backoffs() == b0 + 1);   // the next poll starts a new run
}
static void *signal_type2_helper(void *unused) {
    (void)unused;
    // SignalToAddress type 2 (modify by waiter count): with 0 waiters it writes val + 1 (2); a waiter in g_wait would write val - 1 (0)
    uint64_t r[32] = {(uint64_t)&addr_word, 2, 1, 1};
    svc_thread(0x35, r);
    assert(r[0] == 0);
    return NULL;
}
static void hook_check_not_waiter(void) {
    thr_t *me = pthread_getspecific(g_keythr);
    g_lock();
    assert(me && me->wkind == 0 && !g_wait);
    g_unlock();
    pthread_t s;
    assert(!pthread_create(&s, NULL, signal_type2_helper, NULL));
    assert(!pthread_join(s, NULL));
}
static void *cancel_helper(void *unused) {
    (void)unused;
    uint64_t r[32] = {me_handle};
    svc_thread(0x19, r);
    assert(r[0] == 0);
    return NULL;
}
static void hook_cancel_during_sleep(void) {
    pthread_t s;
    assert(!pthread_create(&s, NULL, cancel_helper, NULL));
    assert(!pthread_join(s, NULL));
}
// A run of SleepThread(ns) with 10 us of guest work between the sleeps, as Unity 6's idle job workers poll their queue: the guest's own sleeps for steps * 10 us, then one more
static void sleep_run(int64_t ns, int steps, int expect_backoff) {
    atomic_fetch_add(&poll_clock, 30000);   // guest work ends the previous run
    uint64_t b0 = backoffs();
    for (int i = 0; i < steps; i++) {
        unsigned s0 = sleep_requests;
        uint64_t r[32] = {(uint64_t)ns}; svc_thread(0x0B, r);
        assert(backoffs() == b0);
        assert(sleep_requests > s0);
        atomic_fetch_add(&poll_clock, 10000);
    }
    unsigned s0 = sleep_requests;
    uint64_t r[32] = {(uint64_t)ns}; svc_thread(0x0B, r);   // a short sleep becomes the back-off's
    assert(backoffs() == b0 + (expect_backoff ? 1 : 0));
    assert(sleep_requests > s0);
}
static void *signaller(void *unused) {
    (void)unused;
    g_lock();
    while (!waiter_ready) g_cond_wait(&ready_cv, NULL);
    assert(g_wait && g_wait->wkind == 2 && g_wait->wkey == (uint64_t)&condition_key);
    g_unlock();
    uint64_t r[32] = {(uint64_t)&condition_key, 1}; svc_thread(0x1D, r);
    return NULL;
}
int main(void) {
    alarm(20);
    me_handle = svc_thread_main_init();
    poll_run(500, 0, 0);
    puts("PASS: 500 ns condition variable waits back off at 2 ms, not before");
    poll_run(30, 1, 0);
    puts("PASS: 30 ns waits between 1 us sleeps back off at 2 ms, not before");
    uint64_t r[32] = {0}; svc_thread(0x45, r); assert(r[0] == 0); uint32_t ev_w = (uint32_t)r[1], ev_r = (uint32_t)r[2];
    poll_run(30, 1, ev_r);
    puts("PASS: the same with WaitSynchronization");

    uint64_t b0 = backoffs();
    atomic_fetch_add(&poll_clock, 20000); poll_step(500, 0, 0);
    assert(backoffs() == b0 + 1);
    for (int i = 0; i < 200; i++) { atomic_fetch_add(&poll_clock, 20001); poll_step(500, 0, 0); }
    assert(backoffs() == b0 + 1);
    puts("PASS: a 20 us gap keeps the run; longer gaps reset it");

    poll_run(500, 0, 0);
    b0 = backoffs();
    r[0] = 0; r[1] = me_handle; svc_thread(0x0C, r); assert(r[0] == 0);
    poll_step(500, 0, 0); assert(backoffs() == b0);
    puts("PASS: an intervening non-poll SVC resets the run");

    sleep_run(25000, 200, 1);
    puts("PASS: 25 us sleeps back off at 2 ms, not before");
    sleep_run(300000, 200, 0);
    puts("PASS: sleeps of 250 us and more keep their own length");
    g_bo_cap = 500000;   // DM_YIELD_BACKOFF=500: the back-off reaches 300 us at 2.4 ms
    sleep_run(300000, 241, 1);
    puts("PASS: with a 500 us cap, 300 us sleeps back off once the back-off is longer");
    g_bo_cap = 0;   // DM_YIELD_BACKOFF=0
    sleep_run(25000, 300, 0);
    puts("PASS: with the back-off off, short sleeps keep their length");
    g_bo_cap = 250000;
    b0 = backoffs();
    for (int i = 0; i < 300; i++) { atomic_fetch_add(&poll_clock, 30000); uint64_t q[32] = {25000}; svc_thread(0x0B, q); }
    assert(backoffs() == b0);
    puts("PASS: 25 us sleeps with 30 us of work between them never back off");

    poll_run(500, 0, 0);
    b0 = backoffs();
    signal_wait = 1;
    pthread_t s; assert(!pthread_create(&s, NULL, signaller, NULL));
    assert(cv_wait(500) == 0);
    assert(!pthread_join(s, NULL)); signal_wait = 0;
    assert(backoffs() == b0 + 1);
    poll_step(500, 0, 0); assert(backoffs() == b0 + 1);
    puts("PASS: a lengthened wait returns at the signal and the next poll starts a new run");

    poll_run(0, 0, 0);
    puts("PASS: zero-timeout condition variable waits time out at once and back off at 2 ms, not before");
    poll_run(0, 0, ev_r);
    puts("PASS: the same with WaitSynchronization");
    addr_run();
    puts("PASS: the same with WaitForAddress, whose run ends when the word moves on");

    // Alternating 0 ns WaitForAddress and SleepThread(0) with two distinct x19–x28 register sets (two-slot signature)
    atomic_store(&addr_word, 1);
    atomic_fetch_add(&poll_clock, 30000);
    uint64_t z0 = backoffs();
    for (int i = 0; i < 202; i++) {
        assert(addr_poll_tag(1, 0xaaaa) == R_TIMEOUT);
        uint64_t yr[32] = {0}; yr[19] = 0xbbbb; svc_thread(0x0B, yr);
        atomic_fetch_add(&poll_clock, 10000);
    }
    assert(backoffs() > z0);
    puts("PASS: alternating zero-timeout WaitForAddress and yield with two register sets backs off");

    z0 = backoffs();
    for (int i = 0; i < 300; i++) { atomic_fetch_add(&poll_clock, 30000); assert(addr_poll(1) == R_TIMEOUT); }
    assert(backoffs() == z0);
    puts("PASS: zero-timeout polls with 30 us of work between them never back off");

    z0 = backoffs(); atomic_fetch_add(&poll_clock, 30000);
    for (int i = 0; i < 300; i++) {   // a loop that works between its tries: its counter moves in x19
        assert(addr_poll_tag(1, (uint64_t)i + 1) == R_TIMEOUT);
        atomic_fetch_add(&poll_clock, 10000);
    }
    assert(backoffs() == z0);
    puts("PASS: zero-timeout tries whose caller's registers move never back off");

    atomic_fetch_add(&poll_clock, 30000);
    assert(cv_wait(0) == R_TIMEOUT && atomic_load(&mutex_word) == 0);
    puts("PASS: a zero-timeout condition variable wait still releases the lock");

    // A backed-off zero-timeout WaitForAddress sleeps after timing out and is never in g_wait, so SignalToAddress type 2 sees 0 waiters and writes val + 1
    atomic_store(&addr_word, 1);
    atomic_fetch_add(&poll_clock, 30000);
    for (int i = 0; i < 200; i++) { assert(addr_poll(1) == R_TIMEOUT); atomic_fetch_add(&poll_clock, 10000); }
    sleep_hook = hook_check_not_waiter;
    assert(addr_poll(1) == R_TIMEOUT);
    sleep_hook = NULL;
    assert(atomic_load(&addr_word) == 2);
    puts("PASS: a backed-off zero-timeout WaitForAddress never enters g_wait or changes SignalToAddress waiter counts");

    // Zero-timeout WaitSynchronization preserves pending and concurrent CancelSynchronization while still backing off
    poll_run(0, 0, ev_r);
    hook_cancel_during_sleep();   // another thread queues a pending cancel without advancing this thread's nsvc
    z0 = backoffs();
    { unsigned s0 = sleep_requests; uint64_t wr[32] = {0, (uint64_t)&ev_r, 1, 0}; svc_thread(0x18, wr);
      assert(wr[0] == R_TIMEOUT && backoffs() == z0 + 1 && sleep_requests == s0 + 1); }
    { uint64_t wr[32] = {0, (uint64_t)&ev_r, 1, (uint64_t)-1}; svc_thread(0x18, wr); assert(wr[0] == R_CANCELLED); }
    poll_run(0, 0, ev_r);
    sleep_hook = hook_cancel_during_sleep;
    { uint64_t wr[32] = {0, (uint64_t)&ev_r, 1, 0}; svc_thread(0x18, wr); assert(wr[0] == R_TIMEOUT); }
    sleep_hook = NULL;
    { uint64_t wr[32] = {0, (uint64_t)&ev_r, 1, (uint64_t)-1}; svc_thread(0x18, wr); assert(wr[0] == R_CANCELLED); }
    puts("PASS: zero-timeout WaitSynchronization preserves pending and concurrent CancelSynchronization");

    // DM_YIELD_BACKOFF=0 disables zero-timeout wait backoff as well
    g_bo_cap = 0;
    atomic_store(&addr_word, 1);
    atomic_fetch_add(&poll_clock, 30000); z0 = backoffs();
    for (int i = 0; i < 300; i++) { assert(addr_poll(1) == R_TIMEOUT); atomic_fetch_add(&poll_clock, 10000); }
    assert(backoffs() == z0);
    g_bo_cap = 250000;
    puts("PASS: with the back-off off, zero-timeout waits never back off");

    // No clock_wait bleed: a zero-timeout wait or short sleep followed by SleepThread(0) after a clock read must not enter clock_wait
    thr_t *me = pthread_getspecific(g_keythr); assert(me);
    {
        uint64_t yr[32] = {0};
        svc_thread(0x0B, yr);   // prime yrun_clk
        dm_clock_reads++;
        atomic_fetch_add(&poll_clock, 30000);
        uint64_t ck0 = me->nclkyield; z0 = backoffs();
        assert(addr_poll(1) == R_TIMEOUT);
        svc_thread(0x0B, yr);
        assert(me->nclkyield == ck0 && backoffs() == z0);

        atomic_fetch_add(&poll_clock, 30000);
        uint64_t pr0 = me->ncwpred; z0 = backoffs(); ck0 = me->nclkyield;
        for (int i = 0; i < 200; i++) {   // 0 through 1.99 ms: neither clock_wait nor backoff yet
            assert(addr_poll(1) == R_TIMEOUT);
            dm_clock_reads++;
            svc_thread(0x0B, yr);
            assert(me->nclkyield == ck0 && me->ncwpred == pr0 && backoffs() == z0);
            atomic_fetch_add(&poll_clock, 10000);
        }
        // Continuing past 2 ms enters normal poll backoff without entering clock_wait
        assert(addr_poll(1) == R_TIMEOUT);
        assert(me->nclkyield == ck0 && me->ncwpred == pr0 && backoffs() == z0 + 1);

        atomic_fetch_add(&poll_clock, 30000);
        ck0 = me->nclkyield;
        uint64_t sr[32] = {25000}; svc_thread(0x0B, sr);   // poll_sleep
        dm_clock_reads++;
        svc_thread(0x0B, yr);
        assert(me->nclkyield == ck0);
    }
    puts("PASS: zero-timeout waits and short sleeps never bleed into clock_wait");

    // Target-aware zero-timeout signature: cycling distinct handles, addresses, or caller lr with identical x19-x28 must not back off
    {
        uint64_t er2[32] = {0}, er3[32] = {0};
        svc_thread(0x45, er2); assert(er2[0] == 0); uint32_t ev2_w = (uint32_t)er2[1], ev2_r = (uint32_t)er2[2];
        svc_thread(0x45, er3); assert(er3[0] == 0); uint32_t ev3_w = (uint32_t)er3[1], ev3_r = (uint32_t)er3[2];
        uint32_t evs[3] = {ev_r, ev2_r, ev3_r};
        uint32_t h = 0;
        atomic_fetch_add(&poll_clock, 30000); z0 = backoffs();
        for (int i = 0; i < 300; i++) {
            h = evs[i % 3];
            uint64_t wr[32] = {0, (uint64_t)&h, 1, 0};
            svc_thread(0x18, wr); assert(wr[0] == R_TIMEOUT);
            atomic_fetch_add(&poll_clock, 10000);
        }
        assert(backoffs() == z0);
        uint64_t cr[32] = {ev2_w}; svc_thread(0x16, cr); cr[0] = ev2_r; svc_thread(0x16, cr);
        cr[0] = ev3_w; svc_thread(0x16, cr); cr[0] = ev3_r; svc_thread(0x16, cr);
    }
    {
        _Atomic uint32_t words[3] = {1, 1, 1};
        atomic_fetch_add(&poll_clock, 30000); z0 = backoffs();
        for (int i = 0; i < 300; i++) {
            uint64_t ar[32] = {(uint64_t)&words[i % 3], 2, 1, 0};
            svc_thread(0x34, ar); assert(ar[0] == R_TIMEOUT);
            atomic_fetch_add(&poll_clock, 10000);
        }
        assert(backoffs() == z0);
    }
    {
        atomic_store(&addr_word, 1);
        atomic_fetch_add(&poll_clock, 30000); z0 = backoffs();
        for (int i = 0; i < 300; i++) {
            uint64_t ar[32] = {(uint64_t)&addr_word, 2, 1, 0};
            ar[30] = 0x100000ull + (uint64_t)(i % 3) * 0x100ull;
            svc_thread(0x34, ar); assert(ar[0] == R_TIMEOUT);
            atomic_fetch_add(&poll_clock, 10000);
        }
        assert(backoffs() == z0);
    }
    {
        // Alternating addr_poll(1) and SleepThread(0) with identical x19-x28 still backs off via the two-slot history
        atomic_store(&addr_word, 1);
        atomic_fetch_add(&poll_clock, 30000); z0 = backoffs();
        for (int i = 0; i < 202; i++) {
            assert(addr_poll(1) == R_TIMEOUT);
            uint64_t yr[32] = {0}; svc_thread(0x0B, yr);
            atomic_fetch_add(&poll_clock, 10000);
        }
        assert(backoffs() > z0);
    }
    puts("PASS: zero-timeout tries across distinct targets or call sites never back off, while alternating wait+yield with identical registers still does");

    r[0] = ev_w; svc_thread(0x16, r); r[0] = ev_r; svc_thread(0x16, r);
    return 0;
}
