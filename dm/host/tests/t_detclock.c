// The deterministic guest clock (DM_DETCLOCK: hle_os.c's virtual time, hle_thread.c's det_idle) through the real thread SVCs, with no game files
// (docs/reference/threads.md, "A game stops at its first frame under DM_DETCLOCK").
//  1. A main thread that presents (the frame tick) only after a worker has slept on the guest clock (SleepThread) and let two timed waits run out (WaitSynchronization,
//     WaitForAddress), frame after frame, beside the threads a game has that wait all the while: one parked in a native HLE wait (as on the notification queue), one waiting
//     with no timeout on an event host code signals (an audio thread on the renderer's event), and a guest thread the host made for itself that waits in host code (the audio
//     renderer's). Before the fix the clock moved only at a present, the worker's first sleep never ended and the main thread never presented again: the alarm (CTest's time
//     limit on Windows) fails the run. Every clock value both threads read is checked against the exact virtual-time trace (sleeps and timeouts end at their deadlines, a
//     present moves the clock to at least the previous present's value plus the step, nothing moves while the main thread does host work), so every run reads the same values
//     and a run that does not fails.
//  2. A main thread that presents after waiting for an event host code signals (vi's vsync event) while a worker sleeps 1 ms at a time: the clock does not jump during that
//     wait, so it is exactly one step on at each present, and the worker reads the presents' values.
#include "../hle_os.c"
#include "../hle_thread.c"
#include <signal.h>
#include <unistd.h>

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "[RED] t_detclock: " __VA_ARGS__); fputc('\n', stderr); exit(1); } } while (0)
#define STEP 16666667ull
#define MS 1000000ull

// ---- What the host files need besides each other ----
struct dm_flags g_flag;
uint64_t g_t0_ns;
module_t *g_mod;
int g_nmod;
__thread _Atomic int dm_host_work;
pthread_key_t g_key18;
void crash_thread_init(void) {}
void guest_thread_init(void) {}
void crash_watch_frame(void) {}
module_t *module_of(const void *p) { (void)p; return NULL; }
void *module_sym(module_t *m, const char *name) { (void)m; (void)name; return NULL; }
#if !DM_NATIVE_EXEC
// The x86-64 execution engine's side of hle_thread.c (aot_thread.c), which this test does not link: no thread here runs recompiled guest code
dm_guest_state *dm_aot_thread_begin(void) { return NULL; }
int dm_aot_thread_run(uint64_t entry, uint64_t arg, uint64_t sp) { (void)entry; (void)arg; (void)sp; abort(); }
void dm_aot_thread_end(void) {}
void dm_aot_thread_exiting(void) {}
void dm_aot_thread_poke(dm_guest_state *st) { (void)st; }
void dm_aot_thread_dump(dm_guest_state *st, uint32_t handle) { (void)st; (void)handle; }
int dm_aot_stop_status(void) { return -1; }
#endif
// The real thread_main registers each thread and waits for StartThread; only the step onto a guest stack is this C entry
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) {
    (void)sp;
    ((void (*)(uint64_t))entry)(arg);
    uint64_t r[32] = {0}; svc_thread(0x0A, r);
    abort();
}
// The SDK's SystemEvent calls hle_bind_system_event makes (as t_system_event.c): a kernel event through CreateEvent, its write end
typedef struct { uint32_t wh, rh; } test_event;
static uint32_t create_event(void *p, int mode, int interprocess) {
    (void)mode; (void)interprocess;
    uint64_t r[32] = {0}; CHECK(svc_thread(0x45, r) && !r[0], "CreateEvent failed");
    ((test_event *)p)->wh = (uint32_t)r[1]; ((test_event *)p)->rh = (uint32_t)r[2]; return 0;
}
static uint32_t writable_event(const void *p) { return ((const test_event *)p)->wh; }
void *guest_sym(const char *name) {
    if (!strcmp(name, "_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb")) return (void *)create_event;
    if (!strcmp(name, "_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE")) return (void *)writable_event;
    return NULL;
}

