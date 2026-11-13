// The sync SVCs through real guest threads, driven the way the SDK's nn::os (and libnx) drive them from user mode: ArbitrateLock / ArbitrateUnlock (0x1A / 0x1B),
// WaitProcessWideKeyAtomic / SignalProcessWideKey (0x1C / 0x1D), WaitForAddress / SignalToAddress (0x34 / 0x35), WaitSynchronization / CancelSynchronization (0x18 / 0x19) and ExitThread.
// Threads are made with CreateThread / StartThread; the real thread_main runs them, and dm_thread_enter below calls a C entry that ends in ExitThread.
// Expected values follow Mesosphere (KConditionVariable, KAddressArbiter, KSynchronization): waiters woken in priority order, the earlier first at equal priority; wake-one and wake-all;
// timeouts, also while a signalled condition waiter waits for its lock; cancellation; the arithmetic of SignalToAddress; the deterministic clock.
// A lost-wakeup stress (a bounded queue over lock and condition, a semaphore over the address arbiter, random timeouts) runs T_SYNC_SECS seconds (default 3) under a watchdog.
// t_sync_wait checks WaitSynchronization's handle rules with hand-made thread records; this test checks the waits' order and wakeups between real threads.
#include "../dm.h"
#include <stdatomic.h>
#include <unistd.h>

// The deterministic clock, which the test advances by hand (t_condvar_stub.c's runs on host time)
static _Atomic int det_on; static _Atomic uint64_t det_ns;
static uint64_t test_guest_ns(void) { return atomic_load(&det_on) ? atomic_load(&det_ns) : now_ns(); }
static int test_detclock(void) { return atomic_load(&det_on); }
#define guest_ns test_guest_ns
#define dm_detclock_enabled test_detclock
#include "../hle_thread.c"
#undef guest_ns
#undef dm_detclock_enabled

#define W_BIT WAITERS_BIT
static int g_fail, g_pass;
#define CHECK(c, ...) do { if (c) { g_pass++; printf("PASS: " __VA_ARGS__); } else { g_fail++; printf("[RED] FAIL: " __VA_ARGS__); } printf("\n"); } while (0)
static uint64_t ms_now(void) { return now_ns() / 1000000; }
static void msleep(unsigned ms) { usleep(ms * 1000); }
static uint64_t sv(int num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) { uint64_t r[32] = {a0, a1, a2, a3}; svc_thread(num, r); return r[0]; }
// Poll until cond holds or ms pass; its value is whether it held
#define WAIT_FOR(cond, ms) ({ uint64_t _t = ms_now(); int _ok; while (!(_ok = (cond)) && ms_now() - _t < (ms)) usleep(200); _ok; })
// Threads waiting in kind (1 lock, 2 condition, 3 address, 4 handles) on addr (the condition key for kind 2; any for kind 4)
static int waiters(int kind, uint64_t addr) {
    int n = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_wait; t; t = t->next) n += t->wkind == kind && (kind == 4 || (kind == 2 ? t->wkey : t->waddr) == addr);
    pthread_mutex_unlock(&G);
    return n;
}

// ---- Guest threads ----
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) { (void)sp; ((void (*)(uint64_t))entry)(arg); abort(); }
typedef struct { void (*fn)(void *); void *arg; } start_t;
static void entry(uint64_t p) { start_t *s = (start_t *)p; s->fn(s->arg); free(s); sv(0x0A, 0, 0, 0, 0); }
static uint32_t spawn(void (*fn)(void *), void *arg, int prio, int core) {
    start_t *s = malloc(sizeof *s); s->fn = fn; s->arg = arg;
    uint64_t r[32] = {0, (uint64_t)entry, (uint64_t)s, 0, (uint64_t)prio, (uint64_t)core}; svc_thread(0x08, r);
    uint32_t h = (uint32_t)r[1]; sv(0x09, h, 0, 0, 0); return h;
}
static void join(uint32_t h) { uint32_t hs[1] = {h}; uint64_t r[32] = {0, (uint64_t)hs, 1, (uint64_t)-1}; svc_thread(0x18, r); if (r[0]) printf("join %#x: %#llx\n", h, r[0]); sv(0x16, h, 0, 0, 0); }

