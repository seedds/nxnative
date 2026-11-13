// Frame generation core (DM_FRAMEGEN): one generated picture between two presented ones, for a game held at 30 fps on a 60 Hz present.
// The game gives no motion vectors (its upscaler is spatial), so the motion comes from the pictures themselves: a luma pyramid of each
// presented picture (quarter size, then halves), coarse-to-fine block matching of the newer against the older (each level starts from the
// best of its 3x3 coarser neighbours' vectors, then searches around it; parabolic sub-pixel refinement at the finest level), a 3x3 median.
// MetalFX's frame interpolator takes the two pictures, that motion field (quarter size) and a flat depth, and makes the middle picture.
// Self-contained (Metal + MetalFX only), so the offline harness (oot-rig/fgtest) runs the same code on captured frames.
#pragma once
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#define FG_LEVELS 5
typedef struct { int32_t R, coarse, patch, sub; float lambda, zbias; int32_t pad[2]; int32_t mvs, mvl, gx, gy; } fg_params;   // pad: untrusted blocks, DM_FG_BAIL; mvs/mvl: motion sum, DM_FG_MVBAIL

typedef struct fg_ctx {
    id<MTLDevice> dev;
    NSUInteger w, h, lw[FG_LEVELS], lh[FG_LEVELS];
    MTLPixelFormat fmt;
    id<MTLComputePipelineState> ps_luma0, ps_down, ps_search, ps_median, ps_fill, ps_resample;
    id<MTLTexture> col[2], pyr[2][FG_LEVELS], mv[FG_LEVELS], mvf, depth, out;
    id<MTLTexture> full[2];
    id<MTLComputePipelineState> ps_conf, ps_guard, ps_detail, ps_pin;
    id<MTLTexture> mvp;   // the motion with still blocks pinned (DM_FG_STILL)
    id<MTLComputePipelineState> ps_pick, ps_interp;   // DM_FG_WARP=3: the in-between picture made here for every motion, MetalFX not used
    id<MTLTexture> msel;   // DM_FG_WARP=3: the motion picked per 2x2 pixels of the generation size
    id<MTLTexture> gfull;   // detail: the generated picture at the picture's own size, with the frame's fine detail moved half the motion (DM_FG_DETAIL)
    id<MTLTexture> conf, gout;   // guard: per motion block, how badly the motion explains the frame; the generated picture with those places taken from the frame   // generation smaller than the picture: the picture's own copy at full size, for the held tick
    id<MTLBuffer> params;
    id<MTL4ArgumentTable> at;
    id<MTLResidencySet> res;
    id<MTL4FXFrameInterpolator> fi;
    int cur, have_prev, depth_ready;
    uint64_t bails;   // generated ticks that showed the frame instead (DM_FG_MVBAIL / DM_FG_BAIL), counted from the previous frame's sums
    float near_, far_, fov, dt;
    int R0, Rn, patch, median;
    float lambda;
} fg_ctx;