// ---- Thread SVCs ----
static uint32_t thread_new(void (*fn)(uint64_t), uint64_t arg) {
    uint64_t r[32] = {0, (uint64_t)(uintptr_t)fn, arg, 0, 44, 1};
    CHECK(svc_thread(0x08, r) && !r[0], "CreateThread failed");
    uint32_t h = (uint32_t)r[1]; r[0] = h;
    CHECK(svc_thread(0x09, r) && !r[0], "StartThread failed");
    return h;
}
static test_event event_new(void) { test_event e; create_event(&e, 0, 0); return e; }
static void event_signal(test_event e) { uint64_t r[32] = {e.wh}; CHECK(svc_thread(0x11, r) && !r[0], "SignalEvent failed"); }
static void event_reset(test_event e) { uint64_t r[32] = {e.rh}; CHECK(svc_thread(0x17, r) && !r[0], "ResetSignal failed"); }
static uint64_t event_wait(test_event e, int64_t ns) {   // WaitSynchronization on the read end: its result
    uint32_t h = e.rh; uint64_t r[32] = {0, (uint64_t)(uintptr_t)&h, 1, (uint64_t)ns};
    CHECK(svc_thread(0x18, r), "WaitSynchronization not handled");
    return r[0];
}
static void sleep_guest(uint64_t ns) { uint64_t r[32] = {ns}; CHECK(svc_thread(0x0B, r), "SleepThread not handled"); }
static thr_t *thread_of(uint32_t h) { pthread_mutex_lock(&G); thr_t *t = h_get(h, O_THREAD); pthread_mutex_unlock(&G); return t; }
static int thread_state(uint32_t h, int what) {   // 0 its last SVC, 1 kernel wait kind, 2 native wait depth, 3 host-owned
    thr_t *t = thread_of(h); pthread_mutex_lock(&G);
    int v = what == 0 ? t->last_svc : what == 1 ? t->wkind : what == 2 ? t->native_wait : t->host_owned;
    pthread_mutex_unlock(&G); return v;
}
static void until_state(uint32_t h, int what, int v) { for (int i = 0; thread_state(h, what) != v; i++) { CHECK(i < 20000, "thread %#x never reached state %d = %d", h, what, v); plat_sleep_ns(100000); } }
static uint64_t g_base;
static uint64_t clock_now(void) { return guest_ns() - g_base; }

// ---- Scenario 1 ----
enum { FRAMES = 9 };
static test_event e_go, e_done, e_never, e_audio;
static uint64_t w_read[FRAMES][4];
static _Atomic int w_stage, finish;
static _Atomic uint32_t addr_word;
static uint64_t sleep_of(int f) { return f % 3 == 2 ? 30 * MS : 5 * MS; }   // every third frame longer than a step
static uint64_t wait_of(int f) { return (uint64_t)(3 + f % 2) * MS; }
static void worker(uint64_t arg) {
    (void)arg;
    for (int f = 0; f < FRAMES; f++) {
        CHECK(event_wait(e_go, -1) == 0, "worker: wait for the frame failed"); event_reset(e_go);
        w_read[f][0] = clock_now(); atomic_store(&w_stage, 1);
        sleep_guest(sleep_of(f));
        w_read[f][1] = clock_now();
        CHECK(event_wait(e_never, (int64_t)wait_of(f)) == R_TIMEOUT, "worker: the timed wait did not time out");
        w_read[f][2] = clock_now();
        uint64_t r[32] = {(uint64_t)(uintptr_t)&addr_word, 2, 0, 1 * MS};   // WaitForAddress: wait while the word equals 0, 1 ms
        CHECK(svc_thread(0x34, r) && r[0] == R_TIMEOUT, "worker: WaitForAddress did not time out (%#llx)", (unsigned long long)r[0]);
        w_read[f][3] = clock_now(); atomic_store(&w_stage, 2);
        event_signal(e_done);
    }
}
static pthread_mutex_t park_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t park_cv = PTHREAD_COND_INITIALIZER;
static void parked(uint64_t arg) {   // a native HLE wait for the whole run, as a game's thread on PopNotificationMessage
    (void)arg;
    pthread_mutex_lock(&park_mu);
    while (!atomic_load(&finish)) { dm_thread_native_wait(1); pthread_cond_wait(&park_cv, &park_mu); dm_thread_native_wait(0); }
    pthread_mutex_unlock(&park_mu);
}
static void audio(uint64_t arg) { (void)arg; CHECK(event_wait(e_audio, -1) == 0, "audio: wait failed"); }   // a host-signalled event, no timeout
static void render(uint64_t arg) {   // the guest thread the host made for itself, waiting in host code on the wall clock
    (void)arg; dm_thread_mark_host();
    while (!atomic_load(&finish)) plat_sleep_ns(1 * MS);
}

