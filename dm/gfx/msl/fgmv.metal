// Frame generation's motion (nvn_mtl.m fg_motion): the motion between two presented pictures, estimated here because the game gives none, as the MetalFX frame interpolator's
// motion texture. MetalFX's own flow finds motion up to about 64 px a frame at 1176x540 (and tears or gives up past 96-192 px: the picture stays on one of the two frames, a
// judder on fast camera turns); with motion vectors it places any pan exactly. Three passes at 1/8 of the picture's size:
//   fgmv_luma    each 8x8 block's mean luma (one per present, kept with its ring slot for the next present's search)
//   fgmv_search  each tile of 8x8 of those (64x64 px of the picture) searched against the previous present's luma within +-FGMV_R (+-192 px), with a sub-pixel fit
//   fgmv_pick    each 1/8 pixel takes, of its tile's vector, its 8 neighbours' and no motion, the one that matches best around it, so a still HUD or a character the camera
//                follows keeps no motion beside the panning background
// The motion texture is at 1/8 size, its vectors in its own pixels pointing to where a pixel was in the previous picture (MetalFX's convention, scale 1)
#include <metal_stdlib>
using namespace metal;

#define FGMV_T 8                       // tile size in 1/8 pixels
#define FGMV_R 24                      // search radius in 1/8 pixels
#define FGMV_P (FGMV_T + 2 * FGMV_R)   // the previous picture's window a tile searches
#define FGMV_N ((2 * FGMV_R + 1) * (2 * FGMV_R + 1))
#define FGMV_TG 256

