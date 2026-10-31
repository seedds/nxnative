// Tessellation on the Vulkan backend (docs/design/vulkan-backend.md section 6 and 2.2, step P8): native control and evaluation stages, which the Metal backend does not draw
// (shaders.md, "Tessellation stages are not drawn": Luigi's Mansion 3's title, Bayonetta 3). No Metal counterpart: new. A quad patch of 4 control points covering the target,
// its levels written by the control shader from a uniform buffer the game binds to the control stage, tessellated with equal spacing; the evaluation shader captures every
// generated vertex (transform feedback, 16 bytes), so the triangles made are counted exactly: uniform levels n give 2 n^2 triangles; inner (3, 4) with outer (2, 3, 4, 5) give
// 24 (2 (3 - 2)(4 - 2) inside, and each edge's strip as many triangles as its outer and inner segments together: 2 + 2, 3 + 1, 4 + 2, 5 + 1); a draw of 8 vertices is two
// patches (the patch size is the control shader's output vertex count: nvnCommandBufferSetPatchSize is not recorded); the triangle domain is made of concentric rings, each strip
// between rings of a and b segments a side 3 (a + b) triangles, and an odd level adds the inner triangle (level 4: 24, level 3: 13), GL's and Vulkan's rule. The quad
// covers every pixel red. Checks: each count, the coverage, and the validation check
#include "t_vk_harness.h"
#include "shaders/gen/pos_vert.h"
#include "shaders/gen/tess_quad_tesc.h"
#include "shaders/gen/tess_quad_tese.h"
#include "shaders/gen/tess_tri_tesc.h"
#include "shaders/gen/tess_tri_tese.h"
#include "shaders/gen/red_frag.h"

static NVNqueue g_q; static hv_cmds g_c; static hv_pool g_p;
static float *g_lv, *g_vb, *g_cap; static uint32_t *g_tctl;
static NVNprogram g_quad, g_tri;
// One draw of `count` control points through prog with these levels into a fresh 8x8 target: the triangles the capture counted (-1 when the counter is not a whole number of
// triangles), and the red pixels in *red
static int tess(const NVNprogram *prog, const float outer[4], const float inner[2], int count, int *red) {
    tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 8, 8, 1, 1, 0);
    const tex_t *rt[1] = {&target};
    memcpy(g_lv, outer, 16); g_lv[4] = inner[0]; g_lv[5] = inner[1]; g_lv[6] = g_lv[7] = 0;
    memset(g_cap, 0, 65536); memset(g_tctl, 0, 32);
    hv_rt(&g_c, 1, rt, NULL); hv_clear_color(&g_c, 0, 15, 0, 0, 0, 1);
    hv_bind(&g_c, prog, 0x1f); hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&g_c, NVN_FACE_NONE);
    va_st a[1] = {{NVN_FORMAT_RGBA32F, 0, 0}}; hv_attrs(&g_c, 1, a); vs_st s[1] = {{16, 0}}; hv_streams(&g_c, 1, s); hv_vbuf(&g_c, 0, g_vb, 8 * 16);
    hv_buf(&g_c, OP_BIND_UBO, 3, 0, g_lv, 32);   // uniform slot 0 of the tessellation control stage (NVN stage 3)
    p_tf_bind b = {0, 0, (uint64_t)(uintptr_t)g_cap, 65536}; hv_emit(&g_c, OP_TF_BIND, &b, sizeof b);
    p_tf_state st = {0, 0, (uint64_t)(uintptr_t)g_tctl}; hv_emit(&g_c, OP_TF_STATE, &st, sizeof st);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_PATCHES, 0, count, 1);
    st.what = 1; hv_emit(&g_c, OP_TF_STATE, &st, sizeof st);
    hv_submit(&g_q, &g_c);
    uint8_t px[256]; *red = 0;
    if (hv_read(&target, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 8, 8, 4, px)) for (int i = 0; i < 64; i++) *red += px[4 * i] == 255 && px[4 * i + 1] == 0;
    return g_tctl[0] % 48 ? -1 : (int)(g_tctl[0] / 48);
}
static void check(const char *label, const NVNprogram *prog, const float outer[4], const float inner[2], int count, int want, int want_red) {
    int red, n = tess(prog, outer, inner, count, &red);
    char name[200]; snprintf(name, sizeof name, "%s: %d triangles (expected %d)%s", label, n, want, want_red ? ", every pixel red" : "");
    if (n != want || (want_red && red != 64)) fprintf(stderr, "%s: %d triangles, %d red pixels\n", label, n, red);
    t(name, n == want && (!want_red || red == 64));
}

