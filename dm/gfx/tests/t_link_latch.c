// Unit test of the display-link callback's late latch deadline (../nvn/nvn_link_latch.h). Pure: no Metal, no clock.
#include "../nvn/nvn_link_latch.h"
#include <math.h>
#include <stdio.h>

static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; printf("FAIL: %s\n", label); } } while (0)
#define NEAR(a, b) (fabs((a) - (b)) < 1e-9)

int main(void) {
    const double T60 = 1.0 / 60, T120 = 1.0 / 120, cb = 100;
    CHECK(NEAR(nvn_link_latch_wait(cb, cb + 0.020, T60, 0, 1), T60 / 3), "far from targetTimestamp: a third of a period at 60 Hz (5.6 ms)");
    CHECK(NEAR(nvn_link_latch_wait(cb, cb + 0.020, T120, 0, 1), T120 / 3), "and of the 120 Hz period");
    CHECK(NEAR(nvn_link_latch_wait(cb, cb + 0.005, T60, 0, 1), 0.005 - T60 / 8), "near targetTimestamp: an eighth of a period before it");
    CHECK(nvn_link_latch_wait(cb, cb + T60 / 8, T60, 0, 1) == 0, "no room before targetTimestamp: no wait");
    CHECK(nvn_link_latch_wait(cb, cb - 0.001, T60, 0, 1) == 0, "a callback past its targetTimestamp: no wait");
    CHECK(nvn_link_latch_wait(cb, cb + 0.020, 0, 0, 1) == 0, "no period: no wait");
    CHECK(nvn_link_latch_wait(cb, cb + 0.020, T60, LINK_LATCH_GIVE_UP - 1, 1) > 0, "a few timeouts in a row: still waits");
    CHECK(nvn_link_latch_wait(cb, cb + 0.020, T60, LINK_LATCH_GIVE_UP, 1) == 0, "too many in a row: stops waiting");
    CHECK(NEAR(nvn_link_latch_wait(cb, cb + 0.020, T60, LINK_LATCH_GIVE_UP, LINK_LATCH_PROBE * 3), T60 / 3), "but tries again every LINK_LATCH_PROBE ticks");
    printf("%d checks, %d failed\n", checks, failed);
    return failed != 0;
}
