// WaitSynchronization through real SVCs. Hold G across notification and clear
// to make the lost-notification window deterministic, without scheduler sleeps.
#include "../hle_thread.c"
#include <assert.h>
#include <unistd.h>

typedef struct { uint32_t write, read; } ends_t;
typedef struct {
    thr_t *thread;
    pthread_t pt;
    uint32_t handles[3];
    int count;
    uint64_t result, index;
} waiter_t;

static ends_t event_new(void) {
    uint64_t r[32] = {0}; assert(svc_thread(0x45, r) && !r[0]);
    return (ends_t){r[1], r[2]};
}
static void close_handle(uint32_t h) {
    uint64_t r[32] = {h}; assert(svc_thread(0x16, r) && !r[0]);
}
static void event_close(ends_t e) { close_handle(e.write); close_handle(e.read); }
static uint64_t wait_now(uint32_t *handles, int n, int64_t timeout, uint64_t *index) {
    uint64_t r[32] = {0, (uint64_t)handles, (uint32_t)n, (uint64_t)timeout};
    assert(svc_thread(0x18, r)); if (index) *index = r[1]; return r[0];
}
static thr_t *new_thread(void) {
    thr_t *t = calloc(1, sizeof *t); assert(t);
    pthread_cond_init(&t->cv, NULL); pthread_cond_init(&t->pcv, NULL);
    t->started = 1; t->prio = 44; t->mask = 7;
    pthread_mutex_lock(&G);
    t->handle = h_new(O_THREAD, t); t->all = g_all; g_all = t;
    pthread_mutex_unlock(&G);
    return t;
}
static void *wait_body(void *arg) {
    waiter_t *w = arg;
    pthread_setspecific(g_keythr, w->thread);
    w->result = wait_now(w->handles, w->count, 500000000, &w->index);
    thread_exit(w->thread);
    return NULL;
}
static void wait_start(waiter_t *w) {
    w->thread = new_thread();
    assert(!pthread_create(&w->pt, NULL, wait_body, w));
    uint64_t deadline = now_ns() + 2000000000;
    for (;;) {
        pthread_mutex_lock(&G); int waiting = w->thread->wkind == 4; pthread_mutex_unlock(&G);
        if (waiting) return;
        assert(now_ns() < deadline); sched_yield();
    }
}
static void wait_finish(waiter_t *w, uint64_t result, unsigned index) {
    assert(!pthread_join(w->pt, NULL));
    if (w->result != result || (!result && w->index != index))
        fprintf(stderr, "wait returned %#llx / %llu; expected %#llx / %u\n", w->result, w->index, result, index);
    assert(w->result == result && (result || w->index == index));
    close_handle(w->thread->handle);
}
// Caller holds G: this is the signal operation shared by SVC11 and host service
// producers. Keeping the lock lets the test clear/close before waiters resume.
static void signal_locked(uint32_t write) {
    event_t *e = h_peek(write, O_EVENT_W); assert(e);
    e->signaled = 1; wake_sync_waiters(e);
}
static void framing(void) {
    ends_t e = event_new();
    uint64_t r[32] = {e.write}; assert(svc_thread(0x11, r) && !r[0]);
    uint32_t handles[65] = {e.read, 0};
    assert(wait_now(handles, 2, 0, NULL) == R_BAD_HANDLE);
    handles[1] = e.write; assert(wait_now(handles, 2, 0, NULL) == R_BAD_HANDLE);
    handles[1] = H_CUR_THREAD; assert(wait_now(handles, 2, 0, NULL) == R_BAD_HANDLE);
    assert(wait_now(handles, -1, 0, NULL) == 0xee01);
    assert(wait_now(handles, 65, 0, NULL) == 0xee01);
    assert(wait_now(NULL, 0, 0, NULL) == R_TIMEOUT);
    handles[1] = e.read; uint64_t index;
    assert(!wait_now(handles, 2, 0, &index) && index == 0);
    for (int i = 0; i < 64; i++) handles[i] = e.read;
    assert(!wait_now(handles, 64, 0, &index) && index == 0);
    // An already signaled object and a poll both precede pending cancellation.
    r[0] = H_CUR_THREAD; assert(svc_thread(0x19, r) && !r[0]);
    assert(!wait_now(handles, 1, -1, NULL));
    r[0] = e.read; assert(svc_thread(0x12, r) && !r[0]);
    assert(wait_now(handles, 1, 0, NULL) == R_TIMEOUT);
    assert(wait_now(handles, 1, -1, NULL) == R_CANCELLED);
    assert(wait_now(handles, 1, 0, NULL) == R_TIMEOUT);
    assert(wait_now(handles, 1, 100000, NULL) == R_TIMEOUT);
    pthread_mutex_lock(&G);
    assert(((event_t *)h_peek(e.read, O_EVENT_R))->refs == 2);
    pthread_mutex_unlock(&G);
    event_close(e);
}
static uint64_t event_svc(int num, uint32_t handle) {
    uint64_t r[32] = {handle}; assert(svc_thread(num, r)); return r[0];
}
static void endpoints(void) {
    ends_t e = event_new();
    assert(wait_now(&e.read, 1, 0, NULL) == R_TIMEOUT);
    assert(event_svc(0x11, e.read) == R_BAD_HANDLE);
    assert(event_svc(0x17, e.write) == R_BAD_HANDLE);
    assert(event_svc(0x17, cur()->handle) == R_BAD_HANDLE);
    assert(event_svc(0x17, H_CUR_THREAD) == R_BAD_HANDLE);
    assert(event_svc(0x17, e.read) == R_BAD_STATE);
    assert(!event_svc(0x12, e.write));
    assert(!event_svc(0x11, e.write));
    assert(!wait_now(&e.read, 1, 0, NULL));
    assert(!wait_now(&e.read, 1, 0, NULL)); // Waiting does not clear a kernel event.
    assert(!event_svc(0x12, e.read));
    assert(wait_now(&e.read, 1, 0, NULL) == R_TIMEOUT);
    assert(!event_svc(0x11, e.write));
    assert(!event_svc(0x17, e.read));
    assert(event_svc(0x17, e.read) == R_BAD_STATE);
    // Allocator final release signals, then closes the producer's endpoint.
    assert(!event_svc(0x11, e.write)); close_handle(e.write);
    assert(!wait_now(&e.read, 1, 0, NULL)); close_handle(e.read);
}
static void signal_then_clear(void) {
    ends_t a = event_new(), b = event_new();
    waiter_t first = {.handles = {a.read, b.read, b.read}, .count = 3};
    waiter_t second = {.handles = {b.read}, .count = 1};
    wait_start(&first); wait_start(&second);
    pthread_mutex_lock(&G);
    signal_locked(b.write);
    ((event_t *)h_peek(b.write, O_EVENT_W))->signaled = 0;
    pthread_mutex_unlock(&G);
    wait_finish(&first, 0, 1); wait_finish(&second, 0, 0);
    assert(wait_now(&b.read, 1, 0, NULL) == R_TIMEOUT);
    event_close(a); event_close(b);
}
static void close_and_reuse(void) {
    ends_t old = event_new();
    waiter_t w = {.handles = {old.read}, .count = 1}; wait_start(&w);
    close_handle(old.read);
    ends_t replacement = event_new(); assert(replacement.write == old.read);
    pthread_mutex_lock(&G);
    signal_locked(replacement.write); assert(w.thread->wkind == 4);
    signal_locked(old.write);
    pthread_mutex_unlock(&G);
    close_handle(old.write);
    wait_finish(&w, 0, 0); event_close(replacement);
}
static void handle_array_snapshot(void) {
    ends_t old = event_new(), replacement = event_new();
    waiter_t w = {.handles = {old.read}, .count = 1}; wait_start(&w);
    pthread_mutex_lock(&G);
    w.handles[0] = replacement.read;
    signal_locked(replacement.write); assert(w.thread->wkind == 4);
    signal_locked(old.write);
    pthread_mutex_unlock(&G);
    wait_finish(&w, 0, 0); event_close(old); event_close(replacement);
}
static void cancellation_and_release(void) {
    ends_t e = event_new();
    waiter_t w = {.handles = {e.read, e.read}, .count = 2}; wait_start(&w);
    close_handle(e.write); close_handle(e.read);
    uint64_t r[32] = {w.thread->handle}; assert(svc_thread(0x19, r) && !r[0]);
    wait_finish(&w, R_CANCELLED, 0);
    w = (waiter_t){.count = 0}; wait_start(&w);
    r[0] = w.thread->handle; assert(svc_thread(0x19, r) && !r[0]);
    wait_finish(&w, R_CANCELLED, 0);
}
static pthread_mutex_t exit_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t exit_cv = PTHREAD_COND_INITIALIZER;
static int may_exit;
static void *exit_body(void *arg) {
    thr_t *t = arg; pthread_setspecific(g_keythr, t);
    pthread_mutex_lock(&exit_mu);
    while (!may_exit) pthread_cond_wait(&exit_cv, &exit_mu);
    pthread_mutex_unlock(&exit_mu); thread_exit(t); return NULL;
}
static void closed_thread_wait(void) {
    thr_t *target = new_thread(); pthread_t pt;
    assert(!pthread_create(&pt, NULL, exit_body, target));
    waiter_t w = {.handles = {target->handle, target->handle}, .count = 2}; wait_start(&w);
    uint32_t old_handle = target->handle; close_handle(old_handle);
    ends_t replacement = event_new(); assert(replacement.write == old_handle);
    pthread_mutex_lock(&exit_mu); may_exit = 1; pthread_cond_signal(&exit_cv); pthread_mutex_unlock(&exit_mu);
    assert(!pthread_join(pt, NULL));
    wait_finish(&w, 0, 0); event_close(replacement);
}
int main(int argc, char **argv) {
    alarm(10); svc_thread_main_init();
    const char *names[] = {"framing", "endpoints", "signal-clear", "close-reuse", "snapshot", "cancel-release", "thread-close"};
    void (*tests[])(void) = {framing, endpoints, signal_then_clear, close_and_reuse, handle_array_snapshot, cancellation_and_release, closed_thread_wait};
    for (unsigned i = 0; i < sizeof tests / sizeof *tests; i++) {
        if (argc > 1 && strcmp(argv[1], names[i])) continue;
        tests[i](); printf("PASS: %s\n", names[i]);
        int handles, live, objects; thr_counts(&handles, &live, &objects);
        assert(handles == 1 && live == 1 && objects == 1);
    }
    return 0;
}