// ---- The SDK's user-mode lock and condition variable over the SVCs (libnx mutex.c / condvar.c; nn::os reads the key before SignalProcessWideKey) ----
typedef _Atomic uint32_t word_t;
static void mtx_lock(word_t *m) {
    uint32_t self = thr_cur_handle();
    for (;;) {
        uint32_t cur = 0;
        if (atomic_compare_exchange_strong(m, &cur, self)) return;
        if ((cur & ~W_BIT) == self) return;
        if (!(cur & W_BIT)) { uint32_t want = cur | W_BIT; if (!atomic_compare_exchange_strong(m, &cur, want)) continue; cur = want; }
        sv(0x1A, cur & ~W_BIT, (uint64_t)m, self, 0);
        if ((atomic_load(m) & ~W_BIT) == self) return;
    }
}
static void mtx_unlock(word_t *m) { uint32_t self = thr_cur_handle(), cur = self; if (!atomic_compare_exchange_strong(m, &cur, 0)) sv(0x1B, (uint64_t)m, 0, 0, 0); }
static uint64_t cv_wait(word_t *key, word_t *m, int64_t ns) {   // holding m; returns holding m
    uint64_t r = sv(0x1C, (uint64_t)m, (uint64_t)key, thr_cur_handle(), (uint64_t)ns);
    if (r == R_TIMEOUT) mtx_lock(m);
    return r;
}
static void cv_signal(word_t *key, int n) { if (atomic_load(key)) sv(0x1D, (uint64_t)key, (uint64_t)(int64_t)n, 0, 0); }

// ---- Who woke, in order ----
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER; static int g_log[16], g_nlog;
static void log_woke(int id) { pthread_mutex_lock(&g_log_mu); g_log[g_nlog++] = id; pthread_mutex_unlock(&g_log_mu); }
static int nlog(void) { pthread_mutex_lock(&g_log_mu); int n = g_nlog; pthread_mutex_unlock(&g_log_mu); return n; }
static void log_reset(void) { pthread_mutex_lock(&g_log_mu); g_nlog = 0; pthread_mutex_unlock(&g_log_mu); }
static int log_is(const int *want, int n) { pthread_mutex_lock(&g_log_mu); int ok = g_nlog == n; for (int i = 0; ok && i < n; i++) ok = g_log[i] == want[i]; pthread_mutex_unlock(&g_log_mu); return ok; }
static const char *log_str(void) { static char b[128]; int o = 0; pthread_mutex_lock(&g_log_mu); for (int i = 0; i < g_nlog; i++) o += snprintf(b + o, sizeof b - o, "%s%d", i ? "," : "", g_log[i]); pthread_mutex_unlock(&g_log_mu); b[o] = 0; return b; }

// Priorities in arrival order (id = index); expected wake order: by priority, the earlier first at equal priority
static const int PRIO[5] = {40, 30, 20, 30, 50}, ORDER[5] = {2, 1, 3, 0, 4};
static word_t g_a, g_m, g_key;
typedef struct { int id; } targ_t; static targ_t g_targ[8];

static void addr_waiter(void *p) { targ_t *a = p; uint64_t r = sv(0x34, (uint64_t)&g_a, 2, 0, (uint64_t)-1); if (r == 0) log_woke(a->id); else printf("addr waiter %d: %#llx\n", a->id, r); }
static void cv_waiter(void *p) { targ_t *a = p; mtx_lock(&g_m); uint64_t r = cv_wait(&g_key, &g_m, -1); if (r == 0) log_woke(a->id); else printf("cv waiter %d: %#llx\n", a->id, r); mtx_unlock(&g_m); }
static void lock_waiter(void *p) { targ_t *a = p; mtx_lock(&g_m); log_woke(a->id); mtx_unlock(&g_m); }

