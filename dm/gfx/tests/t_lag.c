// Frame lag cap unit tests (nvn_lag.h): which fence a frame start waits for.
#include "../nvn/nvn_lag.h"
#include <stdio.h>
static int fail, checks;
static void check(int ok, const char *what) { checks++; fail += !ok; printf("%-70s %s\n", what, ok ? "ok" : "FAIL"); }
int main(void) {
    nvn_lag l = {0};
    check(nvn_lag_fence(&l, 0) == 0, "no present yet: no wait");
    nvn_lag_present(&l, 10);
    check(nvn_lag_fence(&l, 0) == 10, "k 0: the newest present's fence");
    check(nvn_lag_fence(&l, 1) == 0, "k 1 with one present: no wait");
    nvn_lag_present(&l, 25); nvn_lag_present(&l, 31);
    check(nvn_lag_fence(&l, 0) == 31, "k 0 after three presents");
    check(nvn_lag_fence(&l, 1) == 25, "k 1: the present before the newest");
    check(nvn_lag_fence(&l, 2) == 10, "k 2: two before");
    check(nvn_lag_fence(&l, -1) == 0, "lag off (k < 0): no wait");
    for (uint64_t f = 100; f < 140; f++) nvn_lag_present(&l, f);
    check(nvn_lag_fence(&l, 0) == 139 && nvn_lag_fence(&l, 3) == 136, "after the ring wraps");
    check(nvn_lag_fence(&l, NVN_LAG_RING - 1) == 124, "the oldest slot of the ring");
    check(nvn_lag_fence(&l, NVN_LAG_RING) == 0, "k past the ring: no wait");
    printf("t_lag: %d checks, %d failed\n", checks, fail); return fail != 0;
}
