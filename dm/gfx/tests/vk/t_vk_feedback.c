// Feedback draws on the Vulkan backend (docs/design/vulkan-backend.md 5.5, step P8): a draw that samples a subresource attached to its own pass. The Metal backend snapshots the
// attachment for the fragment shaders a profile lists (feedback_snap; rendering.md, "Same-pass feedback read through a snapshot"); the Vulkan backend needs no list: every
// such draw is detected from its descriptors, and when a command of the pass wrote the sampled attachment the pass is split before the draw, so the full barrier between the
// two passes orders those writes before its reads (no barrier can run inside a dynamic-rendering pass on Vulkan 1.3). The port of dm/gfx/tests/t_feedback.m's semantics: a draw
// reads what the attachment held before it (a cleared red read through the combined slot 0 and written back with its channels rotated: blue), a second draw after
// ORDER_FRAGMENTS | INVALIDATE_TEXTURE reads what the first wrote (green), and the same through the separate texture slot 128 with a separate sampler; beyond it, a depth
// attachment written by one draw and read by the next (soft particles: tested, not written) splits the pass once, and a third draw that reads it again does not split it (the
// split follows the attachments written, not every draw). Checks: the pixels, the number of splits, and the validation check (sync validation sees no hazard)
#include "t_vk_harness.h"
#include "shaders/gen/fullscreen_vert.h"
#include "shaders/gen/fb_swz_frag.h"
#include "shaders/gen/fb_sep_frag.h"
#include "shaders/gen/fb_depth_frag.h"
#include "shaders/gen/depth_vert.h"
#include "shaders/gen/depth_frag.h"

uint64_t hvk_test_feedback_splits(void);
static NVNqueue g_q; static hv_cmds g_c; static hv_pool g_p; static NVNtexturePool g_tp; static NVNsamplerPool g_sp; static size_t g_off = 0x20000;
static void make(NVNtexture *t, int format) {
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetDevice(&tb, &g_dev); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetFormat(&tb, format); h_nvnTextureBuilderSetSize2D(&tb, 8, 8); h_nvnTextureBuilderSetFlags(&tb, 0);
    h_nvnTextureBuilderSetStorage(&tb, &g_p.pool, (ptrdiff_t)g_off); g_off += 0x10000;
    h_nvnTextureInitialize(t, &tb);
}
static void pools(void) { p_set_pool tp = {(const dpool_t *)&g_tp}, sp = {(const dpool_t *)&g_sp}; hv_emit(&g_c, OP_SET_TEX_POOL, &tp, sizeof tp); hv_emit(&g_c, OP_SET_SAMP_POOL, &sp, sizeof sp); }
static void bind_comb(int id) { p_bind_tex1 b = {1, 0, (uint64_t)(uint32_t)id | (uint64_t)1 << 20, nvn_dpool_base(&g_tp, 1), nvn_dpool_base(&g_sp, 0)}; hv_emit(&g_c, OP_BIND_TEX1, &b, sizeof b); }
static void barrier(int bits) { int32_t b = bits; hv_emit(&g_c, OP_BARRIER, &b, sizeof b); }
static int all_px(const tex_t *t, const uint8_t want[4], int tol, const char *label) {
    uint8_t px[256]; int ok = hv_read(t, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 8, 8, 4, px);
    for (int i = 0; ok && i < 64; i++) for (int c = 0; c < 4; c++) if (abs((int)px[4 * i + c] - want[c]) > tol) {
        fprintf(stderr, "%s: pixel %d = %u %u %u %u, expected %u %u %u %u\n", label, i, px[4 * i], px[4 * i + 1], px[4 * i + 2], px[4 * i + 3], want[0], want[1], want[2], want[3]); ok = 0; break; }
    return ok;
}

