// Resolution upscaling unit test (CPU only): the pure arithmetic of nvn_mtl.m's upscaling section in ../nvn/nvn_ups.h, with the expectations hand-calculated.
// Regions: equal game regions map to equal scaled regions whatever their offsets (the audit's 1.5x case: x=1,w=2 and x=0,w=2 used to come out 4 and 3 wide), clamped to the texture.
// Occlusion queries: samples counted in a scaled pass come back divided by the factor squared, rounded. Present crop: a scaled window texture's crop in its own pixels.
// Descriptor ranks: which reflection bindings the translator counts as texture descriptors (render_scale[1 + i]).
#include "../nvn/nvn_ups.h"
#include <stdio.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// One region through nvn_ups_region; returns the four values for comparison
static void region(int x, int y, int w, int h, double f, long tw, long th, int out[4]) {
    out[0] = x; out[1] = y; out[2] = w; out[3] = h;
    nvn_ups_region(&out[0], &out[1], &out[2], &out[3], f, tw, th);
}

int main(void) {
    int a[4], b[4];
    // 1.5x: the parity case from the audit. x=1,w=2 → offset 2 (1.5 rounded), width 3; x=0,w=2 → offset 0, width 3: the same width for the same game width
    region(1, 0, 2, 2, 1.5, 0, 0, a); region(0, 0, 2, 2, 1.5, 0, 0, b);
    CHECK(a[0] == 2 && a[2] == 3, "1.5x x=1,w=2 → %d,%d", a[0], a[2]);
    CHECK(b[0] == 0 && b[2] == 3, "1.5x x=0,w=2 → %d,%d", b[0], b[2]);
    CHECK(a[2] == b[2] && a[3] == b[3], "equal game regions must have equal scaled sizes");
    // 2x: exact
    region(3, 5, 7, 9, 2, 0, 0, a);
    CHECK(a[0] == 6 && a[1] == 10 && a[2] == 14 && a[3] == 18, "2x → %d,%d %dx%d", a[0], a[1], a[2], a[3]);
    // 1x: untouched
    region(3, 5, 7, 9, 1, 0, 0, a);
    CHECK(a[0] == 3 && a[1] == 5 && a[2] == 7 && a[3] == 9, "1x must not change a region");
    // a one-texel region never vanishes
    region(0, 0, 1, 1, 1.5, 0, 0, a);
    CHECK(a[2] >= 1 && a[3] >= 1, "1 texel at 1.5x → %dx%d", a[2], a[3]);
    // clamping: a region reaching the game's right edge stays inside the scaled texture (1280x720 at 1.5x = 1920x1080)
    region(1279, 719, 1, 1, 1.5, 1920, 1080, a);
    CHECK(a[0] + a[2] <= 1920 && a[1] + a[3] <= 1080 && a[2] >= 1, "clamped edge texel → %d+%d, %d+%d", a[0], a[2], a[1], a[3]);
    region(1200, 700, 200, 100, 1.5, 1920, 1080, a);
    CHECK(a[0] == 1800 && a[2] == 120 && a[1] == 1050 && a[3] == 30, "clamped region → %d+%d, %d+%d", a[0], a[2], a[1], a[3]);
    // a region entirely outside is empty, not negative
    region(2000, 0, 10, 10, 1.5, 1920, 1080, a);
    CHECK(a[2] == 0 && a[0] == 1920, "outside → %d+%d", a[0], a[2]);
    // the whole texture maps to the whole scaled texture (1280x720 at 1.5x and 2x)
    region(0, 0, 1280, 720, 1.5, 1920, 1080, a);
    CHECK(a[0] == 0 && a[1] == 0 && a[2] == 1920 && a[3] == 1080, "whole 1.5x → %dx%d", a[2], a[3]);
    region(0, 0, 1280, 720, 2, 2560, 1440, a);
    CHECK(a[2] == 2560 && a[3] == 1440, "whole 2x → %dx%d", a[2], a[3]);

    // Occlusion query samples: f² per game pixel, rounded; 1x and an unset factor (0) pass the count through
    CHECK(nvn_ups_samples(400, 2) == 100, "2x: 400 → %llu", (unsigned long long)nvn_ups_samples(400, 2));
    CHECK(nvn_ups_samples(9, 1.5) == 4, "1.5x: 9 / 2.25 = 4 → %llu", (unsigned long long)nvn_ups_samples(9, 1.5));
    CHECK(nvn_ups_samples(10, 1.5) == 4, "1.5x: 10 / 2.25 = 4.44 → %llu", (unsigned long long)nvn_ups_samples(10, 1.5));
    CHECK(nvn_ups_samples(11, 1.5) == 5, "1.5x: 11 / 2.25 = 4.89 → %llu", (unsigned long long)nvn_ups_samples(11, 1.5));
    CHECK(nvn_ups_samples(7, 1) == 7 && nvn_ups_samples(7, 0) == 7, "1x / unset keep the count");
    CHECK(nvn_ups_samples(0, 2) == 0, "no samples stay none");
    CHECK(nvn_ups_samples(1, 2) == 1 && nvn_ups_samples(1, 1.5) == 1 && nvn_ups_samples(2, 2) == 1, "a query that passed anything reports at least 1 → %llu %llu %llu",
          (unsigned long long)nvn_ups_samples(1, 2), (unsigned long long)nvn_ups_samples(1, 1.5), (unsigned long long)nvn_ups_samples(2, 2));

    // Present crop: Breath of the Wild's 1280x720 crop of a 1920x1080 window texture made at 2x (3840x2160)
    int crop[4] = {0, 0, 1280, 720}, out[4];
    CHECK(nvn_ups_crop(crop, 2, 3840, 2160, out) && out[0] == 0 && out[1] == 0 && out[2] == 2560 && out[3] == 1440, "2x crop → %d,%d %dx%d", out[0], out[1], out[2], out[3]);
    CHECK(nvn_ups_crop(crop, 1.5, 2880, 1620, out) && out[2] == 1920 && out[3] == 1080, "1.5x crop → %dx%d", out[2], out[3]);
    int none[4] = {0, 0, 0, 0};
    CHECK(!nvn_ups_crop(none, 2, 3840, 2160, out), "an unset crop is reported unset");
    CHECK(!nvn_ups_crop(NULL, 2, 3840, 2160, out), "a NULL crop is reported unset");
    int off[4] = {320, 180, 1280, 720};
    CHECK(nvn_ups_crop(off, 2, 3840, 2160, out) && out[0] == 640 && out[1] == 360 && out[2] == 2560 && out[3] == 1440, "offset crop → %d,%d %dx%d", out[0], out[1], out[2], out[3]);

    // Descriptor ranks: sampled textures, uniform-buffer handles and buffer textures of array length 1 count; arrays, images, samplers and buffers do not
    CHECK(nvn_ups_tex_desc(NVN_RB_TEX, 0) && nvn_ups_tex_desc(NVN_RB_TEX, 1) && nvn_ups_tex_desc(NVN_RB_TEXU, 0) && nvn_ups_tex_desc(NVN_RB_TBUF, 0), "single textures count");
    CHECK(!nvn_ups_tex_desc(NVN_RB_TEXU, 32) && !nvn_ups_tex_desc(NVN_RB_TEXD, 8) && !nvn_ups_tex_desc(NVN_RB_TEXA, 32), "arrays do not count");
    CHECK(!nvn_ups_tex_desc(NVN_RB_IMG, 0) && !nvn_ups_tex_desc(NVN_RB_SAMP, 0) && !nvn_ups_tex_desc(NVN_RB_CBUF, 0) && !nvn_ups_tex_desc(NVN_RB_SUPPORT, 0), "images, samplers, buffers do not count");

    printf("t_ups: %s (%d failures)\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}
