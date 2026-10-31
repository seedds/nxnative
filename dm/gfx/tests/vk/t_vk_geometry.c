// Geometry shaders on the Vulkan backend (docs/design/vulkan-backend.md section 6 and 2.2, step P8): a native pipeline stage, where the Metal backend emulates the stage with
// compute (nvn_geometry.inc). The Vulkan port of dm/gfx/tests/t_geometry.m's replay cases, which check NVN semantics: a geometry shader with two invocations per input point,
// each emitting a 2x2-pixel quad, over an array draw of 3 points with a first vertex of 7, 2 instances from a base instance of 5 (a per-instance stream gives each instance its
// offset); the same through indexed draws with restart (8-, 16- and 32-bit indices, a base vertex of -3), an indirect array draw, an indirect indexed draw, and an indexed draw
// whose indices a buffer copy writes earlier in the same submission; every pixel of the 32x32 target compared with the quads expected. The Metal test's compute mechanics
// (assembly, compaction, scans, capture indices, run splitting) have no counterpart here. Beyond it: Big Brain Academy's line renderer in small (a line strip with adjacency
// widened into quads by the geometry stage), and layered rendering: the layer from the geometry stage (gs_slice's form, Unreal's FWriteToSliceGS) and from the vertex stage
// (shaderOutputLayer), each layer of a 3-layer array target read back. Checks: the pixels of each case, and the validation check
#include "t_vk_harness.h"
#include "shaders/gen/geo_vert.h"
#include "shaders/gen/geo_quads_geom.h"
#include "shaders/gen/geo_line_geom.h"
#include "shaders/gen/pos2_vert.h"
#include "shaders/gen/lvs_vert.h"
#include "shaders/gen/layer_geom.h"
#include "shaders/gen/layer_vert.h"
#include "shaders/gen/col_frag.h"
#include "shaders/gen/red_frag.h"

