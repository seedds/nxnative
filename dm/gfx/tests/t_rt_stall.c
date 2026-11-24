// An acquire thread that holds its frame's time-constraint policy and then stops acquiring must not keep that policy for whatever it runs next (GitHub issue #1859). rt_retire acts
// only for another acquire thread and counts presented frames, which stand still when the game's only acquire thread goes off to one long piece of CPU work. As t_rt_comp: the real
// acquire path (hle_nvn.c rt_acquire), with only the thread's CPU clock and the policy calls replaced, so the test never gets a real-time policy. The host's timer is left out and the
// check it runs is called here at chosen times from another thread, so no result depends on how this machine schedules
#define RT_STALL_NO_TIMER
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
static _Thread_local uint64_t test_cpu;
static _Atomic unsigned test_policy_calls;        // time-constraint declarations, each made on the declaring thread itself
static _Atomic unsigned test_standard_other;      // standard policies set on a thread from another thread
static _Atomic unsigned test_standard_port_ok;    // of them, on the acquire thread's port
static _Atomic unsigned test_importance;          // a precedence policy set from another thread, with its importance
static mach_port_t test_acquire_port;
static uint64_t test_clock_gettime_nsec_np(clockid_t clock) { return clock == CLOCK_THREAD_CPUTIME_ID ? test_cpu : clock_gettime_nsec_np(clock); }
static kern_return_t test_thread_policy_set(thread_act_t thread, thread_policy_flavor_t flavor, thread_policy_t policy, mach_msg_type_number_t count) {
    if (flavor == THREAD_STANDARD_POLICY && thread != pthread_mach_thread_np(pthread_self())) { test_standard_other++; if (thread == test_acquire_port) test_standard_port_ok++; return KERN_SUCCESS; }
    if (flavor == THREAD_PRECEDENCE_POLICY && thread != pthread_mach_thread_np(pthread_self())) { test_importance = (unsigned)((thread_precedence_policy_t)policy)->importance; return KERN_SUCCESS; }
    if (thread != pthread_mach_thread_np(pthread_self()) || flavor != THREAD_TIME_CONSTRAINT_POLICY || count != THREAD_TIME_CONSTRAINT_POLICY_COUNT) return KERN_INVALID_ARGUMENT;
    test_policy_calls++;
    return KERN_SUCCESS;
}
#define clock_gettime_nsec_np test_clock_gettime_nsec_np
#define thread_policy_set test_thread_policy_set
#include "../nvn/hle_nvn.c"
#undef thread_policy_set
#undef clock_gettime_nsec_np
module_t *g_mod; int g_nmod;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static void acquire(win_t *w) { int idx; CHECK(h_nvnWindowAcquireTexture((NVNwindow *)w, NULL, &idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS, "a synthetic window acquire succeeds"); }
static void *scan_main(void *at) { rt_stall_check(*(uint64_t *)at); return NULL; }
static void scan_at(uint64_t now) { pthread_t th; pthread_create(&th, NULL, scan_main, &now); pthread_join(th, NULL); }
static void frame(win_t *w, uint64_t cpu) { test_cpu += cpu; g_frame++; acquire(w); }
int main(void) {
    setenv("DM_RT", "1", 1); setenv("DM_RT_COMP", "auto", 1); setenv("DM_FPS_TARGET", "0", 1); setenv("DM_PRESENT_SNAP", "0", 1);
    nvn_present_config(60, 1);
    win_t w = {.interval = 1};
    test_acquire_port = pthread_mach_thread_np(pthread_self());
    test_cpu = 1000000000;
    acquire(&w);
    CHECK(test_policy_calls == 1, "the first acquire declares the frame's time-constraint policy");
    for (int i = 0; i < 30; i++) { frame(&w, 1000000); usleep(2000); }   // frames at their pace keep it: 2 ms between acquires
    CHECK(test_standard_other == 0, "an acquire thread that keeps acquiring keeps its policy");
    uint64_t tid = 0; pthread_threadid_np(NULL, &tid); rt_thr *self = NULL;
    for (int i = 0; i < atomic_load(&g_rtt_n); i++) if (g_rtt[i].acq && g_rtt[i].tid == tid) self = &g_rtt[i];
    CHECK(self != NULL, "the acquire thread has its slot");
    if (!self) return 1;
    uint64_t seen = atomic_load(&self->seen_ns), period = atomic_load(&self->period_ns);
    // A slow game: 8 periods of 15 fps are longer than the stall time, so it keeps its policy between frames
    atomic_store(&self->period_ns, 66666667ull);
    scan_at(seen + RT_STALL_NS);
    CHECK(test_standard_other == 0, "a game at 15 fps keeps its policy through 250 ms without an acquire");
    atomic_store(&self->period_ns, period);
    scan_at(seen + RT_STALL_NS - 1);
    CHECK(test_standard_other == 0, "not before it has been away for the stall time");
    scan_at(seen + RT_STALL_NS);
    CHECK(test_standard_other == 1, "a lone acquire thread that stops acquiring is put back on the standard policy while it is still stalled, by a thread other than itself");
    CHECK(test_importance == 15, "with the priority its lost QoS gave it");
    scan_at(seen + 2 * RT_STALL_NS);
    CHECK(test_standard_other == 1, "once");
    CHECK(test_standard_port_ok == test_standard_other, "the standard policy is set on the acquire thread's own port");
    unsigned before = test_policy_calls, stalled = test_standard_other;
    acquire(&w);
    CHECK(test_policy_calls == before + 1, "its next acquire declares the policy again at once");
    for (int i = 0; i < 30; i++) { frame(&w, 1000000); usleep(2000); }
    CHECK(test_standard_other == stalled, "and it keeps the policy while it acquires again");
    printf("t_rt_stall: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
