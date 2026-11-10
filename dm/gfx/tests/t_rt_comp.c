// Exercise the actual acquire path without giving the test a real-time policy.
// Only the OS CPU clock and policy submission are replaced. The supplied CPU
// intervals are work done by this thread, independently of wall-clock pacing.
#include <time.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
static _Thread_local uint64_t test_cpu;
static _Thread_local unsigned test_policy_calls;
static _Thread_local thread_time_constraint_policy_data_t test_policy;
static uint64_t test_clock_gettime_nsec_np(clockid_t clock) {
    return clock == CLOCK_THREAD_CPUTIME_ID ? test_cpu : clock_gettime_nsec_np(clock);
}
static kern_return_t test_thread_policy_set(thread_act_t thread, thread_policy_flavor_t flavor, thread_policy_t policy, mach_msg_type_number_t count) {
    if (thread != pthread_mach_thread_np(pthread_self()) || flavor != THREAD_TIME_CONSTRAINT_POLICY || count != THREAD_TIME_CONSTRAINT_POLICY_COUNT) return KERN_INVALID_ARGUMENT;
    test_policy = *(thread_time_constraint_policy_data_t *)policy;
    test_policy_calls++;
    return KERN_SUCCESS;
}
#define clock_gettime_nsec_np test_clock_gettime_nsec_np
#define thread_policy_set test_thread_policy_set
#include "../nvn/hle_nvn.c"
#undef thread_policy_set
#undef clock_gettime_nsec_np
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static double declared_ms(void) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    return (double)test_policy.computation * tb.numer / tb.denom / 1e6;
}
static void acquire(win_t *w) {
    int idx;
    CHECK(h_nvnWindowAcquireTexture((NVNwindow *)w, NULL, &idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS, "synthetic window acquire succeeds");
}
static void frame(win_t *w, int acquires, uint64_t cpu_per_acquire) {
    for (int i = 0; i < acquires; i++) {
        test_cpu += cpu_per_acquire;
        if (i + 1 == acquires) g_frame++;
        acquire(w);
    }
}
static void *other_thread(void *unused) {
    (void)unused;
    win_t w = {.interval = 1};
    test_cpu = 0;   // a fresh thread's clock is below the first thread's (zero is a valid first sample)
    acquire(&w);
    CHECK(test_policy_calls == 1, "a new acquire thread receives its own initial policy");
    CHECK(fabs(declared_ms() - 2.0) < 0.001, "a new thread does not inherit another thread's CPU budget");
    for (int i = 0; i < 120; i++) frame(&w, 1, 1000000);
    CHECK(fabs(declared_ms() - 1.25) < 0.001, "a thread's CPU samples do not subtract another thread's clock");
    return NULL;
}
int main(int argc, char **argv) {
    int off = argc > 1 && !strcmp(argv[1], "off"), fixed = argc > 1 && !strcmp(argv[1], "fixed");
    setenv("DM_RT", off ? "0" : "1", 1); setenv("DM_RT_COMP", fixed ? "4" : "auto", 1);
    setenv("DM_FPS_TARGET", "0", 1); setenv("DM_PRESENT_SNAP", "0", 1);
    nvn_present_config(60, 1);
    win_t w = {.interval = 1};
    test_cpu = 1000000000;
    acquire(&w);
    if (off || fixed) {
        for (int i = 0; i < 120; i++) frame(&w, 2, 10000000);
        CHECK(test_policy_calls == (off ? 0 : 1), "disabled and fixed modes do not auto-retune");
        CHECK(fabs(declared_ms() - (off ? 0 : 4.0)) < 0.001, "disabled and fixed modes retain their requested budget");
        printf("t_rt_comp %s: %d checks, %d failed\n", argv[1], checks, failed);
        return failed != 0;
    }
    CHECK(test_policy_calls == 1, "first acquire declares a policy");
    for (int i = 0; i < 119; i++) frame(&w, 2, 3000000);
    CHECK(test_policy_calls == 1, "repeated acquires do not complete the 120-frame sample window early");
    frame(&w, 2, 3000000);
    printf("6 ms CPU per frame in two acquires: declared %.3f ms\n", declared_ms());
    CHECK(fabs(declared_ms() - 7.5) < 0.001, "the budget covers the full frame, not each acquire interval");
    pthread_t t;
    CHECK(!pthread_create(&t, NULL, other_thread, NULL), "create a replacement acquire thread");
    pthread_join(t, NULL);
    CHECK(fabs(declared_ms() - 7.5) < 0.001, "another thread's policy does not overwrite this thread's policy");
    // Returning to this thread must also retain its sampling baseline and budget.
    for (int i = 0; i < 120; i++) frame(&w, 2, 3000000);
    CHECK(fabs(declared_ms() - 7.5) < 0.001, "returning to an acquire thread preserves its CPU history");
    unsigned before = test_policy_calls;
    nvn_present_config(30, 1); acquire(&w);
    CHECK(test_policy_calls == before + 1 && fabs(declared_ms() - 7.5) < 0.001, "a period change retains this thread's learned CPU budget");
    nvn_present_config(60, 1); acquire(&w);
    for (int i = 0; i < 120; i++) frame(&w, 1, 20000000);
    CHECK(fabs(declared_ms() - 15.0) < 0.001, "the full-frame estimate still respects the period's 90 percent cap");
    printf("t_rt_comp: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
