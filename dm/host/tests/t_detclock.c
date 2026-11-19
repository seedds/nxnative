// The deterministic guest clock (DM_DETCLOCK: hle_os.c's virtual time, hle_thread.c's det_idle) through the real thread SVCs, with no game files
// (docs/reference/threads.md, "A game stops at its first frame under DM_DETCLOCK").
//  1. A main thread that presents (the frame tick) only after a worker has slept on the guest clock (SleepThread) and let two timed waits run out (WaitSynchronization,
//     WaitForAddress), frame after frame, beside the threads a game has that wait all the while: one parked in a native HLE wait (as on the notification queue), one waiting
//     with no timeout on an event host code signals (an audio thread on the renderer's event), and a guest thread the host made for itself that waits in host code (the audio
//     renderer's). Before the fix the clock moved only at a present, the worker's first sleep never ended and the main thread never presented again: the alarm (CTest's time
//     limit on Windows) fails the run. Every clock value both threads read is checked against the exact virtual-time trace (sleeps and timeouts end at their deadlines, a
//     present moves the clock to at least the previous present's value plus the step, nothing moves while the main thread does host work), so every run reads the same values
//     and a run that does not fails.
//  2. A main thread that presents after waiting for an event host code signals (the audio renderer's kind) while a worker sleeps 1 ms at a time: the clock does not jump during
//     that wait, so it is exactly one step on at each present, and the worker reads the presents' values.
//  3. The SDK's timer-event wait (nn::os::WaitTimerEvent, ported to C below from Mario vs. Donkey Kong's sdk module) on a 5 ms periodic timer, started from 625 clock phases
//     against the 19.2 MHz tick, while every other thread waits: each period takes one timed wait and ends past the timer's wakeup. Before the fix a timeout ended at the call's
//     clock + its nanoseconds, which can fall in the tick before the one the SDK's deadline names; the SDK then found no time left and the timer not yet due, polled without an SVC,
//     and the clock never moved (docs/reference/threads.md, "A timer wait polls for ever under DM_DETCLOCK"). Sleeps and timeouts end where Horizon's kernel ends them: at the
//     tick of the call + the timeout in ticks rounded up + 2 (kdl below), which scenario 1 checks to the nanosecond.
//  4. A game whose main thread works while its presenting thread waits for the display (in NVN's acquire, or where the main thread waits on vi's vsync event) and presents, and
//     which reads its CPU span against the tick: the span is the job it waited for, shorter than a step, the frames one step each at interval 1 and two at 2, the same values in
//     every run. Before, every present moved the clock a step, inside the span (docs/reference/threads.md, the DM_DETCLOCK entry's Note).
// t_detclock <n> runs only scenario n after the common start (to show one scenario's failure on an older host).
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
// Where Horizon's kernel ends a sleep or timeout of ns begun at guest time at (absolute ns): the tick of the call + ns in ticks rounded up + 2 (mesosphère's SleepThread,
// WaitSynchronization, WaitProcessWideKeyAtomic: GetTick() + Tick(ns) + 2), at the first nanosecond of that tick; kdl_rel takes and gives times relative to g_base
static uint64_t kdl(uint64_t at, uint64_t ns) {
    uint64_t tick = at / 625 * 12 + at % 625 * 12 / 625 + ns / 625 * 12 + (ns % 625 * 12 + 624) / 625 + 2;
    return tick / 12 * 625 + (tick % 12 * 625 + 11) / 12;
}
static uint64_t kdl_rel(uint64_t at, uint64_t ns) { return kdl(g_base + at, ns) - g_base; }

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