static void test_priority_order(void) {
    uint32_t h[5];
    log_reset(); atomic_store(&g_a, 0);
    for (int i = 0; i < 5; i++) { g_targ[i].id = i; h[i] = spawn(addr_waiter, &g_targ[i], PRIO[i], i % 3); WAIT_FOR(waiters(3, (uint64_t)&g_a) == i + 1, 2000); }
    for (int i = 0; i < 5; i++) { sv(0x35, (uint64_t)&g_a, 0, 0, 1); WAIT_FOR(nlog() == i + 1, 2000); }
    CHECK(log_is(ORDER, 5), "SignalToAddress wakes in priority order, the earlier first at equal priority: %s (expected 2,1,3,0,4)", log_str());
    for (int i = 0; i < 5; i++) join(h[i]);
    // SignalProcessWideKey(count 1): each woken waiter waits for the lock the signaller holds, then gets it on unlock
    log_reset(); atomic_store(&g_m, 0); atomic_store(&g_key, 0);
    for (int i = 0; i < 5; i++) { h[i] = spawn(cv_waiter, &g_targ[i], PRIO[i], i % 3); WAIT_FOR(waiters(2, (uint64_t)&g_key) == i + 1, 2000); }
    for (int i = 0; i < 5; i++) { mtx_lock(&g_m); cv_signal(&g_key, 1); mtx_unlock(&g_m); WAIT_FOR(nlog() == i + 1, 2000); }
    CHECK(log_is(ORDER, 5) && atomic_load(&g_key) == 0, "SignalProcessWideKey wakes in priority order: %s, condition key %u (expected 2,1,3,0,4 and 0)", log_str(), atomic_load(&g_key));
    for (int i = 0; i < 5; i++) join(h[i]);
    // ArbitrateUnlock hands the lock to the best waiter
    log_reset(); atomic_store(&g_m, 0); mtx_lock(&g_m);
    for (int i = 0; i < 5; i++) { h[i] = spawn(lock_waiter, &g_targ[i], PRIO[i], i % 3); WAIT_FOR(waiters(1, (uint64_t)&g_m) == i + 1, 2000); }
    CHECK(atomic_load(&g_m) == (thr_cur_handle() | W_BIT), "lock word with five waiters = owner | waiters bit (%#x)", atomic_load(&g_m));
    mtx_unlock(&g_m); WAIT_FOR(nlog() == 5, 2000);
    CHECK(log_is(ORDER, 5) && atomic_load(&g_m) == 0, "ArbitrateUnlock hands over in priority order: %s, lock word at the end %#x (expected 2,1,3,0,4 and 0)", log_str(), atomic_load(&g_m));
    for (int i = 0; i < 5; i++) join(h[i]);
}

static void test_wake_one_all(void) {
    uint32_t h[4];
    log_reset(); atomic_store(&g_a, 0);
    for (int i = 0; i < 4; i++) { g_targ[i].id = i; h[i] = spawn(addr_waiter, &g_targ[i], 44, i % 3); }
    WAIT_FOR(waiters(3, (uint64_t)&g_a) == 4, 2000);
    sv(0x35, (uint64_t)&g_a, 0, 0, 1); WAIT_FOR(nlog() == 1, 2000); msleep(30);
    CHECK(nlog() == 1 && waiters(3, (uint64_t)&g_a) == 3, "SignalToAddress count 1 wakes exactly one of four (%d woken, %d still waiting)", nlog(), waiters(3, (uint64_t)&g_a));
    sv(0x35, (uint64_t)&g_a, 0, 0, (uint64_t)-1); WAIT_FOR(nlog() == 4, 2000);
    CHECK(nlog() == 4 && waiters(3, (uint64_t)&g_a) == 0, "SignalToAddress count -1 wakes all the others (%d woken)", nlog());
    for (int i = 0; i < 4; i++) join(h[i]);
    log_reset(); atomic_store(&g_m, 0); atomic_store(&g_key, 0);
    for (int i = 0; i < 4; i++) h[i] = spawn(cv_waiter, &g_targ[i], 44, i % 3);
    WAIT_FOR(waiters(2, (uint64_t)&g_key) == 4, 2000);
    cv_signal(&g_key, 1); WAIT_FOR(nlog() == 1, 2000); msleep(30);
    CHECK(nlog() == 1 && waiters(2, (uint64_t)&g_key) == 3 && atomic_load(&g_key) == 1, "SignalProcessWideKey count 1 wakes exactly one of four, the key stays 1 (%d woken, key %u)", nlog(), atomic_load(&g_key));
    cv_signal(&g_key, -1); WAIT_FOR(nlog() == 4, 2000);
    CHECK(nlog() == 4 && atomic_load(&g_key) == 0 && atomic_load(&g_m) == 0, "SignalProcessWideKey count -1 wakes the rest through the lock one by one, key 0, lock free (%d woken, key %u, lock %#x)", nlog(), atomic_load(&g_key), atomic_load(&g_m));
    for (int i = 0; i < 4; i++) join(h[i]);
}

