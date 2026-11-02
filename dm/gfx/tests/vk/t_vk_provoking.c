// The provoking vertex (docs/design/vulkan-backend.md, the step note "The provoking vertex"; dm/gfx/hostvk/vk_index.c, the rules table): the vertex a flat-interpolated varying
// takes. NVN's is GL's last-vertex convention (OpenGL 4.6 compatibility profile, table 13.2, "last"); the backend makes every pipeline with Vulkan's last-vertex mode where the
// device has VK_EXT_provoking_vertex (vk_device.c, vk_pipeline.c hvk_raster_next). Every NVN primitive with triangles or lines is drawn through the backend, without indices and
// with 16-bit indices (a permutation, so a vertex's value is its index), each vertex with a flat value of its index + 1 (shaders/pv.vert), into two R32UI targets: the flat value
// and the primitive's ID + 1 (shaders/pv.frag), so every pixel names the primitive it belongs to. The oracle below gives, per primitive the device draws (gl_PrimitiveID: a
// converted quad is two triangles), the vertex NVN takes (GL's table) and the vertex the port takes (Vulkan's last-vertex mode over the backend's conversions, as vk_index.c's
// table writes them); every pixel must hold the port's vertex, every primitive must be seen, and the primitives where the port's vertex is not NVN's must be exactly the rows
// vk_index.c declares (the first triangle of a converted quad, the second of a converted quad strip, a polygon's triangles): none for points, lines, line loops, line strips,
// triangles, triangle strips and fans (native, and expanded as on a device without fans). Then the same draws in a second start of the backend (a child process, t_vk_harness.h
// hv_spawn) with VK_EXT_provoking_vertex taken as absent (hvk_test_no_provoking_vertex): one red line, and every pixel holds Vulkan's first-vertex result. Checks: the device's
// extension, one per draw and form, the red lines, the child's exit status, and the validation check of each start.
#include "t_vk_harness.h"
#include "shaders/gen/pv_vert.h"
#include "shaders/gen/pv_frag.h"
#include <math.h>

void hvk_test_index_opts(int no_fans, int no_uint8, int no_list_restart, int64_t limit);   // vk_index.c: -1 keeps an option; limit 0 is the default

enum { W = 64, H = 64, NV = 64 };
static NVNqueue g_q; static hv_pool g_mp; static hv_cmds g_c; static NVNprogram g_prog;
static tex_t g_t[2];                // the flat values, the primitive IDs + 1
static size_t g_data;               // the next free byte of the pool's data region (vertex and index buffers, a region of their own for each draw)
static int g_red_pv;                // the red lines about the provoking vertex
static void on_log(const char *line) {
    fprintf(stderr, "[dm] %s\n", line);
    if (strstr(line, "[RED]") && strstr(line, "VK_EXT_provoking_vertex")) g_red_pv++;
}

