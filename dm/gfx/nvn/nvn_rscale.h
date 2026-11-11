// Internal resolution (DM_INTERNAL_RES): render targets drawn at a multiple of the guest size. Pure helpers, unit-tested in tests/t_rscale.c.
// The translated shaders already carry Ryujinx's render-scale code (FragCoord divide, texel-fetch scale, texture-size unscale), all reading support_buffer.render_scale;
// the reflection says which binding uses which render_scale entry (nvn_refl.h sidx / sfl / frag_coord, written by tools/dmsh_rscale.py).
// A texture is either at the session scale S or at 1x; nvn_mtl.m decides it at creation (nvn_rs_eligible) and demotes a scaled one to 1x when it cannot stay scaled.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NVN_RS_SCALES { 1.0f, 1.5f, 2.0f }   // the scales DM_INTERNAL_RES accepts (the "Internal resolution" row of dm/games/settings.tsv offers the same)

// The scale a DM_INTERNAL_RES value asks for: unset or empty = 1; a number equal to one of NVN_RS_SCALES (1, 1.0, 1.5, 2, 2.0 ...); anything else = 0 (invalid: the caller logs it and uses 1)
static inline float nvn_rs_parse(const char *e) {
    if (!e || !*e) return 1.0f;
    char *end; float v = strtof(e, &end);
    if (end == e || *end) return 0.0f;
    static const float ok[] = NVN_RS_SCALES;
    for (unsigned i = 0; i < sizeof ok / sizeof *ok; i++) if (v == ok[i]) return v;
    return 0.0f;
}

// Host size of a scaled guest extent: ceil(guest * S) (1.5x of an odd size rounds up, so the host texture always covers the scaled guest area)
static inline uint32_t nvn_rs_host(uint32_t g, float s) { return (uint32_t)ceil((double)g * s); }

// A guest span [x, x + w) in host units: start floored, end ceiled, so rectangles that touch in guest units touch in host units at 1.5x too (no gap, no overlap)
static inline void nvn_rs_edges(long x, long w, float s, long *x0, long *x1) { *x0 = (long)floor((double)x * s); *x1 = (long)ceil((double)(x + w) * s); }
// Whether both edges of [x, x + w) land on whole host texels (always at 2x; at 1.5x only even ones): a same-scale copy of such a span is a plain blit, any other resamples
static inline int nvn_rs_whole(long x, long w, float s) { double a = (double)x * s, b = (double)(x + w) * s; return a == floor(a) && b == floor(b); }

// What decides whether a new texture is created scaled (filled in by nvn_mtl.m from tex_t, its format and its pool)
typedef struct {
    int target, flags, samples;     // NVNtextureTarget, NVNtextureFlags
    uint32_t w, h, levels, bpp;     // guest size, mip levels, bytes per pixel (for Ryujinx's width alignment)
    uint8_t compressed, depth_or_single, packaged, init_data;   // block-compressed format; depth / stencil or one-channel format; packaged (CPU-written) texture; storage already holds data the GPU must start from
    uint8_t storage, sparse;        // the host stores its texels back into CPU-readable guest memory (nvn_mtl.m gsb_target); its pool is sparse (nvn_sparse_pool)
} nvn_rs_tex;

// Why a texture is not scaled (or demoted later); the names are what the log prints
enum { NVN_RS_OK, NVN_RS_TARGET, NVN_RS_FORMAT, NVN_RS_FLAGS, NVN_RS_CPU, NVN_RS_STORAGE, NVN_RS_SPARSE, NVN_RS_LEVELS, NVN_RS_SMALL, NVN_RS_SQUARE, NVN_RS_BLUR, NVN_RS_LUT, NVN_RS_MEMORY,
       // demotions after creation
       NVN_RS_MIXED, NVN_RS_UNSUPPORTED, NVN_RS_IMAGE, NVN_RS_CPU_WRITE, NVN_RS_COPY, NVN_RS_UPSCALER, NVN_RS_NWHY };
static const char *const nvn_rs_why_name[NVN_RS_NWHY] = { "ok", "target", "format", "flags", "cpu", "storage", "sparse", "levels", "small", "square", "blur", "lut", "memory",
                                                           "mixed pass", "unsupported binding", "image binding", "cpu write", "copy", "upscaler" };