static void test_timeouts(void) {
    word_t m = 0, key = 0, a = 0;
    mtx_lock(&m); uint64_t t0 = now_ns();
    uint64_t r = sv(0x1C, (uint64_t)&m, (uint64_t)&key, thr_cur_handle(), 5000000);
    uint64_t dt = now_ns() - t0; uint32_t mw = atomic_load(&m);
    CHECK(r == R_TIMEOUT && dt >= 5000000 && dt < 200000000 && mw == 0, "WaitProcessWideKeyAtomic 5 ms: timeout after %.2f ms, lock released (%#x)", dt / 1e6, mw);
    t0 = now_ns(); r = sv(0x34, (uint64_t)&a, 2, 0, 5000000); dt = now_ns() - t0;
    CHECK(r == R_TIMEOUT && dt >= 5000000 && dt < 200000000, "WaitForAddress 5 ms: timeout after %.2f ms", dt / 1e6);
    t0 = now_ns(); r = sv(0x34, (uint64_t)&a, 2, 0, 0); dt = now_ns() - t0;
    CHECK(r == R_TIMEOUT && dt < 5000000, "WaitForAddress timeout 0 returns at once (%#llx)", r);
    uint64_t ev[32] = {0}; svc_thread(0x45, ev); uint32_t hs[1] = {(uint32_t)ev[2]};
    t0 = now_ns(); uint64_t rr[32] = {0, (uint64_t)hs, 1, 5000000}; svc_thread(0x18, rr); dt = now_ns() - t0;
    CHECK(rr[0] == R_TIMEOUT && dt >= 5000000 && dt < 200000000, "WaitSynchronization 5 ms: timeout after %.2f ms", dt / 1e6);
    sv(0x16, ev[1], 0, 0, 0); sv(0x16, ev[2], 0, 0, 0);
    CHECK(waiters(1, (uint64_t)&m) + waiters(2, (uint64_t)&key) + waiters(3, (uint64_t)&a) == 0, "no waiter left behind after the timeouts");
}

// A signalled condition waiter that has to wait for its lock still times out (Mesosphere: the lock wait keeps the timeout and is taken off the owner's waiter list)
static _Atomic uint64_t g_cvl_res, g_cvl_done; static _Atomic int g_cvl_left;
static void cvl_waiter(void *p) {
    int64_t ns = *(int64_t *)p; mtx_lock(&g_m);
    uint64_t r = sv(0x1C, (uint64_t)&g_m, (uint64_t)&g_key, thr_cur_handle(), (uint64_t)ns);
    atomic_store(&g_cvl_left, waiters(1, (uint64_t)&g_m)); atomic_store(&g_cvl_res, r); atomic_store(&g_cvl_done, 1);
    if (r == R_TIMEOUT) mtx_lock(&g_m);
    mtx_unlock(&g_m);
}
static void test_cv_to_lock(void) {
    atomic_store(&g_m, 0); atomic_store(&g_key, 0);
    for (int timed = 0; timed < 2; timed++) {
        static int64_t ns; ns = timed ? 100000000 : -1; atomic_store(&g_cvl_done, 0);
        uint32_t h = spawn(cvl_waiter, &ns, 44, 1);
        WAIT_FOR(waiters(2, (uint64_t)&g_key) == 1, 2000);
        mtx_lock(&g_m); cv_signal(&g_key, 1);
        int moved = WAIT_FOR(waiters(1, (uint64_t)&g_m) == 1, 2000); uint32_t mw = atomic_load(&g_m);
        CHECK(moved && mw == (thr_cur_handle() | W_BIT), "%s: SignalProcessWideKey with the lock held turns the waiter into a lock waiter (lock word %#x)", timed ? "timed" : "untimed", mw);
        if (timed) WAIT_FOR(atomic_load(&g_cvl_done), 2000); else msleep(30);   // the timed wait ends 100 ms in; the untimed one must still be waiting
        if (timed) CHECK(atomic_load(&g_cvl_done) && atomic_load(&g_cvl_res) == R_TIMEOUT && atomic_load(&g_cvl_left) == 0, "timed: the lock wait times out (%#llx) and leaves the lock's waiter list (%d left)", atomic_load(&g_cvl_res), atomic_load(&g_cvl_left));
        else CHECK(!atomic_load(&g_cvl_done), "untimed: still waiting for the lock while it is held");
        mtx_unlock(&g_m); WAIT_FOR(atomic_load(&g_cvl_done), 2000);
        if (!timed) CHECK(atomic_load(&g_cvl_res) == 0, "untimed: woken with the lock on unlock (%#llx)", atomic_load(&g_cvl_res));
        join(h);
        CHECK(atomic_load(&g_m) == 0, "lock free at the end (%#x)", atomic_load(&g_m));
    }
}

