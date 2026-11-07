// Resolution upscaling (DM_UPSCALE): the pure arithmetic of nvn_mtl.m's upscaling section, kept apart from Metal so a CPU-only unit test (tests/t_ups.c) can check it:
// regions of the game's pixels mapped into a scaled texture's, occlusion-query sample counts brought back to the game's pixels, the present crop of a scaled window texture, and
// which reflection bindings count as texture and image descriptors of the shader translator (whose render_scale entries the host fills per bound texture); also which
// render targets are scaled by their flags and size (IMAGE included, a 4K ceiling), or held until a pass draws into them, and which image bindings write their texture, so a scaled one is demoted first; and which
// compute dispatch is the game's own spatial upscaler reading a scaled scene, whose output the host resolves itself (nvn_ups_bypass_shape, nvn_ups_fsr_rect, nvn_ups_bypass_ok).
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

// The most layers of a 2D array target that is scaled: each layer costs the factor squared in memory. KTGL's G-buffer draws into layers of 1600x900 arrays (4 layers seen) beside
// plain 2D targets, so leaving arrays out demoted the whole pass to the game's size; a longer array is more likely history or atlas data than a render target
#define NVN_UPS_MAX_LAYERS 8

// Whether a texture's shape can be scaled: a single-level 2D texture, or a single-level 2D array of at most NVN_UPS_MAX_LAYERS layers (target: NVN_TEXTURE_TARGET_*). The
// translator scales integer fetches and size queries on 2D arrays as on 2D textures (TexturePass.cs TypeSupportsScale masks the array flag off), and the host's copies, clears
// and demotions run per layer. Mipmapped, 3D, cube, multisample and rectangle / buffer textures stay at the game's size
static inline int nvn_ups_shape_ok(int target, uint32_t levels, uint32_t layers, uint32_t samples) {
    if (levels > 1 || samples > 1) return 0;
    if (target == NVN_TEXTURE_TARGET_2D) return layers <= 1;
    return target == NVN_TEXTURE_TARGET_2D_ARRAY && layers >= 1 && layers <= NVN_UPS_MAX_LAYERS;
}

// Whether a texture of the game's size w x h is screen-like for factor f: at least 320x180 and of a screen's aspect, 1.4 to 1.9 (lookup tables, atlases, shadow maps and small
// effect buffers are not), with its scaled size within NVN_UPS_MAX_W x NVN_UPS_MAX_H
static inline int nvn_ups_screen_ok(uint32_t w, uint32_t h, double f) {
    if (f <= 1 || w < 320 || h < 180 || (double)w / h < 1.4 || (double)w / h > 1.9) return 0;
    return lround(w * f) <= NVN_UPS_MAX_W && lround(h * f) <= NVN_UPS_MAX_H;
}

// The verdicts of nvn_ups_classify
enum { NVN_UPS_LEAVE = 0, NVN_UPS_SCALE = 1, NVN_UPS_HOLD = 2 };
// What becomes of a single-level 2D texture (or 2D array: nvn_ups_shape_ok) of the game's size w x h with these NVN flags at factor f (the size and flag part of nvn_mtl.m
// ups_pick; the caller checks the shape and the render-target usage). depth: a depth / stencil format (a render target without COMPRESSIBLE or DISPLAY). A colour render target
// is usually made COMPRESSIBLE (or DISPLAY: the window's), and a colour texture with neither is usually data copied in, but not always: Super Mario Bros. Wonder's scene colour has
// neither flag. Such a texture is held at the game's size (NVN_UPS_HOLD) until a pass draws into it, which shows it is a render target (ups_promote); one that is only ever read
// stays as it was. Left at the game's size (NVN_UPS_LEAVE): factor 1, the flags of NVN_UPS_FLAGS_UNSCALED, a size that is not screen-like (nvn_ups_screen_ok)
static inline int nvn_ups_classify(uint32_t w, uint32_t h, uint32_t flags, int depth, double f) {
    if ((flags & NVN_UPS_FLAGS_UNSCALED) || !nvn_ups_screen_ok(w, h, f)) return NVN_UPS_LEAVE;
    return depth || (flags & (NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_DISPLAY)) ? NVN_UPS_SCALE : NVN_UPS_HOLD;
}
// Whether such a texture is scaled when it is made (nvn_ups_classify's NVN_UPS_SCALE)
static inline int nvn_ups_size_flags_ok(uint32_t w, uint32_t h, uint32_t flags, int depth, double f) { return nvn_ups_classify(w, h, flags, depth, f) == NVN_UPS_SCALE; }

// Whether a reflection binding writes the texture bound to it as an image (an image slot declared write / read_write, or one the shader runs atomics on; the slot's handle either
// bound as an NVN image, IMG, or kept by the game in a uniform buffer, IMGU, as Unreal Engine 5.4 does). A dispatch or draw keeps the game's thread-group counts and
// viewport-free image coordinates, so such a texture must be of the game's size: a scaled one is demoted before the binding (image_prepare)
static inline int nvn_ups_img_writes(int kind, int rw, int atom) {
    return (kind == NVN_RB_IMG || kind == NVN_RB_IMGU) && (rw || atom);
}

