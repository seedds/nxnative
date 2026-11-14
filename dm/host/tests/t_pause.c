// SetThreadActivity (SVC 0x32) / GetThreadContext3 (0x33): once a pause has returned success, the thread must not run guest code until the resume (Horizon's contract, which IL2CPP's
// stop-the-world collector relies on). The stale `paused` flag cases drive the three ways a pause reaches a thread: inside an SVC, in guest code (the SIGUSR2 handler), in host code.
// Build with -DDM_TEST_MODULE_OF: module_of below reports "guest code" through a thread-local flag, so the handler stops a worker only while it spins "in guest code".
// Each case prints PASS or FAIL with its reasons and all of them run; every wait is bounded, so a hang is a failure.
#include "../hle_thread.c"
#include <stdarg.h>
#include <unistd.h>
#include <mach/mach.h>

static __thread int in_guest;
module_t *module_of(const void *p) { (void)p; static char guest_module; return in_guest ? (module_t *)&guest_module : NULL; }

#define WAIT_UNTIL(cond, ms) ({ uint64_t _dl = now_ns() + (uint64_t)(ms) * 1000000ull; int _ok; while (!(_ok = !!(cond)) && now_ns() < _dl) usleep(200); _ok; })

typedef struct {
    char name[48], why[640];
    int failed;
} cs_t;
static void fail(cs_t *c, const char *fmt, ...) {
    size_t n = strlen(c->why);
    if (n) n += snprintf(c->why + n, sizeof c->why - n, "; ");
    va_list ap; va_start(ap, fmt);
    if (n < sizeof c->why) vsnprintf(c->why + n, sizeof c->why - n, fmt, ap);
    va_end(ap);
    c->failed = 1;
}
static int finish(cs_t *c) {
    printf("%s: %s%s%s\n", c->failed ? "FAIL" : "PASS", c->name, c->failed ? ": " : "", c->why);
    fflush(stdout);
    return !c->failed;
}

enum { M_GUEST, M_SVC_FIRST, M_HOST_FIRST, M_HOST_EXIT };
typedef struct {
    thr_t t; pthread_t th; int mode;
    _Atomic uint64_t progress;
    _Atomic int stop, ready, enter_go, in_svc, release, in_spin;
    uint64_t regs[100];   // the gate frame: ctx_fill reads r[96..99]
} wk_t;

static void spin_guest(wk_t *w) {
    in_guest = 1;
    atomic_store(&w->in_spin, 1);
    while (!atomic_load(&w->stop)) atomic_store_explicit(&w->progress, atomic_load_explicit(&w->progress, memory_order_relaxed) + 1, memory_order_relaxed);
    in_guest = 0;
}
static void *worker_main(void *p) {
    wk_t *w = p;
    pthread_setspecific(g_keythr, &w->t);
    w->t.mport = pthread_mach_thread_np(pthread_self());
    atomic_store(&w->ready, 1);
    if (w->mode == M_HOST_EXIT) {   // host code until released, then ExitThread (thread_exit frees a closed object nobody holds)
        while (!atomic_load(&w->release)) atomic_store_explicit(&w->progress, atomic_load_explicit(&w->progress, memory_order_relaxed) + 1, memory_order_relaxed);
        thread_exit(&w->t);
    }
    if (w->mode == M_HOST_FIRST) {   // host code: the handler declines to stop it
        while (!atomic_load(&w->enter_go) && !atomic_load(&w->stop)) atomic_store_explicit(&w->progress, atomic_load_explicit(&w->progress, memory_order_relaxed) + 1, memory_order_relaxed);
    }
    if (w->mode != M_GUEST) {   // what the SVC gate does around a service call (hle.c gate_dispatch)
        thr_svc_enter(w->regs, 0x71000);
        atomic_store(&w->in_svc, 1);
        while (!atomic_load(&w->release) && !atomic_load(&w->stop)) usleep(100);
        thr_svc_leave();
    }
    spin_guest(w);
    return NULL;
}
static wk_t *worker_start(int mode) {
    wk_t *w = calloc(1, sizeof *w);
    thr_t *t = &w->t; w->mode = mode;
    pthread_cond_init(&t->cv, NULL); pthread_cond_init(&t->pcv, NULL);
    t->started = 1; t->prio = 44; t->core = 1; t->mask = 2;
    w->regs[99] = 0x7fff0000;   // the gate block's sp
    pthread_mutex_lock(&G);
    t->handle = h_new(O_THREAD, t); t->all = g_all; g_all = t;
    pthread_mutex_unlock(&G);
    if (pthread_create(&w->th, NULL, worker_main, w)) { perror("pthread_create"); exit(2); }
    t->pt = w->th;
    if (!WAIT_UNTIL(atomic_load(&w->ready), 2000)) { fprintf(stderr, "worker did not start\n"); exit(2); }
    return w;
}
static void worker_end(wk_t *w) {
    atomic_store(&w->stop, 1);
    pthread_join(w->th, NULL);
    thr_t *t = &w->t;
    pthread_mutex_lock(&G);
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    g_h[t->handle - 0x1000].type = O_FREE; g_h[t->handle - 0x1000].obj = NULL;
    pthread_mutex_unlock(&G);
    pthread_cond_destroy(&t->cv); pthread_cond_destroy(&t->pcv);
    free(w);
}