static void test_signal_arith(void) {
    word_t a = 5;
    CHECK(sv(0x35, (uint64_t)&a, 1, 4, 1) == R_BAD_STATE && a == 5, "SignalToAddress increment-if-equal, value differs: invalid state, value kept");
    CHECK(sv(0x35, (uint64_t)&a, 1, 5, 1) == 0 && a == 6, "increment-if-equal: 5 -> %u", a);
    CHECK(sv(0x35, (uint64_t)&a, 2, 6, 1) == 0 && a == 7, "modify-by-waiting-count, no waiters: 6 -> %u (+1)", a);
    uint32_t h[2]; atomic_store(&g_a, 0); log_reset();
    for (int i = 0; i < 2; i++) g_targ[i].id = i;
    for (int i = 0; i < 2; i++) h[i] = spawn(addr_waiter, &g_targ[i], 44, i);
    WAIT_FOR(waiters(3, (uint64_t)&g_a) == 2, 2000);
    CHECK(sv(0x35, (uint64_t)&g_a, 2, 0, 1) == 0 && g_a == 0 && WAIT_FOR(nlog() == 1, 2000), "modify-by-waiting-count, 2 waiters, count 1: value kept (%u), one woken", (unsigned)g_a);
    CHECK(sv(0x35, (uint64_t)&g_a, 2, 0, 1) == 0 && g_a == (uint32_t)-1 && WAIT_FOR(nlog() == 2, 2000), "modify-by-waiting-count, 1 waiter, count 1: value - 1 (%d)", (int)g_a);
    for (int i = 0; i < 2; i++) join(h[i]);
    atomic_store(&g_a, 0); log_reset();
    for (int i = 0; i < 2; i++) h[i] = spawn(addr_waiter, &g_targ[i], 44, i);
    WAIT_FOR(waiters(3, (uint64_t)&g_a) == 2, 2000);
    CHECK(sv(0x35, (uint64_t)&g_a, 2, 0, (uint64_t)-1) == 0 && (int)g_a == -2 && WAIT_FOR(nlog() == 2, 2000), "modify-by-waiting-count, waiters, all: value - 2 (%d), all woken", (int)g_a);
    for (int i = 0; i < 2; i++) join(h[i]);
    word_t b = 3; _Atomic uint64_t q = 0x123456789ull;
    CHECK(sv(0x34, (uint64_t)&b, 0, 3, 0) == R_BAD_STATE, "WaitForAddress less-than, not less: invalid state");
    CHECK(sv(0x34, (uint64_t)&b, 1, 4, 0) == R_TIMEOUT && b == 2, "WaitForAddress decrement-if-less-than: 3 -> %u, then waits (timeout 0)", b);
    CHECK(sv(0x34, (uint64_t)&b, 2, 9, 0) == R_BAD_STATE, "WaitForAddress equal, differs: invalid state");
    CHECK(sv(0x34, (uint64_t)&q, 3, 0x123456789ull, 0) == R_TIMEOUT && sv(0x34, (uint64_t)&q, 3, 0x23456789ull, 0) == R_BAD_STATE, "WaitForAddress 64-bit equal compares all 64 bits");
}

