// The present: the frame drawn into the drawable, cropped (nvn_mtl.m present_pipeline)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
struct Crop { float4 rect; float4 edge; };
// Minification: texels per pixel the filter widens to at most (smaller pictures alias a little again); it reads up to 4 x 3 + 1 texels an axis, 7 bilinear fetches
constant float MINIFY_SCALE_MAX = 3;
// Catmull-Rom (radius 2, in pixels of the picture as shown): sharp, with small negative lobes; Lanczos-2's sines took twice the GPU time for a like picture
static float catmull_rom(float x) {
    x = abs(x);
    return x < 1 ? (1.5 * x - 2.5) * x * x + 1 : x < 2 ? ((-0.5 * x + 2.5) * x - 4) * x + 2 : 0;
}
// One tap of the minifying filter along an axis: texels a and a + 1 of the kernel around centre c (texel units) for scale s, taken by one bilinear fetch at the weights' centroid.
// Adjacent weights of opposite sign (where the kernel crosses zero, both small) are approximated by the fetch at the clamped centroid. Returns the pair's weight, *pos the fetch position
static float minify_pair(int a, int last, float c, float s, thread float *pos) {
    float wa = catmull_rom((float(a) + 0.5 - c) / s), wb = a + 1 <= last ? catmull_rom((float(a) + 1.5 - c) / s) : 0, w = wa + wb;
    *pos = float(a) + 0.5 + (abs(w) > 1e-6 ? clamp(wb / w, 0.0, 1.0) : 0.5);
    return w;
}
// A picture shown at its size or larger takes one bilinear tap. One shown smaller (a 2560x1440 frame in a 1920x1080 rectangle on a 1080p screen) is filtered over each
// pixel's footprint with a Catmull-Rom kernel stretched by the scale, two texels an axis per bilinear fetch: one bilinear tap weighs the texels unevenly by their position, so
// edges come out stair-stepped and shimmer as they move (minification aliasing), and past 2:1 texels are skipped entirely. Fetches stay within the crop (edge: its first and last
// texel centres in uv). 2560x1440 into 1920x1080: 9 fetches a pixel, 0.27 ms of GPU on an M2 Max (one tap: 0.03 ms)
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) {
    float2 uv = in.uv * c.rect.zw + c.rect.xy;
    float2 size = float2(t.get_width(), t.get_height());
    float2 step = abs(float2(dfdx(uv.x), dfdy(uv.y))) * size;   // texels per pixel: uv is linear over the triangle, so the derivatives are exact
    if (step.x < 1.001 && step.y < 1.001) return t.sample(s, clamp(uv, c.edge.xy, c.edge.zw));
    float2 sc = clamp(step, 1.0, MINIFY_SCALE_MAX), p = uv * size;
    float2 lo = min(c.edge.xy, c.edge.zw) * size, hi = max(c.edge.xy, c.edge.zw) * size;   // first and last texel centres of the crop, texel units
    int2 i0 = int2(ceil(p - 0.5 - 2 * sc)), last = int2(floor(p - 0.5 + 2 * sc));
    float4 sum = 0; float wsum = 0;
    for (int y = i0.y; y <= last.y; y += 2) {
        float py, wy = minify_pair(y, last.y, p.y, sc.y, &py); py = clamp(py, lo.y, hi.y);
        for (int x = i0.x; x <= last.x; x += 2) {
            float px, wx = minify_pair(x, last.x, p.x, sc.x, &px);
            sum += wx * wy * t.sample(s, float2(clamp(px, lo.x, hi.x), py) / size); wsum += wx * wy;
        }
    }
    sum /= wsum;
    return max(sum, 0.0);   // the kernel's negative lobes can undershoot 0 (a unorm drawable clamps above 1 itself; a float one keeps a source's range)
}
