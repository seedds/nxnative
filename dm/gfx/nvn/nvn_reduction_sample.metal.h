// Standalone prototype; no game shaders or renderer paths include this yet.
// Only use after validating the sampler contract in nvn_reduction_footprint.h.
#pragma once
#include "nvn_reduction_footprint.h"

inline float4 dm_reduction_sample_lmn_edge(
    texture2d<float, access::sample> tex, float2 uv, float lod,
    float min_lod, float max_lod, bool maximum)
{
    uint mip = dm_reduction_mip_nearest(lod, min_lod, max_lod, tex.get_num_mip_levels());
    dm_reduction_footprint p = dm_reduction_footprint_make(uv.x, uv.y, tex.get_width(mip), tex.get_height(mip));
    float4 value = tex.read(uint2(p.x.lo, p.y.lo), mip);
    if (p.mask & 2u) {
        float4 tap = tex.read(uint2(p.x.hi, p.y.lo), mip);
        value = maximum ? max(value, tap) : min(value, tap);
    }
    if (p.mask & 4u) {
        float4 tap = tex.read(uint2(p.x.lo, p.y.hi), mip);
        value = maximum ? max(value, tap) : min(value, tap);
    }
    if (p.mask & 8u) {
        float4 tap = tex.read(uint2(p.x.hi, p.y.hi), mip);
        value = maximum ? max(value, tap) : min(value, tap);
    }
    return value;
}
