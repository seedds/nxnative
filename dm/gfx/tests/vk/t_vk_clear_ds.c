// Partial depth / stencil clears keep every pixel and stencil bit outside their scissor and mask: the Vulkan port of dm/gfx/tests/t_clear_ds.m, with the same nine cases.
// Each case starts from a new DEPTH32F_STENCIL8 target cleared whole to depth 0.75 and stencil 0xa5 (a load operation of the first submission's pass), then clears depth 0.25 and
// stencil 0x3c under the case's scissor and masks, outside a pass (the clear opens one) or inside one (an empty-scissor clear opened it first); both planes are read back from the
// backend's image. The last case is a new texture with nothing cleared: zero in both planes. The Metal test's "synthetic plane reader compiles" check has no counterpart (the
// readback is a copy): 9 cases, each with its plane check and its readback check, and the check that no validation message came
#include "t_vk_harness.h"

int main(void) {
    hv_up("t_vk_clear_ds");
    hv_device(0);
    NVNqueue queue; NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&queue, &qb);
    hv_pool cp; if (!hv_pool_make(&cp, 65536)) { t("command memory allocated", 0); return hv_finish(); }
    hv_cmds cmds = {cp.mem, 0, 65536};
    struct { int mid; p_scissor sc; int depth, mask; const char *label; } cases[] = {
        {0, {2, 1, 3, 2}, 1, 0, "outside-pass scissored depth clear"},
        {0, {2, 1, 3, 2}, 0, 255, "outside-pass scissored stencil clear"},
        {0, {0, 0, 8, 8}, 0, 15, "outside-pass masked stencil clear"},
        {1, {0, 0, 8, 8}, 0, 15, "inside-pass masked stencil clear"},
        {1, {2, 1, 3, 2}, 1, 15, "inside-pass scissored depth/stencil clear"},
        {0, {2, 1, 0, 2}, 1, 255, "empty scissor writes nothing"},
        {0, {-1, -1, 3, 3}, 1, 15, "negative scissor origin is clamped"},
        {0, {0, 0, 8, 8}, 1, 255, "full clear remains a load action"},
        {0, {0, 0, 8, 8}, 0, 0, "new texture initialization zeros both planes"},
    };
    const int n = (int)(sizeof cases / sizeof *cases);
    for (int i = 0; i < n; i++) {
        tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH32F_STENCIL8, 8, 8, 1, 1, 0);
        int zero_init = i == n - 1;
        if (!zero_init) {
            hv_rt(&cmds, 0, NULL, &target); hv_scissor(&cmds, 0, 0, 8, 8);
            p_clear_ds init = {.75f, 1, 0xa5, 0xff}; hv_emit(&cmds, OP_CLEAR_DS, &init, sizeof init);
            hv_submit(&queue, &cmds);
            hv_rt(&cmds, 0, NULL, &target);
            if (cases[i].mid) { hv_scissor(&cmds, 0, 0, 0, 0); p_clear_ds open = {.5f, 1, 0, 0xff}; hv_emit(&cmds, OP_CLEAR_DS, &open, sizeof open); }   // opens the pass, writes nothing
            hv_scissor(&cmds, cases[i].sc.x, cases[i].sc.y, cases[i].sc.w, cases[i].sc.h);
            p_clear_ds clear = {.25f, cases[i].depth, 0x3c, cases[i].mask}; hv_emit(&cmds, OP_CLEAR_DS, &clear, sizeof clear);
            hv_submit(&queue, &cmds);
        }
        float depth[64]; uint8_t stencil[64]; memset(depth, 0x55, sizeof depth); memset(stencil, 0x55, sizeof stencil);
        int read = hv_read(&target, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 0, 0, 8, 8, 4, depth) && hv_read(&target, VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 0, 0, 8, 8, 1, stencil);
        t("both planes are read back", read);
        int valid = read;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            p_scissor sc = cases[i].sc; int inside = x >= sc.x && x < sc.x + sc.w && y >= sc.y && y < sc.y + sc.h;
            unsigned mask = inside ? (unsigned)cases[i].mask : 0, want_s = zero_init ? 0 : (0xa5 & ~mask) | (0x3c & mask);
            uint32_t want_d = zero_init ? 0 : inside && cases[i].depth ? 0x3e800000u : 0x3f400000u, got_d; memcpy(&got_d, &depth[y * 8 + x], 4);
            if (read && (stencil[y * 8 + x] != want_s || got_d != want_d) && valid)
                fprintf(stderr, "case %d pixel %d,%d: depth %#x stencil %#x, expected %#x and %#x\n", i, x, y, got_d, stencil[y * 8 + x], want_d, want_s);
            valid &= stencil[y * 8 + x] == want_s && got_d == want_d;
        }
        t(cases[i].label, valid);
    }
    hv_pool_free(&cp);
    return hv_finish();
}
