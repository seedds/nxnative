// DM_NVN_VIS_SCALE=<f> (0 < f < 1; a diagnostic): the samples-passed totals the game reads are scaled by f. A scene the host renders larger than
// the game asked for (an internal-resolution override) passes about (actual pixels / the game's pixels) times the samples the game expects, and a
// game that compares a visible-sample count with the area it projects at its own resolution sees a ratio no console run shows. A count above 0 stays
// at least 1, so a query that saw anything still reads as visible. Off (1) by default.
#pragma once
#include <stdint.h>
#include <stdlib.h>

static inline uint64_t nvn_vis_scaled(uint64_t total, double scale) {
    if (!(scale > 0 && scale < 1) || !total) return total;
    uint64_t v = (uint64_t)((double)total * scale);
    return v ? v : 1;
}

// The switch's value: 1 (off) unless it is a number in (0, 1)
static inline double nvn_vis_scale_env(const char *e) {
    double v = e ? strtod(e, NULL) : 1;
    return v > 0 && v < 1 ? v : 1;
}