// ---------- The shapes, in pixels of the 64x64 targets (each draw clears them) ----------
typedef struct { float x, y; } pt;
static uint32_t shape(int prim, pt *v) {
    uint32_t n = 0;
    switch (prim) {
        case NVN_DRAW_PRIMITIVE_LINES: for (int k = 0; k < 4; k++) { v[n++] = (pt){4, 8.0f + 14 * k}; v[n++] = (pt){60, 10.0f + 14 * k}; } break;
        case NVN_DRAW_PRIMITIVE_LINE_STRIP: for (int k = 0; k < 5; k++) v[n++] = (pt){k & 1 ? 60.0f : 4.0f, 6.0f + 13 * k}; break;
        case NVN_DRAW_PRIMITIVE_LINE_LOOP: v[n++] = (pt){8, 8}; v[n++] = (pt){56, 8}; v[n++] = (pt){56, 56}; v[n++] = (pt){8, 56}; break;
        case NVN_DRAW_PRIMITIVE_TRIANGLES:
            for (int q = 0; q < 4; q++) { float x0 = (float)(q & 1) * 32, y0 = (float)(q >> 1) * 32; v[n++] = (pt){x0 + 2, y0 + 2}; v[n++] = (pt){x0 + 30, y0 + 2}; v[n++] = (pt){x0 + 2, y0 + 30}; }
            break;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: for (int k = 0; k < 8; k++) v[n++] = (pt){2.0f + 8 * k, k & 1 ? 62.0f : 2.0f}; break;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN:   // a centre and seven vertices 50 degrees apart: six triangles
            v[n++] = (pt){32, 32};
            for (int m = 0; m < 7; m++) { double a = m * 50.0 * 3.14159265358979 / 180.0; v[n++] = (pt){32 + 30 * (float)cos(a), 32 + 30 * (float)sin(a)}; }
            break;
        case NVN_DRAW_PRIMITIVE_QUADS:
            for (int q = 0; q < 4; q++) { float x0 = (float)(q & 1) * 32, y0 = (float)(q >> 1) * 32;
                v[n++] = (pt){x0 + 2, y0 + 2}; v[n++] = (pt){x0 + 30, y0 + 2}; v[n++] = (pt){x0 + 30, y0 + 30}; v[n++] = (pt){x0 + 2, y0 + 30}; }
            break;
        case NVN_DRAW_PRIMITIVE_QUAD_STRIP: for (int j = 0; j < 5; j++) { v[n++] = (pt){2.0f + 14 * j, 2}; v[n++] = (pt){2.0f + 14 * j, 62}; } break;   // quad j: (2j, 2j+1, 2j+3, 2j+2)
        case NVN_DRAW_PRIMITIVE_POLYGON:   // a hexagon: four triangles
            for (int m = 0; m < 6; m++) { double a = m * 60.0 * 3.14159265358979 / 180.0; v[n++] = (pt){32 + 30 * (float)cos(a), 32 + 30 * (float)sin(a)}; }
            break;
        default: break;
    }
    return n;
}

// ---------- The oracle ----------
// For primitive t of the device's draw (gl_PrimitiveID) of n vertices: the position in the stream of the vertex NVN takes (GL 4.6 compatibility, table 13.2, last-vertex
// convention, 0-based), of the one the port takes with Vulkan's last-vertex mode (last = 1) or first-vertex mode (last = 0) over vk_index.c's conversions, and the primitives drawn
static uint32_t prims_of(int prim, uint32_t n) {
    switch (prim) {
        case NVN_DRAW_PRIMITIVE_LINES: return n / 2;
        case NVN_DRAW_PRIMITIVE_LINE_STRIP: return n >= 2 ? n - 1 : 0;
        case NVN_DRAW_PRIMITIVE_LINE_LOOP: return n >= 2 ? n : 0;
        case NVN_DRAW_PRIMITIVE_TRIANGLES: return n / 3;
        case NVN_DRAW_PRIMITIVE_QUADS: return n / 4 * 2;
        case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return n >= 4 ? (n - 2) / 2 * 2 : 0;
        default: return n >= 3 ? n - 2 : 0;   // strips, fans, polygons
    }
}
static uint32_t nvn_vertex(int prim, uint32_t t, uint32_t n) {
    switch (prim) {
        case NVN_DRAW_PRIMITIVE_LINES: return 2 * t + 1;
        case NVN_DRAW_PRIMITIVE_LINE_STRIP: return t + 1;
        case NVN_DRAW_PRIMITIVE_LINE_LOOP: return t + 1 < n ? t + 1 : 0;
        case NVN_DRAW_PRIMITIVE_TRIANGLES: return 3 * t + 2;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: return t + 2;
        case NVN_DRAW_PRIMITIVE_QUADS: return 4 * (t / 2) + 3;         // a quad's last vertex, for both its triangles
        case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return 2 * (t / 2) + 3;    // quad j's last vertex, 2j+3 (GL's 2i+2, 1-based)
        case NVN_DRAW_PRIMITIVE_POLYGON: return 0;                     // a polygon's first vertex in both conventions
        default: return 0;
    }
}
static uint32_t port_vertex(int prim, uint32_t t, uint32_t n, int last, int fans_expanded) {
    uint32_t j = t / 2, h = t & 1;
    switch (prim) {
        case NVN_DRAW_PRIMITIVE_LINES: return 2 * t + (last ? 1 : 0);
        case NVN_DRAW_PRIMITIVE_LINE_STRIP: return t + (last ? 1 : 0);
        case NVN_DRAW_PRIMITIVE_LINE_LOOP: return t + 1 < n ? t + (last ? 1 : 0) : (last ? 0 : n - 1);   // the converted list: (t, t+1), then (n-1, 0)
        case NVN_DRAW_PRIMITIVE_TRIANGLES: return 3 * t + (last ? 2 : 0);
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: return t + (last ? 2 : 0);
        case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: case NVN_DRAW_PRIMITIVE_POLYGON:   // native: (t+1, t+2, 0), last mode t+2 (VK_EXT_provoking_vertex), first t+1; expanded: (0, t+1, t+2)
            return last ? t + 2 : fans_expanded ? 0 : t + 1;
        case NVN_DRAW_PRIMITIVE_QUADS: return last ? 4 * j + 2 + h : 4 * j;                       // (4j, 4j+1, 4j+2) (4j, 4j+2, 4j+3)
        case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return last ? 2 * j + 3 - h : 2 * j;                  // (2j, 2j+1, 2j+3) (2j, 2j+3, 2j+2)
        default: return 0;
    }
}