// Whether a reflection binding is an image descriptor of the translator's _usedImages list, which follows the texture descriptors in render_scale (TexturePass.cs: ImageLoad's
// scale index is the count of non-array texture descriptors + the image's rank among the non-array images, in binding order). An image whose handle the game keeps in a uniform
// buffer (IMGU) is in that list like a bindless texture (TEXU) is in the textures'. Only image loads are scaled there (stores and atomics are not: IsImageInstructionWithScale),
// which is why a written image is demoted rather than given an entry
static inline int nvn_ups_img_desc(int kind, int n) {
    return (kind == NVN_RB_IMG || kind == NVN_RB_IMGU) && n <= 1;
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

// The game's own spatial upscaler, resolved by the host (nvn_mtl.m ups_bypass_encode; DM_UPSCALE_GAME). A game that renders its 3D below its output size upscales it in a
// compute dispatch whose tap offsets its constants computed for the game's sizes (KTGL's FSR-style pass in Hyrule Warriors: Age of Imprisonment: 48 gathers at normalized
// coordinates of a 1600x900 scene into a 1920x1080 image). With the scene rendered scaled (3200x1800 at 2x), those taps land between texels and span the factor times the
// intended radius, and the player sees a softened picture, while the host already holds more texels than the output needs. The host then writes the output itself, the scaled
// scene filtered down over each output texel's footprint; the game's later passes (sharpening, the copy to the swap chain) read the output unchanged.
// Shape: the dispatch binds exactly one sampled 2D texture (the scene) and exactly one written image (the output), no other texture or image: a pass with more inputs
// (a depth, a motion vector, a history) is a reconstruction whose output is not a resampling of one texture. Returns 1 with the two bindings' indices (b: the shader's
// reflection bindings; written images as nvn_ups_img_writes: an atomic image is not an output)
static inline int nvn_ups_bypass_shape(const nvn_rbind *b, int nb, int *ti, int *oi) {
    int t = -1, o = -1;
    for (int i = 0; i < nb; i++) {
        int k = b[i].kind;
        if (k == NVN_RB_TEX && b[i].n <= 1 && t < 0) t = i;
        else if (k == NVN_RB_IMG && b[i].rw && !b[i].atom && b[i].n <= 1 && o < 0) o = i;
        else if (k == NVN_RB_TEX || k == NVN_RB_TEXU || k == NVN_RB_TEXD || k == NVN_RB_TEXA || k == NVN_RB_TEXP || k == NVN_RB_TBUF || k == NVN_RB_TBUFU || k == NVN_RB_IMG || k == NVN_RB_IMGU) return 0;   // an image or buffer texture bound by a handle in a uniform buffer is another input or output: the resolve binds its two textures by slot
    }
    if (t < 0 || o < 0) return 0;
    *ti = t; *oi = o; return 1;
}

// The part of the scene the upscaler reads, from the upscaler's own constants. A game with dynamic resolution renders into a rectangle at the origin of a fixed texture (KTGL:
// 1152x648 of its 1600x900 scene in the first field), so the whole texture must not be taken as the input: the rest holds stale texels. FSR 1's EASU constants (FsrEasuCon in
// ffx_fsr1.h; c: the first four vec4 of the kernel's constant buffer) name both the rectangle and the texture: con0.xy = viewport / output size, con0.zw = con0.xy / 2 - 1/2,
// con1.xy = 1 / texture size, con1.zw = (con1.x, -con1.y). The texture terms must agree with the bound scene (tw x th, the game's size) and con0.zw with con0.xy, which is
// the fingerprint that the buffer holds these constants and not another kernel's: the viewport is then con0.xy × (ow, oh), rounded, within the texture. Returns 1 with it.
// (Without it the dispatch is left to the game's kernel: a kernel whose rectangle the host cannot read may read any part of the scene.) Tolerances: a relative 1e-4 on the
// reciprocals (floats computed by the game), 1e-3 on the half terms
static inline int nvn_ups_fsr_rect(const float c[16], uint32_t tw, uint32_t th, uint32_t ow, uint32_t oh, uint32_t *vw, uint32_t *vh) {
    if (!tw || !th || !ow || !oh) return 0;
    double rx = 1.0 / tw, ry = 1.0 / th;
    if (fabs(c[4] - rx) > rx * 1e-4 || fabs(c[5] - ry) > ry * 1e-4 || fabs(c[6] - rx) > rx * 1e-4 || fabs(c[7] + ry) > ry * 1e-4) return 0;
    if (!(c[0] > 0) || !(c[1] > 0) || fabs(c[2] - (0.5 * c[0] - 0.5)) > 1e-3 || fabs(c[3] - (0.5 * c[1] - 0.5)) > 1e-3) return 0;
    long w = lround((double)c[0] * ow), h = lround((double)c[1] * oh);
    if (w < 1 || h < 1 || w > (long)tw || h > (long)th) return 0;
    *vw = (uint32_t)w; *vh = (uint32_t)h; return 1;
}

// Whether such a dispatch is an upscale the host resolves: the scene the upscaler reads is iw x ih in the game's pixels (its rectangle, nvn_ups_fsr_rect) and sw x sh texels as
// the host made it, the output image ow x oh (at the game's size: a written image is demoted before the dispatch). The output must be larger than the game's scene in at least
// one direction and smaller in none (a pass between equal sizes is a per-pixel effect, a smaller output a downsample the game filters itself), of the same shape within 1 %
// (the scene maps to the whole output), and at most the scene the host holds in both directions (the resolve only ever filters down; at 1x, or when the dynamic resolution
// drops under the output over the factor, the game's kernel runs as before). 1.5x: a 1600x900 scene is 2400x1350, still past 1920x1080; 2x: 3200x1800
static inline int nvn_ups_bypass_ok(uint32_t iw, uint32_t ih, uint32_t sw, uint32_t sh, uint32_t ow, uint32_t oh) {
    if (!iw || !ih || !ow || !oh || sw <= iw || sh <= ih) return 0;
    if (ow < iw || oh < ih || (ow == iw && oh == ih)) return 0;
    if (ow > sw || oh > sh) return 0;
    double ai = (double)iw / ih, ao = (double)ow / oh;
    return ao > ai * 0.99 && ao < ai * 1.01;
}