// ---- Scenario 3 ----
// The SDK's timer event, ported from Mario vs. Donkey Kong's sdk module (build d5a1029a57d8cd4f): WaitTimerEvent (sdk+0x125110), UpdateSignalStateAndRecalcNextTimeToWakeupUnsafe
// (sdk+0x12ac00), the TimeoutHelper it builds (sdk+0x1251cc to +0x1251f8) and InternalConditionVariableImplByHorizon::TimedWait (sdk+0x131138) with GetLeftTimeOnTarget
// (sdk+0x12ab9c), and TickManager's conversions (ConvertToTimeSpan sdk+0x12aaa8 truncates, ConvertToTick sdk+0x12ab30 rounds up). The tick is what svcGetSystemTick and the
// counter gate answer (hle_svc.c, aot_run.c: guest_ns * 12 / 625)
#define TICK_HZ 19200000ll
#define NS_S 1000000000ll
static int64_t tick_now(void) { return (int64_t)(guest_ns() * 12 / 625); }
static int64_t tick_ns(int64_t t) { return t / TICK_HZ * NS_S + t % TICK_HZ * NS_S / TICK_HZ; }
static int64_t ns_tick(int64_t ns) { return ns / NS_S * TICK_HZ + (ns % NS_S * TICK_HZ + NS_S - 1) / NS_S; }
typedef struct { int64_t next, first, interval; _Atomic uint32_t lock, cv; } sdk_timer;   // a periodic TimerEventType: next wakeup +0x20, first +0x28, interval +0x30
static void timer_start(sdk_timer *tm, int64_t first, int64_t interval) {   // StartPeriodicTimerEvent (sdk+0x124e90): the next wakeup from the current tick's time
    tm->next = tick_ns(tick_now()) + first; tm->first = first; tm->interval = interval;
}
static int timer_update(sdk_timer *tm, int64_t cur) {   // due only once the next wakeup lies before the current time (b.ge at sdk+0x12ac48)
    if (tm->next >= cur) return 0;
    int64_t n = tm->next + tm->interval;
    if (n < cur) n = ((cur - tm->first) / tm->interval + 1) * tm->interval + tm->first;
    tm->next = n; return 1;
}
static int timer_wait(sdk_timer *tm, uint32_t self) {   // one WaitTimerEvent: the timed waits it made
    int waits = 0; unsigned held = 0; int64_t last = -1;
    for (;;) {
        int64_t cur = tick_ns(tick_now());
        if (timer_update(tm, cur)) return waits;
        int64_t left = tm->next - cur, end = left ? tick_now() + ns_tick(left) + 1 : 0;   // a zero difference makes a helper whose end tick is 0
        int64_t left_ticks = end ? end - tick_now() : 0;
        if (left_ticks < 1) {   // TimedWait returns "timed out" without an SVC (sdk+0x13117c): the SDK polls, and only time passing ends that
            if (cur != last) held = 0;
            CHECK(++held < 100000, "timer wait: %u passes with no time left and the timer not due (its wakeup %lld ns, the clock's tick %lld = %lld ns): the SDK polls and holds the "
                  "guest clock", held, (long long)tm->next, (long long)tick_now(), (long long)cur);
            last = cur; continue;
        }
        uint64_t r[32] = {(uint64_t)(uintptr_t)&tm->lock, (uint64_t)(uintptr_t)&tm->cv, self, (uint64_t)tick_ns(left_ticks)};   // WaitProcessWideKeyAtomic
        CHECK(svc_thread(0x1C, r) && r[0] == R_TIMEOUT, "timer wait: WaitProcessWideKeyAtomic returned %#llx, expected a timeout", (unsigned long long)r[0]);
        uint32_t z = 0; CHECK(atomic_compare_exchange_strong(&tm->lock, &z, self), "timer wait: the lock was not free after the timeout");   // InternalCriticalSection::Enter
        waits++;
    }
}
enum { PHASES = 625, PERIODS = 3 };
static test_event e_timer;
static void vibration(uint64_t arg) {   // as rmbl::VibrationThread: a 5 ms periodic timer with first = interval (main+0xd7aea4), waited on in a loop (main+0xd7af00)
    (void)arg;
    uint32_t self = ((thr_t *)pthread_getspecific(g_keythr))->handle;
    sdk_timer tm = {0};
    for (int ph = 0; ph < PHASES; ph++) {
        // a sleep that ends at phase ph of the 625 ns in which the tick counts 12 when it ends at the call's clock + ns (before the fix); the kernel's deadline is a tick's start
        uint64_t now = guest_ns(); sleep_guest(1 * MS + (uint64_t)(ph + 625 - (int)((now + 1 * MS) % 625)) % 625);
        tm.lock = self; timer_start(&tm, 5 * MS, 5 * MS);
        for (int k = 0; k < PERIODS; k++) {
            int64_t due = tm.next; int w = timer_wait(&tm, self);
            CHECK(w == 1 && tick_ns(tick_now()) > due, "timer wait, phase %d, period %d: %d timed waits, the clock %lld ns against the wakeup %lld ns", ph, k, w, (long long)tick_ns(tick_now()), (long long)due);
        }
    }
    event_signal(e_timer);
}

