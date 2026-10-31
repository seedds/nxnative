// Transform feedback on the Vulkan backend (docs/design/vulkan-backend.md section 6 and 2.2, step P8): native VK_EXT_transform_feedback driven by the frontend's records
// (OP_TF_BIND, OP_TF_STATE, OP_RAST_DISCARD, DrawTransformFeedback's indirect draw at the control buffer + 16): the Vulkan port of dm/gfx/tests/t_transform_feedback.m.
// Its cases, with the Metal test's numbers: an interleaved capture of five floats a vertex (stride 20) under rasterizer discard, paused (the byte offset saved into the game's
// control buffer) and resumed (appending: only the vertices that fit are captured, the rest of the buffer untouched); three buffers with their own strides (Pokémon Legends: Z-A's
// skinning layout); a capture with rasterization on and a colour mask of 0 (Donkey Kong Country: Tropical Freeze's fur); a geometry shader's capture, which keeps whole
// triangles only; DrawTransformFeedback drawing the saved count in the same submission, twice with two counts (the GPU reads each in order). Beyond the Metal test (open
// there): indexed and instanced captures, and two capturing pipelines in one pass (the capture pauses into its counters at the pipeline bind and resumes). Checks: the control
// buffers' byte offsets, every captured value, the untouched bytes past a buffer's end, the vertices DrawTransformFeedback drew, and the validation check
#include "t_vk_harness.h"
#include "shaders/gen/xfb5_vert.h"
#include "shaders/gen/xfb5b_vert.h"
#include "shaders/gen/xfb3_vert.h"
#include "shaders/gen/xfb_tri_geom.h"
#include "shaders/gen/pt_vert.h"
#include "shaders/gen/auto_vert.h"
#include "shaders/gen/red_frag.h"

static NVNqueue g_q; static hv_cmds g_c; static hv_pool g_p;
static void tf_bind(int i, void *addr, uint64_t size) { p_tf_bind b = {i, 0, (uint64_t)(uintptr_t)addr, size}; hv_emit(&g_c, OP_TF_BIND, &b, sizeof b); }
static void tf_state(int what, void *ctl) { p_tf_state s = {what, 0, (uint64_t)(uintptr_t)ctl}; hv_emit(&g_c, OP_TF_STATE, &s, sizeof s); }   // 0 begin, 1 end, 2 pause, 3 resume
static void discard(int on) { p_rast_discard d = {on}; hv_emit(&g_c, OP_RAST_DISCARD, &d, sizeof d); }
static void draw_auto(void *ctl) {   // nvnCommandBufferDrawTransformFeedback(POINTS, ctl), as hle_nvn.c records it
    p_draw d; memset(&d, 0, sizeof d); d.prim = NVN_DRAW_PRIMITIVE_POINTS; d.instances = 1; d.ind = (uint64_t)(uintptr_t)ctl + 16; hv_emit(&g_c, OP_DRAW, &d, sizeof d);
}
static int record(const float *p, float n) {   // the five values vertex n captures (xfb5.vert)
    const float want[5] = {n, n + 10, n + 40, n + 50, n + 60};
    int ok = !memcmp(p, want, sizeof want);
    if (!ok) fprintf(stderr, "record of vertex %g: %g %g %g %g %g\n", n, p[0], p[1], p[2], p[3], p[4]);
    return ok;
}

