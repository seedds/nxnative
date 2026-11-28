// Unit test of the display-link callback's late latch deadline (../nvn/nvn_link_latch.h). Pure: no Metal, no clock.
#include "../nvn/nvn_link_latch.h"
#include <math.h>
#include <stdio.h>

static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; printf("FAIL: %s\n", label); } } while (0)
#define NEAR(a, b) (fabs((a) - (b)) < 1e-9)

int main(void) {
    const double T60 = 1.0 / 60, T120 = 1.0 / 120, cb = 100;
    CHECK(NEAR(nvn_link_latch_wait(cb, cb + 0.0165, T60, 0, 1, 0), 0.0165 - T60 / 8), "a whole period to targetTimestamp (a ProMotion screen at 60 Hz): up to an eighth of a period before it (14.4 ms)");
    CHECK(NEAR(nvn_link_latch_wait(cb, cb + 0.020, T120, 0, 1, 0), T120 - T120 / 8), "room past the next callback (frame latency 2): no further than an eighth of a period before it (7.3 ms of an 8.3 ms period)");
    CHECK(NEAR(nvn_link_latch_wait(cb, cb + 0.020, T60, 0, 1, 0), T60 - T60 / 8), "and of a 60 Hz period (14.6 ms)");
    CHECK(NEAR(nvn_link_latch_wait(cb, cb + 0.005, T60, 0, 1, 0), 0.005 - T60 / 8), "near targetTimestamp: an eighth of a period before it");
    CHECK(nvn_link_latch_wait(cb, cb + T60 / 8, T60, 0, 1, 0) == 0, "no room before targetTimestamp: no wait");
    CHECK(nvn_link_latch_wait(cb, cb - 0.001, T60, 0, 1, 0) == 0, "a callback past its targetTimestamp: no wait");
    CHECK(nvn_link_latch_wait(cb, cb + 0.020, 0, 0, 1, 0) == 0, "no period: no wait");
    CHECK(nvn_link_latch_wait(cb, cb + 0.020, T60, LINK_LATCH_GIVE_UP - 1, 1, 0) > 0, "a few timeouts in a row: still waits");
    CHECK(nvn_link_latch_wait(cb, cb + 0.020, T60, LINK_LATCH_GIVE_UP, 1, 0) == 0, "too many in a row: stops waiting");
    CHECK(nvn_link_latch_wait(cb, cb + 0.020, T60, LINK_LATCH_GIVE_UP, LINK_LATCH_PROBE * 3, 0) > 0, "but tries again on every LINK_LATCH_PROBE-th woken callback");
    int probes = 0; for (uint64_t w = 1; w <= 2 * LINK_LATCH_PROBE; w++) probes += nvn_link_latch_wait(cb, cb + 0.020, T60, LINK_LATCH_GIVE_UP, w, 0) > 0;
    CHECK(probes == 2, "counted in woken callbacks, not ticks: two probes in 2 x LINK_LATCH_PROBE woken callbacks, whatever ticks each acquire waits");
    CHECK(nvn_link_latch_wait(cb, cb + 0.020, T60, LINK_LATCH_GIVE_UP, 1, 1) > 0, "the end of a drain into the latch waits through a give-up");
    CHECK(NEAR(nvn_link_latch_reach(0.033, T60), T60 - T60 / 8) && NEAR(nvn_link_latch_wait(cb, cb + 0.033, T60, 0, 1, 0), nvn_link_latch_reach(0.033, T60)),
          "the wait is its reach: 14.6 ms of 33 ms room (a window on a built-in ProMotion screen)");

    // Runtime health: exit on late GPU or skipped ticks after a latched present
    nvn_link_latch_health h = {0};
    CHECK(!nvn_link_latch_check(&h, 2 * T60, T60, 0, 1), "a large gap when the previous tick did not latch does not exit");
    CHECK(nvn_link_latch_check(&h, T60, T60, 1, 0), "a late-GPU feedback exits immediately even on a normal tick gap");
    h = (nvn_link_latch_health){.prev = 1};
    CHECK(nvn_link_latch_check(&h, 2 * T60, T60, 0, 1) && h.skips == 0, "a single 33.3 ms skipped tick right after a drain into the latch exits at once");
    h = (nvn_link_latch_health){.prev = 1};
    CHECK(nvn_link_latch_check(&h, 1.5 * T60, T60, 0, 1), "a 25 ms slip (one 120 Hz refresh late) right after a drain exits too");
    h = (nvn_link_latch_health){.prev = 1};
    CHECK(!nvn_link_latch_check(&h, 1.25 * T60, T60, 0, 1), "a 144 Hz virtual-tick gap (20.83 ms = 1.25 ticks) is not a skipped tick");
    CHECK(!nvn_link_latch_check(&h, 0.30, T60, 0, 1), "a long pause (300 ms) is not counted as a skipped tick");
    h = (nvn_link_latch_health){.prev = 1};
    CHECK(!nvn_link_latch_check(&h, 2 * T60, T60, 0, 0) && h.skips == 1, "one stray skipped tick during steady latched play does not exit");
    CHECK(nvn_link_latch_check(&h, 2 * T60, T60, 0, 0) && h.skips == 0, "a second skipped tick exits immediately");
    h = (nvn_link_latch_health){.prev = 1};
    CHECK(!nvn_link_latch_check(&h, 2 * T60, T60, 0, 0) && h.skips == 1, "one stray skip remembered");
    for (int i = 0; i < LINK_LATCH_GIVE_UP; i++) CHECK(!nvn_link_latch_check(&h, T60, T60, 0, 0), "clean latched tick");
    CHECK(h.skips == 0 && !nvn_link_latch_check(&h, 2 * T60, T60, 0, 0), "LINK_LATCH_GIVE_UP clean latched ticks clear a prior stray skip");
    h.prev = 0;
    CHECK(!nvn_link_latch_check(&h, T60, T60, 0, 0) && h.skips == 0 && h.clean == 0, "leaving the latch clears prior skip state");

    printf("t_link_latch: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
