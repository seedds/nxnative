// The touch controls' settings, the sticks' radial response and the swipe camera (DmTouchControls.h, DmSwipeLook.h),
// natively and without UIKit, and the floating left stick's centre. Run by dm/tools/tests/test_touch_controls.py.
#include <stdio.h>
#include <stdlib.h>
#include "../Sources/DmSwipeLook.h"
#include "../Sources/DmTouchControls.h"

static int g_failed;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); g_failed++; } } while (0)

static const int32_t THIRD = DM_STICK_MAX / 3 * 2;   // 120 points/s against the 180 of full tilt

// Touch samples at touchHz, display frames at displayHz with a phase: every frame after the first sample holds the same tilt
static void t_sampling(int touchHz, int displayHz, double phase) {
    DmSwipeLook s; int32_t x, y; dm_swipe_begin(&s, 0, 0, 0);
    int event = 0;
    for (int frame = 1; frame <= displayHz; frame++) {
        double now = (frame + phase) / displayHz;
        while ((event + 1.0) / touchHz <= now) { event++; double t = (double)event / touchHz; dm_swipe_move(&s, 120 * t, 0, t); }
        dm_swipe_frame(&s, now, 1, 1, &x, &y);
        if (event) CHECK(abs(x - THIRD) <= 1 && y == 0, "a steady swipe gives the same tilt on every frame");
    }
}

static void t_swipe(void) {
    DmSwipeLook s; int32_t x, y;
    dm_swipe_begin(&s, 100, 200, 1);
    dm_swipe_frame(&s, 1, 1, 1, &x, &y); CHECK(x == 0 && y == 0, "no tilt before the finger moves");
    dm_swipe_move(&s, 102, 200, 1 + 1.0 / 60);
    dm_swipe_frame(&s, 1 + 1.0 / 60 + 1.0 / 120, 1, 1, &x, &y); CHECK(abs(x - THIRD) <= 1 && y == 0, "a frame between samples keeps the tilt");
    dm_swipe_frame(&s, 1 + 1.0 / 60 + 0.041, 1, 1, &x, &y); CHECK(x == 0 && y == 0, "a finger at rest stops turning after 40 ms");
    dm_swipe_move(&s, 101, 199, 1 + 2.0 / 60); dm_swipe_frame(&s, 1 + 2.0 / 60, 1, 1, &x, &y); CHECK(x < 0 && y > 0, "a reversal turns at once");
    dm_swipe_move(&s, 1100, 1000, 2); dm_swipe_frame(&s, 2, 1, 1, &x, &y); CHECK(x == 0 && y == 0, "a move after a stall waits for the next sample");
    dm_swipe_begin(&s, 0, 0, 3); dm_swipe_move(&s, 100, 100, 3); dm_swipe_frame(&s, 3, 1, 1, &x, &y); CHECK(x == 0 && y == 0, "a sample at the same time is ignored");
    for (int t = 30; t <= 120; t *= 2) for (int d = 30; d <= 120; d *= 2) { t_sampling(t, d, 0); t_sampling(t, d, 0.3); t_sampling(t, d, 0.7); }
}

static void t_gain(void) {
    DmSwipeLook s; int32_t x, y, normal;
    dm_swipe_begin(&s, 0, 0, 4); dm_swipe_move(&s, 1, 0, 4 + 1.0 / 60);
    dm_swipe_frame(&s, 4 + 1.0 / 60, 1, 1, &normal, &y);
    dm_swipe_frame(&s, 4 + 1.0 / 60, 1, 2, &x, &y); CHECK(abs(x - 2 * normal) <= 1, "200 % doubles a slow swipe");
    dm_swipe_begin(&s, 0, 0, 5); dm_swipe_move(&s, 60, 0, 5 + 1.0 / 60);   // 3600 points/s, far past full tilt
    dm_swipe_frame(&s, 5 + 1.0 / 60, 1, 0.25, &x, &y); CHECK(abs(x - DM_STICK_MAX / 4) <= 1, "25 % keeps a fast swipe at a quarter tilt");
    dm_swipe_frame(&s, 5 + 1.0 / 60, 1, 4, &x, &y); CHECK(x == DM_STICK_MAX, "400 % saturates");
    dm_swipe_begin(&s, 0, 0, 6); dm_swipe_move(&s, 60, 60, 6 + 1.0 / 60);
    dm_swipe_frame(&s, 6 + 1.0 / 60, 1, 0.25, &x, &y); CHECK(x > 0 && y < 0 && hypot(x, y) <= DM_STICK_MAX / 4 + 1, "a diagonal is capped by length");
}

