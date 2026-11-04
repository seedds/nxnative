// The present: the frame drawn into the drawable, cropped (nvn_mtl.m present_pipeline)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
// mode.x (nvn_mtl.m present_mode) says how the source is encoded: 0 as stored (a float source: linear BT.709); 1 a gamma-coded 8-bit BT.709 (sRGB) source for the extended-range BT.2020 layer (fs_linear converts it);
// 2 an HDR10 source (the game's RGB10A2 window: SMPTE ST 2084 code values in BT.2020 primaries), where mode.y is the paper white in nits.
struct Crop { float4 rect; float4 edge; float4 mode; };
// Linear BT.709 and BT.2020 (ITU-R BT.2087; each row sums to 1, so white stays white). Metal's float3x3 takes its columns.
constant float3x3 kRec709To2020 = float3x3(float3(0.627404, 0.069097, 0.016391), float3(0.329283, 0.919540, 0.088013), float3(0.043313, 0.011362, 0.895595));
constant float3x3 kRec2020To709 = float3x3(float3(1.660491, -0.124550, -0.018151), float3(-0.587641, 1.132900, -0.100579), float3(-0.072850, -0.008349, 1.118730));
static inline float3 present_srgb_to_linear(float3 c) {
    float3 lo = c * (1.0 / 12.92);
    float3 hi = pow((c + 0.055) * (1.0 / 1.055), float3(2.4));
    return select(hi, lo, c <= 0.04045);
}
static inline float3 present_linear_to_srgb(float3 l) {
    float3 lo = l * 12.92;
    float3 hi = 1.055 * pow(l, float3(1.0 / 2.4)) - 0.055;
    return select(hi, lo, l <= 0.0031308);
}
// SMPTE ST 2084 EOTF: code value e to absolute luminance (10000 nits = 1.0), divided by the paper white so that 1.0 is the display's SDR white and values above it are headroom
static inline float3 present_pq_to_linear(float3 e, float paper) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 ep = pow(max(e, float3(0.0)), float3(1.0 / m2));
    float3 num = max(ep - c1, float3(0.0));
    float3 den = max(c2 - c3 * ep, float3(1e-6));
    return 10000.0 * pow(num / den, float3(1.0 / m1)) / paper;
}
// The SDR drawable (gamma-coded 8-bit, BT.709): an HDR10 source at its paper white, converted to BT.709 and clipped at SDR white
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) {
    float4 v = t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw));
    if (c.mode.x > 1.5) v.rgb = present_linear_to_srgb(saturate(kRec2020To709 * present_pq_to_linear(v.rgb, c.mode.y)));
    return v;
}
// Highlights roll off into the screen's current headroom H (Crop.mode.z). Values up to a knee are unchanged; the knee is half the headroom or SDR white (1.0), whichever is higher,
// so the mid-range highlights of a bright headroom keep their values. Above the knee the curve has slope 1 at the knee and tends to H, so no highlight clips to a flat colour.
// It acts on the largest channel and scales all three by the same factor, which keeps the hue
static inline float3 present_shoulder(float3 c0, float H) {
    float3 c = clamp(c0, -65504.0, 65504.0);   // the largest half float: an infinite channel of a float source would give inf * 0 below
    float m = max(max(c.r, c.g), c.b);
    float knee = max(1.0, 0.5 * H);
    if (!(H > knee)) return m > 1.0 ? c * (1.0 / m) : c;   // no headroom (an SDR screen): the largest channel goes to SDR white, the others scale with it, so the hue holds
    if (!(m > knee)) return c;
    float k = H - knee;
    float mm = knee + k * (1.0 - exp(-(m - knee) / k));
    return c * (mm / m);
}
// The HDR layer (RGBA16Float, extended linear BT.2020, DM_HDR=1): linear light, where 1.0 is SDR white and values above it use the screen's headroom
fragment float4 fs_linear(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) {
    float4 v = t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw));
    if (c.mode.x > 1.5) v.rgb = present_pq_to_linear(v.rgb, c.mode.y);                 // HDR10 is already BT.2020
    else if (c.mode.x > 0.5) v.rgb = kRec709To2020 * present_srgb_to_linear(v.rgb);   // a BT.709 (sRGB) source: decoded, converted to BT.2020
    else v.rgb = kRec709To2020 * clamp(v.rgb, -65504.0, 65504.0);                      // a float source as stored: linear BT.709, converted to BT.2020 (untested: no local game presents a float window); finite first, so the matrix cannot make a NaN
    v.rgb = present_shoulder(v.rgb, c.mode.z);
    return v;
}