// ---------- A draw and its pixels ----------
static const char *prim_name(int prim) {
    switch (prim) {
        case NVN_DRAW_PRIMITIVE_LINES: return "lines"; case NVN_DRAW_PRIMITIVE_LINE_STRIP: return "line strip"; case NVN_DRAW_PRIMITIVE_LINE_LOOP: return "line loop (converted)";
        case NVN_DRAW_PRIMITIVE_TRIANGLES: return "triangles (native list)"; case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: return "triangle strip";
        case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: return "triangle fan"; case NVN_DRAW_PRIMITIVE_QUADS: return "quads (converted)";
        case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return "quad strip (converted)"; case NVN_DRAW_PRIMITIVE_POLYGON: return "polygon"; default: return "?";
    }
}
static uint8_t *take(size_t bytes) { uint8_t *p = g_mp.mem + (512u << 10) + g_data; g_data += (bytes + 255) & ~(size_t)255; if (g_data > (3u << 20)) { fprintf(stderr, "data region full\n"); exit(2); } return p; }
static uint32_t g_px[2][W * H];
// One draw of prim (indexed: 16-bit indices, a permutation of the 64 vertex slots), read back and compared: every pixel holds the port's vertex (last-vertex mode where the
// device has the extension: last), every primitive is seen, and the primitives whose port vertex is not NVN's are counted (*differ). 1 when all hold
static int one(int prim, int indexed, int last, int fans_expanded, uint32_t *differ, uint32_t *nprims) {
    pt s[NV]; uint32_t n = shape(prim, s), np = prims_of(prim, n);
    float *vb = (float *)take(NV * 16); uint16_t *ib = indexed ? (uint16_t *)take(n * 2 + 4) : NULL;
    memset(vb, 0, NV * 16);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = indexed ? (i * 37u + 11u) % NV : i;   // 37 is prime to 64: distinct slots
        if (ib) ib[i] = (uint16_t)slot;
        float *q = vb + 4 * slot; q[0] = s[i].x / 32.0f - 1.0f; q[1] = s[i].y / 32.0f - 1.0f; q[2] = 0.5f; q[3] = 1.0f;
    }
    const tex_t *rt[2] = {&g_t[0], &g_t[1]};
    hv_rt(&g_c, 2, rt, NULL); hv_scissor(&g_c, 0, 0, W, H);
    hv_clear_color(&g_c, 0, 15, 0, 0, 0, 0); hv_clear_color(&g_c, 1, 15, 0, 0, 0, 0);
    hv_bind(&g_c, &g_prog, 3); hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&g_c, NVN_FACE_NONE);
    va_st a[1]; memset(a, 0, sizeof a); a[0].fmt = NVN_FORMAT_RGBA32F; vs_st st[1] = {{16, 0}};
    hv_attrs(&g_c, 1, a); hv_streams(&g_c, 1, st); hv_vbuf(&g_c, 0, vb, NV * 16);
    p_prim_restart pr = {0, 0}; hv_emit(&g_c, OP_PRIM_RESTART, &pr, sizeof pr);
    if (ib) hv_draw_idx(&g_c, prim, 1, (int)n, ib, 0, 1); else hv_draw(&g_c, prim, 0, (int)n, 1);
    hv_submit(&g_q, &g_c);
    g_data = 0;
    for (int i = 0; i < 2; i++) if (!hv_read(&g_t[i], VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, W, H, 4, g_px[i])) { fprintf(stderr, "%s: readback %d failed\n", prim_name(prim), i); return 0; }
    static uint32_t seen[NV];
    memset(seen, 0, sizeof seen);
    uint32_t bad = 0, stray = 0, first_bad = UINT32_MAX;
    for (int p = 0; p < W * H; p++) {
        uint32_t id = g_px[1][p];
        if (!id) continue;
        if (id > np) { stray++; continue; }
        uint32_t t = id - 1, pos = port_vertex(prim, t, n, last, fans_expanded), want = (ib ? ib[pos] : pos) + 1;
        seen[t]++;
        if (g_px[0][p] != want) { if (!bad) first_bad = (uint32_t)p; bad++; }
    }
    uint32_t unseen = 0, d = 0;
    for (uint32_t t = 0; t < np; t++) { unseen += !seen[t]; d += port_vertex(prim, t, n, last, fans_expanded) != nvn_vertex(prim, t, n); }
    if (bad) fprintf(stderr, "%s: %u pixels hold another vertex; first at (%u, %u): primitive %u, value %u, expected %u\n", prim_name(prim), bad, first_bad % W, first_bad / W,
                     g_px[1][first_bad] - 1, g_px[0][first_bad], (ib ? ib[port_vertex(prim, g_px[1][first_bad] - 1, n, last, fans_expanded)] : port_vertex(prim, g_px[1][first_bad] - 1, n, last, fans_expanded)) + 1);
    *differ = d; *nprims = np;
    return !bad && !stray && !unseen;
}

