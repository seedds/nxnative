// Texture clears (nvnCommandBufferClearTexture) keep the channels and pixels outside their mask and region, whatever the draw scissor: the Vulkan port of dm/gfx/tests/t_clear_tex.m,
// with its seven cases. A 2D array texture of two layers and two levels is first cleared whole, every layer and level, to 17, 34, 51, 68 (each a whole-level clear,
// vkCmdClearColorImage); the case's clear (201 + channel) then goes to one level and layer through a view, under a draw scissor of one pixel it must ignore; every texel of every
// layer and level is read back. The Metal test's pass-dump checks are Metal's (no counterpart): 7 cases, each with 4 readback checks and its texel check, and the validation check
#include "t_vk_harness.h"

// A ClearTexture record: level and layer through a view, the region, the channel mask; the value 17, 34, 51, 68 (init) or 201 + channel, as integers or as unorm
static void clear(hv_cmds *c, const tex_t *t, int ui, int mask, int x, int y, int w, int h, int layer, int level, int init) {
    p_clear_tex r; memset(&r, 0, sizeof r);
    r.tex = t; r.has_view = 1; r.is_uint = (uint8_t)ui; r.mask = mask; r.r = (NVNcopyRegion){x, y, 0, w, h, 1};
    r.view.set_levels = 1; r.view.base_level = (uint8_t)level; r.view.set_layers = 1; r.view.min_layer = (uint16_t)layer;
    for (int ch = 0; ch < 4; ch++) {
        uint32_t v = init ? 17u * (uint32_t)(ch + 1) : 201u + (uint32_t)ch;
        if (ui) r.c.u[ch] = v; else r.c.f[ch] = (float)v / 255.f;
    }
    hv_emit(c, OP_CLEAR_TEX, &r, sizeof r);
}

int main(void) {
    hv_up("t_vk_clear_tex");
    hv_device(0);
    NVNqueue queue; NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&queue, &qb);
    hv_pool cp; if (!hv_pool_make(&cp, 65536)) { t("command memory allocated", 0); return hv_finish(); }
    hv_cmds cmds = {cp.mem, 0, 65536};
    struct { int mask, x, y, w, h, layer, level, ui; const char *label; } cases[] = {
        {8, 0, 0, 8, 8, 0, 0, 0, "alpha-only clear preserves RGB"},
        {5, 2, 1, 3, 2, 0, 0, 0, "masked rectangle preserves other pixels and channels"},
        {15, 2, 1, 3, 2, 0, 0, 0, "whole-channel rectangle"},
        {8, 0, 0, 4, 4, 1, 1, 0, "mip and array layer selection"},
        {8, 0, 0, 8, 8, 0, 0, 1, "integer alpha clear preserves exact values"},
        {0, 0, 0, 8, 8, 0, 0, 0, "empty channel mask writes nothing"},
        {15, 0, 0, 8, 8, 0, 0, 0, "whole clear keeps load-action path"},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        tex_t tex = hv_tex(NVN_TEXTURE_TARGET_2D_ARRAY, cases[i].ui ? NVN_FORMAT_RGBA8UI : NVN_FORMAT_RGBA8, 8, 8, 2, 2, 0);
        for (int layer = 0; layer < 2; layer++) for (int lv = 0; lv < 2; lv++) clear(&cmds, &tex, cases[i].ui, 15, 0, 0, 8 >> lv, 8 >> lv, layer, lv, 1);
        hv_submit(&queue, &cmds);
        hv_scissor(&cmds, 0, 0, 1, 1);   // ClearTexture takes its region, not the draw scissor
        clear(&cmds, &tex, cases[i].ui, cases[i].mask, cases[i].x, cases[i].y, cases[i].w, cases[i].h, cases[i].layer, cases[i].level, 0);
        hv_submit(&queue, &cmds);
        int valid = 1;
        for (int layer = 0; layer < 2; layer++) for (int lv = 0; lv < 2; lv++) {
            int size = 8 >> lv; uint8_t p[256]; memset(p, 0x55, sizeof p);
            int read = hv_read(&tex, VK_IMAGE_ASPECT_COLOR_BIT, (unsigned)lv, (unsigned)layer, 0, 0, (uint32_t)size, (uint32_t)size, 4, p);
            t("readback completes", read);
            valid &= read;
            for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) for (int ch = 0; ch < 4; ch++) {
                int hit = layer == cases[i].layer && lv == cases[i].level && x >= cases[i].x && x < cases[i].x + cases[i].w && y >= cases[i].y && y < cases[i].y + cases[i].h && (cases[i].mask & (1 << ch));
                int want = hit ? 201 + ch : 17 * (ch + 1), got = p[(y * size + x) * 4 + ch];
                if (got != want && valid) fprintf(stderr, "case %u layer %d level %d texel %d,%d channel %d: %d, expected %d\n", i, layer, lv, x, y, ch, got, want);
                valid &= got == want;
            }
        }
        t(cases[i].label, valid);
    }
    hv_pool_free(&cp);
    return hv_finish();
}