static int64_t activity(wk_t *w, int act) { uint64_t r[32] = {w->t.handle, (uint64_t)act}; svc_thread(0x32, r); return (int64_t)r[0]; }
// GetThreadContext3 into *o; returns the SVC's result
static int64_t context(wk_t *w, hz_ctx_t *o) { uint64_t r[32] = {(uint64_t)o, w->t.handle}; memset(o, 0, sizeof *o); svc_thread(0x33, r); return (int64_t)r[0]; }

// A pause request on its own thread (the requester blocks until the target is stopped, so a hang must not hang the test)
typedef struct { thr_t t; pthread_t th; wk_t *w; _Atomic int done; int64_t rc; } req_t;
static void *req_main(void *p) {
    req_t *q = p;
    pthread_setspecific(g_keythr, &q->t);
    q->rc = activity(q->w, 1);
    atomic_store(&q->done, 1);
    return NULL;
}
static req_t *pause_begin(wk_t *w) {
    req_t *q = calloc(1, sizeof *q); q->w = w;
    pthread_cond_init(&q->t.cv, NULL); pthread_cond_init(&q->t.pcv, NULL);
    q->t.started = 1; q->t.prio = 44; q->t.core = 0; q->t.mask = 1;
    if (pthread_create(&q->th, NULL, req_main, q)) { perror("pthread_create"); exit(2); }
    return q;
}
// Waits up to `ms` for the pause to return. On a timeout the target is released and the requester freed (its loop ends on `paused`), and 0 is returned
static int pause_end(req_t *q, int ms, int64_t *rc) {
    int ok = WAIT_UNTIL(atomic_load(&q->done), ms);
    if (!ok) {
        activity(q->w, 0);
        pthread_mutex_lock(&G); q->w->t.paused = 1; pthread_cond_broadcast(&q->w->t.pcv); pthread_mutex_unlock(&G);
        WAIT_UNTIL(atomic_load(&q->done), 2000);
    }
    pthread_join(q->th, NULL);
    *rc = q->rc;
    pthread_cond_destroy(&q->t.cv); pthread_cond_destroy(&q->t.pcv);
    free(q);
    return ok;
}
static int pause_sync(cs_t *c, wk_t *w, const char *what, int ms) {
    int64_t rc = -1; int ok = pause_end(pause_begin(w), ms, &rc);
    if (!ok) fail(c, "%s: pause did not return within %d ms", what, ms);
    else if (rc) fail(c, "%s: pause returned %#llx", what, (unsigned long long)rc);
    return ok && !rc;
}

