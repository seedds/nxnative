// Rasterizer discard suppresses fragments while array, indexed and instanced draws still run their vertex shaders: the Vulkan port of dm/gfx/tests/t_rast_discard.m. The vertex
// shader writes its vertex index and instance into storage buffer 0; with discard on (and an empty scissor and both faces culled besides) the target keeps its blue clear and the
// writes land; with discard off the triangle covers the target in red; a vertex-only producer and consumer separated by NVN_BARRIER_ORDER_PRIMITIVES_BIT, rasterization off,
// see each other's storage writes in order. Checks as the Metal test's: per submission and readback its completion checks, the pixels, the vertex writes, the ordering; and validation
#include "t_vk_harness.h"
#include "shaders/gen/discard_vert.h"
#include "shaders/gen/produce_vert.h"
#include "shaders/gen/consume_vert.h"
#include "shaders/gen/red_frag.h"

static NVNqueue g_q;
static hv_cmds g_c;
static uint8_t *g_px;   // readback rows in guest memory
static tex_t g_t;
static void execute(void) { hv_submit(&g_q, &g_c); t("GPU submission completes", 1); }
static void pixels(int red) {
    memset(g_px, 0xEE, 256);
    hv_t2b(&g_c, &g_t, NULL, g_px, 0, 0, 0, 8, 8, 1); hv_submit(&g_q, &g_c);
    int good = 1;
    for (int i = 0; i < 64; i++) good &= g_px[i * 4] == (red ? 255 : 0) && g_px[i * 4 + 1] == 0 && g_px[i * 4 + 2] == (red ? 0 : 255) && g_px[i * 4 + 3] == 255;
    t(red ? "normal draw rasterizes every pixel red" : "discard leaves every pixel at the blue clear", good);
}
static void discard(int on) { p_rast_discard d = {on}; hv_emit(&g_c, OP_RAST_DISCARD, &d, sizeof d); }

int main(void) {
    hv_up("t_vk_rast_discard");
    hv_device(0);
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&g_q, &qb);
    hv_pool mp; if (!hv_pool_make(&mp, 1 << 18)) { t("memory pool allocated", 0); return hv_finish(); }
    memset(mp.mem, 0, 1 << 18);
    g_c = (hv_cmds){mp.mem + 131072, 0, 65536};
    uint32_t *output = (uint32_t *)(void *)(mp.mem + 65536), *indices = (uint32_t *)(void *)(mp.mem + 8192);
    g_px = mp.mem + 12288;
    indices[0] = 7; indices[1] = 8; indices[2] = 9;
    static const char ssbo[] = "[{\"kind\":\"sbuf\",\"reg\":0,\"name\":\"B\",\"class\":\"ssbo\",\"set\":1,\"binding\":0,\"rw\":true}]";
    static NVNprogram prog, pp, cp;
    const hv_shader sh[2] = {{"discard", 0, HV_SPV(discard_vert_spv), ssbo, NULL}, {"red", 1, HV_SPV(red_frag_spv), NULL, NULL}};
    const hv_shader shp[1] = {{"produce", 0, HV_SPV(produce_vert_spv), ssbo, NULL}}, shc[1] = {{"consume", 0, HV_SPV(consume_vert_spv), ssbo, NULL}};
    hv_program(&prog, &mp, 0, sh, 2); hv_program(&pp, &mp, 1024, shp, 1); hv_program(&cp, &mp, 2048, shc, 1);
    g_t = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 8, 8, 1, 1, 0);
    const tex_t *rt[1] = {&g_t};
    hv_rt(&g_c, 1, rt, NULL);
    hv_bind(&g_c, &prog, 3);
    hv_buf(&g_c, OP_BIND_SSBO, 0, 0, output, 256);
    hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS);
    hv_clear_color(&g_c, 0, 15, 0, 0, 1, 1); execute();
    hv_scissor(&g_c, 0, 0, 0, 0); hv_cull(&g_c, NVN_FACE_FRONT_AND_BACK);
    discard(1);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 4, 3, 1);
    hv_draw_idx(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 2, 3, indices, 0, 2);
    execute(); pixels(0);
    t("discarded array draw writes its three vertex IDs", output[4] == 0x504 && output[5] == 0x505 && output[6] == 0x506);
    t("discarded indexed draw writes both instances", output[7] == 0x507 && output[9] == 0x509 && output[23] == 0x517 && output[25] == 0x519);
    hv_scissor(&g_c, 0, 0, 8, 8); hv_cull(&g_c, NVN_FACE_NONE);
    discard(0); hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1); execute(); pixels(1);
    hv_clear_color(&g_c, 0, 15, 0, 0, 1, 1); discard(1); hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1); execute(); pixels(0);
    // NVN_BARRIER_ORDER_PRIMITIVES_BIT makes one vertex shader's storage writes visible to the next draw's, rasterization off throughout
    hv_buf(&g_c, OP_BIND_SSBO, 0, 0, output, 32768); hv_bind(&g_c, &pp, 1);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_POINTS, 0, 4096, 1);
    int32_t barrier = NVN_BARRIER_ORDER_PRIMITIVES_BIT; hv_emit(&g_c, OP_BARRIER, &barrier, sizeof barrier);
    hv_bind(&g_c, &cp, 1); hv_draw(&g_c, NVN_DRAW_PRIMITIVE_POINTS, 0, 4096, 1); execute();
    int ordered = 1; for (int i = 0; i < 4096; i++) ordered &= output[i + 4096] == (uint32_t)(i + 0x1200);
    t("primitive barrier orders vertex storage writes before the consuming draw", ordered);
    hv_pool_free(&mp);
    return hv_finish();
}