static void t_settings(void) {
    DmTouchControls d = dm_touch_controls(NULL, NULL, NULL, NULL, NULL);
    CHECK(!d.swipe && !d.left_float && d.swipe_gain == 1 && d.left_deadzone == DM_TOUCH_DEAD && d.right_fraction == 1.0 / 3, "unset gives today's behaviour");
    DmTouchControls c = dm_touch_controls("1", "2.5", "0.25", "half", NULL);
    CHECK(c.swipe && c.swipe_gain == 2.5 && c.left_deadzone == 0.25 && c.right_fraction == 0.5, "chosen values are read");
    const char *bad[] = {"", "nan", "inf", "1.0junk", "--1", "1e999"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        DmTouchControls b = dm_touch_controls("0", bad[i], bad[i], bad[i], NULL);
        CHECK(b.swipe_gain == 1 && b.left_deadzone == DM_TOUCH_DEAD && b.right_fraction == 1.0 / 3, "an invalid value gives the default");
    }
    c = dm_touch_controls(NULL, "100", "-1", NULL, NULL); CHECK(c.swipe_gain == 4 && c.left_deadzone == 0, "values above or below the range are clamped");
    c = dm_touch_controls(NULL, "-1", "100", NULL, NULL); CHECK(c.swipe_gain == 0.25 && c.left_deadzone == 0.4, "and the other way");
}

static void t_axes(void) {
    int32_t x, y;
    for (int dx = -100; dx <= 100; dx += 5) for (int dy = -100; dy <= 100; dy += 5) {   // the response before this change, at the default dead zone
        double d = hypot(dx, dy), dz = 77 * DM_TOUCH_DEAD, m = d <= dz ? 0 : fmin((d - dz) / (77 * DM_TOUCH_FULL - dz), 1) / d;
        dm_touch_axes(dx, dy, 77, DM_TOUCH_DEAD, &x, &y);
        CHECK(abs(x - (int32_t)lrint(dx * m * DM_STICK_MAX)) <= 1 && abs(y - (int32_t)lrint(-dy * m * DM_STICK_MAX)) <= 1, "the default dead zone keeps the old response");
    }
    dm_touch_axes(10, 0, 77, 0.25, &x, &y); CHECK(x == 0 && y == 0, "a larger dead zone ignores a small push");
    dm_touch_axes(-60, 0, 77, 0.25, &x, &y); CHECK(x < 0 && y == 0, "and still takes a large one");
    dm_touch_axes(60, -60, 77, 0.4, &x, &y); CHECK(x > 0 && y > 0 && hypot(x, y) <= DM_STICK_MAX + 1, "full tilt stays within the stick's range");
}

static void t_float(void) {
    CHECK(dm_touch_controls(NULL, NULL, NULL, NULL, "1").left_float && !dm_touch_controls(NULL, NULL, NULL, NULL, "0").left_float, "the floating stick is opt-in");
    double cx = 100, cy = 100; int32_t x, y;
    dm_touch_follow(&cx, &cy, 150, 100, 77); CHECK(cx == 100 && cy == 100, "inside the ring the centre stays");
    dm_touch_follow(&cx, &cy, 277, 100, 77); CHECK(cx == 200 && cy == 100, "past the ring the centre keeps the thumb on the rim");
    dm_touch_axes(277 - cx, 100 - cy, 77, DM_TOUCH_DEAD, &x, &y); CHECK(x == DM_STICK_MAX && y == 0, "and the stick stays at full tilt");
    dm_touch_follow(&cx, &cy, 190, 100, 77); dm_touch_axes(190 - cx, 100 - cy, 77, DM_TOUCH_DEAD, &x, &y);
    CHECK(cx == 200 && x < 0 && y == 0, "a reversal turns the axis at once");
    cx = 0; cy = 0; dm_touch_follow(&cx, &cy, 300, 400, 100); CHECK(fabs(cx - 240) < 1e-9 && fabs(cy - 320) < 1e-9, "a diagonal follows along the line to the thumb");
}

int main(void) {
    t_swipe(); t_gain(); t_settings(); t_axes(); t_float();
    printf("touch controls: %s\n", g_failed ? "FAIL" : "PASS");
    return g_failed != 0;
}
