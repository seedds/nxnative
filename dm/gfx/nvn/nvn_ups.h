// Resolution upscaling (DM_UPSCALE): the pure arithmetic of nvn_mtl.m's upscaling section, kept apart from Metal so a CPU-only unit test (tests/t_ups.c) can check it:
// regions of the game's pixels mapped into a scaled texture's, occlusion-query sample counts brought back to the game's pixels, the present crop of a scaled window texture, and
// which reflection bindings count as texture and image descriptors of the shader translator (whose render_scale entries the host fills per bound texture); also which
// render targets are scaled by their flags and size (IMAGE included, a 4K ceiling) and which image bindings write their texture, so a scaled one is demoted first.
#pragma once
#include <stdint.h>
#include <math.h>
#include "nvn_refl.h"
#include "include/nvn.h"

// The NVN texture flags that keep a texture at the game's size: linear layouts (a CPU or display reads their bytes), sparse and video-decode textures. IMAGE is not among them:
// KTGL (Hyrule Warriors: Age of Imprisonment) sets it on every render target whether or not a shader writes it, so the flag alone says nothing; a scaled texture that a shader
// does bind as a written image is demoted when that happens (nvn_ups_img_writes, nvn_mtl.m image_prepare)
#define NVN_UPS_FLAGS_UNSCALED (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_SPARSE | NVN_TEXTURE_FLAGS_VIDEO_DECODE)
// The largest scaled size: a target past 4K gains nothing on a Mac display and costs the factor squared in memory and bandwidth. Switch 1 targets (at most 1920x1080) stay under
// it at 2x; a Switch 2 swap chain already at 3840x2160 (Hyrule Warriors') stays at its size
#define NVN_UPS_MAX_W 3840
#define NVN_UPS_MAX_H 2160

// Whether a single-level 2D texture of the game's size w x h with these NVN flags is a render target to scale by f (the size and flag part of nvn_mtl.m ups_pick; the caller checks
// target, levels, layers, samples and render-target usage). depth: a depth / stencil format (a render target without COMPRESSIBLE or DISPLAY). Screen-like means at least 320x180
// and an aspect of 1.4 to 1.9 (lookup tables, atlases, shadow maps and small effect buffers are not), and the scaled size must stay within NVN_UPS_MAX_W x NVN_UPS_MAX_H.
// A colour texture without COMPRESSIBLE or DISPLAY is data copied in rather than a render target
static inline int nvn_ups_size_flags_ok(uint32_t w, uint32_t h, uint32_t flags, int depth, double f) {
    if (f <= 1 || (flags & NVN_UPS_FLAGS_UNSCALED)) return 0;
    if (!depth && !(flags & (NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_DISPLAY))) return 0;
    if (w < 320 || h < 180 || (double)w / h < 1.4 || (double)w / h > 1.9) return 0;
    long sw = lround(w * f), sh = lround(h * f);
    return sw <= NVN_UPS_MAX_W && sh <= NVN_UPS_MAX_H;
}

// Whether a reflection binding writes the texture bound to it as an image (an image slot declared write / read_write, or one the shader runs atomics on; the slot's handle either
// bound as an NVN image, IMG, or kept by the game in a uniform buffer, IMGU, as Unreal Engine 5.4 does). A dispatch or draw keeps the game's thread-group counts and
// viewport-free image coordinates, so such a texture must be of the game's size: a scaled one is demoted before the binding (image_prepare)
static inline int nvn_ups_img_writes(int kind, int rw, int atom) {
    return (kind == NVN_RB_IMG || kind == NVN_RB_IMGU) && (rw || atom);
}

// Whether a reflection binding is an image descriptor of the translator's _usedImages list, which follows the texture descriptors in render_scale (TexturePass.cs: ImageLoad's
// scale index is the count of non-array texture descriptors + the image's rank among the non-array images, in binding order). Only image loads are scaled there (stores and
// atomics are not: IsImageInstructionWithScale), which is why a written image is demoted rather than given an entry
static inline int nvn_ups_img_desc(int kind, int n) {
    return kind == NVN_RB_IMG && n <= 1;
}

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