static NVNqueue g_q; static hv_cmds g_c; static hv_pool g_p;
static uint8_t g_want[32 * 32];
static void want_rect(int x0, int y0, int x1, int y1) { for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) if (x >= 0 && x < 32 && y >= 0 && y < 32) g_want[y * 32 + x] = 1; }
// The quads the six points give (3 cells, 2 instances: offsets (0, 0) and (1, 4)): each cell (cx, cy) is two 2x2 quads side by side at (4 cx, 4 cy)
static void want_quads(void) {
    memset(g_want, 0, sizeof g_want);
    static const int cell[3][2] = {{0, 0}, {3, 1}, {6, 2}}, off[2][2] = {{0, 0}, {1, 4}};
    for (int i = 0; i < 2; i++) for (int v = 0; v < 3; v++) { int cx = cell[v][0] + off[i][0], cy = cell[v][1] + off[i][1]; want_rect(4 * cx, 4 * cy, 4 * cx + 4, 4 * cy + 2); }
}
// The target cleared black, the records of setup drawn, read back and compared pixel by pixel with g_want (red where set)
static void scene(const char *label, void (*setup)(void)) {
    tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 32, 32, 1, 1, 0);
    const tex_t *rt[1] = {&target};
    hv_rt(&g_c, 1, rt, NULL); hv_clear_color(&g_c, 0, 15, 0, 0, 0, 1);
    setup();
    hv_submit(&g_q, &g_c);
    static uint8_t px[32 * 32 * 4]; int ok = hv_read(&target, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 32, 32, 4, px), bad = 0, red = 0, want = 0;
    for (int i = 0; ok && i < 32 * 32; i++) { int r = px[4 * i] > 200 && px[4 * i + 1] < 50; red += r; want += g_want[i]; if (r != g_want[i]) { if (!bad) fprintf(stderr, "%s: pixel (%d, %d) %s\n", label, i % 32, i / 32, r ? "red, not expected" : "not red"); bad++; } }
    char name[200]; snprintf(name, sizeof name, "%s: %d red pixels, as expected (%d)", label, red, want);
    t(name, ok && !bad);
}
static NVNprogram g_quads, g_line, g_gslayer, g_vslayer;
static float *g_cells, *g_offs, *g_line_v; static uint32_t *g_ix, *g_args, *g_stage;
static void quads_state(void) {
    hv_bind(&g_c, &g_quads, 0x1f); hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&g_c, NVN_FACE_NONE);
    va_st a[2] = {{NVN_FORMAT_RG32F, 0, 0}, {NVN_FORMAT_RG32F, 0, 1}}; hv_attrs(&g_c, 2, a);
    vs_st s[2] = {{8, 0}, {8, 1}}; hv_streams(&g_c, 2, s);
    hv_vbuf(&g_c, 0, g_cells, 16 * 8); hv_vbuf(&g_c, 1, g_offs, 16 * 8);
}
static void array_draw(void) { quads_state(); p_draw d; memset(&d, 0, sizeof d); d.prim = NVN_DRAW_PRIMITIVE_POINTS; d.first = 7; d.count = 3; d.instances = 2; d.base_instance = 5; hv_emit(&g_c, OP_DRAW, &d, offsetof(p_draw, ind)); }
static int g_isz;   // 1, 2, 4
static void indexed_restart(void) {
    quads_state();
    uint32_t all = g_isz == 1 ? 0xffu : g_isz == 2 ? 0xffffu : 0xffffffffu, ix[4] = {12, 10, all, 11};
    for (int i = 0; i < 4; i++) memcpy((uint8_t *)g_ix + i * g_isz, &ix[i], (size_t)g_isz);
    p_prim_restart pr = {1, all}; hv_emit(&g_c, OP_PRIM_RESTART, &pr, sizeof pr);
    p_draw_idx d; memset(&d, 0, sizeof d); d.prim = NVN_DRAW_PRIMITIVE_POINTS; d.itype = g_isz == 1 ? 0 : g_isz == 2 ? 1 : 2; d.count = 4; d.instances = 2; d.base_vertex = -3; d.base_instance = 5;
    d.ibuf = (uint64_t)(uintptr_t)g_ix; hv_emit(&g_c, OP_DRAW_IDX, &d, offsetof(p_draw_idx, ind));
    p_prim_restart off = {0, 0}; hv_emit(&g_c, OP_PRIM_RESTART, &off, sizeof off);
}
static void indirect_array(void) {
    quads_state(); const uint32_t args[4] = {3, 2, 7, 5}; memcpy(g_args, args, sizeof args);
    p_draw d; memset(&d, 0, sizeof d); d.prim = NVN_DRAW_PRIMITIVE_POINTS; d.instances = 1; d.ind = (uint64_t)(uintptr_t)g_args; hv_emit(&g_c, OP_DRAW, &d, sizeof d);
}
static void indirect_indexed(void) {
    quads_state(); const uint32_t ix[3] = {12, 10, 11}; memcpy(g_ix, ix, sizeof ix);
    const int32_t args[5] = {3, 2, 0, -3, 5}; memcpy(g_args, args, sizeof args);
    p_draw_idx d; memset(&d, 0, sizeof d); d.prim = NVN_DRAW_PRIMITIVE_POINTS; d.itype = 2; d.instances = 1; d.ibuf = (uint64_t)(uintptr_t)g_ix; d.ind = (uint64_t)(uintptr_t)g_args;
    hv_emit(&g_c, OP_DRAW_IDX, &d, sizeof d);
}
static void gpu_written_indices(void) {   // the indices are zero when recorded; a copy earlier in the submission writes them
    quads_state(); memset(g_ix, 0, 16); const uint32_t ix[3] = {12, 10, 11}; memcpy(g_stage, ix, sizeof ix);
    hv_b2b(&g_c, g_stage, g_ix, 12);
    p_draw_idx d; memset(&d, 0, sizeof d); d.prim = NVN_DRAW_PRIMITIVE_POINTS; d.itype = 2; d.count = 3; d.instances = 2; d.base_vertex = -3; d.base_instance = 5;
    d.ibuf = (uint64_t)(uintptr_t)g_ix; hv_emit(&g_c, OP_DRAW_IDX, &d, offsetof(p_draw_idx, ind));
}
static void line_strip(void) {
    hv_bind(&g_c, &g_line, 0x1f); hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&g_c, NVN_FACE_NONE);
    const float v[10] = {0, 6, 4, 6, 20, 6, 20, 26, 20, 30}; memcpy(g_line_v, v, sizeof v);
    va_st a[1] = {{NVN_FORMAT_RG32F, 0, 0}}; hv_attrs(&g_c, 1, a); vs_st s[1] = {{8, 0}}; hv_streams(&g_c, 1, s); hv_vbuf(&g_c, 0, g_line_v, sizeof v);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY, 0, 5, 1);
}
// A 3-layer array target, each layer filled by one instance through prog; want: the colour of layer l (RGBA8)
static void layered(const char *label, const NVNprogram *prog, const uint8_t want[3][4]) {
    tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8, 4, 4, 3, 1, 0);
    const tex_t *rt[1] = {&target};
    hv_rt(&g_c, 1, rt, NULL); hv_clear_color(&g_c, 0, 15, 0, 0, 0, 0);
    hv_bind(&g_c, prog, 0x1f); hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&g_c, NVN_FACE_NONE);
    va_st none[1] = {{0, 0, 0}}; hv_attrs(&g_c, 1, none);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 3);
    hv_submit(&g_q, &g_c);
    uint8_t px[3][64]; int ok = hv_read_n(&target, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 3, 0, 0, 4, 4, 4, px);
    for (int l = 0; ok && l < 3; l++) for (int i = 0; i < 16; i++) if (memcmp(px[l] + 4 * i, want[l], 4)) {
        fprintf(stderr, "%s: layer %d pixel %d = %u %u %u %u\n", label, l, i, px[l][4 * i], px[l][4 * i + 1], px[l][4 * i + 2], px[l][4 * i + 3]); ok = 0; break; }
    t(label, ok);
}

