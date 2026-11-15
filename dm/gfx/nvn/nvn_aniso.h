// The anisotropy a sampler is made with (pure function, unit test tests/t_aniso.c). A game sets each sampler's own, 1 to 16; the player's setting (DM_ANISOTROPY: 2, 4, 8 or
// 16; 0 = the game's) raises it for the samplers that filter a mip-mapped colour texture, and never lowers one:
//   - linear magnification, linear minification and linear filtering between mip levels (NVN's LINEAR_MIPMAP_LINEAR, 5), or an anisotropy of the game's own above 1: what a
//     game gives the surface textures of its scene. A sampler that picks one mip level, takes the nearest texel or has no mip filter is a lookup (a dither pattern, a
//     table, a screen-sized target read 1:1), where more samples would change values the shader relies on
//   - no depth comparison (a shadow map's sampler) and no min / max reduction (a depth pyramid's)
//   - mip levels in reach: a LOD clamp that ends at the base level keeps a sampler on one level
#pragma once
#include <stdlib.h>
#include <string.h>
#include "nvn_obj.h"

// Whether a sampler is one of a scene's surface textures: what the two settings below act on (see the rules at the top)
static inline int nvn_surface_sampler(const samp_par *p) {
    unsigned own = p->aniso < 1 ? 1 : p->aniso > 16 ? 16 : (unsigned)p->aniso;
    if (p->cmp_mode || p->reduction || !(p->lod_max > 0)) return 0;
    return own > 1 || (p->magf && p->minf == 5);
}

static inline unsigned nvn_aniso(const samp_par *p, unsigned forced) {
    unsigned own = p->aniso < 1 ? 1 : p->aniso > 16 ? 16 : (unsigned)p->aniso;
    if (forced <= own || !nvn_surface_sampler(p)) return own;
    return forced > 16 ? 16 : forced;
}

// The LOD bias a sampler is made with: the game's own, plus the player's (DM_MIP_BIAS, 0 to -1; negative picks sharper mip levels) on a surface sampler, clamped to Metal's range
static inline float nvn_lod_bias(const samp_par *p, float bias) {
    float b = p->lod_bias;
    if (bias != 0.0f && nvn_surface_sampler(p)) b += bias;
    return b < -16.0f ? -16.0f : b > 15.99f ? 15.99f : b;
}

// DM_MIP_BIAS's value: 0, -0.25, -0.5, -0.75 or -1 (also written 0.5 for -0.5); 0 for unset, empty or "game"; 1 (a value nothing takes) for anything else
static inline float nvn_mip_bias_setting(const char *e, int *bad) {
    *bad = 0;
    if (!e || !*e || !strcmp(e, "game") || !strcmp(e, "0")) return 0.0f;
    char *end; float v = strtof(e, &end);
    if (end == e || *end) { *bad = 1; return 0.0f; }
    if (v > 0) v = -v;
    if (v < -1.0f || v != (float)((int)(v * 4)) / 4.0f) { *bad = 1; return 0.0f; }
    return v;
}

// DM_ANISOTROPY's value: 2, 4, 8 or 16; 0 for "game", unset or empty (the game's own); -1 for anything else
static inline int nvn_aniso_setting(const char *e) {
    if (!e || !*e || !strcmp(e, "game")) return 0;
    if (!strcmp(e, "2")) return 2;
    if (!strcmp(e, "4")) return 4;
    if (!strcmp(e, "8")) return 8;
    if (!strcmp(e, "16")) return 16;
    return -1;
}