static const int k_prims[] = {NVN_DRAW_PRIMITIVE_LINES, NVN_DRAW_PRIMITIVE_LINE_LOOP, NVN_DRAW_PRIMITIVE_LINE_STRIP, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP,
                              NVN_DRAW_PRIMITIVE_TRIANGLE_FAN, NVN_DRAW_PRIMITIVE_QUADS, NVN_DRAW_PRIMITIVE_QUAD_STRIP, NVN_DRAW_PRIMITIVE_POLYGON};
// The primitives of a draw whose port vertex differs from NVN's with the last-vertex mode: vk_index.c's declared rows
static uint32_t declared(int prim, uint32_t np) {
    switch (prim) {
        case NVN_DRAW_PRIMITIVE_QUADS: case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return np / 2;   // one triangle of each quad
        case NVN_DRAW_PRIMITIVE_POLYGON: return np;                                          // every triangle: k+2 is never vertex 0
        default: return 0;
    }
}

static int run(int last) {
    char name[300]; int bad = 0;
    for (unsigned i = 0; i < sizeof k_prims / sizeof *k_prims; i++) for (int indexed = 0; indexed < 2; indexed++) {
        int prim = k_prims[i]; uint32_t d = 0, np = 0;
        int ok = one(prim, indexed, last, 0, &d, &np);
        if (last) {
            uint32_t want = declared(prim, np);
            snprintf(name, sizeof name, "%s, %s: %u primitives, every pixel holds the port's vertex; %u take another vertex than NVN's (%s)", prim_name(prim),
                     indexed ? "16-bit indices" : "no indices", np, d, want ? "declared in vk_index.c" : "none: NVN's last vertex");
            ok = ok && d == want;
        } else
            snprintf(name, sizeof name, "without the extension: %s, %s: %u primitives, every pixel holds Vulkan's first vertex (%u of them not NVN's)", prim_name(prim),
                     indexed ? "16-bit indices" : "no indices", np, d);
        t(name, ok); bad += !ok;
    }
    // Fans and polygons expanded into lists, as on a device without triangle fans (hvk_test_index_opts): (0, k+1, k+2) ends with NVN's vertex
    hvk_test_index_opts(1, -1, -1, -1);
    for (int indexed = 0; indexed < 2; indexed++) {
        uint32_t d = 0, np = 0;
        int ok = one(NVN_DRAW_PRIMITIVE_TRIANGLE_FAN, indexed, last, 1, &d, &np);
        if (last) { snprintf(name, sizeof name, "triangle fan expanded into a list (no fans taken), %s: %u primitives, NVN's vertex on every one", indexed ? "16-bit indices" : "no indices", np); ok = ok && d == 0; }
        else snprintf(name, sizeof name, "without the extension: triangle fan expanded into a list, %s: %u primitives, the list's first vertex (the centre)", indexed ? "16-bit indices" : "no indices", np);
        t(name, ok); bad += !ok;
    }
    hvk_test_index_opts(0, -1, -1, -1);
    return bad;
}

