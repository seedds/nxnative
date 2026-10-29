// Both clear paths of a DEPTH16 texture store the same depth as a fragment writing it: the Vulkan port of dm/gfx/tests/t_depth16_clear.m, with the same 1042 depths (a thousand
// evenly spaced, six fractions, and the float below, at and above the half-way point of twelve 16-bit bins). Each depth goes to its own layer of three 2D array textures: written by
// a fragment shader (gl_FragDepth, the reference), cleared by nvnCommandBufferClearDepthStencil (the load operation of the target's pass) and by nvnCommandBufferClearTexture
// (vkCmdClearDepthStencilImage); the three are read back and compared texel by texel. The Metal test also counts an unadjusted Metal clear for comparison; the Vulkan backend has
// no adjustment to compare against. Checks: the three readbacks and one per clear path (no depth differs), and the validation check
#include "t_vk_harness.h"
#include <math.h>
#include "shaders/gen/depth_vert.h"
#include "shaders/gen/depth_frag.h"

enum { N = 1000 + 6 + 12 * 3, W = 8, H = 8, BATCH = 32 };

int main(void) {
    hv_up("t_vk_depth16_clear");
    hv_device(0);
    NVNqueue queue; NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&queue, &qb);
    static float depth[N];
    for (int i = 0; i < 1000; i++) depth[i] = (float)i / 999.f;
    const float extra[6] = {0.5f, 0.25f, 0.75f, 1.f / 3, 2.f / 3, 32767.5f / 65535};
    for (int i = 0; i < 6; i++) depth[1000 + i] = extra[i];
    const int bins[12] = {0, 1, 2, 3, 127, 128, 255, 256, 32766, 32767, 32768, 65534};
    for (int i = 0; i < 12; i++) {
        float d = (float)((bins[i] + 0.5) / 65535.0);
        depth[1006 + 3 * i] = nextafterf(d, 0); depth[1007 + 3 * i] = d; depth[1008 + 3 * i] = nextafterf(d, 1);
    }
    // Guest memory: the program's data, the full-screen triangle, three depths per layer (one per vertex), the command memory
    hv_pool mp; if (!hv_pool_make(&mp, 1 << 20)) { t("memory pool allocated", 0); return hv_finish(); }
    float *tri = (float *)(void *)(mp.mem + 4096), *dv = (float *)(void *)(mp.mem + 8192);
    const float pos[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    memcpy(tri, pos, sizeof pos);
    for (int i = 0; i < N; i++) for (int k = 0; k < 3; k++) dv[3 * i + k] = depth[i];
    hv_cmds cmds = {mp.mem + 65536, 0, 1 << 19};
    static NVNprogram prog;
    const hv_shader sh[2] = {{"depth_vs", 0, HV_SPV(depth_vert_spv), NULL, "[{\"location\":0,\"type\":\"float\",\"n\":4},{\"location\":1,\"type\":\"float\",\"n\":1}]"},
                             {"depth_fs", 1, HV_SPV(depth_frag_spv), NULL, NULL}};
    hv_program(&prog, &mp, 0, sh, 2);
    tex_t tex[3];   // fragment, ClearDepthStencil, ClearTexture
    for (int k = 0; k < 3; k++) tex[k] = hv_tex(NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_DEPTH16, W, H, N, 1, 0);
    const va_st attrs[2] = {{.fmt = NVN_FORMAT_RGBA32F, .stream = 0}, {.fmt = NVN_FORMAT_R32F, .stream = 1}};
    const vs_st streams[2] = {{16, 0}, {4, 0}};
    for (int i0 = 0; i0 < N; i0 += BATCH) {
        for (int i = i0; i < N && i < i0 + BATCH; i++) {
            p_set_rt rt; memset(&rt, 0, sizeof rt);
            rt.depth = &tex[0]; rt.has_dview = 1; rt.dview.set_layers = 1; rt.dview.min_layer = (uint16_t)i; rt.dview.nlayers = 1;
            hv_emit(&cmds, OP_SET_RT, &rt, sizeof rt);
            if (i == i0) {
                hv_bind(&cmds, &prog, 3); hv_depth(&cmds, 1, 1, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&cmds, NVN_FACE_NONE);
                hv_attrs(&cmds, 2, attrs); hv_streams(&cmds, 2, streams); hv_vbuf(&cmds, 0, tri, sizeof pos);
            }
            p_clear_ds far = {1.f, 1, 0, 0}; hv_emit(&cmds, OP_CLEAR_DS, &far, sizeof far);   // the fragment's layer starts at 1, as the Metal test's
            hv_vbuf(&cmds, 1, dv + 3 * i, 12);
            hv_draw(&cmds, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
            rt.depth = &tex[1]; hv_emit(&cmds, OP_SET_RT, &rt, sizeof rt);
            p_clear_ds clear = {depth[i], 1, 0, 0}; hv_emit(&cmds, OP_CLEAR_DS, &clear, sizeof clear);
            p_clear_tex ct; memset(&ct, 0, sizeof ct); ct.tex = &tex[2]; ct.mask = 15; ct.r = (NVNcopyRegion){0, 0, i, W, H, 1}; ct.c.f[0] = depth[i];
            hv_emit(&cmds, OP_CLEAR_TEX, &ct, sizeof ct);
        }
        hv_submit(&queue, &cmds);
    }
    static uint16_t v[3][N][W * H];
    int read = 1;
    for (int k = 0; k < 3; k++) { int ok = hv_read_n(&tex[k], VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, N, 0, 0, W, H, 2, v[k]); read &= ok;
        t(k == 0 ? "the fragment depths are read back" : k == 1 ? "the ClearDepthStencil depths are read back" : "the ClearTexture depths are read back", ok); }
    int fail[2] = {0, 0}, shown = 0;
    for (int i = 0; read && i < N; i++) {
        int bad[2] = {0, 0};
        for (int p = 0; p < W * H; p++) { bad[0] |= v[1][i][p] != v[0][i][p]; bad[1] |= v[2][i][p] != v[0][i][p]; }
        if ((bad[0] || bad[1]) && shown++ < 10) fprintf(stderr, "  depth %.9f: fragment %u, ClearDepthStencil %u, ClearTexture %u\n", depth[i], v[0][i][0], v[1][i][0], v[2][i][0]);
        fail[0] += bad[0]; fail[1] += bad[1];
    }
    printf("t_vk_depth16_clear: %d depths, ClearDepthStencil differs from the fragment in %d, ClearTexture in %d (0.5: fragment %u, clear %u)\n", N, fail[0], fail[1], v[0][1000][0], v[1][1000][0]);
    t("ClearDepthStencil stores every depth as the fragment does", read && !fail[0]);
    t("ClearTexture stores every depth as the fragment does", read && !fail[1]);
    hv_pool_free(&mp);
    return hv_finish();
}
