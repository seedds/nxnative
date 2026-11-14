// The time-constraint declarations through the real acquire path (hle_nvn.c rt_acquire, rt_tick, rt_retire) and a frame thread (nvn_rt_join / nvn_rt_tick), without giving
// the test a real-time policy: only the thread's CPU clock and the policy calls are replaced. A frame acquired in two calls is measured whole, each acquire thread is measured
// and declared on itself, an acquire thread that stops acquiring goes back to the standard policy, and DM_RT_COMP / DM_RT=0 keep their meaning. With no argument the test
// runs itself in its three modes (auto, fixed, off). From PR #2137 (thehobbit85), carried onto the per-thread slots
#include <time.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
static _Thread_local uint64_t test_cpu;
static _Thread_local unsigned test_policy_calls;
static _Thread_local thread_time_constraint_policy_data_t test_policy;
static _Atomic unsigned test_standard_other;   // standard policies set on another thread (rt_retire)
static uint64_t test_clock_gettime_nsec_np(clockid_t clock) { return clock == CLOCK_THREAD_CPUTIME_ID ? test_cpu : clock_gettime_nsec_np(clock); }
static kern_return_t test_thread_policy_set(thread_act_t thread, thread_policy_flavor_t flavor, thread_policy_t policy, mach_msg_type_number_t count) {
    if (flavor == THREAD_STANDARD_POLICY && thread != pthread_mach_thread_np(pthread_self())) { test_standard_other++; return KERN_SUCCESS; }
    if (thread != pthread_mach_thread_np(pthread_self()) || flavor != THREAD_TIME_CONSTRAINT_POLICY || count != THREAD_TIME_CONSTRAINT_POLICY_COUNT) return KERN_INVALID_ARGUMENT;
    test_policy = *(thread_time_constraint_policy_data_t *)policy; test_policy_calls++;
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
static double declared_ms(void) { mach_timebase_info_data_t tb; mach_timebase_info(&tb); return (double)test_policy.computation * tb.numer / tb.denom / 1e6; }
static void acquire(win_t *w) { int idx; CHECK(h_nvnWindowAcquireTexture((NVNwindow *)w, NULL, &idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS, "a synthetic window acquire succeeds"); }
static void frame(win_t *w, int acquires, uint64_t cpu_per_acquire) {   // one presented frame: the frame count moves before its last acquire
    for (int i = 0; i < acquires; i++) { test_cpu += cpu_per_acquire; if (i + 1 == acquires) g_frame++; acquire(w); }
}
static void *other_thread(void *unused) {
    (void)unused; win_t w = {.interval = 1};
    test_cpu = 0;   // a fresh thread's clock is below the first thread's
    acquire(&w);
    CHECK(test_policy_calls == 1 && fabs(declared_ms() - 2.0) < 0.001, "a new acquire thread declares on itself, with 2 ms, not the first thread's computation");
    for (int i = 0; i < 119; i++) frame(&w, 1, 1000000);
    CHECK(test_standard_other == 0, "the first acquire thread keeps its policy until it has missed a whole window of frames");
    frame(&w, 1, 1000000);
    CHECK(fabs(declared_ms() - 1.25) < 0.001, "a thread's samples are its own clock, not the difference from another thread's");
    CHECK(test_standard_other == 1, "an acquire thread that stopped acquiring for a window goes back to the standard policy");
    return NULL;
}
static void *frame_thread(void *unused) {
    (void)unused; void *h = nvn_rt_join("test frame thread");
    CHECK(h != NULL, "a frame thread gets a slot");
    test_cpu = 0; nvn_rt_tick(h);
    for (int i = 0; i < 119; i++) { test_cpu += 4000000; g_frame++; nvn_rt_tick(h); }
    CHECK(test_policy_calls == 0, "a frame thread declares nothing before its first window");
    test_cpu += 4000000; g_frame++; nvn_rt_tick(h);
    CHECK(test_policy_calls == 1 && fabs(declared_ms() - 5.0) < 0.001, "a frame thread declares the acquire thread's period with its own computation, 1.25 x 4 ms");
    return NULL;
}
static int self_run(const char *self, const char *mode) {
    pid_t pid; char *argv[] = {(char *)self, (char *)mode, NULL}; extern char **environ; int st = 0;
    if (posix_spawn(&pid, self, NULL, NULL, argv, environ)) return 1;
    return waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st);
}
int main(int argc, char **argv) {
    if (argc < 2) { int f = self_run(argv[0], "auto") | self_run(argv[0], "fixed") | self_run(argv[0], "off"); printf("t_rt_comp: %s\n", f ? "FAILED" : "all modes passed"); return f; }
    int off = !strcmp(argv[1], "off"), fixed = !strcmp(argv[1], "fixed");
    setenv("DM_RT", off ? "0" : "1", 1); setenv("DM_RT_COMP", fixed ? "4" : "auto", 1);
    setenv("DM_FPS_TARGET", "0", 1); setenv("DM_PRESENT_SNAP", "0", 1);
    nvn_present_config(60, 1);
    win_t w = {.interval = 1};
    test_cpu = 1000000000;
    acquire(&w);
    if (off || fixed) {
        for (int i = 0; i < 121; i++) frame(&w, 2, 10000000);
        CHECK(test_policy_calls == (off ? 0u : 1u), "the off and fixed modes do not retune");
        CHECK(fabs(declared_ms() - (off ? 0 : 4.0)) < 0.001, "the off and fixed modes keep what they asked for");
        printf("t_rt_comp %s: %d checks, %d failed\n", argv[1], checks, failed);
        return failed != 0;
    }
    CHECK(test_policy_calls == 1 && fabs(declared_ms() - 2.0) < 0.001, "the first acquire declares 2 ms");
    for (int i = 0; i < 120; i++) frame(&w, 2, 3000000);   // the first frame's sample only starts the clock
    CHECK(test_policy_calls == 1, "repeated acquires do not fill the window early");
    frame(&w, 2, 3000000);
    CHECK(fabs(declared_ms() - 7.5) < 0.001, "the computation covers the whole frame (6 ms in two acquires: 7.5 ms), not each acquire");
    pthread_t t;
    CHECK(!pthread_create(&t, NULL, other_thread, NULL), "start a second acquire thread"); pthread_join(t, NULL);
    CHECK(fabs(declared_ms() - 7.5) < 0.001, "another acquire thread's declaration is not made on this thread");
    unsigned before = test_policy_calls; acquire(&w);
    CHECK(test_policy_calls == before + 1 && fabs(declared_ms() - 7.5) < 0.001, "acquiring again after being retired declares at once what this thread had");
    for (int i = 0; i < 120; i++) frame(&w, 2, 3000000);
    CHECK(test_policy_calls == before + 1, "the same need measured again is not declared again");
    CHECK(test_standard_other == 1, "an acquire thread that has ended is retired without a policy call on its port");
    before = test_policy_calls; nvn_present_config(30, 1); acquire(&w);
    CHECK(test_policy_calls == before + 1 && fabs(declared_ms() - 7.5) < 0.001, "a period change keeps the computation measured");
    nvn_present_config(60, 1); acquire(&w);
    for (int i = 0; i < 120; i++) frame(&w, 1, 20000000);
    CHECK(fabs(declared_ms() - 15.0) < 0.001, "the computation is capped at 0.9 x the period");
    CHECK(!pthread_create(&t, NULL, frame_thread, NULL), "start a frame thread"); pthread_join(t, NULL);
    printf("t_rt_comp auto: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