// ---- Scenario 4 ----
// A game whose main thread works while its presenting thread waits for the display, as Mario vs. Donkey Kong's framework (main+0x4381bc): the main thread takes the loop's end
// from the tick, hands the frame before to its presenting thread and works on the next (host work, no guest time, and a job that sleeps 1 ms on the guest clock), reads its CPU
// span against the tick (framework +0x118, main+0x43825c), then waits for that frame's present. The presenting thread waits for the display, either in NVN's acquire until its
// texture is free (4a: dm_detclock_display_wait, one refresh after the previous present at interval 1, two at 2) or where the main thread waits on vi's vsync event (4b: the event
// a host thread signals at 60 Hz of wall time, as hle_sdk16.c's does, and the guest clock under the switch), and presents (an NVN present: dm_detclock_present_paced). The span
// must be the job's sleep (a Switch's CPU span, shorter than a frame), the frames one step each at interval 1 and two at 2, and every value the same in each run; before, the
// present moved the clock a whole step inside the span
enum { PFRAMES = 8, PRUNS = 2 };
static test_event e_vs, e_ready, e_presented, e_job, e_jobdone;
static uint64_t s4_pres[PFRAMES][2];   // the clock before and after each present
static _Atomic int s4_stop, s4_itv, s4_vsync;   // the present interval; 4b
static uint64_t g_v0;   // the vsync times: g_v0 + k × STEP (the clock at the first present)
static void presenter4(uint64_t arg) {
    uint64_t last = arg;   // the previous present's clock
    for (int f = 0; f < PFRAMES; f++) {
        CHECK(event_wait(e_ready, -1) == 0, "presenter: wait for a frame failed"); event_reset(e_ready);
        if (!atomic_load(&s4_vsync)) dm_detclock_display_wait(last + (uint64_t)atomic_load(&s4_itv) * STEP);   // the acquire: the texture is free one refresh (two) after the last present
        s4_pres[f][0] = guest_ns(); dm_detclock_present_paced(atomic_load(&s4_itv)); dm_guest_frame_tick(); s4_pres[f][1] = last = guest_ns();
        event_signal(e_presented);
    }
}
static void job4(uint64_t arg) {
    (void)arg;
    for (int f = 0; f < PFRAMES; f++) {
        CHECK(event_wait(e_job, -1) == 0, "job: wait failed"); event_reset(e_job);
        sleep_guest(1 * MS);
        event_signal(e_jobdone);
    }
}
static void *vsync_host4(void *arg) { (void)arg; while (!atomic_load(&s4_stop)) { plat_sleep_ns(STEP); hle_signal_system_event(&e_vs); } return NULL; }
// One run of PFRAMES frames; out: each frame's loop end (from the run's first) and CPU span in ticks
static void run4(const char *name, int itv, uint64_t out[PFRAMES][2]) {
    atomic_store(&s4_itv, itv);
    thread_new(presenter4, guest_ns()); thread_new(job4, 0);
    uint64_t l0 = 0;
    for (int f = 0; f < PFRAMES; f++) {
        if (atomic_load(&s4_vsync)) { CHECK(event_wait(e_vs, -1) == 0, "main: wait for the vsync failed"); event_reset(e_vs); }
        uint64_t l = guest_ns(); int64_t loop_end = tick_now();
        if (!f) l0 = l;
        event_signal(e_ready);   // the presenting thread waits for the display and presents the frame before while this one works
        dm_host_work = 1; plat_sleep_ns(3 * MS); dm_host_work = 0;
        event_signal(e_job); CHECK(event_wait(e_jobdone, -1) == 0, "main: wait for the job failed"); event_reset(e_jobdone);
        int64_t span = tick_now() - loop_end, want = (int64_t)(kdl(l, 1 * MS) * 12 / 625) - loop_end;
        CHECK(span < (int64_t)(STEP * 12 / 625), "%s, frame %d: the CPU span reads %lld ticks, a step or more (%lld): a present moved the clock while the main thread worked", name, f,
              (long long)span, (long long)(STEP * 12 / 625));
        CHECK(span == want, "%s, frame %d: the CPU span reads %lld ticks, expected %lld (the job's sleep)", name, f, (long long)span, (long long)want);
        CHECK(!f || l - l0 == (uint64_t)(f * itv) * STEP, "%s, frame %d: the loop ended %llu ns after the first, expected %d steps", name, f, (unsigned long long)(l - l0), f * itv);
        CHECK(!atomic_load(&s4_vsync) || (l - g_v0) % STEP == 0, "%s, frame %d: the vsync wait ended %llu ns after the first vsync time, not at a vsync time", name, f,
              (unsigned long long)(l - g_v0));
        out[f][0] = l - l0; out[f][1] = (uint64_t)span;
        if (!atomic_load(&s4_vsync)) { CHECK(event_wait(e_presented, -1) == 0, "main: wait for the present failed"); event_reset(e_presented); }
    }
    if (atomic_load(&s4_vsync)) { CHECK(event_wait(e_vs, -1) == 0, "main: wait for the vsync failed"); event_reset(e_vs); }   // the last present is done once both wait
    for (int f = 0; f < PFRAMES; f++)
        CHECK(s4_pres[f][1] == s4_pres[f][0], "%s, frame %d: the present moved the clock by %llu ns", name, f, (unsigned long long)(s4_pres[f][1] - s4_pres[f][0]));
}
static void scenario4(const char *name, int vsync, int itv) {
    static uint64_t out[PRUNS][PFRAMES][2];
    atomic_store(&s4_vsync, vsync);
    for (int run = 0; run < PRUNS; run++) run4(name, itv, out[run]);
    for (int f = 0; f < PFRAMES; f++)
        CHECK(out[1][f][0] == out[0][f][0] && out[1][f][1] == out[0][f][1], "%s, frame %d: the second run read %llu ns and %llu ticks, the first %llu and %llu", name, f,
              (unsigned long long)out[1][f][0], (unsigned long long)out[1][f][1], (unsigned long long)out[0][f][0], (unsigned long long)out[0][f][1]);
    printf("%s: %d runs of %d frames, %d step(s) apart, the CPU span %llu ticks (the job's 1 ms) where a step is %llu, the presents moved nothing, the same values in each run\n",
           name, PRUNS, PFRAMES, itv, (unsigned long long)out[0][0][1], (unsigned long long)(STEP * 12 / 625));
}

