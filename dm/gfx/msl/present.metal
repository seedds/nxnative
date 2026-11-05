// The present: cropped scanout to the drawable, with explicit HDR transfer conversion.
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
struct Crop { float4 rect; float4 edge; float4 color; };
static float3 srgb_decode(float3 v) { return select(v / 12.92f, pow((v + 0.055f) / 1.055f, float3(2.4f)), v > 0.04045f); }
static float3 srgb_encode(float3 v) { v = max(v, 0.0f); return select(v * 12.92f, 1.055f * pow(v, float3(1.0f / 2.4f)) - 0.055f, v > 0.0031308f); }
static float3 rec709_to_2020(float3 v) {
    return float3(dot(v, float3(0.6274039f, 0.3292830f, 0.0433131f)), dot(v, float3(0.0690973f, 0.9195404f, 0.0113623f)), dot(v, float3(0.0163914f, 0.0880133f, 0.8955953f)));
}
static float3 rec2020_to_709(float3 v) {
    return float3(dot(v, float3(1.6604910f, -0.5876411f, -0.0728499f)), dot(v, float3(-0.1245505f, 1.1328999f, -0.0083494f)), dot(v, float3(-0.0181508f, -0.1005789f, 1.1187297f)));
}
static float3 pq_nits(float3 v) {
    // SMPTE ST 2084 inverse EOTF, absolute luminance in cd/m^2, no SDR clamp.
    float3 p = pow(clamp(v, 0.0f, 1.0f), float3(32.0f / 2523.0f));
    return 10000.0f * pow(max(p - 3424.0f / 4096.0f, 0.0f) / max(2413.0f / 128.0f - (2392.0f / 128.0f) * p, 1e-6f), float3(16384.0f / 2610.0f));
}
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) {
    float4 v = t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw));
    int input = int(c.color.x); bool edr = c.color.y != 0.0f;
    if (!input && !edr) return v; // Preserve the original SDR path byte-for-byte.
    float3 nits;
    if (input == 1) nits = pq_nits(v.rgb); // RGB10A2 scanout: Rec.2020 PQ.
    else if (input == 2) nits = rec709_to_2020(v.rgb) * 80.0f; // linear scRGB, 80-nit unit.
    else nits = rec709_to_2020(srgb_decode(max(v.rgb, 0.0f))) * 100.0f;
    if (edr) return float4(nits / 80.0f, v.a); // CAEDRMetadata opticalOutputScale = 80.
    // SDR screenshots of HDR frames: hue-preserving highlight compression, then Rec.709 encoding.
    float3 rgb = max(rec2020_to_709(nits / 100.0f), 0.0f);
    float y = dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
    float white = max(c.color.w / 100.0f, 1.0f);
    rgb *= (1.0f + y / (white * white)) / (1.0f + y);
    return float4(srgb_encode(rgb), v.a);
}