int main(void) {
    hv_up("t_vk_tessellation");
    hv_device(1);
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&g_q, &qb);
    if (!hv_pool_make(&g_p, 1 << 20)) { t("memory pool allocated", 0); return hv_finish(); }
    memset(g_p.mem, 0, 1 << 20);
    g_c = (hv_cmds){g_p.mem + 512 * 1024, 0, 65536};
    g_lv = (float *)(void *)(g_p.mem + 16384); g_vb = (float *)(void *)(g_p.mem + 20480); g_tctl = (uint32_t *)(void *)(g_p.mem + 24576); g_cap = (float *)(void *)(g_p.mem + 65536);
    const char *vin = "[{\"location\":0,\"type\":\"float\",\"n\":4}]";
    const char *ubo = "[{\"kind\":\"cbuf\",\"reg\":0,\"name\":\"L\",\"class\":\"ubo\",\"set\":0,\"binding\":22}]";
    const hv_shader sq[4] = {{"tess_vs", 0, HV_SPV(pos_vert_spv), NULL, vin}, {"red", 1, HV_SPV(red_frag_spv), NULL, NULL},
                             {"tess_quad_tcs", 3, HV_SPV(tess_quad_tesc_spv), ubo, NULL}, {"tess_quad_tes", 4, HV_SPV(tess_quad_tese_spv), NULL, NULL}};
    const hv_shader st[4] = {{"tess_vs", 0, HV_SPV(pos_vert_spv), NULL, vin}, {"red", 1, HV_SPV(red_frag_spv), NULL, NULL},
                             {"tess_tri_tcs", 3, HV_SPV(tess_tri_tesc_spv), ubo, NULL}, {"tess_tri_tes", 4, HV_SPV(tess_tri_tese_spv), NULL, NULL}};
    hv_program(&g_quad, &g_p, 0, sq, 4); hv_program(&g_tri, &g_p, 2048, st, 4);
    const float quad[8][4] = {{-1, -1, 0, 1}, {1, -1, 0, 1}, {1, 1, 0, 1}, {-1, 1, 0, 1}, {-1, -1, 0, 1}, {1, -1, 0, 1}, {1, 1, 0, 1}, {-1, 1, 0, 1}};
    memcpy(g_vb, quad, sizeof quad);
    const float o4[4] = {4, 4, 4, 4}, i4[2] = {4, 4}, oa[4] = {2, 3, 4, 5}, ia[2] = {3, 4}, o2[4] = {2, 2, 2, 2}, i2[2] = {2, 2}, o1[4] = {1, 1, 1, 1}, i1[2] = {1, 1};
    check("quad domain, uniform level 4", &g_quad, o4, i4, 4, 32, 1);
    check("quad domain, inner (3, 4), outer (2, 3, 4, 5)", &g_quad, oa, ia, 4, 24, 1);
    check("quad domain, uniform level 1 (one quad)", &g_quad, o1, i1, 4, 2, 1);
    check("two quad patches in one draw of 8 control points, uniform level 2", &g_quad, o2, i2, 8, 16, 1);
    const float tri[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
    memcpy(g_vb, tri, sizeof tri);
    const float t4[4] = {4, 4, 4, 0}, t3[4] = {3, 3, 3, 0}, ti4[2] = {4, 0}, ti3[2] = {3, 0};
    check("triangle domain, uniform level 4 (concentric rings of 4, 2 and 0 segments a side: 3 (4 + 2) + 3 (2 + 0))", &g_tri, t4, ti4, 3, 24, 1);
    check("triangle domain, uniform level 3 (rings of 3 and 1 segments a side and the inner triangle: 3 (3 + 1) + 1)", &g_tri, t3, ti3, 3, 13, 1);
    hv_pool_free(&g_p);
    return hv_finish();
}
