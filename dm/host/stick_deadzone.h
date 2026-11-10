// A radial deadzone for each stick of a physical controller, set by the front end: DM_STICK_DEADZONE_L and DM_STICK_DEADZONE_R,
// each a whole percent of the stick's travel from 0 to 50. Unset (or invalid, or 0) leaves the stick exactly as the controller reports it.
// Inside the deadzone the stick reads centred, outside it the remaining travel is rescaled to the full range, so a stick pushed all the way
// still reads full. A square-range diagonal beyond a full radius is left alone, as steam_deadzone does (steam_input.h).
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include "dm_input.h"

// The percent a switch's text means: 1..50, or 0 for "leave the stick as reported" (unset, empty, not a whole number, above 50).
static inline int stick_deadzone_percent(const char *text) {
    if (!text || !*text) return 0;
    char *end; long v = strtol(text, &end, 10);
    return end != text && !*end && v >= 1 && v <= 50 ? (int)v : 0;
}

// x and y in -DM_STICK_MAX..DM_STICK_MAX, percent as stick_deadzone_percent returns it.
static inline void stick_deadzone_apply(int32_t *x, int32_t *y, int percent) {
    if (percent <= 0) return;
    float dz = percent / 100.0f, fx = *x / (float)DM_STICK_MAX, fy = *y / (float)DM_STICK_MAX, radius = hypotf(fx, fy);
    if (radius <= dz) { *x = *y = 0; return; }
    if (radius >= 1) return;
    float scale = (radius - dz) / ((1 - dz) * radius);
    *x = (int32_t)lrintf(fmaxf(-1, fminf(1, fx * scale)) * DM_STICK_MAX);
    *y = (int32_t)lrintf(fmaxf(-1, fminf(1, fy * scale)) * DM_STICK_MAX);
}