static uint32_t g_ev_r, g_ev_w; static _Atomic uint64_t g_cancel_res; static _Atomic int g_cancel_state;
static void cancel_waiter(void *p) {
    (void)p; uint32_t hs[1] = {g_ev_r}; uint64_t r[32] = {0, (uint64_t)hs, 1, (uint64_t)-1};
    if (atomic_load(&g_cancel_state) == 1) WAIT_FOR(atomic_load(&g_cancel_state) == 2, 2000);   // the cancel comes while this thread runs
    svc_thread(0x18, r); atomic_store(&g_cancel_res, r[0]);
}
static uint32_t g_victim; static _Atomic uint64_t g_exit_res, g_exit_idx;
static void exit_waiter(void *p) { (void)p; uint32_t hs[2] = {g_ev_r, g_victim}; uint64_t r[32] = {0, (uint64_t)hs, 2, (uint64_t)-1}; svc_thread(0x18, r); atomic_store(&g_exit_idx, r[1]); atomic_store(&g_exit_res, r[0]); }
static _Atomic int g_victim_go; static void victim(void *p) { (void)p; WAIT_FOR(atomic_load(&g_victim_go), 5000); }
static void test_cancel_exit(void) {
    uint64_t ev[32] = {0}; svc_thread(0x45, ev); g_ev_w = (uint32_t)ev[1]; g_ev_r = (uint32_t)ev[2];
    atomic_store(&g_cancel_res, 1); atomic_store(&g_cancel_state, 0);
    uint32_t h = spawn(cancel_waiter, NULL, 44, 1);
    WAIT_FOR(waiters(4, 0) == 1, 2000); sv(0x19, h, 0, 0, 0); WAIT_FOR(atomic_load(&g_cancel_res) != 1, 2000);
    CHECK(atomic_load(&g_cancel_res) == R_CANCELLED, "CancelSynchronization ends a WaitSynchronization with cancelled (%#llx)", atomic_load(&g_cancel_res));
    join(h);
    atomic_store(&g_cancel_res, 1); atomic_store(&g_cancel_state, 1);
    h = spawn(cancel_waiter, NULL, 44, 1); sv(0x19, h, 0, 0, 0); atomic_store(&g_cancel_state, 2); WAIT_FOR(atomic_load(&g_cancel_res) != 1, 2000);
    CHECK(atomic_load(&g_cancel_res) == R_CANCELLED, "a cancel sent while not waiting ends the next WaitSynchronization at once (%#llx)", atomic_load(&g_cancel_res));
    join(h);
    atomic_store(&g_victim_go, 0); g_victim = spawn(victim, NULL, 44, 2); atomic_store(&g_exit_res, 1);
    uint32_t w = spawn(exit_waiter, NULL, 44, 1);
    WAIT_FOR(waiters(4, 0) == 1, 2000); msleep(10);
    CHECK(atomic_load(&g_exit_res) == 1, "a waiter on an event and a running thread keeps waiting");
    atomic_store(&g_victim_go, 1); WAIT_FOR(atomic_load(&g_exit_res) != 1, 2000);
    CHECK(atomic_load(&g_exit_res) == 0 && atomic_load(&g_exit_idx) == 1, "ExitThread signals the thread's handle: woken with index %llu (%#llx)", atomic_load(&g_exit_idx), atomic_load(&g_exit_res));
    join(w); join(g_victim);
    sv(0x16, g_ev_w, 0, 0, 0); sv(0x16, g_ev_r, 0, 0, 0);
}

// Deterministic clock: timeouts count guest time, and waiters look at their deadline again when the guest clock moves (dm_guest_clock_advanced)
static _Atomic uint64_t g_det_res;
static void det_waiter(void *p) { (void)p; atomic_store(&g_det_res, sv(0x34, (uint64_t)&g_a, 2, 0, 10000000)); }
static void test_det_clock(void) {
    atomic_store(&g_a, 0); atomic_store(&det_ns, 1000000000ull); atomic_store(&det_on, 1); atomic_store(&g_det_res, 1);
    uint32_t h = spawn(det_waiter, NULL, 44, 1);
    WAIT_FOR(waiters(3, (uint64_t)&g_a) == 1, 2000); msleep(30);
    CHECK(atomic_load(&g_det_res) == 1, "deterministic clock: a 10 ms guest timeout does not expire on host time");
    atomic_fetch_add(&det_ns, 5000000); dm_guest_clock_advanced(); msleep(30);
    CHECK(atomic_load(&g_det_res) == 1 && waiters(3, (uint64_t)&g_a) == 1, "guest clock +5 ms: still waiting");
    atomic_fetch_add(&det_ns, 6000000); dm_guest_clock_advanced(); WAIT_FOR(atomic_load(&g_det_res) != 1, 2000);
    CHECK(atomic_load(&g_det_res) == R_TIMEOUT && waiters(3, (uint64_t)&g_a) == 0, "guest clock +11 ms: timeout (%#llx)", atomic_load(&g_det_res));
    join(h); atomic_store(&det_on, 0);
}