int main(void) {
    hv_up("t_vk_feedback");
    hv_device(1);
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&g_q, &qb);
    if (!hv_pool_make(&g_p, 1 << 20)) { t("memory pool allocated", 0); return hv_finish(); }
    memset(g_p.mem, 0, 1 << 20);
    g_c = (hv_cmds){g_p.mem + 0x8000, 0, 0x8000};
    static const char comb[] = "[{\"kind\":\"tex\",\"reg\":0,\"name\":\"t\",\"class\":\"combined\",\"set\":2,\"binding\":2048,\"dim\":\"2d\",\"sampled\":\"float\"},"
                               "{\"kind\":\"samp_tex\",\"reg\":0,\"name\":\"t\",\"class\":\"combined\",\"set\":2,\"binding\":2048,\"dim\":\"2d\",\"sampled\":\"float\"}]";
    static const char sep[] = "[{\"kind\":\"tex\",\"reg\":128,\"name\":\"t\",\"class\":\"combined\",\"set\":2,\"binding\":2176,\"dim\":\"2d\",\"sampled\":\"float\"},"
                              "{\"kind\":\"samp\",\"reg\":0,\"name\":\"t\",\"class\":\"combined\",\"set\":2,\"binding\":2176,\"dim\":\"2d\",\"sampled\":\"float\"}]";
    const char *dvin = "[{\"location\":0,\"type\":\"float\",\"n\":4},{\"location\":1,\"type\":\"float\",\"n\":1}]";
    static NVNprogram pswz, psep, pdep, pwrite;
    const hv_shader s1[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"fb_swz", 1, HV_SPV(fb_swz_frag_spv), comb, NULL}};
    const hv_shader s2[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"fb_sep", 1, HV_SPV(fb_sep_frag_spv), sep, NULL}};
    const hv_shader s3[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"fb_depth", 1, HV_SPV(fb_depth_frag_spv), comb, NULL}};
    const hv_shader s4[2] = {{"depth_vs", 0, HV_SPV(depth_vert_spv), NULL, dvin}, {"depth_fs", 1, HV_SPV(depth_frag_spv), NULL, NULL}};
    hv_program(&pswz, &g_p, 0, s1, 2); hv_program(&psep, &g_p, 1024, s2, 2); hv_program(&pdep, &g_p, 2048, s3, 2); hv_program(&pwrite, &g_p, 3072, s4, 2);
    h_nvnTexturePoolInitialize(&g_tp, &g_p.pool, 0x10000, 64); h_nvnSamplerPoolInitialize(&g_sp, &g_p.pool, 0x18000, 16);
    NVNsamplerBuilder sb; NVNsampler samp; h_nvnSamplerBuilderSetDefaults(&sb); h_nvnSamplerInitialize(&samp, &sb); h_nvnSamplerPoolRegisterSampler(&g_sp, 1, &samp);
    static const uint8_t blue[4] = {0, 0, 255, 255}, green[4] = {0, 255, 0, 255};

    // 1. combined slot 0: a draw reads the red clear of its own attachment and writes blue; after a fragment barrier the next one reads the blue and writes green
    NVNtexture a; make(&a, NVN_FORMAT_RGBA8); h_nvnTexturePoolRegisterTexture(&g_tp, 1, &a, NULL);
    const tex_t *rt[1] = {(const tex_t *)&a};
    uint64_t s0 = 0;
    hv_rt(&g_c, 1, rt, NULL); hv_clear_color(&g_c, 0, 15, 1, 0, 0, 1);
    pools(); hv_bind(&g_c, &pswz, 0x1f); hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&g_c, NVN_FACE_NONE); bind_comb(1);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    hv_submit(&g_q, &g_c);
    uint64_t s1n = hvk_test_feedback_splits();
    t("a draw sampling its attachment reads what it held before (red, written back rotated: blue)", all_px(rt[0], blue, 0, "first feedback draw"));
    t("the clear that opened the pass splits it before the feedback draw (1 split)", s1n - s0 == 1);
    hv_rt(&g_c, 1, rt, NULL); pools(); bind_comb(1);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    barrier(NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_INVALIDATE_TEXTURE_BIT);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    hv_submit(&g_q, &g_c);
    uint64_t s2n = hvk_test_feedback_splits();
    // blue → (rotated) green → (rotated) red: two draws in one pass, the second reading what the first wrote
    static const uint8_t red[4] = {255, 0, 0, 255};
    t("after a fragment barrier the next feedback draw reads what the previous one wrote (blue → green → red)", all_px(rt[0], red, 0, "second and third feedback draws"));
    t("the second draw of the pass splits it (it reads what the first wrote): 1 split", s2n - s1n == 1);

    // 2. the separate texture slot 128 with the separate sampler slot 0
    NVNtexture b; make(&b, NVN_FORMAT_RGBA8); h_nvnTexturePoolRegisterTexture(&g_tp, 2, &b, NULL);
    const tex_t *rt2[1] = {(const tex_t *)&b};
    hv_rt(&g_c, 1, rt2, NULL); hv_clear_color(&g_c, 0, 15, 1, 0, 0, 1);
    pools(); hv_bind(&g_c, &psep, 0x1f);
    p_bind_handles th; memset(&th, 0, sizeof th); th.stage = 1; th.first = NVN_SEPTEX_SLOT0; th.n = 1; th.h[0] = (uint64_t)(2 + nvn_dpool_base(&g_tp, 1));
    hv_emit(&g_c, OP_BIND_TEX, &th, offsetof(p_bind_handles, h) + 8);
    p_bind_handles sh; memset(&sh, 0, sizeof sh); sh.stage = 1; sh.first = 0; sh.n = 1; sh.h[0] = (uint64_t)(1 + nvn_dpool_base(&g_sp, 0)) << 20;
    hv_emit(&g_c, OP_BIND_SAMP, &sh, offsetof(p_bind_handles, h) + 8);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    hv_submit(&g_q, &g_c);
    t("separate texture slot 128: the feedback draw reads the red clear (blue written)", all_px(rt2[0], blue, 0, "separate slot"));

    // 3. a depth attachment written by one draw and read by the next two (tested, not written): one split, before the first reader only
    NVNtexture c, d; make(&c, NVN_FORMAT_RGBA8); make(&d, NVN_FORMAT_DEPTH32F); h_nvnTexturePoolRegisterTexture(&g_tp, 3, &d, NULL);
    float *vb = (float *)(void *)(g_p.mem + 0x1c000);
    const float tri[3][5] = {{-1, -1, 0, 1, 0.25f}, {3, -1, 0, 1, 0.25f}, {-1, 3, 0, 1, 0.25f}};
    memcpy(vb, tri, sizeof tri);
    const tex_t *rt3[1] = {(const tex_t *)&c};
    hv_rt(&g_c, 1, rt3, (const tex_t *)&d); hv_clear_color(&g_c, 0, 15, 0, 0, 0, 1);
    p_clear_ds cd = {1.0f, 1, 0, 0}; hv_emit(&g_c, OP_CLEAR_DS, &cd, sizeof cd);
    hv_bind(&g_c, &pwrite, 0x1f); hv_depth(&g_c, 1, 1, NVN_DEPTH_FUNC_ALWAYS);
    va_st at[2] = {{NVN_FORMAT_RGBA32F, 0, 0}, {NVN_FORMAT_R32F, 16, 0}}; hv_attrs(&g_c, 2, at); vs_st st[1] = {{20, 0}}; hv_streams(&g_c, 1, st); hv_vbuf(&g_c, 0, vb, sizeof tri);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);   // depth 0.25 everywhere
    uint64_t c3 = hvk_test_feedback_splits();
    pools(); hv_bind(&g_c, &pdep, 0x1f); hv_depth(&g_c, 1, 0, NVN_DEPTH_FUNC_ALWAYS); bind_comb(3);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    hv_submit(&g_q, &g_c);
    uint64_t c4 = hvk_test_feedback_splits();
    static const uint8_t quarter[4] = {64, 0, 0, 255};
    t("a draw reading the depth attachment the previous draw wrote reads it (0.25 → red 64)", all_px(rt3[0], quarter, 1, "depth reader"));
    if (c4 - c3 != 1) fprintf(stderr, "depth readers split the pass %llu times\n", (unsigned long long)(c4 - c3));
    t("the depth readers split the pass once (the second reader finds no depth write since)", c4 - c3 == 1);
    (void)green;
    hv_pool_free(&g_p);
    return hv_finish();
}