static uint64_t prog(wk_t *w) { return atomic_load(&w->progress); }
static void check_frozen(cs_t *c, wk_t *w, const char *what) {
    uint64_t a = prog(w); usleep(20000); uint64_t b = prog(w);
    if (a != b) fail(c, "%s: progress moved by %llu while reported paused", what, (unsigned long long)(b - a));
}
static void check_context(cs_t *c, wk_t *w, const char *what, int want_pc) {
    hz_ctx_t o; int64_t rc = context(w, &o);
    if (rc) fail(c, "%s: GetThreadContext3 returned %#llx", what, (unsigned long long)rc);
    else if (!o.sp) fail(c, "%s: GetThreadContext3 returned success with sp 0 (all-zero context)", what);
    else if (want_pc && !o.pc) fail(c, "%s: GetThreadContext3 returned pc 0", what);
}
static int advances(wk_t *w, int ms) { uint64_t a = prog(w); return WAIT_UNTIL(prog(w) != a, ms); }

// 1. Control: a pause of a thread in guest code stops it, the context is real, and the resume releases it
static int control_guest_pause(void) {
    cs_t c = {.name = "control_guest_pause"};
    wk_t *w = worker_start(M_GUEST);
    if (!advances(w, 2000)) fail(&c, "worker never ran");
    if (pause_sync(&c, w, "pause", 2000)) {
        check_frozen(&c, w, "pause");
        check_context(&c, w, "pause", 1);
    }
    if (activity(w, 0)) fail(&c, "resume failed");
    if (!advances(w, 2000)) fail(&c, "progress did not advance after the resume");
    worker_end(w);
    return finish(&c);
}

// 2. A pause that stopped the thread inside an SVC leaves `paused` set after the resume; the thread then leaves the SVC by the fast path and runs guest code
static int stale_after_svc_pause(void) {
    cs_t c = {.name = "stale_after_svc_pause"};
    wk_t *w = worker_start(M_SVC_FIRST);
    if (!WAIT_UNTIL(atomic_load(&w->in_svc), 2000)) fail(&c, "worker never entered the SVC");
    pause_sync(&c, w, "pause in SVC", 2000);
    if (activity(w, 0)) fail(&c, "resume failed");
    atomic_store(&w->release, 1);
    if (!WAIT_UNTIL(atomic_load(&w->in_spin), 2000) || !advances(w, 2000)) fail(&c, "worker did not run guest code after leaving the SVC");
    else if (pause_sync(&c, w, "second pause", 2000)) {
        check_frozen(&c, w, "second pause");
        check_context(&c, w, "second pause", 1);
    }
    activity(w, 0);
    worker_end(w);
    return finish(&c);
}

// 3. A pause requested in host code is satisfied by the thread's SVC entry (thr_svc_enter sets `paused`); the resume comes before the SVC returns
static int stale_after_svc_enter_pause(void) {
    cs_t c = {.name = "stale_after_svc_enter_pause"};
    wk_t *w = worker_start(M_HOST_FIRST);
    if (!advances(w, 2000)) fail(&c, "worker never ran");
    req_t *q = pause_begin(w);
    WAIT_UNTIL(atomic_load(&w->t.pause_req), 2000);
    usleep(5000);   // the requester resends SIGUSR2; the handler declines host code
    if (atomic_load(&q->done)) fail(&c, "pause returned while the worker ran in host code");
    atomic_store(&w->enter_go, 1);
    int64_t rc = -1;
    if (!pause_end(q, 2000, &rc)) fail(&c, "pause did not return after the worker entered an SVC");
    else if (rc) fail(&c, "pause returned %#llx", (unsigned long long)rc);
    if (activity(w, 0)) fail(&c, "resume failed");
    atomic_store(&w->release, 1);
    if (!WAIT_UNTIL(atomic_load(&w->in_spin), 2000) || !advances(w, 2000)) fail(&c, "worker did not run guest code after leaving the SVC");
    else if (pause_sync(&c, w, "second pause", 2000)) {
        check_frozen(&c, w, "second pause");
        check_context(&c, w, "second pause", 1);
    }
    activity(w, 0);
    worker_end(w);
    return finish(&c);
}

