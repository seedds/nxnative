// Timestamped relative touch motion -> right-stick velocity; independent of game code.
#pragma once
#include <math.h>
#include <stdint.h>

typedef struct { double x, y, time, vx, vy; } DmSwipeLook;

static inline void dm_swipe_begin(DmSwipeLook *s, double x, double y, double time) {
    *s = (DmSwipeLook){.x = x, .y = y, .time = time};
}
static inline void dm_swipe_move(DmSwipeLook *s, double x, double y, double time) {
    double dt = time - s->time;
    if (!(dt > 0)) return; // ignore repeated/out-of-order samples
    s->vx = dt <= 0.1 ? (x - s->x) / dt : 0;
    s->vy = dt <= 0.1 ? (y - s->y) / dt : 0;
    s->x = x; s->y = y; s->time = time;
}
static inline void dm_swipe_frame(const DmSwipeLook *s, double now, double scale, double gain,
                                  int32_t *rx, int32_t *ry) {
    *rx = *ry = 0;
    double age = now - s->time;
    if (!(age >= 0 && age < 0.040 && scale > 0 && isfinite(gain) && gain > 0)) return;
    // Hold sampled velocity across display callbacks, including callbacks with no new touch.
    // This avoids alternating tilt/neutral when touch, display and game rates differ.
    // Expire within 40 ms of the last sample; release/cancel resets immediately in the view.
    // At 100% sensitivity, full tilt is 180 layout points/second; the game keeps its speed limit.
    double x = s->vx / (180.0 * scale), y = -s->vy / (180.0 * scale);
    double magnitude = hypot(x, y);
    if (magnitude > 1) { x /= magnitude; y /= magnitude; }
    // Apply sensitivity after normalizing the motion command. Before normalization,
    // a fast swipe could saturate even at 25%, making it indistinguishable from 400%.
    // Below 100% this also scales the maximum command; above it the game still caps speed.
    x *= gain; y *= gain;
    magnitude = hypot(x, y);
    if (magnitude > 1) { x /= magnitude; y /= magnitude; }
    *rx = (int32_t)lrint(x * 32767); *ry = (int32_t)lrint(y * 32767);
}
