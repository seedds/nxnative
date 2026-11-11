// Texture copies drawn as a render pass: colour, depth and stencil (nvn_mtl.m copy_t2t_draw)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }
// p[0] = source origin (texels) + source texels per destination texel, p[1] = destination origin + source level size
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 *p [[buffer(0)]]) {
  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; return t.sample(s, st / p[1].zw, level(0)); }
// The copies around MetalFX's spatial scaler (up_mfx_encode). p[2].x = blend with t1, a smooth picture of the same rectangle (0 = none); p[2].y = 1: the colour squared, sign kept
// (square-root-encoded colour into linear); p[2].zw = t1's coordinate per source texel of the rectangle
fragment float4 fsup(V in [[stage_in]], texture2d<float> t [[texture(0)]], texture2d<float> t1 [[texture(1)]], sampler s [[sampler(0)]], constant float4 *p [[buffer(0)]]) {
  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; float4 c = t.sample(s, st / p[1].zw, level(0));
  if (p[2].x > 0) c = mix(c, t1.sample(s, (st - p[0].xy) * p[2].zw, level(0)), p[2].x);
  if (p[2].y > 0) c.rgb = c.rgb * abs(c.rgb);
  return c; }
struct D { float d [[depth(any)]]; };
fragment D fsd(V in [[stage_in]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 *p [[buffer(0)]]) {
  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; D o; o.d = t.read(uint2(st)); return o; }
struct DS { float d [[depth(any)]]; uint s [[stencil]]; };
fragment DS fsds(V in [[stage_in]], depth2d<float> t [[texture(0)]], texture2d<uint> ts [[texture(1)]], constant float4 *p [[buffer(0)]]) {
  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; DS o; o.d = t.read(uint2(st)); o.s = ts.read(uint2(st)).r; return o; }
