// The OpenGL layer's own shaders: a quad that clears what a scissored or masked glClear covers (hle_gl.m)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; };
vertex V clr_vs(uint i [[vertex_id]], constant float &z [[buffer(0)]]) { V o; float2 t = float2((i << 1) & 2, i & 2); o.p = float4(t * 2 - 1, z, 1); return o; }
fragment float4 clr_fs(constant float4 &c [[buffer(0)]]) { return c; }
fragment uint4 clr_fs_uint(constant uint4 &c [[buffer(0)]]) { return c; }
fragment void clr_fs_none() {}

kernel void read_bc_rgba8(texture2d<float> image [[texture(0)]], device uchar4 *out [[buffer(0)]],
                         constant uint4 &region [[buffer(1)]], constant uint &lv [[buffer(2)]],
                         uint2 p [[thread_position_in_grid]]) {
    if (any(p >= region.zw)) return;
    constexpr sampler nearest(filter::nearest);
    float2 uv = (float2(p + region.xy) + 0.5f) / float2(image.get_width(lv), image.get_height(lv));
    float4 c = image.sample(nearest, uv, level(lv));
    out[p.y * region.z + p.x] = uchar4(rint(saturate(c) * 255.0f));
}