static NSString *const fg_msl = @R"MSL(
#include <metal_stdlib>
using namespace metal;
struct P { int R, coarse, patch, sub; float lambda, zbias; int pad0, pad1, mvs, mvl, gx, gy; };
kernel void luma0(texture2d<float> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler s(filter::linear, address::clamp_to_edge, coord::normalized);
    float2 sz = float2(dst.get_width(), dst.get_height()), uv = (float2(g) + 0.5) / sz, o = 0.25 / sz;
    float3 c = src.sample(s, uv + float2(-o.x, -o.y)).rgb + src.sample(s, uv + float2(o.x, -o.y)).rgb + src.sample(s, uv + float2(-o.x, o.y)).rgb + src.sample(s, uv + float2(o.x, o.y)).rgb;
    dst.write(float4(dot(c * 0.25, float3(0.299, 0.587, 0.114)), 0, 0, 0), g);
}
kernel void down2(texture2d<float> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler s(filter::linear, address::clamp_to_edge, coord::normalized);
    float2 uv = (float2(g) * 2.0 + 1.0) / float2(src.get_width(), src.get_height());
    dst.write(src.sample(s, uv), g);
}
static inline float px(texture2d<float> t, int2 q, int2 lim) { return t.read(uint2(clamp(q, int2(0), lim))).r; }
static float sad(texture2d<float> cur, texture2d<float> prev, int2 q, int2 d, int r, int2 lim) {
    float c = 0;
    for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) c += fabs(px(cur, q + int2(x, y), lim) - px(prev, q + d + int2(x, y), lim));
    return c;
}
kernel void search(texture2d<float> cur [[texture(0)]], texture2d<float> prev [[texture(1)]], texture2d<float> coarse [[texture(2)]], texture2d<float, access::write> mv [[texture(3)]],
                   constant P &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {
    int2 lim = int2(cur.get_width(), cur.get_height()) - 1;
    if (g.x > uint(lim.x) || g.y > uint(lim.y)) return;
    int2 q = int2(g); int r = p.patch;
    float2 base = 0; float bc = 1e30;
    if (p.coarse) {   // the best of the coarser level's vectors around this pixel (x2), so a motion edge takes the side it belongs to
        int2 cl = int2(coarse.get_width(), coarse.get_height()) - 1, cq = q / 2;
        for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
            float2 v = coarse.read(uint2(clamp(cq + int2(x, y), int2(0), cl))).xy * 2.0;
            float c = sad(cur, prev, q, int2(round(v)), r, lim);
            if (c < bc) { bc = c; base = v; }
        }
    }
    int2 b0 = int2(round(base)), best = b0; float bestc = 1e30;
    float cs[3][3];
    for (int y = -p.R; y <= p.R; y++) for (int x = -p.R; x <= p.R; x++) {
        int2 d = b0 + int2(x, y);
        float c = sad(cur, prev, q, d, r, lim) + p.lambda * length(float2(x, y)) + p.zbias * length(float2(d));
        if (c < bestc) { bestc = c; best = d; }
    }
    float2 v = float2(best);
    if (p.sub) {   // parabola through the costs either side of the best, per axis
        float c0 = sad(cur, prev, q, best, r, lim);
        float cl = sad(cur, prev, q, best + int2(-1, 0), r, lim), cr = sad(cur, prev, q, best + int2(1, 0), r, lim);
        float cu = sad(cur, prev, q, best + int2(0, -1), r, lim), cd = sad(cur, prev, q, best + int2(0, 1), r, lim);
        float dx = cl - 2 * c0 + cr, dy = cu - 2 * c0 + cd;
        if (dx > 1e-4) v.x += clamp(0.5 * (cl - cr) / dx, -0.5, 0.5);
        if (dy > 1e-4) v.y += clamp(0.5 * (cu - cd) / dy, -0.5, 0.5);
    }
    (void)cs;
    mv.write(float4(v, 0, 0), g);
}
static inline float med9(thread float *a) {
    for (int i = 0; i < 5; i++) for (int j = i + 1; j < 9; j++) if (a[j] < a[i]) { float t = a[i]; a[i] = a[j]; a[j] = t; }
    return a[4];
}
kernel void median3(texture2d<float> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]], uint2 g [[thread_position_in_grid]]) {
    int2 lim = int2(src.get_width(), src.get_height()) - 1;
    if (g.x > uint(lim.x) || g.y > uint(lim.y)) return;
    float ax[9], ay[9]; int n = 0;
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) { float2 v = src.read(uint2(clamp(int2(g) + int2(x, y), int2(0), lim))).xy; ax[n] = v.x; ay[n] = v.y; n++; }
    dst.write(float4(med9(ax), med9(ay), 0, 0), g);
}
kernel void resample(texture2d<float> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler s(filter::linear, address::clamp_to_edge, coord::normalized);
    dst.write(src.sample(s, (float2(g) + 0.5) / float2(dst.get_width(), dst.get_height())), g);
}
// Guard: the motion's residual per block (prev at q + v against cur at q, 5x5, mean absolute luma difference), mapped to 0 (trusted) .. 1 (not)
kernel void conf(texture2d<float> cur [[texture(0)]], texture2d<float> prev [[texture(1)]], texture2d<float> mv [[texture(2)]], texture2d<float, access::write> dst [[texture(3)]],
                 device P &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {
    int2 lim = int2(cur.get_width(), cur.get_height()) - 1;
    if (g.x > uint(lim.x) || g.y > uint(lim.y)) return;
    float2 vf = mv.read(g).xy;
    int2 d = int2(round(vf));
    atomic_fetch_add_explicit((device atomic_int *)&p.mvs, int(min(length(vf), 512.0) * 16.0), memory_order_relaxed);   // the motion's size, for the fast-turn fallback
    atomic_fetch_add_explicit((device atomic_int *)&p.gx, int(clamp(vf.x, -512.0, 512.0) * 16.0), memory_order_relaxed);   // and its mean, the camera turn's motion
    atomic_fetch_add_explicit((device atomic_int *)&p.gy, int(clamp(vf.y, -512.0, 512.0) * 16.0), memory_order_relaxed);
    float r = sad(cur, prev, int2(g), d, 2, lim) / 25.0;
    float m = 0, a = 0;   // the patch's own contrast: residual relative to it, so a bright or detailed place does not count as a bad match
    for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) m += px(cur, int2(g) + int2(x, y), lim);
    m /= 25.0;
    for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) a += fabs(px(cur, int2(g) + int2(x, y), lim) - m);
    a /= 25.0;
    float rel = r / (a + 0.004 + 0.02 * m), u = smoothstep(p.lambda, p.zbias, rel);
    if (u > 0.5) atomic_fetch_add_explicit((device atomic_int *)&p.pad0, 1, memory_order_relaxed);   // untrusted blocks, for the whole-picture fallback (bail)
    // A still overlay (the HUD) over a moving scene: the block matches itself in place far better than the motion found for it, so the frame is shown
    // there (and, by guard's dilation, one block around) instead of the overlay moved with the scene. DM_FG_STILL (relative residual at zero motion, 0 = off)
    if (p.sub > 0 && length(vf) >= 1.0 && a > 0.01) {
        float r0 = sad(cur, prev, int2(g), int2(0), 2, lim) / 25.0;
        if (r0 / (a + 0.004 + 0.02 * m) < float(p.sub) * 0.01 && r0 < 0.5 * r) u = 2.0;   // 2: still (pin below), shown as the frame
    }
    dst.write(float4(u), g);
}
// Whole-picture fallback: when more than pad1 / 1000 of the blocks are untrusted (a fast camera turn, beyond what the motion search follows), the frame itself
// is shown on the generated tick instead of a picture that is part generated and part frame (seams that look like tearing). DM_FG_BAIL (per mille, 0 = off)
// The same when the mean motion between the two frames is over mvl / 1000 of the picture's width (a fast camera turn: foliage and haze no longer match block
// for block, and the in-between picture breaks into slices). DM_FG_MVBAIL (per mille of the width, 0 = off)
static inline float bail(constant P &q, texture2d<float> cf) {
    float n = float(cf.get_width() * cf.get_height());
    if (q.pad1 > 0 && float(q.pad0) > float(q.pad1) * 0.001 * n) return 1.0;
    if (q.mvl > 0 && float(q.mvs) / 16.0 / n > float(q.mvl) * 0.001 * float(cf.get_width())) return 1.0;
    return 0.0;
}
// Fast motion: the in-between picture made here instead of by the interpolator, whose output breaks into slices then. Per pixel, of three motions (none:
// the HUD and Link held on screen; the picture's mean: the camera turn; the motion found here) the one under which the two frames agree best over a 3x3
// window is taken, and the frames are blended, each moved half of it toward the middle. No blocks, so no slices; where nothing fits, a soft blend
static inline float lum(float4 c) { return dot(c.rgb, float3(0.299, 0.587, 0.114)); }
static float4 warp(texture2d<float> a, texture2d<float> b, texture2d<float> mv, constant P &q, uint2 g, float2 dsz) {
    constexpr sampler s(filter::linear, address::clamp_to_edge, coord::normalized);
    float2 msz = float2(mv.get_width(), mv.get_height()), uv = (float2(g) + 0.5) / dsz, px1 = 1.0 / dsz;
    float n = msz.x * msz.y;
    float2 c[3] = {float2(0), float2(q.gx, q.gy) / 16.0 / n / msz, mv.sample(s, uv).xy / msz};
    float best = 1e9; int k = 0;
    for (int i = 0; i < 3; i++) {
        float2 h = 0.5 * c[i]; float e = 0;
        for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
            float2 o = float2(x, y) * px1 * 2.0;
            e += fabs(lum(a.sample(s, uv + h + o)) - lum(b.sample(s, uv - h + o)));
        }
        e *= i == 0 ? 0.8 : 1.0;   // a little in favour of no motion, so a still overlay stays put
        if (e < best) { best = e; k = i; }
    }
    float2 h = 0.5 * c[k];
    return 0.5 * (a.sample(s, uv + h) + b.sample(s, uv - h));
}
// Catmull-Rom from 9 bilinear taps: sharper than one bilinear tap at a fractional position
static float4 samp_cr(texture2d<float> t, float2 uv) {
    constexpr sampler s(filter::linear, address::clamp_to_edge, coord::normalized);
    float2 sz = float2(t.get_width(), t.get_height()), p = uv * sz - 0.5, tc = floor(p) + 0.5, f = p - floor(p);
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f), w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2, o12 = w2 / w12, t0 = (tc - 1.0) / sz, t3 = (tc + 2.0) / sz, t12 = (tc + o12) / sz;
    float4 r = 0;
    r += t.sample(s, float2(t0.x, t0.y)) * w0.x * w0.y + t.sample(s, float2(t12.x, t0.y)) * w12.x * w0.y + t.sample(s, float2(t3.x, t0.y)) * w3.x * w0.y;
    r += t.sample(s, float2(t0.x, t12.y)) * w0.x * w12.y + t.sample(s, float2(t12.x, t12.y)) * w12.x * w12.y + t.sample(s, float2(t3.x, t12.y)) * w3.x * w12.y;
    r += t.sample(s, float2(t0.x, t3.y)) * w0.x * w3.y + t.sample(s, float2(t12.x, t3.y)) * w12.x * w3.y + t.sample(s, float2(t3.x, t3.y)) * w3.x * w3.y;
    return max(r, 0.0);
}
// DM_FG_WARP=2: as warp, with the motions of the 3x3 blocks around as candidates too (a block's motion is off near its edges), a 3x3 cost at 1-pixel
// steps, and the frames resampled with Catmull-Rom
static float4 warp2(texture2d<float> a, texture2d<float> b, texture2d<float> mv, constant P &q, uint2 g, float2 dsz) {
    constexpr sampler s(filter::linear, address::clamp_to_edge, coord::normalized);
    constexpr sampler sn(filter::nearest, address::clamp_to_edge, coord::normalized);
    float2 msz = float2(mv.get_width(), mv.get_height()), uv = (float2(g) + 0.5) / dsz, px1 = 1.0 / dsz;
    float n = msz.x * msz.y;
    float2 best_v = 0; float best = 1e9;
    for (int i = 0; i < 11; i++) {
        float2 v = i == 0 ? float2(0) : i == 1 ? float2(q.gx, q.gy) / 16.0 / n / msz
                 : mv.sample(sn, uv + float2((i - 2) % 3 - 1, (i - 2) / 3 - 1) / msz).xy / msz;
        float2 h = 0.5 * v; float e = 0;
        for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
            float2 o = float2(x, y) * px1 * 1.5;
            e += fabs(lum(a.sample(s, uv + h + o)) - lum(b.sample(s, uv - h + o)));
        }
        e *= i == 0 ? 0.8 : i == 6 ? 0.95 : 1.0;   // a little in favour of no motion (a still overlay stays put), then of the block's own motion
        if (e < best) { best = e; best_v = v; }
    }
    if (q.patch > 0) {   // DM_FG_W2REF: the chosen motion refined per pixel, steps of DM_FG_W2REF/4 pixel around it (the motion comes from quarter-size pictures)
        float st = float(q.patch) * 0.25; float2 v0 = best_v;
        for (int r = 0; r < 2; r++) {
            float2 c0 = best_v;
            for (int i = 0; i < 9; i++) { if (i == 4) continue;
                float2 v = c0 + float2(i % 3 - 1, i / 3 - 1) * st * px1;
                float2 hh = 0.5 * v; float e = 0;
                for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) { float2 o = float2(x, y) * px1 * 1.5;
                    e += fabs(lum(a.sample(s, uv + hh + o)) - lum(b.sample(s, uv - hh + o))); }
                if (e < best * 0.98) { best = e; best_v = v; } }
            st *= 0.5; }
        if (length((best_v - v0) * dsz) > 3.0) best_v = v0;
    }
    float2 h = 0.5 * best_v;
    float4 ca = samp_cr(a, uv + h), cb = samp_cr(b, uv - h);
    if (q.coarse == 1) return cb;
    if (q.coarse == 3) {   // 3: the blend's coarse picture plus the newer frame's fine detail (two frames' grain averaged loses half of it)
        float2 d1 = px1 * 0.75, d2 = float2(d1.x, -d1.y);
        float4 la = 0.25 * (a.sample(s, uv + h + d1) + a.sample(s, uv + h - d1) + a.sample(s, uv + h + d2) + a.sample(s, uv + h - d2));
        float4 lb = 0.25 * (b.sample(s, uv - h + d1) + b.sample(s, uv - h - d1) + b.sample(s, uv - h + d2) + b.sample(s, uv - h - d2));
        return max(0.5 * (la + lb) + (cb - lb), 0.0); }                                  // DM_FG_W2MODE 1: the newer frame only
    if (q.coarse == 2) { float d = fabs(lum(ca) - lum(cb)); float w = smoothstep(0.02, 0.10, d); return mix(0.5 * (ca + cb), cb, w); }   // 2: blend where they agree, else the newer
    return 0.5 * (ca + cb);
}
// DM_FG_WARP=3, pass 1: per 2x2 pixels of the generation size, the motion (of none, the picture's mean, and the 3x3 blocks' around) under which the two frames
// agree best over a 3x3 window; written in uv units
kernel void pick(texture2d<float> a [[texture(0)]], texture2d<float> b [[texture(1)]], texture2d<float> mv [[texture(2)]], texture2d<float, access::write> dst [[texture(3)]],
                 constant P &q [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler s(filter::linear, address::clamp_to_edge, coord::normalized);
    constexpr sampler sn(filter::nearest, address::clamp_to_edge, coord::normalized);
    float2 msz = float2(mv.get_width(), mv.get_height()), uv = (float2(g) + 0.5) / float2(dst.get_width(), dst.get_height()), px1 = 1.0 / float2(a.get_width(), a.get_height());
    float n = msz.x * msz.y;
    float2 best_v = 0; float best = 1e9;
    for (int i = 0; i < 11; i++) {
        float2 v = i == 0 ? float2(0) : i == 1 ? float2(q.gx, q.gy) / 16.0 / n / msz
                 : mv.sample(sn, uv + float2((i - 2) % 3 - 1, (i - 2) / 3 - 1) / msz).xy / msz;
        float2 h = 0.5 * v; float e = 0;
        for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
            float2 o = float2(x, y) * px1 * 1.5;
            e += fabs(lum(a.sample(s, uv + h + o)) - lum(b.sample(s, uv - h + o)));
        }
        e *= i == 0 ? 0.8 : i == 6 ? 0.95 : 1.0;
        if (e < best) { best = e; best_v = v; }
    }
    dst.write(float4(best_v, best / 9.0, 0), g);
}
// DM_FG_WARP=3, pass 2, at the picture's size: both frames moved half the picked motion toward the middle (Catmull-Rom); the coarse picture is their blend and
// the fine detail the newer frame's (two frames' grain averaged loses half of it, and the picture then looks softer every other tick)
kernel void interp(texture2d<float> a [[texture(0)]], texture2d<float> b [[texture(1)]], texture2d<float> ms [[texture(2)]], texture2d<float, access::write> dst [[texture(3)]],
                   constant P &q [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    constexpr sampler s(filter::linear, address::clamp_to_edge, coord::normalized);
    constexpr sampler sn(filter::nearest, address::clamp_to_edge, coord::normalized);
    float2 dsz = float2(dst.get_width(), dst.get_height()), uv = (float2(g) + 0.5) / dsz, px1 = 1.0 / dsz;
    float2 h;
    if (q.patch == 1) h = 0.5 * ms.sample(s, uv).xy;   // DM_FG_W3SMOOTH 1: the picked motion interpolated between tiles
    else if (q.patch >= 2) {   // 2: of the 2x2 nearest tiles' motions, the one the two frames agree best under at this pixel (3x3, 1-pixel steps)
        float2 msz = float2(ms.get_width(), ms.get_height()), p = uv * msz - 0.5, b0 = (floor(p) + 0.5) / msz; float best = 1e9; h = 0;
        for (int i = 0; i < 4; i++) {
            float2 hh = 0.5 * ms.sample(sn, b0 + float2(i & 1, i >> 1) / msz).xy; float e = 0;
            for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) { if (q.patch == 3 && x != 0 && y != 0) continue;   // 3: a plus of 5 samples, not 3x3
                float2 o = float2(x, y) * px1;
                e += fabs(lum(a.sample(s, uv + hh + o)) - lum(b.sample(s, uv - hh + o))); }
            if (e < best) { best = e; h = hh; } }
    } else h = 0.5 * ms.sample(sn, uv).xy;
    float4 ca = samp_cr(a, uv + h), cb = samp_cr(b, uv - h);
    float k = float(q.coarse) * 0.01;   // DM_FG_DETAIL3: how much of the newer frame's fine detail (percent; 0: the plain blend)
    if (k <= 0) { dst.write(0.5 * (ca + cb), g); return; }
    float2 d1 = px1 * 0.75, d2 = float2(d1.x, -d1.y);
    float4 la = 0.25 * (a.sample(s, uv + h + d1) + a.sample(s, uv + h - d1) + a.sample(s, uv + h + d2) + a.sample(s, uv + h - d2));
    float4 lb = 0.25 * (b.sample(s, uv - h + d1) + b.sample(s, uv - h - d1) + b.sample(s, uv - h + d2) + b.sample(s, uv - h - d2));
    dst.write(max(0.5 * (ca + cb) + k * (0.5 * (la + lb) + (cb - lb) - 0.5 * (ca + cb)), 0.0), g);
}
// The generated picture, with untrusted places (the worst block within one block around) faded to the frame itself
kernel void guard(texture2d<float> gen [[texture(0)]], texture2d<float> cur [[texture(1)]], texture2d<float> cf [[texture(2)]], texture2d<float, access::write> dst [[texture(3)]],
                  texture2d<float> prv [[texture(4)]], texture2d<float> mv [[texture(5)]], constant P &q [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    if (bail(q, cf) > 0.5) { dst.write(q.R == 2 ? warp2(prv, cur, mv, q, g, float2(dst.get_width(), dst.get_height())) : q.R > 0 ? warp(prv, cur, mv, q, g, float2(dst.get_width(), dst.get_height())) : cur.read(g), g); return; }
    constexpr sampler s(filter::linear, address::clamp_to_edge, coord::normalized);
    float2 uv = (float2(g) + 0.5) / float2(dst.get_width(), dst.get_height()), o = 1.0 / float2(cf.get_width(), cf.get_height());
    float m = 0;
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) m = max(m, cf.sample(s, uv + float2(x, y) * o).r);
    m = saturate(max(m, bail(q, cf)));
    dst.write(mix(gen.read(g), cur.read(g), m), g);
}
// Detail: the generation runs smaller than the picture, so its fine detail is gone. The frame's own detail (full size minus its smaller copy), moved by half the
// motion to where the in-between picture has it, is added to the generated picture scaled up; untrusted places take the frame itself, as guard does
kernel void detail(texture2d<float> gen [[texture(0)]], texture2d<float> full [[texture(1)]], texture2d<float> low [[texture(2)]], texture2d<float> mv [[texture(3)]],
                   texture2d<float> cf [[texture(4)]], texture2d<float, access::write> dst [[texture(5)]], texture2d<float> prv [[texture(6)]], constant P &p [[buffer(0)]],
                   constant P &q [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    if (bail(q, cf) > 0.5) { dst.write(q.R == 2 ? warp2(prv, full, mv, q, g, float2(dst.get_width(), dst.get_height())) : q.R > 0 ? warp(prv, full, mv, q, g, float2(dst.get_width(), dst.get_height())) : full.read(g), g); return; }
    constexpr sampler s(filter::linear, address::clamp_to_edge, coord::normalized);
    float2 uv = (float2(g) + 0.5) / float2(dst.get_width(), dst.get_height());
    float2 msz = float2(mv.get_width(), mv.get_height());
    float2 v = mv.sample(s, uv).xy / msz;            // the motion at this place, in uv: the frame's content here was at +v in the frame before
    float2 us = uv - 0.5 * v;                         // half way back: where the frame has what the in-between picture shows here
    float4 d = full.sample(s, us) - low.sample(s, us);
    float4 c = gen.sample(s, uv) + d * p.lambda;
    float m = 0; float2 o = 1.0 / float2(cf.get_width(), cf.get_height());
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) m = max(m, cf.sample(s, uv + float2(x, y) * o).r);
    m = saturate(max(m, bail(q, cf)));
    dst.write(mix(saturate(c), full.read(g), m), g);
}
// Still blocks (conf 2) and the blocks next to them get no motion, so the interpolator does not carry the overlay's pixels out with the scene
kernel void pin(texture2d<float> mv [[texture(0)]], texture2d<float> cf [[texture(1)]], texture2d<float, access::write> dst [[texture(2)]], uint2 g [[thread_position_in_grid]]) {
    int2 lim = int2(mv.get_width(), mv.get_height()) - 1;
    if (g.x > uint(lim.x) || g.y > uint(lim.y)) return;
    float s = 0;
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) s = max(s, cf.read(uint2(clamp(int2(g) + int2(x, y), int2(0), lim))).r);
    dst.write(s > 1.5 ? float4(0) : mv.read(g), g);
}
kernel void fill(texture2d<float, access::write> dst [[texture(0)]], constant P &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    dst.write(float4(p.lambda), g);
}
)MSL";

