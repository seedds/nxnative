// The time-constraint computation a frame thread declares (nvn_rt.h): frames are fed as (frame count, the thread's CPU time) samples, as hle_nvn.c rt_tick takes them
#include "../nvn/nvn_rt.h"
#include <stdio.h>

static int fail, checks;
static void check(int ok, const char *what) { checks++; fail += !ok; printf("%-78s %s\n", what, ok ? "✓" : "✗"); }
// n frames of ms each, one sample a frame; returns the last non-zero declaration (0: none)
static double feed(nvn_rt *r, uint64_t *f, uint64_t *cpu, int n, double ms, double period) {
    double got = 0;
    for (int i = 0; i < n; i++) { *f += 1; *cpu += (uint64_t)(ms * 1e6); double c = nvn_rt_sample(r, *f, *cpu, period); if (c > 0) { got = c; r->comp_ms = c; } }
    return got;
}

int main(void) {
    {   nvn_rt r = {0}; uint64_t f = 10, cpu = 0; nvn_rt_sample(&r, f, cpu, 33.3);
        check(feed(&r, &f, &cpu, RT_WIN - 1, 10, 33.3) == 0, "nothing is declared before a whole window of frames");
        double c = feed(&r, &f, &cpu, 1, 14, 33.3);
        check(c > 15.3 && c < 15.5, "a window of 10 ms frames with one of 14 ms declares 1.1 x the peak, 15.4 ms");
        check(feed(&r, &f, &cpu, RT_WIN, 11, 33.3) == 0, "the next window, 1.25 x 11 = 13.75 ms, is within 15% of it: no new declaration");
        c = feed(&r, &f, &cpu, RT_WIN, 6, 33.3);
        check(c > 7.4 && c < 7.6, "a window of 6 ms frames declares 1.25 x the p90, 7.5 ms");
    }
    {   nvn_rt r = {0}; uint64_t f = 1, cpu = 0; nvn_rt_sample(&r, f, cpu, 33.3);
        double c = feed(&r, &f, &cpu, RT_WIN, 40, 33.3);
        check(c > 29.9 && c < 30.0, "frames longer than the period declare at most 0.9 x the period");
        c = feed(&r, &f, &cpu, RT_WIN, 0.1, 33.3);
        check(c == 1, "an idle thread declares at least 1 ms");
    }
    {   nvn_rt r = {0}; uint64_t cpu = 0; nvn_rt_sample(&r, 100, cpu, 33.3);
        cpu += 50000000; nvn_rt_sample(&r, 200, cpu, 33.3);
        check(r.n == 1 && r.win[0] > 0.49 && r.win[0] < 0.51, "100 frames presented while the thread did 50 ms add one sample of 0.5 ms");
        nvn_rt_sample(&r, 200, cpu + 1000000, 33.3);
        check(r.n == 1, "a second sample in the same frame adds nothing");
    }
    printf("%d/%d checks passed\n", checks - fail, checks);
    return fail != 0;
}