// 4. Resume, then a new pause before the woken signal handler has reacquired G: the handler must stop the thread again and the requester must see it stopped
static int resume_then_pause_while_handler_parked(void) {
    cs_t c = {.name = "resume_then_pause_while_handler_parked"};
    wk_t *w = worker_start(M_GUEST);
    if (!advances(w, 2000)) fail(&c, "worker never ran");
    uint64_t stopped_at = 0; int suspended = 0;
    if (pause_sync(&c, w, "first pause", 2000)) {
        stopped_at = prog(w);
        check_frozen(&c, w, "first pause");
        if (thread_suspend(w->t.mport) == KERN_SUCCESS) suspended = 1; else fail(&c, "thread_suspend failed");
        if (activity(w, 0)) fail(&c, "resume failed");
        req_t *q = pause_begin(w);
        WAIT_UNTIL(atomic_load(&w->t.pause_req), 2000);   // the new request is in before the handler can run
        if (suspended) thread_resume(w->t.mport), suspended = 0;
        int64_t rc = -1;
        if (!pause_end(q, 1000, &rc)) fail(&c, "second pause did not return within 1000 ms while the handler was parked");
        else if (rc) fail(&c, "second pause returned %#llx", (unsigned long long)rc);
        else {
            check_frozen(&c, w, "second pause");
            if (prog(w) != stopped_at) fail(&c, "progress moved by %llu between the stop and the second stop", (unsigned long long)(prog(w) - stopped_at));
            check_context(&c, w, "second pause", 1);
        }
    }
    if (suspended) thread_resume(w->t.mport);
    activity(w, 0);
    worker_end(w);
    return finish(&c);
}

// 5. The target exits and its handle is closed while a pause waits for it (the requester lets go of G to signal and wait): the requester holds a reference,
//    so the object outlives the loop and is freed when it ends. Before, thread_exit freed it under the requester (ASan: heap-use-after-free in svc_thread_impl)
static int exit_and_close_during_pause(void) {
    cs_t c = {.name = "exit_and_close_during_pause"};
    wk_t *w = worker_start(M_HOST_EXIT); thr_t *t = &w->t; pthread_t th = w->th;
    if (!advances(w, 2000)) fail(&c, "worker never ran");
    req_t *q = pause_begin(w);
    WAIT_UNTIL(atomic_load(&t->pause_req), 2000);
    usleep(2000);   // the requester is in its loop: the handler declines host code
    uint64_t r[32] = {t->handle}; svc_thread(0x16, r);
    atomic_store(&w->release, 1);   // the worker exits; w (t is its first member) may be freed from here on
    if (!WAIT_UNTIL(atomic_load(&q->done), 2000)) fail(&c, "pause did not return after the target exited");
    else if (q->rc) fail(&c, "pause returned %#llx", (unsigned long long)q->rc);
    pthread_join(q->th, NULL); pthread_join(th, NULL);
    pthread_mutex_lock(&G); for (thr_t *u = g_all; u; u = u->all) if (u == t) fail(&c, "the exited, closed thread was not freed"); pthread_mutex_unlock(&G);
    pthread_cond_destroy(&q->t.cv); pthread_cond_destroy(&q->t.pcv); free(q);
    return finish(&c);
}

int main(void) {
    alarm(120);   // a bug in the test must not hang the run
    setenv("DM_PREEMPT", "0", 1);
    svc_thread_main_init();
    pthread_key_create(&g_key18, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);
    int ok = 1;
    ok &= control_guest_pause();
    ok &= stale_after_svc_pause();
    ok &= stale_after_svc_enter_pause();
    ok &= resume_then_pause_while_handler_parked();
    ok &= exit_and_close_during_pause();
    printf("%s\n", ok ? "all pause tests passed" : "pause tests FAILED");
    return ok ? 0 : 1;
}
