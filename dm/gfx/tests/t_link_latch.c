// Unit test of the display-link callback's late latch deadline (../nvn/nvn_link_latch.h). Pure: no Metal, no clock.
#include "../nvn/nvn_link_latch.h"
#include <math.h>
#include <stdio.h>

static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; printf("FAIL: %s\n", label); } } while (0)
#define NEAR(a, b) (fabs((a) - (b)) < 1e-9)

int main(void) {
    const double T60 = 1.0 / 60, T120 = 1.0 / 120, cb = 100;
    CHECK(NEAR(nvn_link_latch_wait(cb, cb + 0.0165, T60, 0, 1), 0.0165 - T60 / 8), "a whole period to targetTimestamp (a ProMotion screen at 60 Hz): up to an eighth of a period before it (14.4 ms)");
    CHECK(NEAR(nvn_link_latch_wait(cb, cb + 0.020, T120, 0, 1), 0.020 - T120 / 8), "and an eighth of the 120 Hz period");
    CHECK(NEAR(nvn_link_latch_wait(cb, cb + 0.005, T60, 0, 1), 0.005 - T60 / 8), "near targetTimestamp: an eighth of a period before it");
    CHECK(nvn_link_latch_wait(cb, cb + T60 / 8, T60, 0, 1) == 0, "no room before targetTimestamp: no wait");
    CHECK(nvn_link_latch_wait(cb, cb - 0.001, T60, 0, 1) == 0, "a callback past its targetTimestamp: no wait");
    CHECK(nvn_link_latch_wait(cb, cb + 0.020, 0, 0, 1) == 0, "no period: no wait");
    CHECK(nvn_link_latch_wait(cb, cb + 0.020, T60, LINK_LATCH_GIVE_UP - 1, 1) > 0, "a few timeouts in a row: still waits");
    CHECK(nvn_link_latch_wait(cb, cb + 0.020, T60, LINK_LATCH_GIVE_UP, 1) == 0, "too many in a row: stops waiting");
    CHECK(NEAR(nvn_link_latch_wait(cb, cb + 0.020, T60, LINK_LATCH_GIVE_UP, LINK_LATCH_PROBE * 3), 0.020 - T60 / 8), "but tries again every LINK_LATCH_PROBE ticks");
    nvn_link_gate g = {0};
    CHECK(!nvn_link_gate_open(&g), "the gate starts closed");
    for (int i = 0; i < LINK_LATCH_GATE - 1; i++) nvn_link_gate_note(&g, 0.0013, 0.0083);
    CHECK(!nvn_link_gate_open(&g), "one early frame short of LINK_LATCH_GATE: still closed");
    nvn_link_gate_note(&g, 0.0013, 0.0083);
    CHECK(nvn_link_gate_open(&g), "LINK_LATCH_GATE early frames in a row (1.3 ms of 8.3 ms room): open");
    nvn_link_gate_note(&g, 0.006, 0.0083);
    CHECK(!nvn_link_gate_open(&g), "a present past half the room (a latch that would have timed out or nearly): closed at once");
    for (int i = 0; i < LINK_LATCH_GATE; i++) nvn_link_gate_note(&g, 0.004, 0.0165);
    CHECK(nvn_link_gate_open(&g), "4 ms of 16.5 ms room counts as early: open again after LINK_LATCH_GATE more");
    nvn_link_gate_note(&g, -1, 0);
    CHECK(!nvn_link_gate_open(&g), "a frame whose acquire no callback woke (the game was late for the tick): closed");
    for (int i = 0; i < LINK_LATCH_GATE / 2; i++) nvn_link_gate_note(&g, 0.001, 0.0165);
    nvn_link_gate_note(&g, 0.009, 0.0165);
    for (int i = 0; i < LINK_LATCH_GATE / 2; i++) nvn_link_gate_note(&g, 0.001, 0.0165);
    CHECK(!nvn_link_gate_open(&g), "hysteresis: a slow frame halfway restarts the count");
    printf("%d checks, %d failed\n", checks, failed);
    return failed != 0;
}
