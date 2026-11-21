#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../stick_deadzone.h"

static void t_percent(void) {
    assert(stick_deadzone_percent(NULL) == 0 && stick_deadzone_percent("") == 0);   // unset: the stick is left alone
    assert(stick_deadzone_percent("0") == 0 && stick_deadzone_percent("25") == 25 && stick_deadzone_percent("50") == 50);
    assert(stick_deadzone_percent("51") == 0 && stick_deadzone_percent("-3") == 0 && stick_deadzone_percent("12x") == 0 && stick_deadzone_percent("1.5") == 0);
}
static void t_unset_is_unchanged(void) {
    int32_t x = 1234, y = -4321;
    stick_deadzone_apply(&x, &y, 0);
    assert(x == 1234 && y == -4321);
}
static void t_inside_reads_centred(void) {
    int32_t x = DM_STICK_MAX / 10, y = DM_STICK_MAX / 10;   // radius 0.14, inside 20%
    stick_deadzone_apply(&x, &y, 20);
    assert(x == 0 && y == 0);
}
static void t_outside_is_rescaled_and_radial(void) {
    int32_t x = DM_STICK_MAX / 2, y = 0;   // half travel with a 20% deadzone: (0.5-0.2)/(0.8) = 0.375 of the range
    stick_deadzone_apply(&x, &y, 20);
    assert(abs(x - (int32_t)(0.375f * DM_STICK_MAX)) <= 1 && y == 0);
    int32_t dx = DM_STICK_MAX * 6 / 10, dy = DM_STICK_MAX * 6 / 10;   // a diagonal keeps its angle
    stick_deadzone_apply(&dx, &dy, 20);
    assert(dx == dy && dx > 0);
}
static void t_full_travel_stays_full(void) {
    int32_t x = DM_STICK_MAX, y = 0;
    stick_deadzone_apply(&x, &y, 50);
    assert(x == DM_STICK_MAX && y == 0);
    int32_t cx = DM_STICK_MAX, cy = DM_STICK_MAX;   // a square corner is beyond a full radius: not rotated
    stick_deadzone_apply(&cx, &cy, 30);
    assert(cx == DM_STICK_MAX && cy == DM_STICK_MAX);
    int32_t nx = -DM_STICK_MAX, ny = 0;
    stick_deadzone_apply(&nx, &ny, 10);
    assert(nx == -DM_STICK_MAX);
}
static void t_monotonic(void) {
    int32_t last = -1;
    for (int v = 0; v <= DM_STICK_MAX; v += 97) {
        int32_t x = v, y = 0;
        stick_deadzone_apply(&x, &y, 35);
        assert(x >= last); last = x;
    }
}
int main(void) {
    t_percent(); t_unset_is_unchanged(); t_inside_reads_centred(); t_outside_is_rescaled_and_radial(); t_full_travel_stays_full(); t_monotonic();
    puts("t_stick_deadzone: ok");
    return 0;
}
