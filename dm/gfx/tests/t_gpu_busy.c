// GPU keep-busy policy unit tests (nvn_gpu_busy.h, DM_NVN_GPU_BUSY): the switch's values, when the work runs, how a dispatch's length follows the clock, and the duty's idle time
#include "../nvn/nvn_gpu_busy.h"
#include <math.h>
#include <stdio.h>

static int fail, checks;
static void check(int ok, const char *what) { checks++; fail += !ok; printf("%-78s %s\n", what, ok ? "✓" : "✗"); }

int main(void) {
    nvn_gpu_busy_cfg c;
    check(nvn_gpu_busy_parse(NULL, &c) && c.duty == 0, "unset: off");
    check(nvn_gpu_busy_parse("", &c) && c.duty == 0, "empty: off");
    check(nvn_gpu_busy_parse("0", &c) && c.duty == 0, "0: off");
    check(nvn_gpu_busy_parse("1", &c) && c.duty == GPU_BUSY_DUTY && c.groups == GPU_BUSY_GROUPS, "1: on with the default duty and threadgroups");
    check(nvn_gpu_busy_parse("60", &c) && c.duty == 60 && c.groups == GPU_BUSY_GROUPS, "60: a duty of 60%");
    check(nvn_gpu_busy_parse("80:4", &c) && c.duty == 80 && c.groups == 4, "80:4: a duty of 80%, 4 threadgroups a dispatch");
    check(!nvn_gpu_busy_parse("101", &c) && c.duty == 0, "101: refused, off");
    check(!nvn_gpu_busy_parse("-5", &c) && c.duty == 0, "-5: refused, off");
    check(!nvn_gpu_busy_parse("on", &c) && c.duty == 0, "on: refused, off");
    check(!nvn_gpu_busy_parse("50:0", &c) && c.duty == 0, "50:0: no threadgroups, refused");
    check(!nvn_gpu_busy_parse("50:65", &c) && c.duty == 0, "50:65: more threadgroups than the cap, refused");
    check(!nvn_gpu_busy_parse("50x", &c) && c.duty == 0, "50x: trailing text, refused");

    nvn_gpu_busy_cfg on = {100, 1}, off = {0, 1};
    check(!nvn_gpu_busy_active(&on, 0), "no interval read yet: no work");
    check(!nvn_gpu_busy_active(&on, 1), "interval 1 (60 fps): no work");
    check(nvn_gpu_busy_active(&on, 2), "interval 2 (30 fps): work");
    check(nvn_gpu_busy_active(&on, 3), "interval 3 (20 fps): work");
    check(!nvn_gpu_busy_active(&off, 2), "switch off: no work at interval 2");

    check(nvn_gpu_busy_iters(4096, 1.0, 1.0) == 4096, "a dispatch on target keeps its length");
    check(nvn_gpu_busy_iters(4096, 0.5, 1.0) == 8192, "half the target: twice as long");
    check(nvn_gpu_busy_iters(4096, 0.01, 1.0) == 8192, "far under the target: at most twice a step");
    check(nvn_gpu_busy_iters(4096, 4.0, 1.0) == 2048, "far over the target (the clock fell): at most half a step");
    check(nvn_gpu_busy_iters(4096, 0, 1.0) == 8192, "no GPU time read: grows");
    check(nvn_gpu_busy_iters(GPU_BUSY_ITERS_MIN, 50.0, 1.0) == GPU_BUSY_ITERS_MIN, "never under the floor");
    check(nvn_gpu_busy_iters(GPU_BUSY_ITERS_MAX, 0.1, 1.0) == GPU_BUSY_ITERS_MAX, "never over the cap");
    {   // a clock that falls to 0.4 times and rises back: the length settles on the target within a few dispatches each way
        uint32_t n = 4096; double per_ms = 4096.0;   // multiply-adds a thread per ms at the high clock
        for (int i = 0; i < 20; i++) n = nvn_gpu_busy_iters(n, n / per_ms, 1.0);
        double hi = n / per_ms;
        per_ms *= 0.4; for (int i = 0; i < 20; i++) n = nvn_gpu_busy_iters(n, n / per_ms, 1.0);
        double lo = n / per_ms;
        check(fabs(hi - 1.0) < 0.01 && fabs(lo - 1.0) < 0.01, "the length follows a clock change back to the target GPU time");
    }

    check(nvn_gpu_busy_gap_ms(1.0, 1.0, 100) == 0, "duty 100%: no idle time");
    check(nvn_gpu_busy_gap_ms(1.0, 1.4, 100) == 0, "duty 100% with a round trip longer than the dispatch: none either");
    check(fabs(nvn_gpu_busy_gap_ms(1.0, 1.0, 50) - 1.0) < 1e-9, "duty 50%: as long idle as busy");
    check(fabs(nvn_gpu_busy_gap_ms(1.0, 1.4, 50) - 0.6) < 1e-9, "duty 50%: the round trip counts toward the idle time");
    check(fabs(nvn_gpu_busy_gap_ms(1.0, 1.0, 80) - 0.25) < 1e-9, "duty 80%: a quarter of the busy time idle");
    check(nvn_gpu_busy_gap_ms(1.0, 3.0, 50) == 0, "a round trip longer than the whole period: no gap");
    check(nvn_gpu_busy_gap_ms(-1, 1.0, 50) == 0, "a failed dispatch: no gap from it");

    printf("%d / %d checks passed\n", checks - fail, checks);
    return fail != 0;
}
