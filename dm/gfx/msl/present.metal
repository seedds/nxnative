// The present: the frame drawn into the drawable, cropped (nvn_mtl.m present_pipeline). hdr.x (present_mode): 1 = an 8-bit picture, sRGB-encoded bytes as the Switch scans them
// out, into an extended-range (linear) target, else 0; hdr.y: add texture 1's highlights (HDR output, nvn_mtl.m snap_excess), read at in.uv * ex.zw + ex.xy
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
struct Crop { float4 rect; float4 edge; float4 hdr; float4 ex; };
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], texture2d<float> x [[texture(1)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) {
    float4 v = t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw));
    if (c.hdr.x == 1) v.rgb = select(pow((v.rgb + 0.055) / 1.055, float3(2.4)), v.rgb / 12.92, v.rgb <= 0.04045);
    if (c.hdr.y > 0) v.rgb += x.sample(s, in.uv * c.ex.zw + c.ex.xy).rgb;
    return v;
}
