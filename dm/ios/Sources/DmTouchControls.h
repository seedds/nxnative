// Shared touch-control settings and radial stick conversion; no UIKit or game-specific code.
#pragma once
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int swipe; double swipe_gain, left_deadzone, right_fraction; } DmTouchControls;
static inline double dm_touch_number(const char *text, double fallback, double lo, double hi) {
    if (!text || !*text) return fallback;
    char *end; double n = strtod(text, &end);
    if (end == text || !isfinite(n)) return fallback;
    while (isspace((unsigned char)*end)) ++end;
    if (*end) return fallback;
    return fmin(fmax(n, lo), hi);
}
static inline DmTouchControls dm_touch_controls(const char *swipe, const char *gain, const char *deadzone, const char *area) {
    return (DmTouchControls){swipe && !strcmp(swipe, "1"),
        dm_touch_number(gain, 1, 0.25, 4), dm_touch_number(deadzone, 0.10, 0, 0.40),
        area && !strcmp(area, "half") ? 0.5 : 1.0 / 3};
}
static inline int dm_touch_right_area(const DmTouchControls *settings, double x, double width) {
    return width > 0 && x >= width - width * settings->right_fraction;
}
static inline void dm_touch_axes(double dx, double dy, double radius, double deadzone,
                                 int32_t *x, int32_t *y) {
    *x = *y = 0;
    double d = hypot(dx, dy);
    if (!(radius > 0 && d > radius * deadzone)) return;
    double m = fmin((d - radius * deadzone) / (radius * 0.8 - radius * deadzone), 1) / d;
    *x = (int32_t)lrint(dx * m * 32767); *y = (int32_t)lrint(-dy * m * 32767);
}
