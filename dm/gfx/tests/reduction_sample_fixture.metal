// Asset-free compile/readback fixture. The host supplies synthetic R32Float
// mip levels and query records; this file never embeds a game's shader source.
#include <metal_stdlib>
using namespace metal;
#include "../nvn/nvn_reduction_sample.metal.h"

struct ReductionQuery { float2 uv; float lod; uint maximum; };
kernel void reduction_sample_fixture(
    texture2d<float, access::sample> tex [[texture(0)]],
    device const ReductionQuery *queries [[buffer(0)]],
    device float4 *output [[buffer(1)]], uint gid [[thread_position_in_grid]])
{
    ReductionQuery q = queries[gid];
    output[gid] = dm_reduction_sample_lmn_edge(tex, q.uv, q.lod, 0.0f, 1000.0f, q.maximum != 0);
}
