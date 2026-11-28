// DM_NVN_ANISO=<2..16>: a floor on the anisotropy of the game's texture samplers (default off: the game's own value, as on the console).
// Only samplers that filter linearly in the texture and across mips take it, the ones that sample surface textures: not a nearest filter
// (lookup tables, pixel art), not a sampler without mips or clamped to one LOD (a screen-space read, where derivatives are isotropic anyway),
// not a depth comparison (shadow maps) or a min / max reduction (depth pyramids), whose results anisotropy would change. A game value above
// the floor stays.
#pragma once
#include <stdlib.h>

// minf: NVN minification filter (0 nearest, 1 linear, 2 / 3 nearest / linear with nearest mip, 4 / 5 with linear mips); magf: 0 nearest, 1 linear
static inline float nvn_aniso_floor(int minf, int magf, int cmp_mode, int reduction, float lod_min, float lod_max, float aniso, int floor_) {
    float lo = lod_min < 0.0f ? 0.0f : lod_min;
    if (floor_ <= 1 || magf == 0 || !(minf == 3 || minf == 5) || cmp_mode || reduction || lod_max <= lo) return aniso;
    return aniso > (float)floor_ ? aniso : (float)floor_;
}

// The switch's value: 0 (off) unless it is an exact integer in 2..16; read once
static inline int nvn_aniso_env(const char *e) {
    if (!e || !*e) return 0;
    char *end = NULL;
    long v = strtol(e, &end, 10);
    if (!end || *end != '\0' || v < 2 || v > 16) return 0;
    return (int)v;
}
