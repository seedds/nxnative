// Cropped scanout to the drawable, with explicit HDR transfer conversion.
#include <metal_stdlib>
using namespace metal;

struct V {
    float4 p [[position]];
    float2 uv;
};

vertex V vs(uint id [[vertex_id]]) {
    float2 uv = float2((id << 1) & 2, id & 2);
    V output;
    output.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    output.uv = uv;
    return output;
}

struct Crop {
    float4 rect;
    float4 edge;
    // x: input encoding (0 SDR, 1 PQ, 2 scRGB); y: EDR output; z: reserved; w: content peak in nits.
    float4 color;
};

static float3 srgb_decode(float3 value) {
    return select(value / 12.92f, pow((value + 0.055f) / 1.055f, float3(2.4f)), value > 0.04045f);
}

static float3 srgb_encode(float3 value) {
    value = max(value, 0.0f);
    return select(value * 12.92f, 1.055f * pow(value, float3(1.0f / 2.4f)) - 0.055f, value > 0.0031308f);
}

static float3 rec709_to_2020(float3 value) {
    return float3(dot(value, float3(0.6274039f, 0.3292830f, 0.0433131f)),
                  dot(value, float3(0.0690973f, 0.9195404f, 0.0113623f)),
                  dot(value, float3(0.0163914f, 0.0880133f, 0.8955953f)));
}

static float3 rec2020_to_709(float3 value) {
    return float3(dot(value, float3(1.6604910f, -0.5876411f, -0.0728499f)),
                  dot(value, float3(-0.1245505f, 1.1328999f, -0.0083494f)),
                  dot(value, float3(-0.0181508f, -0.1005789f, 1.1187297f)));
}

static float3 pq_nits(float3 value) {
    // SMPTE ST 2084 EOTF: PQ signal to absolute luminance in cd/m^2, without an SDR clamp.
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;
    float3 signal = pow(clamp(value, 0.0f, 1.0f), float3(1.0f / m2));
    float3 numerator = max(signal - c1, 0.0f);
    float3 denominator = max(c2 - c3 * signal, 1e-6f);
    return 10000.0f * pow(numerator / denominator, float3(1.0f / m1));
}

fragment float4 fs(V in [[stage_in]], texture2d<float> texture [[texture(0)]], sampler filtering [[sampler(0)]],
                   constant Crop &crop [[buffer(0)]]) {
    float2 uv = clamp(in.uv * crop.rect.zw + crop.rect.xy, crop.edge.xy, crop.edge.zw);
    float4 sample = texture.sample(filtering, uv);
    int input_encoding = int(crop.color.x);
    bool edr_output = crop.color.y != 0.0f;
    if (input_encoding == 0 && !edr_output) {
        return sample; // Preserve the original SDR path byte-for-byte.
    }

    float3 nits;
    if (input_encoding == 1) {
        nits = pq_nits(sample.rgb); // RGB10A2 scanout: Rec.2020 PQ.
    } else if (input_encoding == 2) {
        nits = rec709_to_2020(sample.rgb) * 80.0f; // Linear scRGB, 80-nit unit.
    } else {
        nits = rec709_to_2020(srgb_decode(max(sample.rgb, 0.0f))) * 100.0f;
    }
    if (edr_output) {
        return float4(nits / 80.0f, sample.a); // CAEDRMetadata opticalOutputScale = 80.
    }

    // SDR screenshots of HDR frames: compress highlights, then encode Rec.709 RGB.
    float3 rgb = max(rec2020_to_709(nits / 100.0f), 0.0f);
    float luminance = dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
    float white = max(crop.color.w / 100.0f, 1.0f);
    rgb *= (1.0f + luminance / (white * white)) / (1.0f + luminance);
    return float4(srgb_encode(rgb), sample.a);
}
