// The mouse as a stick (mouse_stick.h): the settings and their defaults, movement and its clamp to the unit circle, the Y axis, recentering and its half-life,
// a stick held in place, the button names, the Switch units and merging with the keyboard
#include "../mouse_stick.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
#define NEAR(a, b) (fabs((a) - (b)) < 1e-6)

int main(void) {
    mouse_stick m;
    CHECK(ms_config(&m, NULL, NULL, NULL, NULL) == MS_OFF, "unset: off");
    CHECK(ms_config(&m, "0", NULL, NULL, NULL) == MS_OFF, "an unknown value: off");
    CHECK(ms_config(&m, "off", NULL, NULL, NULL) == MS_OFF, "off");
    CHECK(ms_config(&m, "Right", NULL, NULL, NULL) == MS_RIGHT, "right, any case");
    CHECK(m.btn_left == 1ull << 9 && m.btn_right == 1ull << 8, "right: ZR and ZL");
    CHECK(ms_config(&m, "left", NULL, NULL, NULL) == MS_LEFT, "left");
    CHECK(m.btn_left == 1ull << 0 && m.btn_right == 1ull << 1, "left: A and B");
    CHECK(NEAR(m.sens, MS_SENS_BASE) && NEAR(m.half_life, MS_HALF_LIFE_MS / 1000), "the defaults");

    ms_move(&m, 50, 0);
    CHECK(NEAR(m.x, 0.5) && NEAR(m.y, 0), "50 points right: half way");
    ms_move(&m, 0, 20);
    CHECK(NEAR(m.y, -0.2), "the mouse moving down pushes the stick down (AppKit's deltaY is down)");
    ms_move(&m, 1000, -1000);
    CHECK(NEAR(hypot(m.x, m.y), 1), "clamped to the unit circle");
    CHECK(m.x > 0 && m.y > 0, "and keeps its direction");

    ms_center(&m); ms_move(&m, 100, 0);
    CHECK(ms_tick(&m, 10.0) && NEAR(m.x, 1), "the first tick only starts the clock");
    ms_tick(&m, 10.0 + m.half_life);
    CHECK(NEAR(m.x, 0.5), "one half-life later: half");
    CHECK(!ms_tick(&m, 20.0) && m.x == 0, "long after: centred, nothing more to publish");

    ms_config(&m, "left", "2", "0", NULL);
    CHECK(NEAR(m.sens, 2 * MS_SENS_BASE) && m.half_life == 0, "sensitivity 2, no recentering");
    ms_move(&m, 25, 0); ms_tick(&m, 1); ms_tick(&m, 100);
    CHECK(NEAR(m.x, 0.5), "a stick held in place stays");
    ms_config(&m, "left", "-1", "x", NULL);
    CHECK(NEAR(m.sens, MS_SENS_BASE) && NEAR(m.half_life, MS_HALF_LIFE_MS / 1000), "a negative sensitivity and a value that is no number: the defaults");
    ms_config(&m, "left", "0", "-5", NULL);
    CHECK(NEAR(m.sens, MS_SENS_BASE) && NEAR(m.half_life, MS_HALF_LIFE_MS / 1000), "zero sensitivity and a negative half-life: the defaults");

    ms_config(&m, "left", NULL, NULL, "zr,plus");
    CHECK(m.btn_left == 1ull << 9 && m.btn_right == 1ull << 10, "names, any case");
    ms_config(&m, "left", NULL, NULL, "Y");
    CHECK(m.btn_left == 1ull << 3 && m.btn_right == 1ull << 1, "only the left one given: the right keeps its default");
    ms_config(&m, "left", NULL, NULL, ",");
    CHECK(m.btn_left == 0 && m.btn_right == 0, "empty names: the buttons do nothing");
    ms_config(&m, "left", NULL, NULL, "Q,R");
    CHECK(m.btn_left == 1ull << 0 && m.btn_right == 1ull << 7, "an unknown name keeps the default");
    CHECK(ms_key_bit("ZL", 1) == 0, "a cut name is no name");

    CHECK(ms_axis(1) == 32767 && ms_axis(-2) == -32767 && ms_axis(0) == 0, "Switch units, clamped");
    CHECK(ms_merge(32767, -100) == 32767 && ms_merge(100, -32767) == -32767 && ms_merge(0, 5) == 5, "the more deflected source wins");

    printf("t_mouse_stick: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
