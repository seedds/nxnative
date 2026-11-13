// Anisotropic filtering raised for the game's textures (DM_ANISO, a launcher setting; pure functions, unit test tests/t_aniso.c). Off, the default, every sampler state
// takes the game's own anisotropy (clamped to Metal's 1 to 16). With DM_ANISO=<2..16> each graphics layer (NVN samp_by_id, Vulkan vkCreateSampler, GL tex_sampler) also gets a
// raised state, by aniso_for, for a sampler that filters linearly in both directions, has a mip filter and the regular LOD range (minimum 0, maximum at least 1), or one the game
// already gave anisotropy, and binds it only with a 2D or 2D-array texture whose view has more than one level (aniso_tex_ok; NVN samp_aniso, Vulkan's combined image samplers,
// GL's per-texture state). A texture read at one level, through a one-level texture or a sampler clamped to level 0, spreads the extra samples across that level wherever a
// shader's coordinates jump within a pixel quad: Breath of the Wild's anti-aliasing pass then found its edges wrong and the picture turned jagged (yuzu gates its anisotropy on the
// same LOD range). Samplers with a depth comparison, a min / max reduction or unnormalized coordinates, and samplers bound apart from their texture (NVN separate samplers, pools
// and arrays; Vulkan separate samplers), keep the game's value; samples at an explicit level (textureLod, compute shaders) are never anisotropic
#pragma once
#include <stdio.h>
#include <stdlib.h>

enum { ANISO_MIP_NONE, ANISO_MIP_NEAREST, ANISO_MIP_LINEAR };
static inline unsigned aniso_clamp(float v) { return !(v >= 1) ? 1 : v > 16 ? 16 : (unsigned)v; }   // NaN and values below 1 give 1
// setting: DM_ANISO (0 off, else 2 to 16); game: the anisotropy the game set; mip: ANISO_MIP_*; lod_min, lod_max: the sampler's LOD clamp; excluded: non-zero for a depth
// comparison, a min / max reduction or unnormalized coordinates
static inline unsigned aniso_rule(unsigned setting, float game, int min_linear, int mag_linear, int mip, float lod_min, float lod_max, int excluded) {
    unsigned g = aniso_clamp(game);
    if (setting < 2 || excluded) return g;
    int mips = mip != ANISO_MIP_NONE && lod_min <= 0 && lod_max >= 1;   // a sampler clamped to level 0 reads one level, as a one-level texture does
    return (min_linear && mag_linear && mips) || g > 1 ? (g > setting ? g : setting) : g;
}
// The levels a texture view exposes (NVN tex_by_id's rule): from its base level to the end of the texture when it sets no count or 0, cut to the levels the texture has
static inline unsigned aniso_view_levels(unsigned tex_levels, int set_levels, unsigned base, unsigned n) {
    if (!set_levels) return tex_levels;
    if (base >= tex_levels) return 0;
    unsigned left = tex_levels - base; return n && n < left ? n : left;
}
// The levels a w x h texture can have (Metal makes no more): 1 + floor(log2(max(w, h)))
static inline unsigned aniso_level_cap(unsigned w, unsigned h) { unsigned m = w > h ? w : h, n = 1; while (m > 1) { m >>= 1; n++; } return n; }
// Whether a raised state may be bound with a texture: a 2D or 2D-array view with more than one level
static inline int aniso_tex_ok(int is_2d, unsigned levels) { return is_2d && levels > 1; }
// DM_ANISO, read once: 0 when unset, 0 or 1 (off); 2 to 16 samples; anything else is red and off
static inline unsigned aniso_setting(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_ANISO"); char *end = NULL; long n = e && *e ? strtol(e, &end, 10) : 0; int s = 0;
        if (e && *e && (*end || n < 0 || n > 16)) fprintf(stderr, "[dm] [RED] DM_ANISO=%s: 0 (off) or 2 to 16; off\n", e);
        else if (n >= 2) { s = (int)n; fprintf(stderr, "[dm] anisotropic filtering (DM_ANISO): %dx for linear samplers of textures with mips (once for each graphics layer)\n", s); }
        v = s;   // a race of two first readers stores the same value; each translation unit that includes this header has its own copy, so a layer logs it once
    }
    return (unsigned)v;
}
static inline unsigned aniso_for(float game, int min_linear, int mag_linear, int mip, float lod_min, float lod_max, int excluded) {
    return aniso_rule(aniso_setting(), game, min_linear, mag_linear, mip, lod_min, lod_max, excluded);
}
