// The picture for one eye of a headset, on a screen in front of the player (dm/host/immersive_mac.m), from the present's picture and the depth of its scene (nvn_mtl.m, the stereo tap).
// A depth buffer's value is affine in 1 / distance, so the distance of a pixel from its far plane (w) is proportional to 1 / distance whatever the game's near and far planes are:
// the picture's horizontal parallax, far * (1 - w / conv), needs no constant of the game. Each eye's picture is the source shifted by half its parallax; fs searches the source pixel
// along the row from the nearest parallax to the farthest, so a near surface covers a far one, and what one eye sees behind an edge is filled from the far side.
// A HUD lies over the scene's depth, so it would bend with the scene: with the picture before the game's overlays (base), a pixel where the two differ is the HUD's and is
// drawn whole at the nearest parallax, in front of everything; the scene's pixels come from the base, so no HUD colour is moved with the scene
#include <metal_stdlib>
using namespace metal;
struct U {
    float4x4 mvp;   // the screen's corners (-1..1, -1..1, 0) to clip space
    float4 drect;   // the scene in the depth texture: origin, size (the size's y negative for a bottom-up one)
    float4 par;     // eye (-0.5 left, 0.5 right), far parallax and near limit (fractions of the picture's width), conv (w at the screen's plane; 0: no depth, flat)
    uint4 opt;      // the render target layer, the depth's far plane is 0, the base marks the overlays (HUD)
};
struct V { float4 p [[position]]; float2 uv; uint layer [[render_target_array_index]]; };
vertex V vs(uint id [[vertex_id]], constant U &u [[buffer(0)]]) {
    float2 uv = float2(id & 1, id >> 1);
    V o; o.p = u.mvp * float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1); o.uv = uv; o.layer = u.opt.x; return o;
}
static float near_w(depth2d<float> d, sampler s, float2 uv, U u) {
    float a = d.sample(s, saturate(uv) * u.drect.zw + u.drect.xy);
    return u.opt.y ? a : 1 - a;
}
// The present is opaque: alpha may hold a scene mask or change in a post-process
// without changing its visible picture. Only RGB differences can mark a HUD pixel;
// using alpha moves those invisible mask regions to the HUD's depth and splits geometry.
// The alignment word (align_depth): bit 0 the depth is mapped bottom-up, bit 1 this frame's base is upside down against the picture; bit 8 (a byte of its own, which the
// host's fill leaves alone) the last base whose orientation showed was upside down
#define ALIGN_DEPTH_FLIP 1u
#define ALIGN_BASE_FLIP 2u
#define ALIGN_BASE_HELD 0x100u
static float2 base_uv(float2 uv, uint align) { return align & ALIGN_BASE_FLIP ? float2(uv.x, 1 - uv.y) : uv; }
static bool overlay(texture2d<float> c, texture2d<float> b, sampler s, float2 uv, uint align) { return any(abs(c.sample(s, uv).rgb - b.sample(s, base_uv(uv, align)).rgb) > 1e-4); }
static float parallax(depth2d<float> d, sampler s, float2 uv, U u) { return clamp(u.par.y * (1 - near_w(d, s, uv, u) / u.par.w), -u.par.z, u.par.y); }
// Match the scene before postprocessing to the present, rather than assuming its
// offscreen render target has the window's orientation. Work on a small grid; log
// luminance tolerates exposure/tone mapping. Both images use the same 4x4 cell taps.
static float cell_luma(texture2d<float> t, float2 uv, float2 step) {
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    float sum = 0;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        float3 c = max(t.sample(s, uv + (float2(x, y) - 1.5) * step * 0.25).rgb, 0.0);
        sum += log2(1 + dot(c, float3(0.2126, 0.7152, 0.0722)));
    }
    return sum / 16;
}
kernel void reference(texture2d<float> scene [[texture(0)]], texture2d<float, access::write> out [[texture(1)]],
                      constant float4 &rect [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
    if (p.x >= out.get_width() || p.y >= out.get_height()) return;
    float2 size(out.get_width(), out.get_height());
    out.write(float4(cell_luma(scene, rect.xy + (float2(p) + 0.5) / size * rect.zw, rect.zw / size)), p);
}
// The base is the texture an engine composites into the window, copied as it was before its overlays (the tap's st_feed); a composite may flip it (Unity's final blit on
// Untitled Goose Game), so its orientation is the one that matches the picture. The scene's luminance (the base so oriented, else the picture) then chooses the depth's mapping
kernel void align_depth(texture2d<float> scene [[texture(0)]], texture2d<float> ref [[texture(1)]], texture2d<float> pic [[texture(2)]],
                        device uint &align [[buffer(0)]], constant uint4 &in [[buffer(1)]]) {   // one thread: 576 cells; in: the declared depth origin, a reference was taken, a base
    constexpr uint W = 32, H = 18;
    float2 sa = 0, saa = 0, same = 0, sab[2] = {0, 0}; float sb = 0, sbb = 0;
    for (uint i = 0; i < W * H; i++) {
        uint x = i % W, y = i / W;
        float2 step = 1.0 / float2(W, H);
        float2 a = float2(cell_luma(scene, (float2(x, y) + 0.5) * step, step), cell_luma(scene, (float2(x, H - 1 - y) + 0.5) * step, step));   // upright, upside down
        float p = cell_luma(pic, (float2(x, y) + 0.5) * step, step);
        float b = in.y ? ref.read(uint2(x, y)).r : 0, r = in.y ? ref.read(uint2(x, H - 1 - y)).r : 0;
        sa += a; saa += a * a; same += select(float2(0), float2(1), abs(a - p) < 1.0 / 64);
        sab[0] += a.x * float2(b, r); sab[1] += a.y * float2(b, r); sb += b; sbb += b * b;
    }
    float n = W * H; float2 va = saa - sa * sa / n; float vb = sbb - sb * sb / n;
    // The cells where the base equals the picture: a HUD differs in both orientations and so cannot tip it (a bright one would swamp a correlation), cells alike both
    // ways (a flat sky) hold no evidence. A composite's orientation is its engine's, the same every frame: a frame with too little evidence (a logo centred on a flat
    // background) keeps the last one
    uint up = in.z && (same.y > same.x + 2 ? 1 : same.x > same.y + 2 ? 0 : (align & ALIGN_BASE_HELD) != 0);
    float v = up ? va.y : va.x, s = up ? sa.y : sa.x; float2 corr = (sab[up] - s * sb / n) * rsqrt(max(v * vb, 1e-12));
    // A flat/faded or unrelated target provides no orientation evidence. Do not
    // overturn the declared origin unless one mapping has a strong, distinct match.
    uint flip = in.x;
    if (in.y && v > 1e-6 && vb > 1e-6 && max(corr.x, corr.y) > 0.8 && abs(corr.x - corr.y) > 0.2)
        flip = corr.y > corr.x;
    align = (flip ? ALIGN_DEPTH_FLIP : 0) | (up ? ALIGN_BASE_FLIP : 0) | (in.z ? (up ? ALIGN_BASE_HELD : 0) : align & ALIGN_BASE_HELD);
}
fragment float4 fs(V in [[stage_in]], texture2d<float> c [[texture(0)]], depth2d<float> d [[texture(1)]], texture2d<float> b [[texture(2)]], sampler ls [[sampler(0)]], sampler ns [[sampler(1)]],
                   constant U &input [[buffer(0)]], constant uint &align [[buffer(1)]]) {
    U u = input;
    if (bool(align & ALIGN_DEPTH_FLIP) != (u.drect.w < 0)) { u.drect.y += u.drect.w; u.drect.w = -u.drect.w; }
    float2 uv = in.uv;
    if (u.par.w > 0) {
        const int N = 24;
        float lo = -u.par.z, hi = u.par.y, e = u.par.x, ph = lo, pv = parallax(d, ns, uv - float2(e * lo, 0), u), hit = lo;
        if (pv > lo) {
            hit = hi;
            for (int i = 1; i <= N; i++) {
                float h = mix(lo, hi, float(i) / N), v = parallax(d, ns, uv - float2(e * h, 0), u);
                if (v <= h) { float a0 = pv - ph, a1 = v - h; hit = mix(ph, h, saturate(a0 / max(a0 - a1, 1e-6))); break; }
                ph = h; pv = v;
            }
        }
        if (u.opt.z) { float2 h = saturate(uv - float2(e * lo, 0)); if (overlay(c, b, ns, h, align)) return c.sample(ls, h); }   // the HUD, in front
        uv.x -= e * hit;
        if (u.opt.z) return b.sample(ls, base_uv(saturate(uv), align));
    }
    return c.sample(ls, saturate(uv));
}
// The nearest w of each cell of a small grid (4 x 4 taps a cell), and the share of its taps that are overlays, read back to place the screen's plane in the scene and to tell a HUD
// from an effect over the whole picture (immersive_mac.m conv_update)
fragment float2 fs_near(V in [[stage_in]], texture2d<float> c [[texture(0)]], depth2d<float> d [[texture(1)]], texture2d<float> b [[texture(2)]], sampler ns [[sampler(1)]],
                        constant U &u [[buffer(0)]], constant uint &align [[buffer(1)]]) {
    float2 cell = float2(dfdx(in.uv.x), dfdy(in.uv.y)); float m = 0, o = 0;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { float2 t = in.uv + (float2(x, y) - 1.5) * 0.25 * cell; m = max(m, near_w(d, ns, t, u)); o += overlay(c, b, ns, saturate(t), align) ? 1.0 / 16 : 0; }
    return float2(m, o);
}
