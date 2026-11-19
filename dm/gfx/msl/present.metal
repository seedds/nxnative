// The present: the frame drawn into the drawable, cropped (nvn_mtl.m present_pipeline)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
// mode.x (nvn_mtl.m present_mode) says how the source is encoded: 0 as stored (and the game's HDR10 window for the PQ layer);
// 1 an HDR10 source (the game's RGB10A2 window: SMPTE ST 2084 code values in BT.2020 primaries) for an SDR target (a screenshot), where mode.y is the paper white in nits; 2 a gamma-coded BT.709 source for the PQ layer (RGB10A2 in BT.2100 PQ, DM_HDR): fs encodes it as HDR10 at the paper white;
// 3 a float source (linear BT.709, negative values outside BT.709 as in scRGB) for the PQ layer, encoded the same way (untested: no local game presents a float window); 4 the HDR10 window for the PQ layer without metadata,
// rolled off above mode.z nits into mode.w nits (hle_vi.c dm_hdr_rolloff). Modes 1 and 4 compress with present_shoulder: knee mode.z, target mode.w, and the source's peak xf.x (in nits for 4; in
// paper whites for 1, whose knee and target are 0.75 and 1); xf.y is mode 4's knee as a PQ code value: a pixel whose channels are all at or below it is the window's as it is.
// xf.z is the SDR picture's display gamma for modes 1 and 2 (nvn_mtl.m present_sdr_gamma): the game's 8-bit bytes are sRGB-encoded, but the screens they are made and seen on
// (the Switch's own, a TV) show them through a power curve, 2.2 by default; 0 is the piecewise sRGB curve, which lifts the shadows (code 16 to 2.3 times the light of 2.2).
// The conversions (1, 2, 3) write alpha 1: the layers are opaque, and the PQ layer has 2 bits of alpha. Mode 0 is origin/main's sample, bit for bit
struct Crop { float4 rect; float4 edge; float4 mode; float4 xf; };
// Linear BT.709 and BT.2020 (ITU-R BT.2087; each row sums to 1, so white stays white). Metal's float3x3 takes its columns.
constant float3x3 kRec709To2020 = float3x3(float3(0.627404, 0.069097, 0.016391), float3(0.329283, 0.919540, 0.088013), float3(0.043313, 0.011362, 0.895595));
constant float3x3 kRec2020To709 = float3x3(float3(1.660491, -0.124550, -0.018151), float3(-0.587641, 1.132900, -0.100579), float3(-0.072850, -0.008349, 1.118730));
// An SDR picture's code values to linear light and back: a pure power curve of display gamma g (BT.709 primaries, D65), or the piecewise sRGB curve when g is 0
static inline float3 present_sdr_to_linear(float3 c, float g) {
    if (g > 0.0) return pow(max(c, float3(0.0)), float3(g));
    float3 lo = c * (1.0 / 12.92);
    float3 hi = pow((c + 0.055) * (1.0 / 1.055), float3(2.4));
    return select(hi, lo, c <= 0.04045);
}
static inline float3 present_linear_to_sdr(float3 l, float g) {
    if (g > 0.0) return pow(max(l, float3(0.0)), float3(1.0 / g));
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
// SMPTE ST 2084 inverse EOTF: linear light where 1.0 is the paper white, to code values (10000 nits = 1.0). pow() gets a positive base (the library is built with fast math)
static inline float3 present_linear_to_pq(float3 l, float paper) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 ym = pow(max(l * (paper / 10000.0), float3(1e-30)), float3(m1));
    return pow((c1 + c2 * ym) / (1.0 + c3 * ym), float3(m2));
}
// The brightest channel m compressed above the knee k towards the target t, every channel scaled alike so the hue holds (linear light, in the target's primaries, no negative values).
// A rational shoulder: slope 1 at the knee, rising, and the source's peak s landing exactly on t (s at or below t: values are only clipped at t); one division a pixel
static inline float3 present_shoulder(float3 n, float k, float t, float s) {
    float m = max(max(n.r, n.g), n.b);
    if (!(m > k)) return n;
    float a = t - k, x = m - k, f = min(k + x / (1.0 + x * (1.0 / a - 1.0 / max(s - k, a))), t);
    return n * (f / m);
}
// Every target. The PQ HDR layer (RGB10A2 in BT.2100 PQ): the game's HDR10 window as it is (mode 0) or rolled off (4), an SDR or float source encoded as HDR10 at the paper white
// (modes 2, 3). The SDR drawable (gamma-coded 8-bit, BT.709) and a screenshot: an HDR10 source at its paper white, converted to BT.709 first and then its highlights compressed into
// SDR white with their hue (present_shoulder); compressing in BT.2020 before the conversion turned an orange highlight yellow (its red to green 6.9 became 4.6)
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) {
    float4 v = t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw));
    if (c.mode.x > 3.5) {   // the PQ layer without metadata: the game's HDR10 window, its pixels above the knee (mode.z, nits) compressed into the screen's peak (mode.w)
        if (max(max(v.r, v.g), v.b) > c.xf.y) v.rgb = present_linear_to_pq(present_shoulder(present_pq_to_linear(v.rgb, 1.0), c.mode.z, c.mode.w, c.xf.x), 1.0);
    } else if (c.mode.x > 2.5) {   // the PQ layer: a float source (linear BT.709) as HDR10 at the paper white; converted before negatives are dropped, so scRGB's wider colors keep their BT.2020 value
        v = float4(present_linear_to_pq(max(kRec709To2020 * clamp(v.rgb, -65504.0, 65504.0), 0.0), c.mode.y), 1.0);
    } else if (c.mode.x > 1.5) v = float4(present_linear_to_pq(kRec709To2020 * present_sdr_to_linear(v.rgb, c.xf.z), c.mode.y), 1.0);   // the PQ layer: an SDR source as HDR10 at the paper white
    else if (c.mode.x > 0.5) v = float4(present_linear_to_sdr(saturate(present_shoulder(max(kRec2020To709 * present_pq_to_linear(v.rgb, c.mode.y), 0.0), c.mode.z, c.mode.w, c.xf.x)), c.xf.z), 1.0);
    return v;
}