kernel void fgmv_luma(texture2d<float, access::read> src [[texture(0)]], texture2d<half, access::write> dst [[texture(1)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= dst.get_width() || g.y >= dst.get_height()) return;
    uint2 mx = uint2(src.get_width() - 1, src.get_height() - 1), o = g * 8;
    float s = 0;
    for (uint y = 0; y < 8; y += 2) for (uint x = 0; x < 8; x += 2) {   // 16 of the 64: the mean of a block whose detail the search does not need
        float3 c = src.read(min(o + uint2(x, y), mx)).rgb;
        s += dot(c, float3(0.299, 0.587, 0.114));
    }
    dst.write(half4(half(s / 16)), g);
}

// tiles: xy the vector (1/8 pixels, pointing to the previous picture), z how far its match beats the tile's mean (0 = a flat tile, nothing to go by)
kernel void fgmv_search(texture2d<half, access::read> cur [[texture(0)]], texture2d<half, access::read> prev [[texture(1)]], texture2d<half, access::write> tiles [[texture(2)]],
                        uint2 tg [[threadgroup_position_in_grid]], uint li [[thread_index_in_threadgroup]]) {
    threadgroup half c[FGMV_T * FGMV_T];
    threadgroup half p[FGMV_P * FGMV_P];
    threadgroup float bc[FGMV_TG], bs[FGMV_TG], sm[FGMV_TG];
    threadgroup int bk[FGMV_TG];
    int2 mx = int2(cur.get_width() - 1, cur.get_height() - 1), t0 = int2(tg) * FGMV_T;
    for (uint i = li; i < FGMV_T * FGMV_T; i += FGMV_TG) c[i] = cur.read(uint2(clamp(t0 + int2(i % FGMV_T, i / FGMV_T), int2(0), mx))).r;
    for (uint i = li; i < FGMV_P * FGMV_P; i += FGMV_TG) p[i] = prev.read(uint2(clamp(t0 - FGMV_R + int2(i % FGMV_P, i / FGMV_P), int2(0), mx))).r;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float best = INFINITY, bsad = 0, sum = 0; int bkk = FGMV_N / 2;
    for (int k = (int)li; k < FGMV_N; k += FGMV_TG) {
        int ox = k % (2 * FGMV_R + 1), oy = k / (2 * FGMV_R + 1);   // window offsets (FGMV_R, FGMV_R = no motion)
        float sad = 0;
        for (int y = 0; y < FGMV_T; y++) for (int x = 0; x < FGMV_T; x++) sad += fabs(float(c[y * FGMV_T + x]) - float(p[(y + oy) * FGMV_P + x + ox]));
        sad *= 1.0f / (FGMV_T * FGMV_T);
        float cost = sad + 0.0004f * float(abs(ox - FGMV_R) + abs(oy - FGMV_R));   // ties (repeated patterns, flat areas) go to the smaller motion
        sum += sad;
        if (cost < best) { best = cost; bsad = sad; bkk = k; }
    }
    bc[li] = best; bs[li] = bsad; bk[li] = bkk; sm[li] = sum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint s = FGMV_TG / 2; s > 0; s >>= 1) {
        if (li < s) {
            if (bc[li + s] < bc[li] || (bc[li + s] == bc[li] && bk[li + s] < bk[li])) { bc[li] = bc[li + s]; bs[li] = bs[li + s]; bk[li] = bk[li + s]; }
            sm[li] += sm[li + s];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (li) return;
    int ox = bk[0] % (2 * FGMV_R + 1), oy = bk[0] / (2 * FGMV_R + 1);
    float2 v = float2(ox - FGMV_R, oy - FGMV_R);
    // sub-pixel: a parabola through the costs beside the best one
    if (ox > 0 && ox < 2 * FGMV_R && oy > 0 && oy < 2 * FGMV_R) {
        float s0 = bs[0], sx[2], sy[2];
        for (int d = 0; d < 2; d++) {
            int dd = d ? 1 : -1; float a = 0, b = 0;
            for (int y = 0; y < FGMV_T; y++) for (int x = 0; x < FGMV_T; x++) {
                float cv = float(c[y * FGMV_T + x]);
                a += fabs(cv - float(p[(y + oy) * FGMV_P + x + ox + dd])); b += fabs(cv - float(p[(y + oy + dd) * FGMV_P + x + ox]));
            }
            sx[d] = a * (1.0f / (FGMV_T * FGMV_T)); sy[d] = b * (1.0f / (FGMV_T * FGMV_T));
        }
        float qx = sx[0] - 2 * s0 + sx[1], qy = sy[0] - 2 * s0 + sy[1];
        if (qx > 1e-5f) v.x += clamp(0.5f * (sx[0] - sx[1]) / qx, -0.5f, 0.5f);
        if (qy > 1e-5f) v.y += clamp(0.5f * (sy[0] - sy[1]) / qy, -0.5f, 0.5f);
    }
    float mean = sm[0] / FGMV_N, conf = mean > 1e-3f ? clamp((mean - bs[0]) / mean, 0.0f, 1.0f) : 0.0f;
    tiles.write(half4(half(v.x), half(v.y), half(conf), 0), tg);
}

// The 3x3 window's mean absolute difference between cur at g and prev at g + v (bilinear, in 1/8 pixels)
static float fgmv_cost(texture2d<half, access::read> cur, texture2d<half, access::sample> prev, int2 g, float2 v, float2 inv, int2 mx) {
    constexpr sampler ls(coord::normalized, address::clamp_to_edge, filter::linear);
    float s = 0;
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
        int2 q = clamp(g + int2(x, y), int2(0), mx);
        s += fabs(float(cur.read(uint2(q)).r) - float(prev.sample(ls, (float2(q) + 0.5f + v) * inv).r));
    }
    return s * (1.0f / 9);
}
kernel void fgmv_pick(texture2d<half, access::read> cur [[texture(0)]], texture2d<half, access::sample> prev [[texture(1)]], texture2d<half, access::read> tiles [[texture(2)]],
                      texture2d<half, access::write> mot [[texture(3)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= mot.get_width() || g.y >= mot.get_height()) return;
    int2 mx = int2(cur.get_width() - 1, cur.get_height() - 1), tm = int2(tiles.get_width() - 1, tiles.get_height() - 1), t = int2(g) / FGMV_T, gi = int2(g);
    float2 inv = 1.0f / float2(prev.get_width(), prev.get_height());
    float2 bv = 0; float bc = fgmv_cost(cur, prev, gi, 0, inv, mx) - 0.004f;   // no motion wins ties: a still HUD and flat areas stay put
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
        half4 tv = tiles.read(uint2(clamp(t + int2(x, y), int2(0), tm)));
        if (tv.z < 0.05h) continue;   // a flat tile's vector says nothing
        float2 v = float2(tv.xy); float cst = fgmv_cost(cur, prev, gi, v, inv, mx) - (x == 0 && y == 0 ? 0.001f : 0);
        if (cst < bc) { bc = cst; bv = v; }
    }
    mot.write(half4(half(bv.x), half(bv.y), 0, 0), g);
}

// The HUD (nvn_mtl.m fg_keep): with motion anywhere in the picture MetalFX moves still pixels beside it too, so a still HUD over a panning background smears even when its
// own vectors say it stays (probe: 0.03-0.09 of mean error on the HUD, against 0.0015 with no motion at all). Pixels that did not change since the previous present and have
// detail around them (a HUD, the character the camera follows when it stands still), and still pixels near those (a HUD's flat fill), are taken from the present as it is.
// A flat still area alone is left to the interpolator: something thin that crossed it between the two presents is in the generated frame there.
//   fgmv_still  per 4x4 block of the picture: 1 still, 2 still with detail (a 6x6 window: no channel changed by more than 2/255; luma spread over 0.08)
//   fgmv_hud    per block: still, with a still detailed block within 3 blocks (12 px)
//   fgmv_keep   per pixel of the generated frame: the present's own where its block is kept
kernel void fgmv_still(texture2d<float, access::read> cur [[texture(0)]], texture2d<float, access::read> prev [[texture(1)]], texture2d<half, access::write> st [[texture(2)]],
                       uint2 g [[thread_position_in_grid]]) {
    if (g.x >= st.get_width() || g.y >= st.get_height()) return;
    int2 mx = int2(cur.get_width() - 1, cur.get_height() - 1), o = int2(g) * 4 - 1;
    float d = 0, lo = 1e9, hi = -1e9;
    for (int y = 0; y < 6; y++) for (int x = 0; x < 6; x++) {
        uint2 p = uint2(clamp(o + int2(x, y), int2(0), mx)); float3 a = cur.read(p).rgb, b = prev.read(p).rgb;
        float3 e = fabs(a - b); d = max(d, max(e.x, max(e.y, e.z)));
        float l = dot(a, float3(0.299, 0.587, 0.114)); lo = min(lo, l); hi = max(hi, l);
    }
    half v = d <= 2.5h / 255 ? (hi - lo > 0.08 ? 2.0h : 1.0h) : 0.0h;
    st.write(half4(v), g);
}
kernel void fgmv_hud(texture2d<half, access::read> st [[texture(0)]], texture2d<half, access::write> keep [[texture(1)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= keep.get_width() || g.y >= keep.get_height()) return;
    int2 mx = int2(st.get_width() - 1, st.get_height() - 1), gi = int2(g);
    half k = 0;
    if (st.read(g).r > 0.5h)
        for (int y = -3; y <= 3 && k == 0; y++) for (int x = -3; x <= 3; x++) if (st.read(uint2(clamp(gi + int2(x, y), int2(0), mx))).r > 1.5h) { k = 1; break; }
    keep.write(half4(k), g);
}
kernel void fgmv_keep(texture2d<float, access::read> cur [[texture(0)]], texture2d<half, access::read> keep [[texture(1)]], texture2d<float, access::write> out [[texture(2)]],
                      uint2 g [[thread_position_in_grid]]) {
    if (g.x >= out.get_width() || g.y >= out.get_height()) return;
    if (keep.read(min(g / 4, uint2(keep.get_width() - 1, keep.get_height() - 1))).r > 0.5h) out.write(cur.read(g), g);
}