static void setup(void) {
    hv_device(0);
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&g_q, &qb);
    if (!hv_pool_make(&g_mp, 4u << 20)) { t("memory pool allocated", 0); exit(hv_finish()); }
    g_c = (hv_cmds){g_mp.mem + (64u << 10), 0, 256u << 10};
    static const char vin[] = "[{\"location\":0,\"type\":\"float\"}]";
    const hv_shader sh[2] = {{"pv_vert", 0, HV_SPV(pv_vert_spv), NULL, vin}, {"pv_frag", 1, HV_SPV(pv_frag_spv), NULL, NULL}};
    hv_program(&g_prog, &g_mp, 0, sh, 2);
    for (int i = 0; i < 2; i++) g_t[i] = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R32UI, W, H, 1, 1, 0);
}

int main(int argc, char **argv) {
    char name[300];
    if (argc > 1 && !strcmp(argv[1], "off")) {   // the second start: the extension taken as absent
        hvk_test_no_provoking_vertex(1);
        hostvk_set_log(on_log);
        const hostvk_dev *d = hv_up("t_vk_provoking without VK_EXT_provoking_vertex");
        setup();
        snprintf(name, sizeof name, "without the extension: one red line about VK_EXT_provoking_vertex (%d), the device made without it", g_red_pv);
        t(name, g_red_pv == 1 && !d->x.provoking_last);
        run(0);
        hv_pool_free(&g_mp);
        return hv_finish();
    }
    hostvk_set_log(on_log);
    const hostvk_dev *d = hv_up("t_vk_provoking");
    setup();
    printf("note: VK_EXT_provoking_vertex: %s, provokingVertexModePerPipeline %u\n", d->x.provoking_last ? "enabled with provokingVertexLast" : "absent", (unsigned)d->x.provoking_per_pipeline);
    if (!d->x.provoking_last) {   // a device without it: the red line was said, and the first-vertex result is what it draws
        snprintf(name, sizeof name, "the device lacks VK_EXT_provoking_vertex: one red line (%d)", g_red_pv);
        t(name, g_red_pv == 1);
        run(0);
    } else {
        snprintf(name, sizeof name, "the device has VK_EXT_provoking_vertex with provokingVertexLast: enabled, no red line (%d)", g_red_pv);
        t(name, g_red_pv == 0);
        run(1);
        // The second start, with the extension taken as absent
        int rc = hv_spawn(argv[0], "off");
        snprintf(name, sizeof name, "the start without VK_EXT_provoking_vertex (a child process) passed: exit status %d", rc);
        t(name, rc == 0);
    }
    hv_pool_free(&g_mp);
    return hv_finish();
}
