// The present: the frame drawn into the drawable, cropped (nvn_mtl.m present_pipeline)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
struct Crop { float4 rect; float4 edge; };
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) { return t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)); }
// Edge adaptive spatial upsampling (DM_PRESENT_UPSCALE=fsr): AMD FidelityFX Super Resolution 1.0's EASU (MIT licence, Copyright (c) 2021 Advanced Micro Devices, Inc.),
// in one pass straight into the drawable, in half precision as FSR's own FsrEasuH. 12 texels around the output pixel's position in the source; the local edge direction and length (from the luma of the 2x2 nearest
// texels' crosses) shape a Lanczos-like kernel that is stretched along the edge, and the result is clamped to the 2x2 nearest texels' range so it cannot ring. Cheaper than
// MetalFX's spatial scaler: no copy of the input, no output texture, one draw
static half easu_luma(half3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }
static void easu_set(thread half2 &dir, thread half &len, half w, half lA, half lB, half lC, half lD, half lE) {
  half dc = lD - lC, cb = lC - lB;
  half lenX = 1.0h / max(max(abs(dc), abs(cb)), 1.0h / 16384.0h);
  half dirX = lD - lB; dir.x += dirX * w;
  lenX = saturate(abs(dirX) * lenX); len += lenX * lenX * w;
  half ec = lE - lC, ca = lC - lA;
  half lenY = 1.0h / max(max(abs(ec), abs(ca)), 1.0h / 16384.0h);
  half dirY = lE - lA; dir.y += dirY * w;
  lenY = saturate(abs(dirY) * lenY); len += lenY * lenY * w;
}
static void easu_tap(thread half3 &aC, thread half &aW, half2 off, half2 dir, half2 len, half lob, half clp, half3 c) {
  half2 v = half2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len;
  half d2 = min(dot(v, v), clp);
  half wB = 0.4h * d2 - 1.0h, wA = lob * d2 - 1.0h;
  wB *= wB; wA *= wA;
  wB = 1.5625h * wB - 0.5625h;
  half w = wB * wA;
  aC += c * w; aW += w;
}
fragment float4 easu(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) {
  float2 d = 1.0 / float2(t.get_width(), t.get_height());   // one source texel in uv
  float2 uv = in.uv * c.rect.zw + c.rect.xy;             // where this output pixel's centre falls in the source
  float2 pq = uv / d - 0.5, fp = floor(pq); half2 pp = half2(pq - fp);    // texel f (the 2x2's top left) and the position within the 2x2
  int2 q = int2(fp), lo0 = int2(c.edge.xy / d), hi0 = int2(c.edge.zw / d);   // texel reads (no filtering), clamped to the valid rectangle's texels
  #define T(x, y) half3(t.read(uint2(clamp(q + int2(x, y), lo0, hi0))).rgb)
  half3 b = T(0, -1), cc = T(1, -1), e = T(-1, 0), f = T(0, 0), g = T(1, 0), h = T(2, 0);
  half3 i = T(-1, 1), j = T(0, 1), k = T(1, 1), l = T(2, 1), n = T(0, 2), o = T(1, 2);
  #undef T
  half bL = easu_luma(b), cL = easu_luma(cc), eL = easu_luma(e), fL = easu_luma(f), gL = easu_luma(g), hL = easu_luma(h);
  half iL = easu_luma(i), jL = easu_luma(j), kL = easu_luma(k), lL = easu_luma(l), nL = easu_luma(n), oL = easu_luma(o);
  half2 dir = 0; half len = 0;
  easu_set(dir, len, (1 - pp.x) * (1 - pp.y), bL, eL, fL, gL, jL);
  easu_set(dir, len, pp.x * (1 - pp.y), cL, fL, gL, hL, kL);
  easu_set(dir, len, (1 - pp.x) * pp.y, fL, iL, jL, kL, nL);
  easu_set(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
  half dirR = dot(dir, dir); bool zro = dirR < 1.0h / 16384.0h;
  dirR = zro ? 1 : rsqrt(dirR); dir.x = zro ? 1 : dir.x; dir *= dirR;
  len = len * 0.5; len *= len;
  half stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
  half2 len2 = half2(1.0h + (stretch - 1.0h) * len, 1.0h - 0.5h * len);
  half lob = 0.5h + (0.21h - 0.5h) * len, clp = 1.0h / lob;
  half3 aC = 0; half aW = 0;
  easu_tap(aC, aW, half2( 0, -1) - pp, dir, len2, lob, clp, b);
  easu_tap(aC, aW, half2( 1, -1) - pp, dir, len2, lob, clp, cc);
  easu_tap(aC, aW, half2(-1,  1) - pp, dir, len2, lob, clp, i);
  easu_tap(aC, aW, half2( 0,  1) - pp, dir, len2, lob, clp, j);
  easu_tap(aC, aW, half2( 0,  0) - pp, dir, len2, lob, clp, f);
  easu_tap(aC, aW, half2(-1,  0) - pp, dir, len2, lob, clp, e);
  easu_tap(aC, aW, half2( 1,  1) - pp, dir, len2, lob, clp, k);
  easu_tap(aC, aW, half2( 2,  1) - pp, dir, len2, lob, clp, l);
  easu_tap(aC, aW, half2( 2,  0) - pp, dir, len2, lob, clp, h);
  easu_tap(aC, aW, half2( 1,  0) - pp, dir, len2, lob, clp, g);
  easu_tap(aC, aW, half2( 1,  2) - pp, dir, len2, lob, clp, o);
  easu_tap(aC, aW, half2( 0,  2) - pp, dir, len2, lob, clp, n);
  half3 lo = min(min(f, g), min(j, k)), hi = max(max(f, g), max(j, k));
  return float4(float3(clamp(aC / aW, lo, hi)), 1);
}