int main(void) {
    hv_up("t_vk_transform_feedback");
    hv_device(1);
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&g_q, &qb);
    if (!hv_pool_make(&g_p, 1 << 20)) { t("memory pool allocated", 0); return hv_finish(); }
    memset(g_p.mem, 0, 1 << 20);
    g_c = (hv_cmds){g_p.mem + 512 * 1024, 0, 65536};
    uint32_t *ctl = (uint32_t *)(void *)(g_p.mem + 16384), *ctl2 = ctl + 16, *ctl3 = ctl + 32, *ctl4 = ctl + 48;
    float *out = (float *)(void *)(g_p.mem + 32768), *second = (float *)(void *)(g_p.mem + 36864), *third = (float *)(void *)(g_p.mem + 40960);
    uint32_t *written = (uint32_t *)(void *)(g_p.mem + 45056), *written2 = written + 64;
    uint16_t *idx = (uint16_t *)(void *)(g_p.mem + 49152);
    static NVNprogram p5, p5b, p3, pgeo, pauto, p5r;
    static const char ssbo[] = "[{\"kind\":\"sbuf\",\"reg\":0,\"name\":\"B\",\"class\":\"ssbo\",\"set\":1,\"binding\":0,\"rw\":true}]";
    const hv_shader s5[1] = {{"xfb5", 0, HV_SPV(xfb5_vert_spv), NULL, NULL}}, s5b[1] = {{"xfb5b", 0, HV_SPV(xfb5b_vert_spv), NULL, NULL}}, s3[1] = {{"xfb3", 0, HV_SPV(xfb3_vert_spv), NULL, NULL}};
    const hv_shader sgeo[2] = {{"pt", 0, HV_SPV(pt_vert_spv), NULL, NULL}, {"xfb_tri", 2, HV_SPV(xfb_tri_geom_spv), NULL, NULL}};
    const hv_shader sauto[1] = {{"auto", 0, HV_SPV(auto_vert_spv), ssbo, NULL}}, s5r[2] = {{"xfb5r", 0, HV_SPV(xfb5_vert_spv), NULL, NULL}, {"red", 1, HV_SPV(red_frag_spv), NULL, NULL}};
    hv_program(&p5, &g_p, 0, s5, 1); hv_program(&p5b, &g_p, 1024, s5b, 1); hv_program(&p3, &g_p, 2048, s3, 1); hv_program(&pgeo, &g_p, 3072, sgeo, 2);
    hv_program(&pauto, &g_p, 4096, sauto, 1); hv_program(&p5r, &g_p, 5120, s5r, 2);
    tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 8, 8, 1, 1, 0);
    const tex_t *rt[1] = {&target};
    hv_rt(&g_c, 1, rt, NULL); hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&g_c, NVN_FACE_NONE);

    // 1. capture under rasterizer discard, paused: the byte offset saved (3 vertices of 20 bytes)
    hv_bind(&g_c, &p5, 0x1f); discard(1); tf_bind(0, out, 100);
    tf_state(0, ctl); hv_draw(&g_c, NVN_DRAW_PRIMITIVE_POINTS, 2, 3, 1); tf_state(2, ctl);
    hv_submit(&g_q, &g_c);
    if (ctl[0] != 60) fprintf(stderr, "pause saved %u\n", ctl[0]);
    t("pause saves the byte offset reached (3 vertices of 20 bytes: 60)", ctl[0] == 60);
    t("interleaved captured values and order (vertices 2, 3, 4)", record(out, 2) && record(out + 5, 3) && record(out + 10, 4));
    // 2. resumed in another submission: appends; a buffer of 100 bytes takes 2 more vertices, and its end is not crossed
    tf_state(3, ctl); hv_draw(&g_c, NVN_DRAW_PRIMITIVE_POINTS, 9, 3, 1); tf_state(1, ctl2);
    hv_submit(&g_q, &g_c);
    if (ctl2[0] != 100) fprintf(stderr, "end saved %u\n", ctl2[0]);
    t("resume appends only the vertices that fit (100 bytes), saved into the end's control buffer", ctl2[0] == 100);
    t("the appended vertices 9 and 10 follow the paused capture", record(out + 15, 9) && record(out + 20, 10));
    t("overflow leaves the bytes past the buffer untouched", out[25] == 0 && out[26] == 0);
    // 3. DrawTransformFeedback in the same submission as the capture, twice with two counts: the saved byte offset / 20 vertices, read by the GPU in order
    memset(out, 0, 512);
    tf_bind(0, out, 100); tf_state(0, ctl3); hv_draw(&g_c, NVN_DRAW_PRIMITIVE_POINTS, 6, 3, 1); tf_state(1, ctl3);
    hv_bind(&g_c, &pauto, 0x1f); hv_buf(&g_c, OP_BIND_SSBO, 0, 0, written, 256); draw_auto(ctl3);
    hv_bind(&g_c, &p5, 0x1f); tf_bind(0, second, 100); tf_state(0, ctl4); hv_draw(&g_c, NVN_DRAW_PRIMITIVE_POINTS, 0, 2, 1); tf_state(1, ctl4);
    hv_bind(&g_c, &pauto, 0x1f); hv_buf(&g_c, OP_BIND_SSBO, 0, 0, written2, 256); draw_auto(ctl4);
    hv_submit(&g_q, &g_c);
    t("the capture before DrawTransformFeedback saved 60 bytes", ctl3[0] == 60 && record(out, 6) && record(out + 10, 8));
    if (written[0] != 0x800 || written[2] != 0x802 || written[3]) fprintf(stderr, "DrawTransformFeedback wrote %#x %#x %#x %#x\n", written[0], written[1], written[2], written[3]);
    t("DrawTransformFeedback draws the saved count (3 vertices) in the same submission", written[0] == 0x800 && written[1] == 0x801 && written[2] == 0x802 && written[3] == 0);
    t("a second capture's DrawTransformFeedback draws its own count (2 vertices)", ctl4[0] == 40 && written2[0] == 0x800 && written2[1] == 0x801 && written2[2] == 0);
    // 4. three buffers, their own strides (12, 4, 4)
    memset(out, 0, 1024); memset(second, 0, 256); memset(third, 0, 256);
    hv_bind(&g_c, &p3, 0x1f); tf_bind(0, out, 36); tf_bind(1, second, 12); tf_bind(2, third, 12);
    tf_state(0, ctl); hv_draw(&g_c, NVN_DRAW_PRIMITIVE_POINTS, 4, 3, 1); tf_state(1, ctl);
    hv_submit(&g_q, &g_c);
    const float position[9] = {4, 14, 24, 5, 15, 25, 6, 16, 26}, packed[3] = {44, 45, 46};
    t("three streams advance by their own byte strides (36, 12, 12)", ctl[0] == 36 && ctl[1] == 12 && ctl[2] == 12);
    t("separate position stream", !memcmp(out, position, sizeof position));
    t("separate packed streams", !memcmp(second, packed, sizeof packed) && !memcmp(third, packed, sizeof packed));
    tf_bind(1, NULL, 0); tf_bind(2, NULL, 0);
    // 5. rasterization on, no colour written (channel mask 0), no SetRasterizerDiscard: Tropical Freeze's fur capture
    memset(out, 0, 1024);
    chmask_st none = {0}; hv_state(&g_c, 2, &none, sizeof none);
    hv_bind(&g_c, &p5r, 0x1f); discard(0); tf_bind(0, out, 100);
    tf_state(0, ctl); hv_draw(&g_c, NVN_DRAW_PRIMITIVE_POINTS, 1, 3, 1); tf_state(1, ctl);
    hv_submit(&g_q, &g_c);
    t("a rasterized draw with no colour writes captures (60 bytes)", ctl[0] == 60 && record(out, 1) && record(out + 5, 2) && record(out + 10, 3));
    chmask_st all = {0xffffffffu}; hv_state(&g_c, 2, &all, sizeof all);
    // 6. a geometry shader's capture keeps whole triangles: 3 points → 3 triangles, a buffer of 7 vertices takes 2
    memset(out, 0, 1024);
    hv_bind(&g_c, &pgeo, 0x1f); discard(1); tf_bind(0, out, 56);
    tf_state(0, ctl); hv_draw(&g_c, NVN_DRAW_PRIMITIVE_POINTS, 0, 3, 1); tf_state(1, ctl);
    hv_submit(&g_q, &g_c);
    int tri = 1; for (int v = 0; v < 6; v++) tri &= out[2 * v] == (float)(v / 3) && out[2 * v + 1] == (float)(v % 3);
    if (ctl[0] != 48) fprintf(stderr, "geometry capture saved %u\n", ctl[0]);
    t("geometry capture keeps complete primitives (2 triangles, 48 bytes)", ctl[0] == 48 && tri);
    t("the incomplete third triangle is not captured", out[12] == 0 && out[13] == 0);
    // 7. indexed and instanced captures (open on Metal): indices 6 2 4, then 2 vertices x 2 instances; 100 bytes take 5 vertices
    memset(out, 0, 1024); idx[0] = 6; idx[1] = 2; idx[2] = 4;
    hv_bind(&g_c, &p5, 0x1f); tf_bind(0, out, 100);
    tf_state(0, ctl); hv_draw_idx(&g_c, NVN_DRAW_PRIMITIVE_POINTS, 1, 3, idx, 0, 1); hv_draw(&g_c, NVN_DRAW_PRIMITIVE_POINTS, 0, 2, 2); tf_state(1, ctl);
    hv_submit(&g_q, &g_c);
    t("an indexed capture follows the index order (6, 2, 4)", record(out, 6) && record(out + 5, 2) && record(out + 10, 4));
    t("an instanced capture follows it until the buffer is full (0, 1)", ctl[0] == 100 && record(out + 15, 0) && record(out + 20, 1));
    // 8. two capturing pipelines in one pass: the capture pauses at the pipeline bind and resumes where it was
    memset(out, 0, 1024);
    hv_bind(&g_c, &p5, 0x1f); tf_bind(0, out, 100);
    tf_state(0, ctl); hv_draw(&g_c, NVN_DRAW_PRIMITIVE_POINTS, 0, 2, 1); hv_bind(&g_c, &p5b, 0x1f); hv_draw(&g_c, NVN_DRAW_PRIMITIVE_POINTS, 0, 1, 1); tf_state(1, ctl);
    hv_submit(&g_q, &g_c);
    t("two capturing pipelines in one pass append in order (0, 1, then the other program's 100)", ctl[0] == 60 && record(out, 0) && record(out + 5, 1) && record(out + 10, 100));
    hv_pool_free(&g_p);
    return hv_finish();
}
