// Resolution upscaling: the game's own spatial upscaler resolved by the host (nvn_mtl.m ups_bypass_encode). The game's kernel reads its scene, rendered larger than it
// asked (DM_UPSCALE), with tap offsets its constants computed for the game's size, and writes an output image of the game's size that is smaller than the scene the host
// holds. This kernel writes that output instead: every output texel is the scene filtered down over its footprint, so the picture keeps the detail of the larger render.
// The textures are bound as the game's kernel binds them (an sRGB scene decodes on read and the output encodes on write), so the filter runs in linear light.
#include <metal_stdlib>
using namespace metal;
// ow, oh: the output image; sw, sh: the rectangle of the scene read, at its origin (the part the game's upscaler reads: with dynamic resolution less than the texture);
// filter: 0 box (the exact area average of the footprint), 1 tent (a triangle of the footprint's width, centred on it), 2 Lanczos-2 (its kernel stretched to the footprint,
// two lobes each side)
struct P { uint ow, oh, sw, sh, filter; };
#define TAPS 10   // the most texels an axis of the tent / Lanczos window covers
// sin(pi x) / (pi x), 1 at 0
static inline float sinc(float x) { float t = M_PI_F * x; return abs(x) < 1e-4f ? 1.0f : fast::sin(t) / t; }   // fast::sin: a weight, not a coordinate; |t| < 2 pi
kernel void ups_resolve(texture2d<float> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]], constant P &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {
  if (g.x >= p.ow || g.y >= p.oh) return;
  float2 r = float2(p.sw, p.sh) / float2(p.ow, p.oh);   // scene texels per output texel on each axis (3200x1800 → 1920x1080: 1.667)
  float2 a = float2(g) * r, b = a + r, c = a + r * 0.5f;   // the footprint [a, b) and its centre, in scene texels
  float4 sum = 0; float wsum = 0;
  if (p.filter == 0) {   // box: each scene texel weighs the area it shares with the footprint
    int2 lo = int2(floor(a)), hi = int2(ceil(b)) - 1;
    lo = clamp(lo, int2(0), int2(p.sw - 1, p.sh - 1)); hi = clamp(hi, int2(0), int2(p.sw - 1, p.sh - 1));
    for (int y = lo.y; y <= hi.y; y++) { float wy = min(b.y, float(y + 1)) - max(a.y, float(y));
      for (int x = lo.x; x <= hi.x; x++) { float w = wy * (min(b.x, float(x + 1)) - max(a.x, float(x))); sum += src.read(uint2(x, y)) * w; wsum += w; } }
  } else {   // tent or Lanczos-2 around the centre: radius r (tent) or 2r (Lanczos), weights by the texel centre's distance in footprint units, separable: each axis's
    // weights are computed once per output texel (the sines of Lanczos cost more than the reads: 2.8 ms a frame with one pair per read, 0.5 ms this way)
    float2 rad = p.filter == 1 ? r : 2.0f * r;
    int2 lo = int2(floor(c - rad + 0.5f)), hi = int2(floor(c + rad - 0.5f));
    lo = clamp(lo, int2(0), int2(p.sw - 1, p.sh - 1)); hi = clamp(hi, int2(0), int2(p.sw - 1, p.sh - 1));
    hi = min(hi, lo + TAPS - 1);   // at most TAPS texels an axis: 2r up to 4 (a 1920x1080 scene at 2x) spans 8 or 9
    float wx[TAPS], wy[TAPS];   // indexed by unrolled loops only, so they stay in registers (a dynamic index would put them in thread memory)
#pragma clang loop unroll(full)
    for (int i = 0; i < TAPS; i++) {
      float tx = (float(lo.x + i) + 0.5f - c.x) / r.x, ty = (float(lo.y + i) + 0.5f - c.y) / r.y;
      wx[i] = p.filter == 1 ? max(1.0f - abs(tx), 0.0f) : (abs(tx) < 2.0f ? sinc(tx) * sinc(tx * 0.5f) : 0.0f);
      wy[i] = p.filter == 1 ? max(1.0f - abs(ty), 0.0f) : (abs(ty) < 2.0f ? sinc(ty) * sinc(ty * 0.5f) : 0.0f); }
#pragma clang loop unroll(full)
    for (int j = 0; j < TAPS; j++) { if (lo.y + j > hi.y) break;
#pragma clang loop unroll(full)
      for (int i = 0; i < TAPS; i++) { if (lo.x + i > hi.x) break; float w = wy[j] * wx[i]; sum += src.read(uint2(lo.x + i, lo.y + j)) * w; wsum += w; } }
  }
  dst.write(wsum > 0 ? sum / wsum : src.read(uint2(clamp(int2(c), int2(0), int2(p.sw - 1, p.sh - 1)))), g);   // Lanczos weights are normalized so flat areas stay flat; an empty footprint cannot happen but reads the centre
}
