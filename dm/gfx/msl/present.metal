// The present: the frame drawn into the drawable, cropped (nvn_mtl.m present_pipeline)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
struct Crop { float4 rect; float4 edge; };
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) { return t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)); }

// HDR (DM_HDR, nvn/hdr.h): the game's HDR window texture is HDR10, RGB10A2 holding PQ (SMPTE ST 2084) code values of BT.2020 colours. hdr.x = 10000 / paper white (nits), so
// paper white is 1.0, the screen's SDR white; hdr.y = the screen's current EDR headroom. The constants and the matrix are hdr.h's
struct CropHdr { float4 rect; float4 edge; float4 hdr; };
static float3 pq_eotf(float3 e) {   // code value → nits / 10000
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0, c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
    float3 p = pow(clamp(e, 0.0, 1.0), 1.0 / m2);
    return pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}
static float3 bt2020_to_709(float3 c) {   // linear; rows of hdr.h's HDR_BT2020_TO_709
    return float3(dot(float3(1.660491, -0.587641, -0.072850), c), dot(float3(-0.124550, 1.132900, -0.008349), c), dot(float3(-0.018151, -0.100579, 1.118730), c));
}
static float3 pq_linear(V in, texture2d<float> t, sampler s, constant CropHdr& c) {   // linear BT.709, paper white = 1
    return bt2020_to_709(pq_eotf(t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)).rgb) * c.hdr.x);
}
static float3 srgb_encode(float3 c) { c = clamp(c, 0.0, 1.0); return select(1.055 * pow(c, 1.0 / 2.4) - 0.055, 12.92 * c, c <= 0.0031308); }
static float3 srgb_decode(float3 c) { return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045); }
// PQ → an extended linear sRGB drawable (RGBA16Float, kCGColorSpaceExtendedLinearSRGB, wantsExtendedDynamicRangeContent): up to the current headroom. Values below 0 (colours outside
// BT.709) are kept, as extended sRGB carries them
fragment float4 fs_edr(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant CropHdr& c [[buffer(0)]]) {
    return float4(min(pq_linear(in, t, s, c), c.hdr.y), 1);
}
// PQ → an 8-bit SDR target (the screenshots, and a drawable still in the SDR format while the layer switches): linear up to 0.8 of paper white, then an exponential shoulder
// 1 − 0.2·e^(−(m − 0.8)/0.2) on the largest channel m (hue kept; continuous with slope 1 at the knee): paper white 0.93 (sRGB 247), 1.5× paper white 0.99, brighter nears 1.
// Then the sRGB curve. hdr.h's hdr_sdr_shoulder is the same curve
fragment float4 fs_pq_sdr(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant CropHdr& c [[buffer(0)]]) {
    float3 l = max(pq_linear(in, t, s, c), 0.0); float m = max(max(l.r, l.g), l.b);
    if (m > 0.8) l *= (1 - 0.2 * exp(-(m - 0.8) / 0.2)) / m;
    return float4(srgb_encode(l), 1);
}
// An SDR picture (the game's gamma bytes) into the extended linear drawable, while the layer switches back: decoded, so it shows as on the SDR layer
fragment float4 fs_sdr_lin(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant CropHdr& c [[buffer(0)]]) {
    return float4(srgb_decode(t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)).rgb), 1);
}