int main(void) {
    hv_up("t_vk_geometry");
    hv_device(1);
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&g_q, &qb);
    if (!hv_pool_make(&g_p, 1 << 20)) { t("memory pool allocated", 0); return hv_finish(); }
    memset(g_p.mem, 0, 1 << 20);
    g_c = (hv_cmds){g_p.mem + 512 * 1024, 0, 65536};
    g_cells = (float *)(void *)(g_p.mem + 16384); g_offs = (float *)(void *)(g_p.mem + 20480); g_ix = (uint32_t *)(void *)(g_p.mem + 24576);
    g_args = (uint32_t *)(void *)(g_p.mem + 28672); g_stage = (uint32_t *)(void *)(g_p.mem + 32768); g_line_v = (float *)(void *)(g_p.mem + 36864);
    const float cells[3][2] = {{0, 0}, {3, 1}, {6, 2}}, offs[2][2] = {{0, 0}, {1, 4}};
    for (int v = 0; v < 3; v++) { g_cells[2 * (7 + v)] = cells[v][0]; g_cells[2 * (7 + v) + 1] = cells[v][1]; }
    for (int i = 0; i < 2; i++) { g_offs[2 * (5 + i)] = offs[i][0]; g_offs[2 * (5 + i) + 1] = offs[i][1]; }
    const char *vin = "[{\"location\":0,\"type\":\"float\",\"n\":2},{\"location\":1,\"type\":\"float\",\"n\":2}]", *vin1 = "[{\"location\":0,\"type\":\"float\",\"n\":2}]";
    const hv_shader sq[3] = {{"geo", 0, HV_SPV(geo_vert_spv), NULL, vin}, {"red", 1, HV_SPV(red_frag_spv), NULL, NULL}, {"geo_quads", 2, HV_SPV(geo_quads_geom_spv), NULL, NULL}};
    const hv_shader sl[3] = {{"pos2", 0, HV_SPV(pos2_vert_spv), NULL, vin1}, {"red", 1, HV_SPV(red_frag_spv), NULL, NULL}, {"geo_line", 2, HV_SPV(geo_line_geom_spv), NULL, NULL}};
    const hv_shader sg[3] = {{"lvs", 0, HV_SPV(lvs_vert_spv), NULL, NULL}, {"col", 1, HV_SPV(col_frag_spv), NULL, NULL}, {"layer_gs", 2, HV_SPV(layer_geom_spv), NULL, NULL}};
    const hv_shader sv[2] = {{"layer_vs", 0, HV_SPV(layer_vert_spv), NULL, NULL}, {"col", 1, HV_SPV(col_frag_spv), NULL, NULL}};
    hv_program(&g_quads, &g_p, 0, sq, 3); hv_program(&g_line, &g_p, 1024, sl, 3); hv_program(&g_gslayer, &g_p, 2048, sg, 3); hv_program(&g_vslayer, &g_p, 3072, sv, 2);

    want_quads();
    scene("array draw: 3 points from vertex 7, 2 instances from 5, 2 invocations each", array_draw);
    g_isz = 1; scene("indexed with restart, 8-bit indices, base vertex -3", indexed_restart);
    g_isz = 2; scene("indexed with restart, 16-bit indices", indexed_restart);
    g_isz = 4; scene("indexed with restart, 32-bit indices", indexed_restart);
    scene("indirect array draw {3, 2, 7, 5}", indirect_array);
    scene("indirect indexed draw {3, 2, 0, -3, 5}", indirect_indexed);
    scene("indexed draw whose indices a copy earlier in the submission writes", gpu_written_indices);
    memset(g_want, 0, sizeof g_want); want_rect(4, 5, 20, 7); want_rect(19, 6, 21, 26);
    scene("line strip with adjacency widened into quads by the geometry stage (Big Brain Academy's lines)", line_strip);
    static const uint8_t gs_want[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}}, vs_want[3][4] = {{0, 255, 0, 255}, {0, 0, 255, 255}, {255, 0, 0, 255}};
    layered("the geometry stage picks the layer (gs_slice's form): each of 3 layers drawn by its instance", &g_gslayer, gs_want);
    if (hvk_device()->x.output_layer) layered("the vertex stage writes the layer (shaderOutputLayer): each of 3 layers drawn by its instance", &g_vslayer, vs_want);
    else printf("note: no shaderOutputLayer on this device: the vertex-stage layer case is not run\n");
    hv_pool_free(&g_p);
    return hv_finish();
}
