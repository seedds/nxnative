// Kernel-state injection plus real host event bindings and SVC handle lifetimes.
#include <sys/timex.h>
static int fixture_ntp_gettime(struct ntptimeval *out);
#define ntp_gettime fixture_ntp_gettime
#include "../hle_network_clock.c"
#undef ntp_gettime
#include "../hle_thread.c"
#include <mach-o/getsect.h>
#include <unistd.h>

struct dm_flags g_flag;
module_t *g_mod;
int g_nmod;
uint64_t g_t0_ns;
__thread _Atomic int dm_host_work;
pthread_key_t g_key18;
void crash_thread_init(void) {}
void guest_thread_init(void) {}
void guest_thread_set_id(uint64_t id) { (void)id; }
void guest_gate_stack(int own_stack) { (void)own_stack; }
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) { (void)entry; (void)arg; (void)sp; abort(); }
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
uint64_t guest_ns(void) { return now_ns(); }
int dm_detclock_enabled(void) { return 0; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_backtrace(uint64_t fp) { (void)fp; }

typedef struct { uint32_t wh, rh; int mode; } fixture_event;
static unsigned created, destroyed, reads, checks, failures;
static uint32_t callback_signal;
static _Atomic int callback_payload;
static thr_t *callback_peer;
static struct ntptimeval sample;
static int sample_status;
#define CHECK(c, message) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: %s\n", message); } } while (0)
static int fixture_ntp_gettime(struct ntptimeval *out) { reads++; *out = sample; errno = EIO; return sample_status; }
static void synced_sample(void) {
    sample = (struct ntptimeval){.time = {1700000000, 123456789}, .time_state = TIME_OK, .maxerror = 150000, .esterror = 500000};
    sample_status = TIME_OK;
}
// The tiny SDK adapters use the production SVC layer; they do not mock binding,
// lifetime lookup, signal, clear, close or kernel-handle reuse.
static uint32_t create_event(void *p, int mode, int interprocess) {
    CHECK(interprocess == 1, "event is an interprocess SystemEvent");
    uint64_t r[32] = {0}; CHECK(svc_thread(0x45, r) && !r[0], "create kernel event");
    fixture_event *event = p; event->wh = (uint32_t)r[1]; event->rh = (uint32_t)r[2]; event->mode = mode; created++;
    if (callback_signal) {
        // A real SDK/SVC callback can wake a higher-priority pinned caller
        // while Init holds its service lock. The handoff must end when that
        // caller blocks on the lock, allowing Init to return and unlock it.
        r[0] = callback_signal; CHECK(svc_thread(0x11,r) && !r[0], "SDK callback wakes pinned peer");
        pthread_mutex_lock(&G); int waiting = callback_peer->native_wait; pthread_mutex_unlock(&G);
        CHECK(waiting && atomic_load(&callback_payload) == 1, "contended task caller is not guest-runnable during the SDK handoff");
    }
    return 0;
}
static uint32_t writable_event(const void *p) { return ((const fixture_event *)p)->wh; }
static void destroy_event(void *p) {
    fixture_event *event = p;
    uint64_t r[32] = {event->wh}; CHECK(svc_thread(0x16, r) && !r[0], "close event write handle");
    r[0] = event->rh; CHECK(svc_thread(0x16, r) && !r[0], "close event read handle");
    event->wh = event->rh = 0; destroyed++;
}
void *guest_sym(const char *name) {
    if (!strcmp(name, "_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb")) return create_event;
    if (!strcmp(name, "_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE")) return writable_event;
    if (!strcmp(name, "_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE")) return destroy_event;
    CHECK(0, "unexpected SDK callback"); return NULL;
}
static int try_event(fixture_event *event) {
    uint32_t handle = event->rh;
    uint64_t r[32] = {0, (uintptr_t)&handle, 1, 0};
    CHECK(svc_thread(0x18, r), "try-wait kernel event");
    if (r[0]) { CHECK(r[0] == R_TIMEOUT, "unsignaled event times out"); return 0; }
    if (event->mode) { r[0] = event->rh; CHECK(svc_thread(0x17, r) && !r[0], "auto-clear resets observed event"); }
    return 1;
}
extern const struct mach_header_64 _mh_execute_header;
static void *hook(const char *name) {
    unsigned long bytes = 0; void *fn = NULL; unsigned count = 0;
    const hle_force_ent *entries = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    for (size_t i = 0; entries && i < bytes / sizeof *entries; i++) if (!strcmp(entries[i].name, name)) { count++; fn = entries[i].fn; }
    CHECK(count == 1 && fn, name); return fn;
}
#define CLOCK_SYMBOL(suffix) "_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask" suffix
static void *repeat_start(void *object) {
    for (unsigned i = 0; i < 100; i++) if (h_time_NcStart(object) != RES_OK) return (void *)1;
    return NULL;
}
static uint32_t callback_wait_handle;
static void *callback_object;
static void *callback_worker(void *unused) {
    (void)unused; pthread_setspecific(g_keythr,callback_peer);
    uint32_t handle = callback_wait_handle;
    uint64_t r[32] = {0,(uintptr_t)&handle,1,(uint64_t)-1}; svc_thread(0x18,r);
    if (r[0]) { atomic_store(&callback_payload,-1); thread_exit(callback_peer); return NULL; }
    atomic_store(&callback_payload,1);
    Result result = h_time_NcResult(callback_object);
    atomic_store(&callback_payload,result != RES_OK ? 2 : -2);
    thread_exit(callback_peer); return NULL;
}
static void callback_handoff(void) {
    uint64_t key = 0;
    h_time_NcCtor(&key); callback_object = &key;
    uint64_t r[32] = {0}; CHECK(svc_thread(0x45,r) && !r[0], "create callback handoff event");
    uint32_t write_handle = (uint32_t)r[1];
    callback_signal = write_handle; callback_wait_handle = (uint32_t)r[2];
    thr_t *self = cur(); uint64_t saved_mask = self->mask; int saved_core = self->core, saved_priority = self->prio;
    self->mask = 1; self->core = 0; self->prio = 44;
    callback_peer = calloc(1,sizeof *callback_peer);
    if (!callback_peer) abort();
    pthread_cond_init(&callback_peer->cv,NULL); pthread_cond_init(&callback_peer->pcv,NULL);
    callback_peer->started = 1; callback_peer->prio = 30; callback_peer->core = 0; callback_peer->mask = 1;
    pthread_mutex_lock(&G); callback_peer->handle = h_new(O_THREAD,callback_peer); callback_peer->all = g_all; g_all = callback_peer; pthread_mutex_unlock(&G);
    pthread_t thread; int error = pthread_create(&thread,NULL,callback_worker,NULL);
    CHECK(!error,"start pinned guest peer");
    if (!error) {
        alarm(5);
        for (;;) { pthread_mutex_lock(&G); int waiting = callback_peer->wkind == 4; pthread_mutex_unlock(&G); if (waiting) break; usleep(100); }
        // Plain pthread_mutex_lock in the production registry makes this exact
        // callback deadlock. The alarm bounds a failed regression run.
        CHECK(h_time_NcInit(&key,0) == RES_OK,"initialization returns from its real scheduler handoff");
        alarm(0); callback_signal = 0;
        pthread_join(thread,NULL);
        CHECK(atomic_load(&callback_payload) == 2,"peer resumes after initialization releases the lock");
    }
    callback_signal = 0;
    h_time_NcDtor(&key);
    r[0] = callback_wait_handle; CHECK(svc_thread(0x16,r) && !r[0],"close callback read handle");
    r[0] = write_handle; CHECK(svc_thread(0x16,r) && !r[0],"close callback write handle");
    pthread_mutex_lock(&G);
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == callback_peer) { *p = callback_peer->all; break; }
    g_h[callback_peer->handle - 0x1000].type = O_FREE; g_h[callback_peer->handle - 0x1000].obj = NULL;
    pthread_mutex_unlock(&G);
    pthread_cond_destroy(&callback_peer->cv); pthread_cond_destroy(&callback_peer->pcv);
    free(callback_peer); callback_peer = NULL;
    self->mask = saved_mask; self->core = saved_core; self->prio = saved_priority;
}
int main(void) {
    svc_thread_main_init(); synced_sample();
    Result (*get_time)(int64_t *) = hook("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE");
    unsigned (*accurate)(void) = hook("_ZN2nn4time26StandardNetworkSystemClock20IsAccuracySufficientEv");
    void (*ctor)(void *) = hook(CLOCK_SYMBOL("C1Ev"));
    void (*dtor)(void *) = hook(CLOCK_SYMBOL("D1Ev"));
    Result (*init)(void *, int) = hook(CLOCK_SYMBOL("10InitializeENS_2os14EventClearModeE"));
    Result (*start)(void *) = hook(CLOCK_SYMBOL("9StartTaskEv"));
    void *(*event)(void *) = hook(CLOCK_SYMBOL("14GetFinishEventEv"));
    Result (*result)(void *) = hook(CLOCK_SYMBOL("9GetResultEv"));
    if (!get_time || !accurate || !ctor || !dtor || !init || !start || !event || !result) return 1;
    uint64_t output[3] = {0x1234, 0x55, 0x5678}; unsigned before;
    errno = EDOM;
    CHECK(get_time((int64_t *)&output[1]) == 0 && output[1] == 1700000000 && output[0] == 0x1234 && output[2] == 0x5678, "synchronized kernel time writes only the PosixTime");
    CHECK(errno == EDOM && accurate(), "kernel query preserves errno and agrees with accuracy");
    before = reads; CHECK(get_time(NULL) != 0 && reads == before, "null output fails without querying");
    setenv("DM_FIXED_TIME", "1", 1); setenv("DM_NETWORK", "offline", 1);
    CHECK(get_time((int64_t *)&output[1]) == 0 && output[1] == 1700000000, "network time does not use fixed/user time or launcher protocol selection");
    for (int test = 0; test < 9; test++) {
        synced_sample();
        switch (test) {
            case 0: sample_status = -1; break;
            case 1: sample_status = TIME_ERROR; sample.time_state = TIME_ERROR; break;
            case 2: sample.time_state = TIME_ERROR; break;
            case 3: sample.maxerror = 1000001; break;
            case 4: sample.esterror = 1000001; break;
            case 5: sample.maxerror = -1; break;
            case 6: sample.esterror = -1; break;
            case 7: sample.time.tv_sec = 0; break;
            default: sample.time.tv_nsec = 1000000000; break;
        }
        output[1] = 0x55; errno = EDOM;
        CHECK(get_time((int64_t *)&output[1]) != 0 && output[1] == 0x55 && !accurate(), "bad kernel status/precision/structure fails closed with untouched output");
        CHECK(errno == EDOM, "failed query preserves errno");
    }
    synced_sample(); sample.time.tv_nsec = -1;
    CHECK(!accurate(), "negative fractional time fails");
    synced_sample(); sample.maxerror = sample.esterror = 1000000;
    CHECK(accurate(), "exactly one second error bound is accepted");
    sample_status = TIME_INS; sample.time_state = TIME_INS;
    CHECK(!accurate(), "leap-warning state fails conservatively");
    synced_sample();
    uint64_t a[3] = {0xabc,0xdef,0x123}, b[3] = {0xabc,0xdef,0x123};
    CHECK(start(a) != 0 && !event(a) && result(a) != 0, "unconstructed task cannot complete");
    ctor(a); ctor(b); before = reads;
    CHECK(!event(a) && result(a) != 0 && start(a) != 0 && reads == before, "constructor does not create completion or read time");
    CHECK(init(a, 2) != 0 && !event(a), "invalid clear mode creates no event");
    CHECK(init(a, 0) == 0 && init(b, 1) == 0, "two clear modes initialize independently");
    fixture_event *ae = event(a), *be = event(b);
    CHECK(ae && be && ae != be && ae->mode == 0 && be->mode == 1, "each task owns its SDK event with the requested mode");
    CHECK(!try_event(ae) && !try_event(be) && result(a) != 0, "GetFinishEvent does not signal before StartTask");
    before = reads; CHECK(start(a) == 0 && reads == before + 1 && result(a) == 0, "StartTask performs one real query before publishing its result");
    CHECK(try_event(ae) && try_event(ae) && !try_event(be), "manual completion stays signaled without completing the other task");
    sample_status = TIME_ERROR; sample.time_state = TIME_ERROR;
    CHECK(start(b) == 0 && result(b) != 0 && try_event(be) && !try_event(be), "unsynchronized task completes with failure and auto-clear event");
    CHECK(result(a) == 0, "another task's failure does not replace the first result");
    CHECK(start(a) == 0 && result(a) != 0 && try_event(ae), "restarting a completed task uses a new query and replaces stale success");
    synced_sample(); CHECK(start(b) == 0 && result(b) == 0 && try_event(be) && !try_event(be), "a later actual synchronization can recover with a fresh completion");
    before = created; CHECK(init(a, 0) == 0 && init(a, 1) != 0 && created == before, "reinitialize is idempotent only for the same clear mode");
    uint64_t old_binding = hle_system_event_binding(ae); uint32_t old_handle = ae->wh;
    dtor(a); CHECK(result(a) != 0 && start(a) != 0 && !event(a), "destroyed task is unavailable");
    ctor(a); CHECK(init(a, 0) == 0, "object storage can be reused"); ae = event(a);
    CHECK(ae->wh == old_handle && hle_system_event_binding(ae) != old_binding, "reused handle has a new binding lifetime");
    CHECK(!hle_signal_system_event_binding(old_binding) && !try_event(ae), "stale completion cannot signal the replacement event");
    CHECK(a[0] == 0xabc && a[1] == 0xdef && a[2] == 0x123 && !memcmp(a,b,sizeof a), "SDK-private task storage stays untouched");
    dtor(a); dtor(a); dtor(b); ctor(NULL); dtor(NULL);
    CHECK(clock_task_count == 0 && g_nse == 0 && created == destroyed, "all events and registry entries retire exactly once");
    // Capacity failure is recoverable, not a borrowed event from another task.
    uint8_t keys[NETWORK_CLOCK_MAX_TASKS + 1];
    for (unsigned i = 0; i < sizeof keys; i++) ctor(&keys[i]);
    CHECK(clock_task_count == NETWORK_CLOCK_MAX_TASKS && init(&keys[NETWORK_CLOCK_MAX_TASKS],0) != 0, "registry capacity fails closed");
    for (unsigned i = 0; i < sizeof keys; i++) dtor(&keys[i]);
    CHECK(clock_task_count == 0, "bounded registry capacity is reusable");
    // Completed tasks can be queried/restarted independently on different
    // callers; signaling uses the real host kernel lock and captured lifetime.
    ctor(a); ctor(b); CHECK(!init(a,0) && !init(b,1), "prepare concurrent tasks");
    pthread_t threads[2];
    int first = pthread_create(&threads[0],NULL,repeat_start,a);
    int second = pthread_create(&threads[1],NULL,repeat_start,b);
    CHECK(!first && !second, "start concurrent task callers");
    void *first_result = (void *)1, *second_result = (void *)1;
    if (!first) pthread_join(threads[0], &first_result);
    if (!second) pthread_join(threads[1], &second_result);
    CHECK(!first_result && !second_result && !result(a) && !result(b), "concurrent completed results remain independent");
    dtor(a); dtor(b);
    CHECK(clock_task_count == 0 && g_nse == 0 && created == destroyed, "concurrent task cleanup leaks no events");
    callback_handoff();
    CHECK(clock_task_count == 0 && g_nse == 0 && created == destroyed,"SDK handoff cleanup retires the clock event");
    printf("network clock: %u checks, %u failed\n", checks, failures);
    return failures != 0;
}