// A timeout racing a wake: whichever comes first, the waiter leaves every queue (a stale entry would change SignalToAddress's waiter count)
static _Atomic int g_race_stop; static _Atomic uint64_t g_race_n, g_race_woke;
static void race_waiter(void *p) {
    (void)p;
    while (!atomic_load(&g_race_stop)) {
        uint64_t r = sv(0x34, (uint64_t)&g_a, 2, 0, 20000 + (atomic_load(&g_race_n) % 7) * 10000);
        atomic_fetch_add(&g_race_n, 1); if (!r) atomic_fetch_add(&g_race_woke, 1);
    }
}
static void test_timeout_race(void) {
    atomic_store(&g_a, 0); atomic_store(&g_race_stop, 0); atomic_store(&g_race_n, 0); atomic_store(&g_race_woke, 0);
    uint32_t h[3]; for (int i = 0; i < 3; i++) h[i] = spawn(race_waiter, NULL, 44, i);
    uint64_t t0 = ms_now(); while (ms_now() - t0 < 500) { sv(0x35, (uint64_t)&g_a, 0, 0, (uint64_t)-1); usleep(13); }
    atomic_store(&g_race_stop, 1); WAIT_FOR(waiters(3, (uint64_t)&g_a) == 0, 500);
    sv(0x35, (uint64_t)&g_a, 0, 0, (uint64_t)-1); for (int i = 0; i < 3; i++) join(h[i]);
    CHECK(waiters(3, (uint64_t)&g_a) == 0 && atomic_load(&g_race_woke) > 0 && atomic_load(&g_race_woke) < atomic_load(&g_race_n),
          "timeout racing wake-all: %llu waits, %llu woken, the rest timed out, none left queued", atomic_load(&g_race_n), atomic_load(&g_race_woke));
    // With no waiter left, SignalToAddress modify-by-waiting-count adds 1
    atomic_store(&g_a, 0); CHECK(sv(0x35, (uint64_t)&g_a, 2, 0, (uint64_t)-1) == 0 && g_a == 1, "modify-by-waiting-count afterwards sees no waiter (value %u)", (unsigned)g_a);
}

