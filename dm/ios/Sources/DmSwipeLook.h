// Swipe camera (DM_TOUCH_SWIPE): finger velocity as right-stick tilt. No UIKit, so tests/t_touch_controls.c runs it natively.
// The velocity is held across display frames (touch, display and game run at different rates) and expires 40 ms after the
// last sample, because UIKit sends none while a finger rests.
#pragma once
#include <math.h>
#include <stdint.h>
#include "dm_input.h"

#define DM_SWIPE_FULL 180.0   // points per second (times the layout scale) for full tilt at 100 %
#define DM_SWIPE_HOLD 0.040   // seconds a sample stays valid

typedef struct { double x, y, time, vx, vy; } DmSwipeLook;

static inline void dm_swipe_begin(DmSwipeLook *s, double x, double y, double time) {
    *s = (DmSwipeLook){.x = x, .y = y, .time = time};
}
static inline void dm_swipe_move(DmSwipeLook *s, double x, double y, double time) {
    double dt = time - s->time;
    if (!(dt > 0)) return;   // a repeated or out-of-order sample
    s->vx = dt <= 0.1 ? (x - s->x) / dt : 0;   // after a stall, wait for the next sample
    s->vy = dt <= 0.1 ? (y - s->y) / dt : 0;
    s->x = x; s->y = y; s->time = time;
}
static inline void dm_swipe_frame(const DmSwipeLook *s, double now, double scale, double gain, int32_t *rx, int32_t *ry) {
    *rx = *ry = 0;
    double age = now - s->time;
    if (!(age >= 0 && age < DM_SWIPE_HOLD && scale > 0)) return;
    double x = s->vx / (DM_SWIPE_FULL * scale), y = -s->vy / (DM_SWIPE_FULL * scale), m = hypot(x, y);
    if (m > 1) { x /= m; y /= m; }
    x *= gain; y *= gain;   // after the clamp, so a fast swipe at 25 % stays a quarter tilt
    m = hypot(x, y);
    if (m > 1) { x /= m; y /= m; }
    *rx = (int32_t)lrint(x * DM_STICK_MAX); *ry = (int32_t)lrint(y * DM_STICK_MAX);
}
