// Shared C/MSL coordinate math for a restricted software reduction sampler.
// Contract: normalized 2D coordinates, linear min/mag, nearest mip selection,
// CLAMP_TO_EDGE in both axes, no comparison, no anisotropy or LOD bias.
// This header is not connected to the renderer or shader translation chain.
#pragma once
#ifdef __METAL_VERSION__
#define DM_REDUCTION_FLOOR(x) metal::floor(x)
#define DM_REDUCTION_CEIL(x) metal::ceil(x)
#else
#include <math.h>
#define DM_REDUCTION_FLOOR(x) floorf(x)
#define DM_REDUCTION_CEIL(x) ceilf(x)
#endif

typedef struct { unsigned lo, hi; float fraction; } dm_reduction_axis;
typedef struct {
    dm_reduction_axis x, y;
    unsigned mask; // 00, 10, 01, 11: only texels with nonzero bilinear weights
} dm_reduction_footprint;

static inline dm_reduction_axis dm_reduction_axis_make(float uv, unsigned extent) {
    // Clamp before signed conversion: negative/outside/infinite coordinates
    // must never become unsigned out-of-range texture reads. NaN maps to zero,
    // matching the existing translated sample expressions' UV sanitization.
    if (!(uv >= 0.0f)) uv = 0.0f;
    if (uv > 1.0f) uv = 1.0f;
    if (!extent) extent = 1; // valid texture views always have nonzero extents
    float p = uv * (float)extent - 0.5f;
    float base = DM_REDUCTION_FLOOR(p);
    int lo = (int)base, hi = lo + 1, last = (int)extent - 1;
    dm_reduction_axis a;
    a.lo = (unsigned)(lo < 0 ? 0 : lo > last ? last : lo);
    a.hi = (unsigned)(hi < 0 ? 0 : hi > last ? last : hi);
    a.fraction = p - base;
    return a;
}

static inline dm_reduction_footprint dm_reduction_footprint_make(float u, float v, unsigned width, unsigned height) {
    dm_reduction_footprint p;
    p.x = dm_reduction_axis_make(u, width);
    p.y = dm_reduction_axis_make(v, height);
    unsigned x = p.x.fraction > 0.0f, y = p.y.fraction > 0.0f;
    p.mask = 1u | (x ? 2u : 0u) | (y ? 4u : 0u) | (x && y ? 8u : 0u);
    return p;
}

static inline unsigned dm_reduction_mip_nearest(float lod, float min_lod, float max_lod, unsigned mip_count) {
    if (!(lod >= min_lod)) lod = min_lod;
    if (lod > max_lod) lod = max_lod;
    // OpenGL/NVIDIA nearest-mip convention: an exact half tie chooses the lower
    // level. The observed Z-A uses only integer explicit LODs; fractional NVN
    // tie/quantization behavior needs a separate hardware fixture before use.
    float mip = DM_REDUCTION_CEIL(lod + 0.5f) - 1.0f;
    if (!(mip > 0.0f) || mip_count <= 1) return 0;
    if (mip >= (float)(mip_count - 1)) return mip_count - 1;
    return (unsigned)mip;
}

#undef DM_REDUCTION_FLOOR
#undef DM_REDUCTION_CEIL
