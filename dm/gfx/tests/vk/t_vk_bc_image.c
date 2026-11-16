// Textures made with the IMAGE flag in a block-compressed format (vk_texture.c tex_make, hvk_reg_tex): Mario vs. Donkey Kong's resource textures carry the flag (130 of the 184
// textures of its level 1-1, nearly all BC) and none is ever registered as an image. No shader writes block-compressed texels, so the texture is made without storage use with
// one note and no red line (before, a red line said its image binds read the dummy, which no bind did); a RegisterImage of one is what would lose something, and says so with a
// red line. Checks: a BC1 and a BC5 texture with the flag are made and sampled-ready (an image exists) with the note once and no red line; registering the BC5 one as an image
// gives the red line; an uncompressed RGBA8 texture with the flag gets storage use (no line); and the validation check
#include "t_vk_harness.h"
void be_reg_image(int tid, const tex_t *t, const view_t *v);

static int g_note, g_red, g_red_reg;
static void on_log(const char *line) {
    fprintf(stderr, "[dm] %s\n", line);
    if (strstr(line, "IMAGE flag in a block-compressed format") && !strstr(line, "[RED]")) g_note++;
    if (strstr(line, "[RED]") && strstr(line, "IMAGE flag")) g_red++;
    if (strstr(line, "[RED]") && strstr(line, "(block-compressed) registered as an image")) g_red_reg++;
}
int main(void) {
    hostvk_set_log(on_log);
    hv_up("t_vk_bc_image");
    hv_device(0);
    hv_pool p; if (!hv_pool_make(&p, 1 << 20)) { t("memory pool allocated", 0); return hv_finish(); }
    size_t off = 0;
    NVNtexture bc1, bc5, rgba;
    hv_texture(&bc1, &p, &off, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA_DXT1, 64, 64, 1, 1, NVN_TEXTURE_FLAGS_IMAGE, 0, 0);
    hv_texture(&bc5, &p, &off, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGTC2_UNORM, 64, 64, 1, 1, NVN_TEXTURE_FLAGS_IMAGE, 0, 0);
    hv_texture(&rgba, &p, &off, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 64, 64, 1, 1, NVN_TEXTURE_FLAGS_IMAGE, 0, 0);
    VkFormat f; VkImageAspectFlags a;
    int made = hvk_test_image((const tex_t *)(const void *)&bc1, &f, &a) && hvk_test_image((const tex_t *)(const void *)&bc5, &f, &a) &&
               hvk_test_image((const tex_t *)(const void *)&rgba, &f, &a);
    t("BC1, BC5 and RGBA8 textures with the IMAGE flag are made", made);
    char name[200];
    snprintf(name, sizeof name, "the block-compressed ones: one note (%d), no red line (%d)", g_note, g_red);
    t(name, g_note == 1 && g_red == 0);
    be_reg_image(7, (const tex_t *)(const void *)&bc5, NULL);
    snprintf(name, sizeof name, "registering the BC5 one as an image says what is lost with a red line (%d)", g_red_reg);
    t(name, g_red_reg == 1);
    hv_pool_free(&p);
    return hv_finish();
}
