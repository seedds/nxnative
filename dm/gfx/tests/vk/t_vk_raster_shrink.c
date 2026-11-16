// DM_VK_RASTER_SHRINK=<k> (vk_commands.c shrink_k; docs/env_switches.tsv): every pass renders into the top-left 1/k of its targets, its render area, viewport, scissor and
// in-pass clears scaled by 1/k, while the commands stay the same. A 16 x 16 RGBA8 target at k = 4: ClearTexture paints the whole of it green (outside any pass: unscaled), a
// colour clear inside a pass makes it blue (the render area: the top-left 4 x 4), and a red triangle over the whole target under the scissor of its left half (0, 0, 8, 16)
// lands on the left half of that corner (0, 0, 2, 4). Without the switch the same commands give red on the left half and blue on the right: a child process runs each
// (t_vk_raster_shrink off / on, t_vk_harness.h hv_spawn), as the switch is read once. Checks: each pixel's colour in both runs, and the validation check
#include "t_vk_harness.h"
#include "shaders/gen/fullscreen_vert.h"
#include "shaders/gen/red_frag.h"

static void env_put(const char *k, const char *v) {
#if defined(_WIN32)
    char b[128]; snprintf(b, sizeof b, "%s=%s", k, v ? v : ""); _putenv(b);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}
enum { N = 16 };
static int phase(int on) {
    env_put("DM_VK_RASTER_SHRINK", on ? "4" : NULL);   // read once, at the first pass
    hv_up(on ? "t_vk_raster_shrink (DM_VK_RASTER_SHRINK=4)" : "t_vk_raster_shrink (off)");
    hv_device(0);
    NVNqueue q; NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&q, &qb);
    hv_pool mp; if (!hv_pool_make(&mp, 1 << 18)) { t("memory pool allocated", 0); return hv_finish(); }
    memset(mp.mem, 0, 1 << 18);
    hv_cmds c = {mp.mem + 131072, 0, 65536};
    static NVNprogram prog;
    const hv_shader sh[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"red", 1, HV_SPV(red_frag_spv), NULL, NULL}};
    hv_program(&prog, &mp, 0, sh, 2);
    tex_t tgt = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, N, N, 1, 1, 0);
    p_clear_tex ct; memset(&ct, 0, sizeof ct); ct.tex = &tgt; ct.mask = 15; ct.r = (NVNcopyRegion){0, 0, 0, N, N, 1}; ct.c.f[1] = 1; ct.c.f[3] = 1;
    hv_emit(&c, OP_CLEAR_TEX, &ct, sizeof ct);   // green everywhere
    const tex_t *rt[1] = {&tgt};
    hv_rt(&c, 1, rt, NULL);
    hv_bind(&c, &prog, 3); hv_depth(&c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&c, NVN_FACE_NONE);
    hv_clear_color(&c, 0, 15, 0, 0, 1, 1);   // blue: the pass's load operation over its render area
    hv_scissor(&c, 0, 0, N / 2, N);
    hv_draw(&c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);   // red over the whole target, under the scissor of its left half
    hv_submit(&q, &c);
    uint8_t px[N * N * 4];
    int ok = hv_read(&tgt, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, N, N, 4, px), bad = 0;
    for (int y = 0; y < N && ok; y++) for (int x = 0; x < N; x++) {
        const uint8_t *p = px + 4 * (y * N + x);
        int want;   // 0 red, 1 blue, 2 green
        if (on) want = x < 2 && y < 4 ? 0 : x < 4 && y < 4 ? 1 : 2;
        else want = x < N / 2 ? 0 : 1;
        const int got = p[0] > 200 && p[1] < 50 && p[2] < 50 ? 0 : p[2] > 200 && p[0] < 50 && p[1] < 50 ? 1 : p[1] > 200 && p[0] < 50 && p[2] < 50 ? 2 : 3;
        bad += got != want;
    }
    char name[200];
    snprintf(name, sizeof name, on ? "k = 4: red at the corner's left half (2 x 4), blue in the rest of the 4 x 4 corner, green outside it (%d pixels differ)"
                                   : "off: red on the left half, blue on the right (%d pixels differ)", bad);
    t(name, ok && !bad);
    hv_pool_free(&mp);
    return hv_finish();
}
int main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "off") || !strcmp(argv[1], "on"))) return phase(!strcmp(argv[1], "on"));
    int r0 = hv_spawn(argv[0], "off"), r1 = hv_spawn(argv[0], "on");
    printf("t_vk_raster_shrink: the run without the switch %s, the run with it %s\n", r0 ? "failed" : "passed", r1 ? "failed" : "passed");
    return r0 || r1;
}
