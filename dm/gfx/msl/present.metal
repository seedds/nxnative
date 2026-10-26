// The present: the frame drawn into the drawable, cropped (nvn_mtl.m present_pipeline)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
struct Crop { float4 rect; float4 edge; float4 box; };
// box.x: taps per axis (1 = one bilinear sample). A single bilinear tap reads 4 texels whatever the reduction, so a picture
// scaled above the viewport and sampled down this way throws nearly all of it away. With n taps the whole footprint is
// averaged, which makes DM_PRESENT_UPSCALE_OUT above 1.0 real supersampling. box.yz: one source texel in uv. box.w: sharpening
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) {
  float2 uv = clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw);
  int n = int(c.box.x);
  if (n <= 1) return t.sample(s, uv);
  float4 a = 0;
  for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
    float2 o = (float2(x, y) - 0.5 * float(n - 1)) * c.box.yz;
    a += t.sample(s, clamp(uv + o, c.edge.xy, c.edge.zw));
  }
  return a / float(n * n);
}
// Contrast adaptive sharpening of the presented picture (DM_SHARPEN): the 5 tap cross around the pixel. The low pass is the mean
// of the four neighbours; adding back the difference lifts edges and leaves flat areas alone. Clamped to the neighbourhood's own
// range, so it cannot ring or halo. Taken instead of fs, not after it, so sharpening costs no extra pass
fragment float4 sharp(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) {
  float2 uv = clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw), d = c.box.yz;
  float3 e = t.sample(s, uv).rgb;
  float3 n0 = t.sample(s, clamp(uv + float2(-d.x, 0), c.edge.xy, c.edge.zw)).rgb;
  float3 n1 = t.sample(s, clamp(uv + float2( d.x, 0), c.edge.xy, c.edge.zw)).rgb;
  float3 n2 = t.sample(s, clamp(uv + float2(0, -d.y), c.edge.xy, c.edge.zw)).rgb;
  float3 n3 = t.sample(s, clamp(uv + float2(0,  d.y), c.edge.xy, c.edge.zw)).rgb;
  float3 lo = min(min(n0, n1), min(n2, n3)), hi = max(max(n0, n1), max(n2, n3));
  float3 r = e + (e - (n0 + n1 + n2 + n3) * 0.25) * c.box.w;
  return float4(clamp(r, min(lo, e), max(hi, e)), 1);
}
