// Exercise notification and the timed recheck through real thread SVCs.
// --storm measures Catherine's rapid wake/block pattern without CI timing assertions.
#include "../dm.h"
#include <assert.h>
#include <signal.h>
#include <stdatomic.h>
#include <unistd.h>
static int observe_handoff_wait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
#define pthread_cond_timedwait observe_handoff_wait
#include "../hle_thread.c"
#undef pthread_cond_timedwait

enum { WAIT_STORM, WAIT_NOTIFY, WAIT_BACKSTOP };
static _Atomic int wait_case, entered_wait;
static int observe_handoff_wait(pthread_cond_t *cv, pthread_mutex_t *mu, const struct timespec *deadline) {
    int phase = atomic_load(&wait_case);
    if (phase != WAIT_STORM && (cv == &g_hand_cv || cv == &cur()->hcv)) {
        atomic_store(&entered_wait, 1);
        // In the notification case a missing signal must fail the watchdog,
        // even if a timer would have rescued the handoff in production.
        if (phase == WAIT_NOTIFY) return pthread_cond_wait(cv, mu);
    }
    return pthread_cond_timedwait(cv, mu, deadline);
}

static void timeout(int sig) { (void)sig; const char m[] = "[RED] handoff wait test timed out\n"; write(2, m, sizeof m - 1); _exit(99); }
// The real thread_main registers the pthread and waits for StartThread; only the jump to a guest stack is replaced by this C entry point
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) {
    (void)sp;
    ((void (*)(uint64_t))entry)(arg);
    uint64_t r[32] = {0}; svc_thread(0x0A, r);
    abort();
}
static uint32_t ev_w, ev_r;
static _Atomic int done, stop;
static pthread_mutex_t host_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t host_cv = PTHREAD_COND_INITIALIZER;
static int host_resume;
static void worker(uint64_t unused) {
    (void)unused;
    for (;;) {
        uint32_t h = ev_r; uint64_t r[32] = {0, (uint64_t)&h, 1, (uint64_t)-1};
        svc_thread(0x18, r); assert(r[0] == 0);
        r[0] = ev_r; svc_thread(0x17, r); assert(r[0] == 0);
        if (atomic_load(&stop)) return;
        int phase = atomic_load(&wait_case);
        if (phase == WAIT_STORM) {
            uint64_t t = now_ns(); while (now_ns() - t < 8000) {}
        } else {
            // Stay guest-runnable until the signaller has reached its wait.
            while (!atomic_load(&entered_wait)) sched_yield();
            if (phase == WAIT_BACKSTOP) {
                pthread_mutex_lock(&host_mu);
                while (!host_resume) dm_guest_cond_wait_ns(&host_cv, &host_mu, 1000000000);  // sets sleeping without hand_signal
                pthread_mutex_unlock(&host_mu);
            }
        }
        atomic_fetch_add(&done, 1);
    }
}
static void signal_worker(void) { uint64_t r[32] = {ev_w}; assert(svc_thread(0x11, r) && r[0] == 0); }
static void until_blocked(uint32_t wh) {
    for (;;) {
        g_lock();
        thr_t *t = h_get(wh, O_THREAD);
        int blocked = t && t->wkind == 4;
        g_unlock();
        if (blocked) return;
        sched_yield();
    }
}
static int storm(int n) {
    uint64_t p0 = atomic_load(&g_hand_timeouts);
    uint64_t h0 = g_preempt_n, ns0 = g_preempt_ns, t0 = now_ns(); int unordered = 0;
    for (int i = 0; i < n; i++) { int want = atomic_load(&done) + 1; signal_worker(); if (atomic_load(&done) != want) unordered++; }
    uint64_t polls = atomic_load(&g_hand_timeouts) - p0;
    uint64_t handoffs = g_preempt_n - h0;
    printf("%d signals (%llu waited) in %.1f ms, wait %.1f µs each, %llu timer polls, %d early returns\n", n,
           (unsigned long long)handoffs, (now_ns() - t0) / 1e6, handoffs ? (g_preempt_ns - ns0) / 1e3 / handoffs : 0, (unsigned long long)polls, unordered);
    return !unordered;
}
int main(int argc, char **argv) {
    int all = 0, measure = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--all-peers")) all = 1;
        else if (!strcmp(argv[i], "--storm")) measure = 1;
        else abort();
    }
    signal(SIGALRM, timeout); alarm(20);
    setenv("DM_PREEMPT", all ? "1" : "0", 1);
    svc_thread_main_init();
    thr_t *self = cur(); self->mask = 1; self->core = 0; self->prio = 44;
    uint64_t r[32] = {0};
    assert(svc_thread(0x45, r) && r[0] == 0); ev_w = (uint32_t)r[1]; ev_r = (uint32_t)r[2];
    uint64_t c[32] = {0, (uint64_t)worker, 0, 0, 43, 0};
    assert(svc_thread(0x08, c) && c[0] == 0);
    uint32_t wh = (uint32_t)c[1];
    c[0] = wh; assert(svc_thread(0x09, c) && c[0] == 0);
    until_blocked(wh);
    int ok = 1;
    if (measure) {
        ok &= storm(3000);
        ok &= storm(3000);
    } else {
        atomic_store(&wait_case, WAIT_NOTIFY);
        signal_worker();
        assert(atomic_load(&entered_wait) && atomic_load(&done) == 1);
        puts("PASS: worker's guest wait notifies the signaller");

        atomic_store(&entered_wait, 0);
        atomic_store(&wait_case, WAIT_BACKSTOP);
        uint64_t polls = atomic_load(&g_hand_timeouts);
        signal_worker();
        assert(atomic_load(&entered_wait) && atomic_load(&done) == 1 && atomic_load(&g_hand_timeouts) > polls);
        puts("PASS: timed recheck releases the signaller during a host wait");
        pthread_mutex_lock(&host_mu);
        host_resume = 1;
        pthread_cond_signal(&host_cv);
        pthread_mutex_unlock(&host_mu);
        until_blocked(wh);
        assert(atomic_load(&done) == 2);
    }
    atomic_store(&stop, 1); signal_worker();
    uint32_t h = wh; uint64_t w[32] = {0, (uint64_t)&h, 1, (uint64_t)-1};
    assert(svc_thread(0x18, w) && w[0] == 0);
    w[0] = wh; assert(svc_thread(0x16, w) && w[0] == 0);
    for (int i = 0; i < 2; i++) { w[0] = i ? ev_r : ev_w; assert(svc_thread(0x16, w) && w[0] == 0); }
    return ok ? 0 : 1;
}
