// The cost of a same-core handoff's wait (dm_yield_to_core_peers).
// Catherine: Full Body's main thread (priority 44, core 0) and job_update_0 (priority 43, core 0) trade a lock 5,000-6,200 times a second in some scenes: each release wakes
// the worker, the main thread waits until it blocks again, and the worker blocks after 5-20 µs. The wait was a loop of 50 µs nanosleeps, so every handoff cost at least one
// timer wakeup (docs/reference/threads.md, "A same-core handoff waits without a timer per handoff").
// Real thread SVCs: the worker is a guest thread made by CreateThread, woken by an event as the game's lock wakes it, and the main thread's SignalEvent waits for it
#include "../dm.h"
#include <assert.h>
#include <signal.h>
#include <stdatomic.h>
#include <unistd.h>
#include "../hle_thread.c"
static void timeout(int sig) { (void)sig; const char m[] = "[RED] handoff wait test timed out\n"; write(2, m, sizeof m - 1); _exit(99); }
// The real thread_main registers the pthread and waits for StartThread; only the jump to a guest stack is replaced by this C entry point
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) {
    (void)sp;
    ((void (*)(uint64_t))entry)(arg);
    uint64_t r[32] = {0}; svc_thread(0x0A, r);
    abort();
}
static uint32_t ev_w, ev_r;
static _Atomic int done, stop, host_sleep;
static void worker(uint64_t unused) {   // job_update_0's shape: wait, a few µs of work, wait again; or a host sleep in its work (the backstop case)
    (void)unused;
    for (;;) {
        uint32_t h = ev_r; uint64_t r[32] = {0, (uint64_t)&h, 1, (uint64_t)-1};
        svc_thread(0x18, r); assert(r[0] == 0);
        r[0] = ev_r; svc_thread(0x17, r); assert(r[0] == 0);
        if (atomic_load(&stop)) return;
        if (atomic_load(&host_sleep)) dm_guest_sleep_ns(5000000);   // stops running without a guest wait: nothing signals the waiter
        else { uint64_t t = now_ns(); while (now_ns() - t < 8000) {} }
        atomic_fetch_add(&done, 1);
    }
}
static void signal_worker(void) { uint64_t r[32] = {ev_w}; assert(svc_thread(0x11, r) && r[0] == 0); }
static uint64_t g_last_polls;
// n handoffs in a row: each must return after the worker's work (the ordering the handoff exists for) and without a timer wakeup of the waiter in the common case
static int storm(const char *what, int n) {
    uint64_t p0 = atomic_load(&g_hpoll), h0 = g_preempt_n, ns0 = g_preempt_ns, t0 = now_ns(); int unordered = 0;
    for (int i = 0; i < n; i++) { int want = atomic_load(&done) + 1; signal_worker(); if (atomic_load(&done) != want) unordered++; }
    uint64_t polls = atomic_load(&g_hpoll) - p0, handoffs = g_preempt_n - h0;
    // a loaded machine can keep the worker off a CPU past the spin, and then the backstop may fire: a quarter of the handoffs is far from the one poll per handoff of the old wait
    int ok = !unordered && handoffs >= (uint64_t)n * 9 / 10 && polls * 4 < (uint64_t)n;
    printf("%s: %s: %d handoffs (%llu waited) in %.1f ms, wait %.1f µs each, %llu timer polls of 50 µs, %d returned before the worker's work\n", ok ? "PASS" : "FAIL", what, n,
           (unsigned long long)handoffs, (now_ns() - t0) / 1e6, handoffs ? (g_preempt_ns - ns0) / 1e3 / handoffs : 0, (unsigned long long)polls, unordered);
    g_last_polls = polls;
    return ok;
}
int main(int argc, char **argv) {
    (void)argv;
    signal(SIGALRM, timeout); alarm(20);
    setenv("DM_PREEMPT", argc > 1 ? "1" : "0", 1);
    svc_thread_main_init();
    thr_t *self = cur(); self->mask = 1; self->core = 0; self->prio = 44;
    uint64_t r[32] = {0};
    assert(svc_thread(0x45, r) && r[0] == 0); ev_w = (uint32_t)r[1]; ev_r = (uint32_t)r[2];
    uint64_t c[32] = {0, (uint64_t)worker, 0, 0, 43, 0};
    assert(svc_thread(0x08, c) && c[0] == 0);
    uint32_t wh = (uint32_t)c[1];
    c[0] = wh; assert(svc_thread(0x09, c) && c[0] == 0);
    for (;;) { g_lock(); thr_t *w = h_get(wh, O_THREAD); int waiting = w->wkind == 4; g_unlock(); if (waiting) break; usleep(100); }
    int ok = 1;
    ok &= storm("lock storm", 3000);
    ok &= storm("lock storm again", 3000);
    // The backstop: a held thread that stops running without a guest wait (a host sleep here; host GPU work alike) releases the waiter within a poll period, not at the end of its sleep
    atomic_store(&host_sleep, 1);
    uint64_t t0 = now_ns(); signal_worker(); uint64_t waited = now_ns() - t0; int early = atomic_load(&done) == 6000;
    while (atomic_load(&done) != 6001) usleep(100);
    int bs = early && waited < 4000000;
    printf("%s: held thread in a host sleep releases the waiter after %.0f µs, before its work %d\n", bs ? "PASS" : "FAIL", waited / 1e3, early);
    ok &= bs;
    atomic_store(&host_sleep, 0); atomic_store(&stop, 1); signal_worker();
    uint32_t h = wh; uint64_t w[32] = {0, (uint64_t)&h, 1, (uint64_t)-1};
    assert(svc_thread(0x18, w) && w[0] == 0);
    w[0] = wh; assert(svc_thread(0x16, w) && w[0] == 0);
    for (int i = 0; i < 2; i++) { w[0] = i ? ev_r : ev_w; assert(svc_thread(0x16, w) && w[0] == 0); }
    return ok ? 0 : 1;
}
