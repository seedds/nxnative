// The present: the frame drawn into the drawable, cropped (nvn_mtl.m present_pipeline). One fragment function per route (nvn_mtl.m present_route), chosen from the window's format and
// the drawable's:
//   fs           an 8-bit window into an 8-bit drawable: the stored bytes as they are (the SDR present)
//   fs_hdr_itm   an 8-bit window into the RGBA16Float EDR drawable: sRGB decode, then BT.2446 Method A, a target luminance never below SDR's, applied in ICtCp (1.0 = the screen's SDR white)
//   fs_pq_linear a PQ (BT.2100) window into the EDR drawable: PQ decode, divided by the paper white vi reports, so that white is EDR 1.0, BT.2020 to BT.709, clipped at 0
//   fs_pq_to_sdr a PQ window into an 8-bit drawable or capture: PQ decode, / paper white, BT.2020 to BT.709, clipped to 1, sRGB encode
// The inverse tone map and the PQ curves are ported from mtld3d (unix/unix/src/metal/present.msl); P_SDR follows BT.2446's formula for 100 nits (mtld3d's 5.4395 is 87 nits)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
// 16 floats (nvn_mtl.m present_uniforms): the crop's rectangle and its clamped edges in texture coordinates; BT.2446's terms for the peak L_HDR in nits (mtld3d hdr_uniforms:
// L_HDR, p_hdr = 1 + 32 (L_HDR / 10000)^(1/2.4), log2 p_hdr, 1 / (p_hdr - 1)); x: the paper white W in nits, which both HDR routes divide by, then spare
struct Crop { float4 rect; float4 edge; float4 itm; float4 pq; };
static float4 src_sample(V in, texture2d<float> t, sampler s, constant Crop& c) { return t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)); }
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) { return src_sample(in, t, s, c); }

// BT.2446-A's SDR white is L_SDR = 100 nits (input 1.0)
constant float P_SDR = 5.6969577f;   // 1 + 32 * (L_SDR / 10000)^(1/2.4)
constant float LOG_P_SDR = 1.7399323f;   // log(P_SDR)
static float3 srgb_eotf(float3 e) { return select(pow((e + 0.055f) / 1.055f, float3(2.4f)), e / 12.92f, e <= float3(0.04045f)); }
static float3 srgb_oetf(float3 l) { return select(1.055f * pow(l, float3(1.0f / 2.4f)) - 0.055f, l * 12.92f, l <= float3(0.0031308f)); }
// BT.709 linear -> LMS (through BT.2020) and LMS-PQ -> ICtCp (BT.2100-2), column-major (each spec row is a column here), and their exact inverses (mtld3d's LMS -> BT.709
// is off by 7e-4, which leaves a pixel kept at its SDR luminance 0.03 % darker)
constant float3x3 M_BT709_TO_LMS = float3x3(float3(0.2958197875977f, 0.1562652587891f, 0.0351760864258f), float3(0.6230921020508f, 0.7272338867188f, 0.1564789062500f),
                                            float3(0.0810847778320f, 0.1164960937500f, 0.8083459472656f));
constant float3x3 M_LMS_TO_BT709 = float3x3(float3(6.1740117436f, -1.3246879448f, -0.0122371492f), float3(-5.3216416751f, 2.5608829838f, -0.2641562583f),
                                            float3(0.1476250327f, -0.2361870399f, 1.2763909094f));
constant float3x3 M_LMS_PQ_TO_ICTCP = float3x3(float3(0.5f, 1.61370003f, 4.37806224f), float3(0.5f, -3.32339620f, -4.24553966f), float3(0.0f, 1.70969617f, -0.13252264f));
constant float3x3 M_ICTCP_TO_LMS_PQ = float3x3(float3(1.0000000067f, 0.9999999933f, 0.9999999808f), float3(0.0086064749f, -0.0086064749f, 0.5600462947f),
                                               float3(0.1110335352f, -0.1110335352f, -0.3206319682f));
