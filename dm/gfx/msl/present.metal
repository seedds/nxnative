// The present: the frame drawn into the drawable, cropped (nvn_mtl.m present_pipeline)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
struct Crop { float4 rect; float4 edge; };
// Auto-HDR (nvn_mtl.m present_pipeline, front_mac.m hdr_apply): on a screen with HDR on, the drawable is extended linear sRGB in half floats, where 1.0 is SDR white and the
// screen shows up to its headroom above it. The game's picture is SDR (sRGB bytes, read raw): decoded to linear, it stays as it is up to HDR_KNEE of white and its highlights
// are lifted from there to hdr_peak at white, along a curve that starts with the picture's own slope (no step at the knee), scaling the colour by its luminance (no hue shift)
constant float hdr_peak [[function_constant(0)]];
constant bool hdr_on = is_function_constant_defined(hdr_peak);
constant float HDR_KNEE = 0.55;
static float3 srgb_linear(float3 c) { return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045); }
static float3 hdr_lift(float3 l) {
    float y = dot(l, float3(0.2126, 0.7152, 0.0722));
    if (y <= HDR_KNEE) return l;
    float t = min((y - HDR_KNEE) / (1.0 - HDR_KNEE), 1.0);
    float y2 = HDR_KNEE + (1.0 - HDR_KNEE) * t + (hdr_peak - 1.0) * t * t;
    return l * (y2 / y);
}
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) {
    float4 px = t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw));
    if (hdr_on) return float4(hdr_lift(srgb_linear(saturate(px.rgb))), 1.0);
    return px;
}
