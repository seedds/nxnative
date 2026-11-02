// Breath of the Wild's tone mapping pass for HDR output (game.tsv hdr; nvn_mtl.m, HDR output): the host compiles this in place of the translated pixel shader
// f7ee6fe10b925543 (uking_pass_shader) and draws it with that shader's reflection, so the argument buffer below keeps the translation's bindings in their order.
// The game's pass, written out:
//   x = min(scene, 10000) + glow                                     (both RG11B10 float)
//   l = dot(x, (0.2989, 0.587, 0.1145)), v(x) = 1 - exp(-x)          (the luminance weights after the first come from the shader's constants, c1)
//   t = saturate(mix(x * v(l) / l, v(x), v(l)^2))                    (dark colours tone mapped by luminance, keeping their hue; bright ones per channel, going white)
//   saturation grade: s = (r + g + b) * 2/3, g = c3[0].y * (1 - (s - 1)^2) + c3[0].x, out = m + (t - m) * g with m the largest channel
// written into an RGB10A2 target (which clamps it to [0, 1]); the AA pass, the HUD and the copy into the swapchain follow. This writes that result unchanged, and the same pass
// tone mapped up to the screen's headroom into the host's side texture, as what it adds to the SDR result (rgb) and the SDR result's luminance (a): nvn_mtl.m keeps it wherever
// the presented picture still has that luminance (hdr_merge) and adds it in the present. The HDR curve is the game's up to the knee (v = knee), then an exponential shoulder with the game's
// slope there (1 - knee) that rises toward the peak instead of 1 (with a peak of 1 it is the game's curve: nothing is added); the blend weight and the saturation grade are the
// game's own, so colours change only in how bright the highlights get.
#include <metal_stdlib>
using namespace metal;

struct Cb { float4 v[4096]; };
struct Border { uint mask; float4 k[32]; };   // the translation's border colours: texture i sampled with its border where mask bit i is set (the chain's dm_border_t)
struct Args {
    constant Cb *c1 [[id(0)]];   // the shader's constants: v[0] = (0.587, 0.1145, 2/3)
    constant Cb *c3 [[id(1)]];   // uniform block 0: v[0].x / .y = the saturation grade's bias and scale (0.99, 0.18 in the field)
    texture2d<float> scene [[id(2)]];
    sampler scene_s [[id(3)]];
    texture2d<float> glow [[id(4)]];
    sampler glow_s [[id(5)]];
    sampler scene_w [[id(6)]];   // the border-colour samplers and their mix, as the translation samples (its dm_bs)
    sampler glow_w [[id(7)]];
    constant Border *border [[id(8)]];
};
struct In { float4 uv [[user(locn0)]]; };
struct Out { float4 c [[color(0)]]; };
// The host's HDR constants (nvn_mtl.m hdr_consts, fragment buffer 30): peak = the brightest value to tone map to, in SDR whites (the screen's EDR headroom; 1 = SDR); gain = the
// excess's scale; knee = the SDR value where the curve leaves the game's (DM_HDR_KNEE)
struct Hdr { float peak, gain, knee, pad; };

constant float kLog2e = 1.44269502f;

static float3 game_curve(float3 x) { return 1.0f - exp2(x * -kLog2e); }
static float3 hdr_curve(float3 x, float peak, float knee) {
    float xk = -log2(1.0f - knee) / kLog2e, slope = 1.0f - knee, room = peak - knee;   // where the game's curve reaches the knee, and its slope there
    float3 up = knee + room * (1.0f - exp2((x - xk) * (-slope * kLog2e / room)));
    return select(game_curve(x), up, x > xk);
}
static float3 tex(texture2d<float> t, sampler s, sampler w, constant Border *b, uint i, float2 uv) {
    float3 r = t.sample(s, uv).xyz;
    if (b->mask >> i & 1u) r += (t.sample(w, uv).xyz - r) * b->k[i].xyz;
    return r;
}
static float3 grade(float3 t, float3 ref, float bias, float scale, float third2) {
    float s = (ref.r + ref.g + ref.b) * third2 - 1.0f;
    float g = scale * (1.0f - s * s) + bias;
    float m = max(t.r, max(t.g, t.b));
    return m + (t - m) * g;
}

fragment Out main0(In in [[stage_in]], float4 pos [[position]], constant Args &a [[buffer(0)]], constant Hdr &h [[buffer(30)]],
                   texture2d<half, access::write> side [[texture(0)]]) {
    float2 uv = select(in.uv.xy, float2(0.0f), isnan(in.uv.xy));
    float3 x = min(tex(a.scene, a.scene_s, a.scene_w, a.border, 0, uv), 10000.0f) + tex(a.glow, a.glow_s, a.glow_w, a.border, 1, uv);
    float4 k = a.c1->v[0];
    float3 lw = float3(0.2989f, k.x, k.y);
    float l = dot(x, lw);
    float il = l > 0.0f ? 1.0f / l : 0.0f;
    float vl = game_curve(float3(l)).x, w = vl * vl;
    float3 sdr = saturate(mix(x * (vl * il), game_curve(x), w));
    float bias = a.c3->v[0].x, scale = a.c3->v[0].y;
    float3 o = grade(sdr, sdr, bias, scale, k.z), shown = saturate(o);   // what the game writes; what its target keeps
    float3 more = 0.0f;
    if (h.peak > 1.0001f) {
        float knee = clamp(h.knee, 0.05f, 0.95f);
        float3 hdr = clamp(mix(x * (hdr_curve(float3(l), h.peak, knee).x * il), hdr_curve(x, h.peak, knee), w), 0.0f, h.peak);
        more = max(clamp(grade(hdr, sdr, bias, scale, k.z), 0.0f, h.peak) - shown, 0.0f) * max(h.gain, 0.0f);
    }
    uint2 p = uint2(pos.xy);
    if (p.x < side.get_width() && p.y < side.get_height()) side.write(half4(half3(more), half(dot(shown, lw))), p);
    Out out;
    out.c = float4(o, 0.0f);
    return out;
}
