// Texture copies drawn as a render pass: colour, depth and stencil (nvn_mtl.m copy_t2t_draw)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }
// p[0] = source origin (texels) + source texels per destination texel, p[1] = destination origin + source level size
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 *p [[buffer(0)]]) {
  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; return t.sample(s, st / p[1].zw, level(0)); }
struct D { float d [[depth(any)]]; };
fragment D fsd(V in [[stage_in]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 *p [[buffer(0)]]) {
  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; D o; o.d = t.read(uint2(st)); return o; }
struct DS { float d [[depth(any)]]; uint s [[stencil]]; };
fragment DS fsds(V in [[stage_in]], depth2d<float> t [[texture(0)]], texture2d<uint> ts [[texture(1)]], constant float4 *p [[buffer(0)]]) {
  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; DS o; o.d = t.read(uint2(st)); o.s = ts.read(uint2(st)).r; return o; }
// A same-size copy that sharpens (DM_UP_SHARPEN, after MetalFX's spatial scaler in nvn_mtl.m up_mfx_encode): AMD FidelityFX FSR 1 RCAS (robust contrast-adaptive sharpening, MIT licence) with its
// noise damping, on colour tonemapped reversibly (c / (1 + max channel)) so HDR scene colour sharpens like display colour. p[2].x = sharpness, 0..1 (FSR's exp2(-stops))
static float3 rcas_tm(float3 c) { return c / (1.0 + max(max(c.r, c.g), max(c.b, 0.0))); }
fragment float4 fsr(V in [[stage_in]], texture2d<float> t [[texture(0)]], constant float4 *p [[buffer(0)]]) {
  int2 c = int2(p[0].xy + (in.p.xy - p[1].xy)), hi = int2(p[1].zw) - 1;
  float4 e4 = t.read(uint2(clamp(c, int2(0), hi)));
  float3 b = rcas_tm(t.read(uint2(clamp(c + int2(0, -1), int2(0), hi))).rgb), d = rcas_tm(t.read(uint2(clamp(c + int2(-1, 0), int2(0), hi))).rgb);
  float3 f = rcas_tm(t.read(uint2(clamp(c + int2(1, 0), int2(0), hi))).rgb), h = rcas_tm(t.read(uint2(clamp(c + int2(0, 1), int2(0), hi))).rgb), e = rcas_tm(max(e4.rgb, 0.0));
  float3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));
  float3 hitMin = min(mn4, e) / (4.0 * mx4 + 1e-5), hitMax = (1.0 - max(mx4, e)) / (4.0 * mn4 - 4.0 - 1e-5);
  float3 lobe3 = max(-hitMin, hitMax);
  float lobe = max(-0.1875, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * p[2].x;
  float bl = b.b * 0.5 + (b.r * 0.5 + b.g), dl = d.b * 0.5 + (d.r * 0.5 + d.g), el = e.b * 0.5 + (e.r * 0.5 + e.g), fl = f.b * 0.5 + (f.r * 0.5 + f.g), hl = h.b * 0.5 + (h.r * 0.5 + h.g);
  float nz = 0.25 * (bl + dl + fl + hl) - el;
  nz = saturate(abs(nz) / (max(max(max(bl, dl), max(el, fl)), hl) - min(min(min(bl, dl), min(el, fl)), hl) + 1e-5));
  lobe *= -0.5 * nz + 1.0;
  float3 o = saturate((lobe * (b + d + h + f) + e) / (4.0 * lobe + 1.0));
  o = o / max(1.0 - max(o.r, max(o.g, o.b)), 1.0 / 1024.0);   // back to scene colour
  return float4(o, e4.a);
}
