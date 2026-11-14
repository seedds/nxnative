// Unit tests for the anisotropy a sampler is made with (nvn_aniso.h): the game's own without the player's setting, raised only for samplers that filter a mip-mapped colour
// texture, never lowered. The expected values are written out by hand.
#include "../nvn/nvn_aniso.h"
#include <stdio.h>

static int fail, total;
static void t(const char *name, unsigned want, unsigned got) {
    total++; fail += want != got;
    printf("%-78s expected %2u → got %2u %s\n", name, want, got, want == got ? "✓" : "✗");
}
int main(void) {
    const samp_par tri = {.minf = 5, .magf = 1, .lod_min = -1000, .lod_max = 1000, .aniso = 1};            // trilinear, the game asks for no anisotropy
    samp_par p;
    t("no setting: the game's own (1)", 1, nvn_aniso(&tri, 0));
    t("trilinear sampler, setting 16", 16, nvn_aniso(&tri, 16));
    t("trilinear sampler, setting 4", 4, nvn_aniso(&tri, 4));
    p = tri; p.aniso = 8;
    t("the game's own 8, no setting", 8, nvn_aniso(&p, 0));
    t("the game's own 8, setting 4: never lowered", 8, nvn_aniso(&p, 4));
    t("the game's own 8, setting 16", 16, nvn_aniso(&p, 16));
    p = tri; p.aniso = 2; p.minf = 3;
    t("the game's own 2 on a sampler that picks one mip level, setting 16: raised", 16, nvn_aniso(&p, 16));
    p = tri; p.minf = 3;
    t("linear, nearest mip level, the game's own 1, setting 16: a lookup, kept", 1, nvn_aniso(&p, 16));
    p = tri; p.minf = 4;
    t("nearest texel, linear between mip levels, setting 16: kept", 1, nvn_aniso(&p, 16));
    p = tri; p.minf = 1;
    t("linear without mip levels (a screen-sized target), setting 16: kept", 1, nvn_aniso(&p, 16));
    p = tri; p.minf = 0; p.magf = 0;
    t("nearest (a table), setting 16: kept", 1, nvn_aniso(&p, 16));
    p = tri; p.magf = 0;
    t("nearest magnification, setting 16: kept", 1, nvn_aniso(&p, 16));
    p = tri; p.cmp_mode = 1; p.cmp_func = 4;
    t("depth comparison (a shadow map), setting 16: kept", 1, nvn_aniso(&p, 16));
    p = tri; p.reduction = 1;
    t("min reduction (a depth pyramid), setting 16: kept", 1, nvn_aniso(&p, 16));
    p = tri; p.lod_max = 0;
    t("LOD clamped to the base level, setting 16: kept", 1, nvn_aniso(&p, 16));
    p = tri; p.aniso = 0;
    t("an anisotropy below 1 is 1", 1, nvn_aniso(&p, 0));
    p = tri; p.aniso = 64;
    t("an anisotropy above 16 is 16", 16, nvn_aniso(&p, 0));
    t("a setting above 16 is 16", 16, nvn_aniso(&tri, 32));
    t("setting: unset", 0, (unsigned)nvn_aniso_setting(NULL));
    t("setting: empty", 0, (unsigned)nvn_aniso_setting(""));
    t("setting: game", 0, (unsigned)nvn_aniso_setting("game"));
    t("setting: 2", 2, (unsigned)nvn_aniso_setting("2"));
    t("setting: 4", 4, (unsigned)nvn_aniso_setting("4"));
    t("setting: 8", 8, (unsigned)nvn_aniso_setting("8"));
    t("setting: 16", 16, (unsigned)nvn_aniso_setting("16"));
    t("setting: 3 is refused", 1, nvn_aniso_setting("3") == -1);
    t("setting: 16x is refused", 1, nvn_aniso_setting("16x") == -1);
    p = tri; p.lod_bias = 0.5f;
    t("LOD bias: no setting keeps the game's", 1, nvn_lod_bias(&p, 0.0f) == 0.5f);
    t("LOD bias: -1 on a surface sampler", 1, nvn_lod_bias(&p, -1.0f) == -0.5f);
    p.minf = 3;
    t("LOD bias: a lookup sampler keeps the game's", 1, nvn_lod_bias(&p, -1.0f) == 0.5f);
    p = tri; p.cmp_mode = 1;
    t("LOD bias: a shadow sampler keeps the game's", 1, nvn_lod_bias(&p, -1.0f) == 0.0f);
    p = tri; p.lod_bias = -16;
    t("LOD bias: clamped to Metal's -16", 1, nvn_lod_bias(&p, -1.0f) == -16.0f);
    int bad; float v;
    v = nvn_mip_bias_setting(NULL, &bad); t("bias setting: unset", 1, v == 0.0f && !bad);
    v = nvn_mip_bias_setting("-0.5", &bad); t("bias setting: -0.5", 1, v == -0.5f && !bad);
    v = nvn_mip_bias_setting("0.5", &bad); t("bias setting: 0.5 means -0.5", 1, v == -0.5f && !bad);
    v = nvn_mip_bias_setting("-1", &bad); t("bias setting: -1", 1, v == -1.0f && !bad);
    v = nvn_mip_bias_setting("-1.5", &bad); t("bias setting: -1.5 is refused", 1, bad);
    v = nvn_mip_bias_setting("-0.3", &bad); t("bias setting: -0.3 is refused", 1, bad);
    v = nvn_mip_bias_setting("sharp", &bad); t("bias setting: text is refused", 1, bad);
    printf("%d / %d passed\n", total - fail, total);
    return fail != 0;
}
