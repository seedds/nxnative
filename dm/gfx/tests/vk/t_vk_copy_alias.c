// Copies see the GPU writes queued before them: the Vulkan counterpart of dm/gfx/tests/t_copy_alias.m. A producer (a buffer copy, an inline update, a fill) writes 256 bytes
// and a consumer copy reads them back in the same submission; the producer's destination and the consumer's source are in one memory pool or in two (two Vulkan buffers: the
// pools' imports), back to back or with a render pass between them, and the consumer reads at the producer's address or at an offset into it (a partial overlap). The Metal
// test's two other subjects need what this platform does not have yet: aliases of one allocation as two no-copy Metal buffers (Metal's), and sparse pools mapped twice onto one
// physical pool, which hle_nvn.c does not alias off Apple (vpool_map: no vm_remap; the aliasing comes with the host port): those cases are not run here, and say so.
// Checks: per case the bytes read back, and the validation check
#include "t_vk_harness.h"
#include "shaders/gen/fullscreen_vert.h"
#include "shaders/gen/red_frag.h"

int main(void) {
    hv_up("t_vk_copy_alias");
    hv_device(0);
    NVNqueue queue; NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&queue, &qb);
    hv_pool a, b; if (!hv_pool_make(&a, 1 << 18) || !hv_pool_make(&b, 16384)) { t("memory pools allocated", 0); return hv_finish(); }
    hv_cmds c = {a.mem + 131072, 0, 65536};
    static NVNprogram prog;
    const hv_shader sh[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"red", 1, HV_SPV(red_frag_spv), NULL, NULL}};
    hv_program(&prog, &a, 0, sh, 2);
    tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 8, 8, 1, 1, 0);
    const tex_t *rt[1] = {&target};
    const size_t n = 256;
    char name[200];
    static const char *const ops[3] = {"copy", "inline update", "fill"};
    for (int split = 0; split < 2; split++) for (int two = 0; two < 2; two++) for (int shift = 0; shift < 2; shift++) for (int op = 0; op < 3; op++) {
        uint8_t *io = a.mem + 8192, *dst = two ? b.mem + 4096 : a.mem + 16384, *res = a.mem + 32768;
        memset(dst, 0, n); memset(res, 0xEE, n);
        uint32_t value = op == 2 ? 0x78563412u : 0x6d6d6d6du + (uint32_t)(split * 8 + two * 4 + shift * 2 + op);
        for (size_t i = 0; i < n; i += 4) memcpy(io + i, &value, 4);
        if (op == 0) hv_b2b(&c, io, dst, n);
        else if (op == 1) hv_update(&c, dst, io, (uint32_t)n);
        else hv_fill(&c, dst, n, value);
        if (split) { hv_rt(&c, 1, rt, NULL); hv_bind(&c, &prog, 3); hv_depth(&c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&c, NVN_FACE_NONE); hv_draw(&c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1); }
        size_t at = shift ? 100 : 0;   // a consumer reading from the middle of what the producer wrote (and past it, where the destination is still zero)
        hv_b2b(&c, dst + at, res, n);
        hv_submit(&queue, &c);
        uint8_t want[256]; memcpy(want, io, n - at); memset(want + (n - at), 0, at);
        snprintf(name, sizeof name, "%s, %s, %s, consumer at +%u: the copy reads the preceding %s", ops[op], two ? "two pools" : "one pool", split ? "a pass between" : "back to back", (unsigned)at, ops[op]);
        if (memcmp(res, want, n)) fprintf(stderr, "%s: byte 0 = %#x, expected %#x\n", name, res[0], want[0]);
        t(name, !memcmp(res, want, n));
    }
    printf("note: not run here: sparse pools mapped twice onto one physical pool (hle_nvn.c vpool_map does not alias them off Apple), and Metal's no-copy buffer aliases\n");
    hv_pool_free(&b); hv_pool_free(&a);
    return hv_finish();
}
