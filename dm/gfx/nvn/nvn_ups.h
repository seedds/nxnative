// Resolution upscaling (DM_UPSCALE): the pure arithmetic of nvn_mtl.m's upscaling section, kept apart from Metal so a CPU-only unit test (tests/t_ups.c) can check it:
// regions of the game's pixels mapped into a scaled texture's, occlusion-query sample counts brought back to the game's pixels, the present crop of a scaled window texture, and
// which reflection bindings count as texture descriptors of the shader translator (whose render_scale entries the host fills per bound texture).
#pragma once
#include <stdint.h>
#include <math.h>
#include "nvn_refl.h"

// A region of the game's pixels → the scaled texture's. Offset and size are rounded separately (lround), so two regions of the same game size always get the same scaled size
// whatever their offsets: a copy's source and destination regions then match. Rounding the ends instead (floor of the offset, ceil of the end) gave x=1,w=2 → 4 wide but x=0,w=2 → 3
// wide at 1.5x, which stretched colour copies and made integer / depth copies fail for unequal sizes. The region is then clamped to the scaled level size tw x th (0 = no clamp);
// a region that falls entirely outside keeps a width / height of 0.
static inline void nvn_ups_region(int *x, int *y, int *w, int *h, double f, long tw, long th) {
    long x0 = lround(*x * f), y0 = lround(*y * f), sw = lround(*w * f), sh = lround(*h * f);
    if (*w > 0 && sw < 1) sw = 1;
    if (*h > 0 && sh < 1) sh = 1;
    if (tw > 0) { if (x0 > tw) x0 = tw; if (x0 + sw > tw) sw = tw - x0; }
    if (th > 0) { if (y0 > th) y0 = th; if (y0 + sh > th) sh = th - y0; }
    *x = (int)x0; *y = (int)y0; *w = (int)(sw < 0 ? 0 : sw); *h = (int)(sh < 0 ? 0 : sh);
}

// Samples passed by an occlusion query counted in a pass drawn at factor f: each game pixel became f x f samples, so the count is divided by f² and rounded (2.25 at 1.5x).
// A query that passed any sample reports at least 1: a tiny light whose single game pixel passed must not read as fully occluded (1 / 4 would round to 0).
static inline uint64_t nvn_ups_samples(uint64_t n, double f) {
    if (f <= 1 || !n) return n;
    long long r = llround((double)n / (f * f));
    return (uint64_t)(r < 1 ? 1 : r);
}

// The present crop (x, y, w, h in the game's pixels; w or h 0 = the whole texture) of a window texture made at factor f and sw x sh texels, in the texture's own pixels.
// Returns 0 when the crop is unset (the caller keeps its whole-texture default). The crop is clamped to the texture as nvn_ups_region does.
static inline int nvn_ups_crop(const int crop[4], double f, long sw, long sh, int out[4]) {
    if (!crop || !(crop[2] || crop[3])) return 0;
    out[0] = crop[0]; out[1] = crop[1]; out[2] = crop[2]; out[3] = crop[3];
    nvn_ups_region(&out[0], &out[1], &out[2], &out[3], f, sw, sh);
    return 1;
}

// Whether a reflection binding is a texture descriptor of the translator's _usedTextures list (Ryujinx.Graphics.Shader ResourceManager.GetDescriptors): every t-prefixed binding
// (sampled textures, bindless ones read from a uniform buffer, buffer textures) of array length 1. The translator's TexelFetchScale / TextureSizeUnscale helpers read
// render_scale[1 + i] where i is the binding's rank in that list, which lists the non-array descriptors first in binding order; arrays (n > 1: TEXD / TEXA, uniform-buffer handle
// arrays) come after them and are never scaled, and images, samplers and buffers are other lists. The reflection keeps the MSL argument order, which is the binding order.
static inline int nvn_ups_tex_desc(int kind, int n) {
    return (kind == NVN_RB_TEX || kind == NVN_RB_TEXU || kind == NVN_RB_TBUF) && n <= 1;
}