static void timeout(int sig) { (void)sig; const char m[] = "[RED] t_detclock: timed out: a thread waits on the guest clock and nothing moves it\n"; write(2, m, sizeof m - 1); _exit(99); }
int main(int argc, char **argv) {
    int only = argc > 1 ? atoi(argv[1]) : 0;   // one scenario (3, 1, 2, 4 or 5) after the common start; all by default
#if !defined(_WIN32)
    signal(SIGALRM, timeout); alarm(30);
#else
    (void)timeout;   // no alarm on Windows: CTest's time limit bounds the run there
#endif
    setenv("DM_DETCLOCK", "16666667", 1);
    setenv("DM_PREEMPT", "0", 1);
    svc_thread_main_init();
    e_go = event_new(); e_done = event_new(); e_never = event_new();
    hle_bind_system_event(&e_audio, 1); hle_bind_system_event(&e_vsync, 1);   // bound like the service-layer events (the audio renderer's)
    uint32_t hp = thread_new(parked, 0), ha = thread_new(audio, 0), hr = thread_new(render, 0);
    until_state(hp, 2, 1); until_state(ha, 1, 4); until_state(hr, 3, 1);
    uint32_t hw = thread_new(worker, 0);
    until_state(hw, 1, 4);   // waiting for the first frame
    dm_guest_frame_tick();   // the first present: the switch takes the clock over
    CHECK(dm_detclock_enabled(), "DM_DETCLOCK is not on after the first present");
    g_v0 = g_base = guest_ns();

    // 3, while every other thread waits
    if (!only || only == 3) {
        e_timer = event_new();
        uint64_t t3 = guest_ns(); thread_new(vibration, 0);
        CHECK(event_wait(e_timer, -1) == 0, "main: wait for the timer thread failed"); event_reset(e_timer);
        printf("timer waits: %d phases, %d periods each, one timed wait a period, %.3f ms of guest time\n", PHASES, PERIODS, (guest_ns() - t3) / 1e6);
    }
    dm_guest_frame_tick();   // a present: the frames below count from it
    g_base = guest_ns();

    // 1: presents of a layer that does not wait for the display (the guest Vulkan and OpenGL layers) move the clock one step past the previous present's value
    uint64_t p = 0;
    for (int f = 0; f < FRAMES && (!only || only == 1); f++) {
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
        uint64_t want[4] = {p, kdl_rel(p, s), 0, 0};   // each sleep and timeout ends at the kernel's deadline from the one before
        want[2] = kdl_rel(want[1], w); want[3] = kdl_rel(want[2], 1 * MS);
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
    p = clock_now();
    if (!only || only == 2) {
        vs_req = plat_sema_create(0);
        pthread_t vt; CHECK(!pthread_create(&vt, NULL, vsync_host, NULL), "pthread_create failed");
        uint32_t h2 = thread_new(worker2, 0);
        for (int g = 0; g < VFRAMES; g++) {
            plat_sema_signal(vs_req);
            CHECK(event_wait(e_vsync, -1) == 0, "main: wait for the host's event failed"); event_reset(e_vsync);
            CHECK(clock_now() == p, "host frame %d: the clock is %llu ns after the wait, expected %llu (it ran ahead of the present)", g, (unsigned long long)clock_now(), (unsigned long long)p);
            CHECK(atomic_load(&w2_n) == g + 1, "host frame %d: worker 2 slept %d times, expected %d", g, atomic_load(&w2_n), g + 1);
            dm_guest_frame_tick();
            p += STEP;
            CHECK(clock_now() == p, "host frame %d: the present moved the clock to %llu ns, expected %llu", g, (unsigned long long)clock_now(), (unsigned long long)p);
        }
        pthread_join(vt, NULL);
        for (int g = 0; g < VFRAMES; g++)
            CHECK(w2_read[g] == p - (uint64_t)(VFRAMES - g) * STEP, "worker 2's read %d is %llu ns, expected %llu", g, (unsigned long long)w2_read[g], (unsigned long long)(p - (uint64_t)(VFRAMES - g) * STEP));
        printf("frames on a host-signalled event: the clock one step on at each of %d presents, worker 2 read each present's value\n", VFRAMES);
        atomic_store(&w2_stop, 1);   // worker 2 wakes by a jump once the main thread waits for it
        uint64_t r[32] = {0, (uint64_t)(uintptr_t)&h2, 1, (uint64_t)-1};
        CHECK(svc_thread(0x18, r) && r[0] == 0, "main: wait for worker 2's exit failed");
    }

    // 4: 4a at interval 1 and 2, then 4b
    if (!only || only == 4) {
        e_ready = event_new(); e_presented = event_new(); e_job = event_new(); e_jobdone = event_new();
        scenario4("4a, the acquire waits for the display, interval 1", 0, 1);
        scenario4("4a, interval 2", 0, 2);
        hle_bind_system_event(&e_vs, 1); dm_detclock_vsync_event(&e_vs);   // bound and marked as hle_sdk16.c's GetDisplayVsyncEvent does
        pthread_t vt; CHECK(!pthread_create(&vt, NULL, vsync_host4, NULL), "pthread_create failed");
        scenario4("4b, the main thread waits on vi's vsync event", 1, 1);
        atomic_store(&s4_stop, 1); pthread_join(vt, NULL);
    }

    // The end: every thread let go
    atomic_store(&finish, 1);
    pthread_mutex_lock(&park_mu); pthread_cond_broadcast(&park_cv); pthread_mutex_unlock(&park_mu);
    hle_signal_system_event(&e_audio);
    (void)hw;
    puts("the deterministic guest clock: sleeps and timeouts on it end where Horizon's kernel ends them, run on while the presenting thread waits for them, at exact virtual "
         "times; the display's pace is a wait on it; no jump while a thread runs or the presenting thread waits for a host-signalled event");
    return 0;
}
