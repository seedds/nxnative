// The present: the frame drawn into the drawable, cropped (nvn_mtl.m present_pipeline)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
struct Crop { float4 rect; float4 edge; };
// Minification: texels per pixel the filter widens to at most (smaller pictures alias a little again). A radius-1 tent
// spans at most 2 * 3 = 6 texels an axis (3 bilinear fetches, 9 in 2D; at most 4 in 2D up to 2:1).
constant float MINIFY_SCALE_MAX = 3;
// Non-negative tent (radius 1, in pixels of the picture as shown): exact with hardware bilinear pairing (no negative lobes
// that cross zero within a pair), 4 fetches a pixel up to 2:1 and at most 9 up to 3:1.
static float tent(float x) { return max(1.0 - abs(x), 0.0); }
// One tap along an axis: texels a and a + 1 of the tent around centre p (texel units) for scale s, taken by one bilinear
// fetch at their weighted centroid, clamped to the crop's [lo, hi] texel centres and converted to normalized uv.
static float minify_pair(int a, int last, float p, float s, float lo, float hi, float inv_size, thread float &pos) {
    float wa = tent((float(a) + 0.5 - p) / s);
    float wb = a + 1 <= last ? tent((float(a) + 1.5 - p) / s) : 0.0;
    float w = wa + wb;
    pos = clamp(float(a) + 0.5 + (w > 1e-6 ? wb / w : 0.5), lo, hi) * inv_size;
    return w;
}
// Builds up to 3 bilinear taps (pos in normalized uv, normalized wgt) for one axis in registers (no dynamic array indexing,
// avoiding stack spills on Apple GPUs). A non-minified axis takes one bilinear tap.
static void minify_axis(float step, float p, float lo, float hi, float inv_size, thread float3 &pos, thread float3 &wgt) {
    if (step < 1.001) { pos = float3(clamp(p, lo, hi) * inv_size, 0.0, 0.0); wgt = float3(1.0, 0.0, 0.0); return; }
    float s = min(step, MINIFY_SCALE_MAX);
    int i0 = int(floor(p - 0.5 - s)) + 1, last = min(int(ceil(p - 0.5 + s)) - 1, i0 + 5);
    float p0 = 0.0, p1 = 0.0, p2 = 0.0;
    float w0 = minify_pair(i0, last, p, s, lo, hi, inv_size, p0);
    float w1 = i0 + 2 <= last ? minify_pair(i0 + 2, last, p, s, lo, hi, inv_size, p1) : 0.0;
    float w2 = i0 + 4 <= last ? minify_pair(i0 + 4, last, p, s, lo, hi, inv_size, p2) : 0.0;
    float wsum = w0 + w1 + w2;
    if (wsum <= 1e-6) { pos = float3(clamp(p, lo, hi) * inv_size, 0.0, 0.0); wgt = float3(1.0, 0.0, 0.0); return; }
    pos = float3(p0, p1, p2);
    wgt = float3(w0, w1, w2) / wsum;
}
static float4 sample_row(texture2d<float> t, sampler s, float3 px, float3 wx, float py) {
    float4 r = wx.x * t.sample(s, float2(px.x, py));
    if (wx.y > 0.0) r += wx.y * t.sample(s, float2(px.y, py));
    if (wx.z > 0.0) r += wx.z * t.sample(s, float2(px.z, py));
    return r;
}
// A picture shown at its size or larger takes one bilinear tap. One shown smaller (e.g. a 2560x1440 frame in a 1920x1080
// rectangle or a 1280x720 window) is filtered over each pixel's footprint with a separable tent kernel stretched by the
// scale, two texels an axis per bilinear fetch: one bilinear tap weighs texels unevenly by pixel-centre phase, so edges
// stair-step and crawl as they move (minification aliasing), and past 2:1 texels are skipped entirely. Fetches stay within
// the crop (edge: its first and last texel centres in uv).
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) {
    float2 uv = in.uv * c.rect.zw + c.rect.xy;
    float2 size = float2(t.get_width(), t.get_height());
    float2 step = abs(float2(dfdx(uv.x), dfdy(uv.y))) * size;   // texels per pixel: uv is linear over the triangle, so the derivatives are exact
    if (step.x < 1.001 && step.y < 1.001) return t.sample(s, clamp(uv, c.edge.xy, c.edge.zw));
    float2 inv_size = 1.0 / size, p = uv * size;
    float2 lo = min(c.edge.xy, c.edge.zw) * size, hi = max(c.edge.xy, c.edge.zw) * size;   // first and last texel centres of the crop, texel units
    float3 px, wx, py, wy;
    minify_axis(step.x, p.x, lo.x, hi.x, inv_size.x, px, wx);
    minify_axis(step.y, p.y, lo.y, hi.y, inv_size.y, py, wy);
    float4 sum = wy.x * sample_row(t, s, px, wx, py.x);
    if (wy.y > 0.0) sum += wy.y * sample_row(t, s, px, wx, py.y);
    if (wy.z > 0.0) sum += wy.z * sample_row(t, s, px, wx, py.z);
    return sum;
}
