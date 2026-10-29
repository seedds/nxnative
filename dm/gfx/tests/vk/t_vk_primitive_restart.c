// Indexed fans and quads keep restart-separated polygons apart: the Vulkan port of dm/gfx/tests/t_primitive_restart.m, its twelve cases (1-, 2- and 4-byte indices, the all-ones
// restart index or a custom one, triangle fans and quads) over the same index list, two rectangles lit and the gap and outside untouched. Vulkan restarts strips and fans at the
// all-ones index only and has no quads, so most cases go through the backend's index conversion (nvn_primitive.h). Added for the Vulkan backend: quads whose indices a GPU copy
// writes earlier in the same submission, which the CPU cannot read when the draw is encoded: the backend's GPU index helper (shaders/quad_index.comp) expands them, for each
// index size. Checks: per case the pixels, and the validation check
#include "t_vk_harness.h"
#include "shaders/gen/restart_vert.h"
#include "shaders/gen/red_frag.h"

static int lit_ok(const uint8_t *px) {   // rows 3-12 of the 32×16 target: the two rectangles lit, the gap and the sides untouched
    int good = 1;
    for (int y = 3; y < 13; y++) for (int x = 0; x < 32; x++) { int lit = (x >= 2 && x < 11) || (x >= 21 && x < 30); good &= (px[(y * 32 + x) * 4] > 127) == lit; }
    if (!good) { fprintf(stderr, "row 8: "); for (int x = 0; x < 32; x++) fputc(px[(8 * 32 + x) * 4] > 127 ? '#' : '.', stderr); fputc('\n', stderr); }
    return good;
}

int main(void) {
    hv_up("t_vk_primitive_restart");
    hv_device(0);
    NVNqueue queue; NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&queue, &qb);
    hv_pool mp; if (!hv_pool_make(&mp, 1 << 18)) { t("memory pool allocated", 0); return hv_finish(); }
    hv_cmds cmds = {mp.mem + 65536, 0, 65536};
    uint8_t *ibuf = mp.mem + 8192, *stage = mp.mem + 12288;
    static NVNprogram prog;
    const hv_shader sh[2] = {{"restart", 0, HV_SPV(restart_vert_spv), NULL, NULL}, {"red", 1, HV_SPV(red_frag_spv), NULL, NULL}};
    hv_program(&prog, &mp, 0, sh, 2);
    char name[200];
    for (int isz = 1; isz <= 4; isz *= 2) for (int custom = 0; custom < 2; custom++) for (int prim = NVN_DRAW_PRIMITIVE_TRIANGLE_FAN; prim <= NVN_DRAW_PRIMITIVE_QUADS; prim++) {
        uint32_t restart = custom ? 13 : isz == 1 ? 255 : isz == 2 ? 65535 : UINT32_MAX;
        uint32_t indices[] = {restart, 0, 1, restart, restart, 0, 1, 2, 3, restart, 4, 5, 6, 7, restart};
        for (unsigned i = 0; i < sizeof indices / sizeof *indices; i++) memcpy(ibuf + i * (unsigned)isz, &indices[i], (size_t)isz);
        tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 32, 16, 1, 1, 0);
        const tex_t *rt[1] = {&target};
        hv_rt(&cmds, 1, rt, NULL); hv_clear_color(&cmds, 0, 15, 0, 0, 0, 0);
        hv_bind(&cmds, &prog, 3); hv_depth(&cmds, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&cmds, NVN_FACE_NONE);
        p_prim_restart pr = {1, restart}; hv_emit(&cmds, OP_PRIM_RESTART, &pr, sizeof pr);
        hv_draw_idx(&cmds, prim, isz == 1 ? 0 : isz == 2 ? 1 : 2, (int)(sizeof indices / sizeof *indices), ibuf, 0, 1);
        hv_submit(&queue, &cmds);
        uint8_t px[32 * 16 * 4]; int ok = hv_read(&target, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 32, 16, 4, px) && lit_ok(px);
        snprintf(name, sizeof name, "indices=%d restart=%s %s: two polygons lit, gap and outside untouched", isz, custom ? "custom" : "all ones", prim == NVN_DRAW_PRIMITIVE_QUADS ? "quads" : "fan");
        t(name, ok);
    }
    // Quads whose indices the GPU copies in place earlier in the same submission (no restart): the GPU index helper
    for (int isz = 1; isz <= 4; isz *= 2) {
        const uint32_t indices[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        for (unsigned i = 0; i < 8; i++) memcpy(stage + i * (unsigned)isz, &indices[i], (size_t)isz);
        memset(ibuf, 0xEE, 64);   // what the CPU sees when the draw is encoded: not indices
        tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 32, 16, 1, 1, 0);
        const tex_t *rt[1] = {&target};
        hv_rt(&cmds, 1, rt, NULL); hv_clear_color(&cmds, 0, 15, 0, 0, 0, 0);
        hv_bind(&cmds, &prog, 3); hv_depth(&cmds, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&cmds, NVN_FACE_NONE);
        p_prim_restart pr = {0, 0}; hv_emit(&cmds, OP_PRIM_RESTART, &pr, sizeof pr);
        hv_b2b(&cmds, stage, ibuf, (uint64_t)(8 * isz));
        hv_draw_idx(&cmds, NVN_DRAW_PRIMITIVE_QUADS, isz == 1 ? 0 : isz == 2 ? 1 : 2, 8, ibuf, 0, 1);
        hv_submit(&queue, &cmds);
        uint8_t px[32 * 16 * 4]; int ok = hv_read(&target, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 32, 16, 4, px) && lit_ok(px);
        snprintf(name, sizeof name, "indices=%d quads written by a GPU copy in the same submission: expanded on the GPU, both lit", isz);
        t(name, ok);
    }
    hv_pool_free(&mp);
    return hv_finish();
}