static id<MTLComputePipelineState> fg_pso(id<MTLDevice> dev, id<MTLLibrary> lib, NSString *name) {
    NSError *e = nil; id<MTLFunction> f = [lib newFunctionWithName:name];
    id<MTLComputePipelineState> ps = f ? [dev newComputePipelineStateWithFunction:f error:&e] : nil;
    if (!ps) NSLog(@"framegen: pipeline %@ failed: %@", name, e);
    return ps;
}
static id<MTLTexture> fg_tex(fg_ctx *c, MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage u) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.storageMode = MTLStorageModePrivate; d.usage = u;
    id<MTLTexture> t = [c->dev newTextureWithDescriptor:d];
    if (t) [c->res addAllocation:t];
    return t;
}
static int fg_env_i(const char *k, int def) { const char *e = getenv(k); return e && *e ? atoi(e) : def; }
static float fg_env_f(const char *k, float def) { const char *e = getenv(k); return e && *e ? (float)atof(e) : def; }

// w x h: the presented picture's size; fmt: its format (also the generated picture's)
static fg_ctx *fg_create(id<MTLDevice> dev, NSUInteger w, NSUInteger h, MTLPixelFormat fmt, NSString **why) {
    if (![MTLFXFrameInterpolatorDescriptor supportsMetal4FX:dev]) { *why = @"MetalFX frame interpolation (Metal 4) is not supported on this GPU"; return NULL; }
    fg_ctx *c = calloc(1, sizeof *c); c->dev = dev; c->w = w; c->h = h; c->fmt = fmt;
    c->R0 = fg_env_i("DM_FG_R0", 4); c->Rn = fg_env_i("DM_FG_RN", 1); c->patch = fg_env_i("DM_FG_PATCH", 1); c->median = fg_env_i("DM_FG_MEDIAN", 1);
    c->lambda = fg_env_f("DM_FG_LAMBDA", 0.02f);
    c->near_ = 0.1f; c->far_ = 1000.f; c->fov = 60.f; c->dt = 1.f / 30.f;
    NSError *e = nil;
    MTLCompileOptions *o = [MTLCompileOptions new];
    id<MTLLibrary> lib = [dev newLibraryWithSource:fg_msl options:o error:&e];
    if (!lib) { *why = [NSString stringWithFormat:@"shaders: %@", e]; free(c); return NULL; }
    c->ps_luma0 = fg_pso(dev, lib, @"luma0"); c->ps_down = fg_pso(dev, lib, @"down2"); c->ps_search = fg_pso(dev, lib, @"search");
    c->ps_median = fg_pso(dev, lib, @"median3"); c->ps_fill = fg_pso(dev, lib, @"fill"); c->ps_resample = fg_pso(dev, lib, @"resample");
    c->ps_conf = fg_pso(dev, lib, @"conf"); c->ps_guard = fg_pso(dev, lib, @"guard"); c->ps_detail = fg_pso(dev, lib, @"detail"); c->ps_pin = fg_pso(dev, lib, @"pin"); c->ps_pick = fg_pso(dev, lib, @"pick"); c->ps_interp = fg_pso(dev, lib, @"interp");
    if (!c->ps_pick || !c->ps_interp || !c->ps_pin || !c->ps_detail || !c->ps_conf || !c->ps_guard || !c->ps_luma0 || !c->ps_down || !c->ps_search || !c->ps_median || !c->ps_fill || !c->ps_resample) { *why = @"pipelines"; free(c); return NULL; }
    MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new]; rd.label = @"framegen";
    c->res = [dev newResidencySetWithDescriptor:rd error:&e];
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 7; ad.maxBufferBindCount = 2;
    c->at = [dev newArgumentTableWithDescriptor:ad error:&e];
    if (!c->res || !c->at) { *why = @"residency set / argument table"; free(c); return NULL; }
    NSUInteger lw = (w + 3) / 4, lh = (h + 3) / 4;
    for (int l = 0; l < FG_LEVELS; l++) { c->lw[l] = lw; c->lh[l] = lh; lw = (lw + 1) / 2; lh = (lh + 1) / 2; }
    MTLFXFrameInterpolatorDescriptor *d = [MTLFXFrameInterpolatorDescriptor new];
    d.colorTextureFormat = fmt; d.outputTextureFormat = fmt; d.depthTextureFormat = MTLPixelFormatR32Float; d.motionTextureFormat = MTLPixelFormatRG16Float;
    d.inputWidth = c->lw[0]; d.inputHeight = c->lh[0]; d.outputWidth = w; d.outputHeight = h;
    id<MTL4Compiler> comp = [dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&e];
    c->fi = comp ? [d newFrameInterpolatorWithDevice:dev compiler:comp] : nil;
    if (!c->fi) {   // a depth format, if a colour one is refused
        d.depthTextureFormat = MTLPixelFormatDepth32Float;
        c->fi = comp ? [d newFrameInterpolatorWithDevice:dev compiler:comp] : nil;
    }
    if (!c->fi) { *why = @"MetalFX made no frame interpolator for these formats"; free(c); return NULL; }
    MTLTextureUsage ru = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    for (int i = 0; i < 2; i++) {
        c->col[i] = fg_tex(c, fmt, w, h, c->fi.colorTextureUsage | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
        for (int l = 0; l < FG_LEVELS; l++) c->pyr[i][l] = fg_tex(c, MTLPixelFormatR16Float, c->lw[l], c->lh[l], ru);
    }
    for (int l = 0; l < FG_LEVELS; l++) c->mv[l] = fg_tex(c, MTLPixelFormatRG16Float, c->lw[l], c->lh[l], ru | c->fi.motionTextureUsage);
    c->mvf = fg_tex(c, MTLPixelFormatRG16Float, c->lw[0], c->lh[0], ru | c->fi.motionTextureUsage);
    c->mvp = fg_tex(c, MTLPixelFormatRG16Float, c->lw[0], c->lh[0], ru | c->fi.motionTextureUsage);
    MTLPixelFormat df = c->fi.depthTextureFormat;
    c->depth = fg_tex(c, df, c->lw[0], c->lh[0], c->fi.depthTextureUsage | (df == MTLPixelFormatR32Float ? MTLTextureUsageShaderWrite : MTLTextureUsageRenderTarget));
    c->out = fg_tex(c, fmt, w, h, c->fi.outputTextureUsage | MTLTextureUsageShaderRead);
    c->conf = fg_tex(c, MTLPixelFormatR16Float, c->lw[0], c->lh[0], ru);
    c->msel = fg_tex(c, MTLPixelFormatRGBA32Float, (w + 1) / 2, (h + 1) / 2, ru);
    c->gout = fg_tex(c, fmt, w, h, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
    c->params = [dev newBufferWithLength:sizeof(fg_params) * 16 options:MTLResourceStorageModeShared];
    [c->res addAllocation:c->params]; [c->res commit];
    for (int i = 0; i < 2; i++) if (!c->col[i]) { *why = @"textures"; free(c); return NULL; }
    if (!c->depth || !c->out || !c->mvf || !c->mvp || !c->conf || !c->gout || !c->msel) { *why = @"textures"; free(c); return NULL; }
    return c;
}

static void fg_disp(id<MTL4ComputeCommandEncoder> ce, fg_ctx *c, id<MTLComputePipelineState> ps, NSArray<id<MTLTexture>> *t, int pi, NSUInteger w, NSUInteger h) {
    [ce setComputePipelineState:ps];
    for (NSUInteger i = 0; i < t.count; i++) [c->at setTexture:t[i].gpuResourceID atIndex:i];
    if (pi >= 0) [c->at setAddress:c->params.gpuAddress + sizeof(fg_params) * (NSUInteger)pi atIndex:0];
    [ce setArgumentTable:c->at];
    [ce dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(16, 8, 1)];
    [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
}

// Takes src (the newly presented picture, w x h, fmt) into the history and, when there is an older one, encodes the picture between them into c->out.
// Returns c->out when one was made, else nil. *copy: the history copy of src (stays valid until the next fg_encode).
static id<MTLTexture> fg_encode(fg_ctx *c, id<MTL4CommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> *copy, int reset) {
    int i = c->cur ^= 1, j = i ^ 1;
    if (reset) c->have_prev = 0;
    [cb useResidencySet:c->res];
    id<MTL4ComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit | MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
    fg_params *pp = c->params.contents;
    if (src.width == c->w && src.height == c->h && src.pixelFormat == c->fmt) {
        [ce copyFromTexture:src toTexture:c->col[i]];
        [ce barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
    } else {   // the generation runs smaller than the picture; the held tick still shows the picture at its own size
        if (!c->full[i] || c->full[i].width != src.width || c->full[i].height != src.height || c->full[i].pixelFormat != src.pixelFormat) {
            c->full[i] = fg_tex(c, src.pixelFormat, src.width, src.height, MTLTextureUsageShaderRead); [c->res commit];
        }
        if (c->full[i]) [ce copyFromTexture:src toTexture:c->full[i]];
        fg_disp(ce, c, c->ps_resample, @[src, c->col[i]], -1, c->w, c->h);
    }
    if (!c->depth_ready) { pp[15] = (fg_params){.lambda = 0.5f}; fg_disp(ce, c, c->ps_fill, @[c->depth], 15, c->lw[0], c->lh[0]); c->depth_ready = 1; }
    fg_disp(ce, c, c->ps_luma0, @[c->col[i], c->pyr[i][0]], -1, c->lw[0], c->lh[0]);
    for (int l = 1; l < FG_LEVELS; l++) fg_disp(ce, c, c->ps_down, @[c->pyr[i][l - 1], c->pyr[i][l]], -1, c->lw[l], c->lh[l]);
    if (copy) *copy = c->full[i] && c->full[i].width == src.width && c->full[i].height == src.height ? c->full[i] : c->col[i];
    if (!c->have_prev) { [ce endEncoding]; c->have_prev = 1; return nil; }
    int nosearch = fg_env_i("DM_FG_NOSEARCH", 0);
    if (nosearch) { pp[14] = (fg_params){.lambda = 0}; fg_disp(ce, c, c->ps_fill, @[c->mv[0]], 14, c->lw[0], c->lh[0]); }
    else for (int l = FG_LEVELS - 1; l >= 0; l--) {
        pp[l] = (fg_params){.R = l == FG_LEVELS - 1 ? c->R0 : c->Rn, .coarse = l != FG_LEVELS - 1, .patch = c->patch, .sub = l == 0, .lambda = c->lambda, .zbias = c->lambda * 0.25f};
        fg_disp(ce, c, c->ps_search, @[c->pyr[i][l], c->pyr[j][l], c->mv[l == FG_LEVELS - 1 ? l : l + 1], c->mv[l]], l, c->lw[l], c->lh[l]);
    }
    id<MTLTexture> motion = c->mv[0];
    if (c->median && !nosearch) { fg_disp(ce, c, c->ps_median, @[c->mv[0], c->mvf], -1, c->lw[0], c->lh[0]); motion = c->mvf; }
    int guard = fg_env_i("DM_FG_GUARD", 1);
    if (guard && pp[13].mvl > 0 && (double)pp[13].mvs / 16.0 / (double)(c->lw[0] * c->lh[0]) > pp[13].mvl * 0.001 * (double)c->lw[0]) c->bails++;
    if (guard) { pp[13] = (fg_params){.R = fg_env_i("DM_FG_WARP", 3), .coarse = fg_env_i("DM_FG_WARP", 3) == 3 ? fg_env_i("DM_FG_DETAIL3", 100) : fg_env_i("DM_FG_W2MODE", 0), .patch = fg_env_i("DM_FG_WARP", 3) == 3 ? fg_env_i("DM_FG_W3SMOOTH", 3) : fg_env_i("DM_FG_W2REF", 0), .lambda = fg_env_f("DM_FG_GT0", 0.6f), .zbias = fg_env_f("DM_FG_GT1", 1.2f), .pad = {0, fg_env_i("DM_FG_BAIL", 0)}, .sub = fg_env_i("DM_FG_STILL", 30), .mvs = 0, .mvl = fg_env_i("DM_FG_MVBAIL", 12), .gx = 0, .gy = 0};
        fg_disp(ce, c, c->ps_conf, @[c->pyr[i][0], c->pyr[j][0], motion, c->conf], 13, c->lw[0], c->lh[0]);
        if (pp[13].sub > 0) { fg_disp(ce, c, c->ps_pin, @[motion, c->conf, c->mvp], -1, c->lw[0], c->lh[0]); motion = c->mvp; } }
    if (guard && pp[13].R == 3) {   // DM_FG_WARP=3: the in-between picture made here at the picture's size, MetalFX not used
        id<MTLTexture> A = c->col[j], B = c->col[i];
        if (c->full[i] && c->full[j] && c->full[j].width == c->full[i].width && c->full[j].height == c->full[i].height) { A = c->full[j]; B = c->full[i]; }
        int full = A != c->col[j]; id<MTLTexture> D = full ? c->gfull : c->gout;
        if (!D || D.width != B.width || D.height != B.height || D.pixelFormat != B.pixelFormat) {
            D = fg_tex(c, B.pixelFormat, B.width, B.height, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite); [c->res commit];
            if (full) c->gfull = D; else c->gout = D; }
        [c->at setAddress:c->params.gpuAddress + sizeof(fg_params) * 13 atIndex:1];
        fg_disp(ce, c, c->ps_pick, @[c->col[j], c->col[i], motion, c->msel], -1, c->msel.width, c->msel.height);
        fg_disp(ce, c, c->ps_interp, @[A, B, c->msel, D], -1, B.width, B.height);
        [ce barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
        [ce endEncoding];
        return D;
    }
    [ce barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [ce endEncoding];
    if (fg_env_i("DM_FG_NOFI", 0)) return c->col[i];
    id<MTL4FXFrameInterpolator> fi = c->fi;
    fi.colorTexture = c->col[i]; fi.prevColorTexture = c->col[j]; fi.motionTexture = motion; fi.depthTexture = c->depth; fi.outputTexture = c->out;
    fi.motionVectorScaleX = fg_env_f("DM_FG_MVSCALE", 1.f); fi.motionVectorScaleY = fg_env_f("DM_FG_MVSCALEY", fi.motionVectorScaleX);
    fi.deltaTime = c->dt; fi.nearPlane = c->near_; fi.farPlane = c->far_; fi.fieldOfView = c->fov; fi.aspectRatio = (float)c->w / c->h;
    fi.jitterOffsetX = 0; fi.jitterOffsetY = 0; fi.shouldResetHistory = reset; fi.depthReversed = YES;
    [fi encodeToCommandBuffer:cb];
    int det = guard && fg_env_i("DM_FG_DETAIL", 1) && c->full[i] && (c->full[i].width != c->w || c->full[i].height != c->h);
    if (!guard) return c->out;
    ce = [cb computeCommandEncoder];
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
    [c->at setAddress:c->params.gpuAddress + sizeof(fg_params) * 13 atIndex:1];   // the untrusted-block count and motion sum conf made (bail)
    if (det) {
        id<MTLTexture> F = c->full[i];
        if (!c->gfull || c->gfull.width != F.width || c->gfull.height != F.height || c->gfull.pixelFormat != F.pixelFormat) {
            c->gfull = fg_tex(c, F.pixelFormat, F.width, F.height, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite); [c->res commit];
        }
        if (c->gfull) {
            pp[12] = (fg_params){.lambda = fg_env_f("DM_FG_DETAIL_GAIN", 1.0f)};
            fg_disp(ce, c, c->ps_detail, @[c->out, F, c->col[i], motion, c->conf, c->gfull, c->full[j] && c->full[j].width == F.width && c->full[j].height == F.height ? c->full[j] : F], 12, F.width, F.height);
            [ce barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
            [ce endEncoding];
            return c->gfull;
        }
    }
    fg_disp(ce, c, c->ps_guard, @[c->out, c->col[i], c->conf, c->gout, c->col[j], motion], -1, c->w, c->h);
    [ce barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [ce endEncoding];
    return c->gout;
}