// ---- Lost-wakeup stress: a bounded queue over lock + condition (random timeouts), a counting semaphore over WaitForAddress / SignalToAddress, a watchdog on progress ----
enum { QN = 4 };
static word_t s_m, s_ne, s_nf; static int s_q[QN], s_qh, s_qn; static _Atomic uint64_t s_prod, s_cons, s_sum_in, s_sum_out, s_progress;
static _Atomic int s_stop; static word_t s_sem = 2; static _Atomic int s_inside, s_inside_max;
static int64_t rnd_timeout(unsigned *seed) { unsigned r = rand_r(seed) % 4; return r == 0 ? 0 : r == 1 ? 1000000 : r == 2 ? 50000 : -1; }
static void s_producer(void *p) {
    unsigned seed = (unsigned)(uintptr_t)p;
    for (int v = 1;; v++) {
        int stop = atomic_load(&s_stop), item = stop ? -1 : v;
        mtx_lock(&s_m);
        while (s_qn == QN) cv_wait(&s_nf, &s_m, rnd_timeout(&seed));
        s_q[(s_qh + s_qn++) % QN] = item; cv_signal(&s_ne, 1);
        mtx_unlock(&s_m);
        if (stop) return;
        atomic_fetch_add(&s_prod, 1); atomic_fetch_add(&s_sum_in, (uint64_t)v); atomic_fetch_add(&s_progress, 1);
    }
}
static void s_consumer(void *p) {
    unsigned seed = (unsigned)(uintptr_t)p;
    for (;;) {
        mtx_lock(&s_m);
        while (s_qn == 0) cv_wait(&s_ne, &s_m, rnd_timeout(&seed));
        int item = s_q[s_qh]; s_qh = (s_qh + 1) % QN; s_qn--; cv_signal(&s_nf, 1);
        mtx_unlock(&s_m);
        if (item < 0) return;
        atomic_fetch_add(&s_cons, 1); atomic_fetch_add(&s_sum_out, (uint64_t)item); atomic_fetch_add(&s_progress, 1);
    }
}
static void sem_acquire(unsigned *seed) {
    for (;;) {
        uint32_t v = atomic_load(&s_sem);
        if ((int32_t)v > 0) { if (atomic_compare_exchange_strong(&s_sem, &v, v - 1)) return; continue; }
        sv(0x34, (uint64_t)&s_sem, 0, 1, (uint64_t)rnd_timeout(seed));   // wait while the count is below 1
    }
}
static void sem_release(void) { atomic_fetch_add(&s_sem, 1); sv(0x35, (uint64_t)&s_sem, 0, 0, 1); }
static void s_sem_user(void *p) {
    unsigned seed = (unsigned)(uintptr_t)p;
    while (!atomic_load(&s_stop)) {
        sem_acquire(&seed);
        int in = atomic_fetch_add(&s_inside, 1) + 1, mx = atomic_load(&s_inside_max);
        while (in > mx && !atomic_compare_exchange_weak(&s_inside_max, &mx, in)) {}
        for (volatile int i = 0; i < 50; i++) {}
        atomic_fetch_sub(&s_inside, 1); sem_release(); atomic_fetch_add(&s_progress, 1);
    }
}
static void test_stress(void) {
    const char *e = getenv("T_SYNC_SECS"); int secs = e ? atoi(e) : 3;
    uint32_t h[10]; int n = 0;
    // Below the main thread's priority (44): a thread of higher priority on its core that never blocks (a semaphore user while permits are free) would hold its StartThread
    // for good, as Horizon's scheduler would (svc_thread's same-core handoff), and the watchdog would never run
    for (int i = 0; i < 3; i++) h[n++] = spawn(s_producer, (void *)(uintptr_t)(i + 1), 45 + i, i % 3);
    for (int i = 0; i < 3; i++) h[n++] = spawn(s_consumer, (void *)(uintptr_t)(i + 11), 46 + i, (i + 1) % 3);
    for (int i = 0; i < 4; i++) h[n++] = spawn(s_sem_user, (void *)(uintptr_t)(i + 21), 47 + i, i % 3);
    uint64_t t0 = ms_now(), last = 0, last_t = t0;
    while (ms_now() - t0 < (uint64_t)secs * 1000) {
        msleep(100); uint64_t p = atomic_load(&s_progress);
        if (p != last) { last = p; last_t = ms_now(); }
        else if (ms_now() - last_t > 5000) { printf("[RED] FAIL: no progress for 5 s (a lost wakeup); thread dump on stderr\n"); svc_thread_dump(); _exit(1); }
    }
    atomic_store(&s_stop, 1);   // each producer sends one -1, which stops one consumer; the semaphore users stop on s_stop
    for (int i = 0; i < n; i++) join(h[i]);
    CHECK(s_prod == s_cons && s_sum_in == s_sum_out && s_qn == 0, "stress %d s: %llu produced = %llu consumed, the sums equal, no lost wakeup", secs, s_prod, s_cons);
    CHECK(atomic_load(&s_inside_max) <= 2 && atomic_load(&s_sem) == 2, "stress: the address semaphore let at most 2 in (max %d), its count back to 2 (%u)", atomic_load(&s_inside_max), atomic_load(&s_sem));
}

// A watchdog thread rather than an alarm signal: the thread dump is not async-signal-safe
static void *watchdog(void *p) { sleep((unsigned)(uintptr_t)p); printf("[RED] FAIL: t_sync timed out; thread dump on stderr\n"); svc_thread_dump(); _exit(1); }
int main(void) {
    const char *e = getenv("T_SYNC_SECS"); pthread_t wd; pthread_create(&wd, NULL, watchdog, (void *)(uintptr_t)(30 + (e ? atoi(e) : 3)));
    setenv("DM_PREEMPT", "0", 1); setvbuf(stdout, NULL, _IOLBF, 0);
    svc_thread_main_init();
    test_priority_order();
    test_wake_one_all();
    test_timeouts();
    test_cv_to_lock();
    test_signal_arith();
    test_cancel_exit();
    test_det_clock();
    test_timeout_race();
    test_stress();
    printf(g_fail ? "[RED] t_sync: %d of %d checks failed\n" : "t_sync: all %d checks passed\n", g_fail ? g_fail : g_pass, g_pass + g_fail);
    fflush(stdout); _exit(g_fail != 0);
}