// ---- Scenario 2 ----
enum { VFRAMES = 6 };
static test_event e_vsync;
static uint64_t w2_read[VFRAMES + 2];
static _Atomic int w2_n, w2_stop;
static plat_sema *vs_req;
static void worker2(uint64_t arg) {
    (void)arg;
    while (!atomic_load(&w2_stop)) {
        int n = atomic_load(&w2_n); if (n < VFRAMES + 2) w2_read[n] = clock_now();
        atomic_store(&w2_n, n + 1);
        sleep_guest(1 * MS);
    }
}
static void *vsync_host(void *arg) {   // a host thread, as hle_sdk16.c's vsync thread: signals the bound event when asked, once the worker has read the frame's clock, 10 ms later
    (void)arg;
    for (int g = 0; g < VFRAMES; g++) {
        plat_sema_wait(vs_req, -1);
        for (int i = 0; atomic_load(&w2_n) < g + 1; i++) { CHECK(i < 20000, "worker 2 did not read frame %d's clock", g); plat_sleep_ns(100000); }
        plat_sleep_ns(10 * MS);
        hle_signal_system_event(&e_vsync);
    }
    return NULL;
}

static void timeout(int sig) { (void)sig; const char m[] = "[RED] t_detclock: timed out: a thread waits on the guest clock and nothing moves it\n"; write(2, m, sizeof m - 1); _exit(99); }
int main(void) {
#if !defined(_WIN32)
    signal(SIGALRM, timeout); alarm(30);
#else
    (void)timeout;   // no alarm on Windows: CTest's time limit bounds the run there
#endif
    setenv("DM_DETCLOCK", "16666667", 1);
    setenv("DM_PREEMPT", "0", 1);
    svc_thread_main_init();
    e_go = event_new(); e_done = event_new(); e_never = event_new();
    hle_bind_system_event(&e_audio, 1); hle_bind_system_event(&e_vsync, 1);   // bound like the service-layer events (vi's vsync, the renderer's)
    uint32_t hp = thread_new(parked, 0), ha = thread_new(audio, 0), hr = thread_new(render, 0);
    until_state(hp, 2, 1); until_state(ha, 1, 4); until_state(hr, 3, 1);
    uint32_t hw = thread_new(worker, 0);
    until_state(hw, 1, 4);   // waiting for the first frame
    dm_guest_frame_tick();   // the first present: the switch takes the clock over
    CHECK(dm_detclock_enabled(), "DM_DETCLOCK is not on after the first present");
    g_base = guest_ns();

    // 1
    uint64_t p = 0;
    for (int f = 0; f < FRAMES; f++) {
        CHECK(clock_now() == p, "frame %d starts at %llu ns, expected %llu", f, (unsigned long long)clock_now(), (unsigned long long)p);
        atomic_store(&w_stage, 0);
        event_signal(e_go);
        dm_host_work = 1;   // as inside a submit: the worker sleeps meanwhile, and nothing may move the clock while this thread runs
        until_state(hw, 0, 0x0B);
        plat_sleep_ns(20 * MS);
        CHECK(clock_now() == p && atomic_load(&w_stage) == 1, "frame %d: the clock moved (%llu ns) or the worker went on while the main thread ran", f, (unsigned long long)clock_now());
        dm_host_work = 0;
        CHECK(event_wait(e_done, -1) == 0, "main: wait for the worker failed"); event_reset(e_done);
        uint64_t m = clock_now(), s = sleep_of(f), w = wait_of(f);
        uint64_t want[4] = {p, p + s, p + s + w, p + s + w + 1 * MS};
        for (int k = 0; k < 4; k++)
            CHECK(w_read[f][k] == want[k], "frame %d: the worker's read %d is %llu ns, expected %llu", f, k, (unsigned long long)w_read[f][k], (unsigned long long)want[k]);
        CHECK(m == want[3], "frame %d: the main thread read %llu ns before its present, expected %llu", f, (unsigned long long)m, (unsigned long long)want[3]);
        dm_guest_frame_tick();
        uint64_t next = want[3] > p + STEP ? want[3] : p + STEP;
        CHECK(clock_now() == next, "frame %d: the present moved the clock to %llu ns, expected %llu", f, (unsigned long long)clock_now(), (unsigned long long)next);
        printf("frame %d: worker %llu, %llu, %llu, %llu ns; present %llu ns\n", f, (unsigned long long)w_read[f][0], (unsigned long long)w_read[f][1],
               (unsigned long long)w_read[f][2], (unsigned long long)w_read[f][3], (unsigned long long)next);
        p = next;
    }

    // 2
    vs_req = plat_sema_create(0);
    pthread_t vt; CHECK(!pthread_create(&vt, NULL, vsync_host, NULL), "pthread_create failed");
    uint32_t h2 = thread_new(worker2, 0);
    for (int g = 0; g < VFRAMES; g++) {
        plat_sema_signal(vs_req);
        CHECK(event_wait(e_vsync, -1) == 0, "main: wait for the vsync failed"); event_reset(e_vsync);
        CHECK(clock_now() == p, "vsync frame %d: the clock is %llu ns after the vsync wait, expected %llu (it ran ahead of the present)", g, (unsigned long long)clock_now(), (unsigned long long)p);
        CHECK(atomic_load(&w2_n) == g + 1, "vsync frame %d: worker 2 slept %d times, expected %d", g, atomic_load(&w2_n), g + 1);
        dm_guest_frame_tick();
        p += STEP;
        CHECK(clock_now() == p, "vsync frame %d: the present moved the clock to %llu ns, expected %llu", g, (unsigned long long)clock_now(), (unsigned long long)p);
    }
    pthread_join(vt, NULL);
    for (int g = 0; g < VFRAMES; g++)
        CHECK(w2_read[g] == p - (uint64_t)(VFRAMES - g) * STEP, "worker 2's read %d is %llu ns, expected %llu", g, (unsigned long long)w2_read[g], (unsigned long long)(p - (uint64_t)(VFRAMES - g) * STEP));
    printf("vsync frames: the clock one step on at each of %d presents, worker 2 read each present's value\n", VFRAMES);

    // The end: every thread let go (worker 2 wakes by a jump once the main thread waits for it)
    atomic_store(&w2_stop, 1);
    uint64_t r[32] = {0, (uint64_t)(uintptr_t)&h2, 1, (uint64_t)-1};
    CHECK(svc_thread(0x18, r) && r[0] == 0, "main: wait for worker 2's exit failed");
    atomic_store(&finish, 1);
    pthread_mutex_lock(&park_mu); pthread_cond_broadcast(&park_cv); pthread_mutex_unlock(&park_mu);
    hle_signal_system_event(&e_audio);
    (void)hw;
    puts("the deterministic guest clock: sleeps and timeouts on it run on while the presenting thread waits for them, at exact virtual times; no jump while that thread "
         "runs or waits for a host-signalled event");
    return 0;
}
