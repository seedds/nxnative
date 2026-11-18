// The player's touch-control settings (DM_TOUCH_SWIPE, _SWIPE_GAIN, _LEFT_DEADZONE, _RIGHT_AREA, _LEFT_FLOAT) and the touch
// sticks' radial response. No UIKit, so tests/t_touch_controls.c runs it natively.
#pragma once
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dm_input.h"

#define DM_TOUCH_FULL 0.8          // full tilt at this fraction of the ring radius (the knob still travels to the ring edge)
#define DM_TOUCH_DEAD 0.1          // the sticks' dead zone (times the ring radius); the left one follows DM_TOUCH_LEFT_DEADZONE
#define DM_TOUCH_FLOAT_GRAB 1.65   // floating left stick: a touch-down this many ring radii from its home grabs it (fixed stick: 1.4)

typedef struct { int swipe, left_float; double swipe_gain, left_deadzone, right_fraction; } DmTouchControls;

// A number within [lo, hi]; unset, empty, non-numeric or trailing text gives the fallback
static inline double dm_touch_number(const char *text, double fallback, double lo, double hi) {
    if (!text || !*text) return fallback;
    char *end; double n = strtod(text, &end);
    if (end == text || !isfinite(n)) return fallback;
    while (isspace((unsigned char)*end)) ++end;
    if (*end) return fallback;
    return fmin(fmax(n, lo), hi);
}
static inline DmTouchControls dm_touch_controls(const char *swipe, const char *gain, const char *deadzone, const char *area, const char *left_float) {
    return (DmTouchControls){swipe && atoi(swipe), left_float && atoi(left_float), dm_touch_number(gain, 1, 0.25, 4),
                             dm_touch_number(deadzone, DM_TOUCH_DEAD, 0, 0.4), area && !strcmp(area, "half") ? 0.5 : 1.0 / 3};
}
// Stick axes from the knob's offset (dx, dy, points, y down): nothing inside the dead zone, then rising to full tilt at DM_TOUCH_FULL
static inline void dm_touch_axes(double dx, double dy, double radius, double deadzone, int32_t *x, int32_t *y) {
    *x = *y = 0;
    double d = hypot(dx, dy), dz = radius * deadzone;
    if (!(radius > 0 && d > dz)) return;
    double m = fmin((d - dz) / (radius * DM_TOUCH_FULL - dz), 1) / d;
    *x = (int32_t)lrint(dx * m * DM_STICK_MAX); *y = (int32_t)lrint(-dy * m * DM_STICK_MAX);   // host: up is positive
}
// A floating stick's centre follows a finger past its radius just far enough to keep the finger on the rim, so a reversal responds at once
static inline void dm_touch_follow(double *cx, double *cy, double px, double py, double radius) {
    double dx = px - *cx, dy = py - *cy, d = hypot(dx, dy);
    if (!(radius > 0 && d > radius)) return;
    *cx = px - dx * radius / d; *cy = py - dy * radius / d;
}
