// Unit tests for the anisotropy of the game's sampler states (sampler_aniso.h, DM_ANISO): which samplers the setting raises, which keep the game's value, the LOD range,
// the clamp to Metal's 1 to 16, the texture gate (a view's levels as tex_by_id counts them, the levels a texture's size allows, 2D only), and the switch's parsing (each case in a
// child process, since the setting is read once)
#include "../nvn/sampler_aniso.h"
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail, total;
static void t(const char *name, unsigned want, unsigned got) {
    total++; fail += want != got;
    printf("%-72s expected %2u, got %2u %s\n", name, want, got, want == got ? "✓" : "✗");
}
static unsigned setting_of(const char *v) {   // aniso_setting() in a child with DM_ANISO=v (NULL: unset)
    int fd[2]; if (pipe(fd)) return 99;
    pid_t pid = fork();
    if (pid == 0) {
        if (v) setenv("DM_ANISO", v, 1); else unsetenv("DM_ANISO");
        unsigned s = aniso_setting(); if (write(fd[1], &s, sizeof s) != sizeof s) _exit(1); _exit(0);
    }
    unsigned s = 99; close(fd[1]); if (read(fd[0], &s, sizeof s) != sizeof s) s = 99; close(fd[0]); waitpid(pid, NULL, 0);
    return s;
}
int main(void) {
    enum { N = ANISO_MIP_NONE, MN = ANISO_MIP_NEAREST, ML = ANISO_MIP_LINEAR };
    const float L0 = 0, L1 = 1000;   // the default LOD range (NVN -1000..1000 starts at or below 0)
    volatile float nan = 0.f; nan = nan / nan;   // a NaN the compiler cannot fold
    // Off: the game's value, clamped
    t("off: trilinear 1x keeps 1x", 1, aniso_rule(0, 1, 1, 1, ML, L0, L1, 0));
    t("off: game 4x keeps 4x", 4, aniso_rule(0, 4, 1, 1, ML, L0, L1, 0));
    t("off: game 0 (never set) is 1x", 1, aniso_rule(0, 0, 1, 1, ML, L0, L1, 0));
    t("off: game 32x is clamped to 16x", 16, aniso_rule(0, 32, 1, 1, ML, L0, L1, 0));
    t("off: game NaN is 1x", 1, aniso_rule(0, nan, 1, 1, ML, L0, L1, 0));
    t("setting 1 is off", 1, aniso_rule(1, 1, 1, 1, ML, L0, L1, 0));
    // On: linear samplers with mips and the regular LOD range, and samplers the game gave anisotropy
    t("16x: trilinear 1x is raised", 16, aniso_rule(16, 1, 1, 1, ML, L0, L1, 0));
    t("16x: trilinear 1x with NVN's default LOD range (-1000..1000) is raised", 16, aniso_rule(16, 1, 1, 1, ML, -1000, 1000, 0));
    t("8x: trilinear 1x is raised to 8x", 8, aniso_rule(8, 1, 1, 1, ML, L0, L1, 0));
    t("8x: the game's 16x is kept (never lowered)", 16, aniso_rule(8, 16, 1, 1, ML, L0, L1, 0));
    t("16x: linear sampler with a nearest mip filter is raised", 16, aniso_rule(16, 1, 1, 1, MN, L0, L1, 0));
    t("16x: nearest minification with a nearest mip filter keeps 1x", 1, aniso_rule(16, 1, 0, 1, MN, L0, L1, 0));
    t("16x: linear sampler without mips keeps 1x", 1, aniso_rule(16, 1, 1, 1, N, L0, L1, 0));
    t("16x: nearest minification keeps 1x", 1, aniso_rule(16, 1, 0, 1, ML, L0, L1, 0));
    t("16x: nearest magnification keeps 1x", 1, aniso_rule(16, 1, 1, 0, ML, L0, L1, 0));
    t("16x: trilinear clamped to level 0 (max LOD 0) keeps 1x", 1, aniso_rule(16, 1, 1, 1, ML, 0, 0, 0));
    t("16x: trilinear with max LOD 0.25 (Vulkan's GL-style no mips) keeps 1x", 1, aniso_rule(16, 1, 1, 1, ML, 0, 0.25f, 0));
    t("16x: trilinear with min LOD 2 keeps 1x", 1, aniso_rule(16, 1, 1, 1, ML, 2, 1000, 0));
    t("16x: trilinear with max LOD exactly 1 is raised", 16, aniso_rule(16, 1, 1, 1, ML, 0, 1, 0));
    t("16x: game 4x, nearest filters, no mips: raised (the game asked for it)", 16, aniso_rule(16, 4, 0, 0, N, L0, L1, 0));
    t("16x: game 2x, linear without mips: raised (the game asked for it)", 16, aniso_rule(16, 2, 1, 1, N, L0, L1, 0));
    t("16x: game 2x clamped to level 0: raised (the game asked for it)", 16, aniso_rule(16, 2, 1, 1, ML, 0, 0, 0));
    t("16x: excluded (comparison, reduction or unnormalized) keeps the game's 1x", 1, aniso_rule(16, 1, 1, 1, ML, L0, L1, 1));
    t("16x: excluded keeps the game's 4x", 4, aniso_rule(16, 4, 1, 1, ML, L0, L1, 1));
    // The texture gate: a view's levels (tex_by_id's rule), the size's cap, 2D only
    t("view: no level range set: all 8 levels", 8, aniso_view_levels(8, 0, 3, 1));
    t("view: levels 2 + 3 of 8", 3, aniso_view_levels(8, 1, 2, 3));
    t("view: levels 0 + 0 of 8 (0 = to the end)", 8, aniso_view_levels(8, 1, 0, 0));
    t("view: levels 5 + 0 of 8 (0 = to the end)", 3, aniso_view_levels(8, 1, 5, 0));
    t("view: levels 7 + 2 of 8 are cut to 1", 1, aniso_view_levels(8, 1, 7, 2));
    t("view: base level at the texture's level count: 0", 0, aniso_view_levels(8, 1, 8, 1));
    t("view: base level past the texture's levels: 0", 0, aniso_view_levels(8, 1, 9, 1));
    t("level cap: 1600x900 has 11 levels", 11, aniso_level_cap(1600, 900));
    t("level cap: 1x1 has 1 level", 1, aniso_level_cap(1, 1));
    t("level cap: 256x2 has 9 levels", 9, aniso_level_cap(256, 2));
    t("texture gate: 2D with 8 levels", 1, aniso_tex_ok(1, 8));
    t("texture gate: 2D with 1 level", 0, aniso_tex_ok(1, 1));
    t("texture gate: 2D view of 0 levels", 0, aniso_tex_ok(1, 0));
    t("texture gate: not 2D (cube, 3D) with 8 levels", 0, aniso_tex_ok(0, 8));
    // The switch
    t("DM_ANISO unset is off", 0, setting_of(NULL));
    t("DM_ANISO= (empty) is off", 0, setting_of(""));
    t("DM_ANISO=0 is off", 0, setting_of("0"));
    t("DM_ANISO=1 is off", 0, setting_of("1"));
    t("DM_ANISO=4", 4, setting_of("4"));
    t("DM_ANISO=16", 16, setting_of("16"));
    t("DM_ANISO=17 is red and off", 0, setting_of("17"));
    t("DM_ANISO=-2 is red and off", 0, setting_of("-2"));
    t("DM_ANISO=16x is red and off", 0, setting_of("16x"));
    printf("anisotropy: %d checks, %d failed\n", total, fail);
    return fail != 0;
}
