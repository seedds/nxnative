// Integer colour attachments ignore blending while a normalized one beside them keeps it: the Vulkan port of dm/gfx/tests/t_integer_blend.m. Vulkan requires blendEnable off
// for an attachment whose format cannot blend (VUID-vkCmdDraw-blendEnable-04727), so each of the Metal test's 19 integer formats is drawn into, beside an RGBA8 target, with
// ONE + ONE blending asked for on both; the validation layer counts nothing for that draw, and the RGBA8 target holds the blended value (its clear, 0.25, plus the shader's 0.5).
// Then R32UI and RGBA8UI side by side, both integer, keep the shader's literal bits (the Metal test's last two checks). Checks: per format the clean draw and the blended
// neighbour (38), the two literal readbacks, the readbacks they need, and the validation check
#include "t_vk_harness.h"
#include "shaders/gen/fullscreen_vert.h"
#include "shaders/gen/uout_frag.h"
#include "shaders/gen/iout_frag.h"
#include "shaders/gen/uu_frag.h"

static void blend_on(hv_cmds *c, int targets) {
    for (int i = 0; i < targets; i++) {
        blend_st b; memset(&b, 0, sizeof b); b.target = (unsigned)i;
        b.src_rgb = b.dst_rgb = b.src_a = b.dst_a = NVN_BLEND_FUNC_ONE; b.eq_rgb = b.eq_a = NVN_BLEND_EQUATION_ADD;
        hv_state(c, 0, &b, sizeof b);
    }
    color_st cs; memset(&cs, 0, sizeof cs); cs.blend_en = (1u << targets) - 1; cs.logic_op = NVN_LOGIC_OP_COPY; hv_state(c, 1, &cs, sizeof cs);
}

int main(void) {
    hv_up("t_vk_integer_blend");
    hv_device(0);
    NVNqueue queue; NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&queue, &qb);
    hv_pool mp; if (!hv_pool_make(&mp, 1 << 18)) { t("memory pool allocated", 0); return hv_finish(); }
    hv_cmds cmds = {mp.mem + 65536, 0, 65536};
    static NVNprogram pu, pi, puu;
    const hv_shader su[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"uout", 1, HV_SPV(uout_frag_spv), NULL, NULL}};
    const hv_shader si[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"iout", 1, HV_SPV(iout_frag_spv), NULL, NULL}};
    const hv_shader suu[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"uu", 1, HV_SPV(uu_frag_spv), NULL, NULL}};
    hv_program(&pu, &mp, 0, su, 2); hv_program(&pi, &mp, 1024, si, 2); hv_program(&puu, &mp, 2048, suu, 2);
    static const struct { int fmt, sint; const char *name; } formats[] = {
        {NVN_FORMAT_R8UI, 0, "R8UI"}, {NVN_FORMAT_R8I, 1, "R8I"}, {NVN_FORMAT_R16UI, 0, "R16UI"}, {NVN_FORMAT_R16I, 1, "R16I"}, {NVN_FORMAT_R32UI, 0, "R32UI"},
        {NVN_FORMAT_R32I, 1, "R32I"}, {NVN_FORMAT_RG8UI, 0, "RG8UI"}, {NVN_FORMAT_RG8I, 1, "RG8I"}, {NVN_FORMAT_RG16UI, 0, "RG16UI"}, {NVN_FORMAT_RG16I, 1, "RG16I"},
        {NVN_FORMAT_RG32UI, 0, "RG32UI"}, {NVN_FORMAT_RG32I, 1, "RG32I"}, {NVN_FORMAT_RGBA8UI, 0, "RGBA8UI"}, {NVN_FORMAT_RGBA8I, 1, "RGBA8I"}, {NVN_FORMAT_RGB10A2UI, 0, "RGB10A2UI"},
        {NVN_FORMAT_RGBA16UI, 0, "RGBA16UI"}, {NVN_FORMAT_RGBA16I, 1, "RGBA16I"}, {NVN_FORMAT_RGBA32UI, 0, "RGBA32UI"}, {NVN_FORMAT_RGBA32I, 1, "RGBA32I"},
    };
    char name[200];
    for (unsigned i = 0; i < sizeof formats / sizeof *formats; i++) {
        tex_t it = hv_tex(NVN_TEXTURE_TARGET_2D, formats[i].fmt, 8, 8, 1, 1, 0), nt = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 8, 8, 1, 1, 0);
        const tex_t *rt[2] = {&it, &nt};
        uint32_t errors = hvk_device()->val_errors;
        hv_rt(&cmds, 2, rt, NULL);
        hv_clear_color(&cmds, 1, 15, .25f, .25f, .25f, .25f);
        hv_bind(&cmds, formats[i].sint ? &pi : &pu, 3); hv_depth(&cmds, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&cmds, NVN_FACE_NONE); blend_on(&cmds, 2);
        hv_draw(&cmds, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
        hv_submit(&queue, &cmds);
        snprintf(name, sizeof name, "%s attachment with blending asked for: the draw is valid (blending off for it)", formats[i].name);
        t(name, hvk_device()->val_errors == errors);
        uint8_t px[256]; int ok = hv_read(&nt, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 8, 8, 4, px);
        for (int p = 0; ok && p < 256; p++) ok &= px[p] >= 190 && px[p] <= 192;
        if (!ok) fprintf(stderr, "%s: the RGBA8 neighbour reads %u %u %u %u, expected 191 (0.25 + 0.5)\n", formats[i].name, px[0], px[1], px[2], px[3]);
        snprintf(name, sizeof name, "%s beside RGBA8: the normalized attachment keeps the blending it asked for", formats[i].name);
        t(name, ok);
    }
    tex_t a = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R32UI, 4, 4, 1, 1, 0), b = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8UI, 4, 4, 1, 1, 0);
    const tex_t *rt[2] = {&a, &b};
    hv_rt(&cmds, 2, rt, NULL); hv_bind(&cmds, &puu, 3); blend_on(&cmds, 2);
    hv_draw(&cmds, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    hv_submit(&queue, &cmds);
    uint32_t pixels[16]; int ok = hv_read(&a, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 4, 4, 4, pixels);
    t("R32UI readback completes", ok);
    int lit = ok; for (int i = 0; i < 16; i++) lit &= pixels[i] == 0x12345678u;
    t("R32Uint output preserves all literal bits", lit);
    ok = hv_read(&b, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 4, 4, 4, pixels);
    t("RGBA8UI readback completes", ok);
    lit = ok; for (int i = 0; i < 16; i++) lit &= pixels[i] == 0x44332211u;
    t("RGBA8Uint output preserves four literal channels", lit);
    hv_pool_free(&mp);
    return hv_finish();
}