// Whether a texture is created at the session scale (NVN_RS_OK) or why not.
// Rules: 2D / 2D array / 2D multisample (array) targets only (Ryujinx IsUpscaleCompatible), no CPU-written content, no flag that lets the CPU or a non-render path see the texels,
// no texture whose texels the host stores back into guest memory (the store-back copies host texels one for one) and none in a sparse pool (its pages are guest-sized),
// then Ryujinx's UpscaleSafeMode on the guest size (TextureCache.cs): <= 3 levels, both edges >= 8, not possibly square unless depth or one channel, not a 16:9 target below 360 lines (blur and bloom chains), not w == h*h (a 3D lookup table laid out in 2D)
static inline int nvn_rs_eligible(const nvn_rs_tex *t) {
    if (t->target != 1 && t->target != 4 && t->target != 5 && t->target != 6) return NVN_RS_TARGET;
    if (t->compressed) return NVN_RS_FORMAT;
    // VIDEO_DECODE, LINEAR, SPARSE, LINEAR_RENDER_TARGET. IMAGE is allowed: games set it on ordinary render targets, so an image binding demotes instead (nvn_mtl.m rs_bind_check)
    if (t->flags & (0x2 | 0x10 | 0x20 | 0x40)) return NVN_RS_FLAGS;
    // Otherwise the CPU reaches the texels only through ReadTexels, copies and the CPU-write tracking, which read a scaled texture at 1x or demote it
    if (t->packaged || t->init_data) return NVN_RS_CPU;
    if (t->storage) return NVN_RS_STORAGE;
    if (t->sparse) return NVN_RS_SPARSE;
    if (t->levels > 3) return NVN_RS_LEVELS;
    if (t->w < 8 || t->h < 8) return NVN_RS_SMALL;
    uint32_t al = 64 / (t->bpp && t->bpp <= 64 ? t->bpp : 1);   // GOB width (64 bytes) in pixels
#define NVN_RS_UP(v) (((v) + al - 1) / al * al)
    if (!t->depth_or_single && NVN_RS_UP(t->w) == NVN_RS_UP(t->h)) return NVN_RS_SQUARE;
    if (t->h < 360) {
        uint32_t aw = (uint32_t)ceilf(t->h / 9.0f * 16.0f);
        if (t->w >= aw / al * al && t->w <= NVN_RS_UP(aw)) return NVN_RS_BLUR;
    }
#undef NVN_RS_UP
    if ((uint64_t)t->w == (uint64_t)t->h * t->h) return NVN_RS_LUT;
    return NVN_RS_OK;
}

// Pass consensus: Metal attachments must agree in size and one viewport serves them all, so a pass is scaled only when every attachment is.
// Returns 1 for a scaled pass; for a mix, 0 and the bit mask of scaled attachments to demote (bit i = scaled[i])
static inline int nvn_rs_consensus(const uint8_t *scaled, int n, unsigned *demote) {
    unsigned m = 0; int all = n > 0;
    for (int i = 0; i < n; i++) { if (scaled[i]) m |= 1u << i; else all = 0; }
    *demote = all ? 0 : m;
    return all;
}

// Ryujinx UpdateScale (TextureBindingsManager.cs): in the fragment stage, a scaled texture whose guest size has the render target's aspect gets -S,
// which tells TexelFetchScale to add the FragCoord fraction (one texel per host pixel instead of S x S blocks)
static inline int nvn_rs_same_aspect(uint32_t tw, uint32_t th, uint32_t pw, uint32_t ph) { return tw && th && (float)pw / tw == (float)ph / th; }

// Support-block key: 2 bits per render_scale entry (0 = 1, 1 = S, 2 = -S); entry 0 is the pass scale, entry 1 + sidx a binding's texture scale (NVN_RS_MAXIDX + 1 = 72 entries + slot 0)
enum { NVN_RS_V_ONE, NVN_RS_V_S, NVN_RS_V_NEG };
#define NVN_RS_KEY_SLOTS 73
typedef struct { uint64_t w[(NVN_RS_KEY_SLOTS * 2 + 63) / 64]; } nvn_rs_key;
static inline void nvn_rs_key_init(nvn_rs_key *k) { memset(k, 0, sizeof *k); }
static inline void nvn_rs_key_set(nvn_rs_key *k, unsigned slot, unsigned v) { unsigned b = slot * 2; k->w[b / 64] = (k->w[b / 64] & ~(3ull << (b % 64))) | ((uint64_t)v << (b % 64)); }
static inline unsigned nvn_rs_key_get(const nvn_rs_key *k, unsigned slot) { unsigned b = slot * 2; return (unsigned)(k->w[b / 64] >> (b % 64)) & 3; }
static inline int nvn_rs_key_empty(const nvn_rs_key *k) { for (unsigned i = 0; i < sizeof k->w / sizeof *k->w; i++) if (k->w[i]) return 0; return 1; }
static inline float nvn_rs_key_val(unsigned v, float s) { return v == NVN_RS_V_S ? s : v == NVN_RS_V_NEG ? -s : 1.0f; }
