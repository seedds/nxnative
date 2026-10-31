// The present: the frame drawn into the drawable, cropped (nvn_mtl.m present_pipeline)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
struct Crop { float4 rect; float4 edge; };
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) { return t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)); }
// the bars' fill (DM_BAR_FILL=blur, present_encode_m4): a 9-tap Gaussian along x (fs_blurh) or y (fs_blurv) with taps two texels apart, and the texture darkened to half (fs_dim); they take no constants
float4 blur9(texture2d<float> t, sampler s, float2 uv, float2 d) { float4 r = t.sample(s, uv) * 0.227027027; r += (t.sample(s, uv + d * 1.3846153846) + t.sample(s, uv - d * 1.3846153846)) * 0.3162162162; r += (t.sample(s, uv + d * 3.2307692308) + t.sample(s, uv - d * 3.2307692308)) * 0.0702702703; return float4(r.rgb, 1); }
fragment float4 fs_blurh(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return blur9(t, s, in.uv, float2(2.0 / t.get_width(), 0)); }
fragment float4 fs_blurv(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return blur9(t, s, in.uv, float2(0, 2.0 / t.get_height())); }
fragment float4 fs_dim(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return float4(t.sample(s, in.uv).rgb * 0.5, 1); }