// BT.2020 linear -> BT.709 linear (BT.2087), column-major
constant float3x3 M_BT2020_TO_BT709 = float3x3(float3(1.6604910f, -0.1245505f, -0.0181508f), float3(-0.5876411f, 1.1328999f, -0.1005789f), float3(-0.0728499f, -0.0083494f, 1.1187297f));
// SMPTE ST 2084: nits (0..10000) <-> PQ code (0..1)
constant float PQ_M1 = 2610.0f / 16384.0f, PQ_M2 = 2523.0f / 4096.0f * 128.0f, PQ_C1 = 3424.0f / 4096.0f, PQ_C2 = 2413.0f / 4096.0f * 32.0f, PQ_C3 = 2392.0f / 4096.0f * 32.0f;
static float3 pq_oetf(float3 nits) { float3 y = pow(max(nits, 0.0f) / 10000.0f, float3(PQ_M1)); return pow((PQ_C1 + PQ_C2 * y) / (1.0f + PQ_C3 * y), float3(PQ_M2)); }
static float3 pq_eotf(float3 pq) {
    float3 e = pow(max(pq, 0.0f), float3(1.0f / PQ_M2));
    return 10000.0f * pow(max(e - PQ_C1, 0.0f) / max(PQ_C2 - PQ_C3 * e, 1e-20f), float3(1.0f / PQ_M1));
}
// BT.2446 Method A: the luminance of an SDR pixel (1.0 = L_SDR) to HDR nits for the peak u.x = L_HDR (u: L_HDR, p_hdr, log2 p_hdr, 1 / (p_hdr - 1))
static float bt2446_a(float y_sdr, float4 u) {
    float yp_c = log(pow(y_sdr, 1.0f / 2.4f) * (P_SDR - 1.0f) + 1.0f) / LOG_P_SDR;
    float yp_0 = yp_c / 1.0770f, yp_1 = (-2.7811f + sqrt(max(4.83307641f - 4.604f * yp_c, 0.0f))) / -2.302f, yp_2 = (yp_c - 0.5f) / 0.5f;
    float yp_p = yp_0 <= 0.7399f ? yp_0 : yp_2 >= 0.9909f ? yp_2 : yp_1;
    return pow((exp2(yp_p * u.z) - 1.0f) * u.w, 2.4f) * u.x;
}
// The inverse tone map of an 8-bit window: Method A takes the SDR picture as L_SDR = 100 nits, and its output in nits is divided by the paper white W (c.pq.x), so EDR 1.0 is
// the screen's SDR white on both HDR routes. L_HDR (c.itm.x, nvn_mtl.m hdr_itm_lhdr) is the headroom x W within PQ's range (10000 nits), so white shows at the headroom.
// Where Method A is darker than SDR (L_HDR below about 1000), each pixel takes a smooth maximum of both luminances in ln luminance, max(a, b) + k h^2 / 4 with
// h = max(k - |a - b|, 0) / k and width k = min(ITM_SMOOTH, L_HDR / W - 1): never below either curve, its slope continuous where they cross, and the SDR picture itself at a
// headroom of 1. The new luminance scales the pixel's I, Ct and Cp (taken at W nits a unit) by one ratio, PQ(Y_out) / PQ(Y x W), which keeps its ICtCp hue angle; a strong
// primary can leave the gamut and is clipped at 0. The matrices assume BT.709 primaries, shown in the screen's colour space as the SDR present shows its bytes. Alpha is 1:
// the Switch's scan-out ignores the window's alpha. The numbers behind ITM_SMOOTH and the ranges: docs/reference/rendering.md, HDR output
constant float ITM_SMOOTH = 0.12f;   // ln luminance: under the smallest gap of Method A over SDR at L_HDR 1000 for an 8-bit grey code (0.134 at code 1; wider above 1000)
fragment float4 fs_hdr_itm(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) {
    float3 lin = srgb_eotf(saturate(src_sample(in, t, s, c).rgb));
    float w = c.pq.x, y = max(dot(lin, float3(0.2126f, 0.7152f, 0.0722f)), 1e-20f), ys = y * w, ya = max(bt2446_a(y, c.itm), 1e-30f);
    float k = clamp(c.itm.x / w - 1.0f, 0.0f, ITM_SMOOTH), a = log(ys), b = log(ya);
    if (b <= a - k) return float4(lin, 1);
    float h = k > 0.0f ? max(k - abs(a - b), 0.0f) / k : 0.0f, yo = exp(max(a, b) + k * h * h * 0.25f);
    float r = pq_oetf(float3(yo)).x / max(pq_oetf(float3(ys)).x, 1e-9f);
    float3 ictcp = M_LMS_PQ_TO_ICTCP * pq_oetf(M_BT709_TO_LMS * (lin * w));
    return float4(max(M_LMS_TO_BT709 * pq_eotf(M_ICTCP_TO_LMS_PQ * (ictcp * r)), 0.0f) / w, 1);
}
// The PQ window in linear BT.709 values, which the layer (the screen's own colour space, as for fs_hdr_itm) shows in the panel's primaries: the same look as the SDR present and
// the inverse tone map, which show 8-bit values that way. A colour outside BT.709 is clipped at 0
fragment float4 fs_pq_linear(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) {
    return float4(max(M_BT2020_TO_BT709 * (pq_eotf(src_sample(in, t, s, c).rgb) / c.pq.x), 0.0f), 1);
}
fragment float4 fs_pq_to_sdr(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) {
    return float4(srgb_oetf(saturate(M_BT2020_TO_BT709 * (pq_eotf(src_sample(in, t, s, c).rgb) / c.pq.x))), 1);
}
