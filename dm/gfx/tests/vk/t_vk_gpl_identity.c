// The same draws through every kind of pipeline the backend makes give the same pixels, bit for bit (docs/design/vulkan-backend.md 3.2 "Same output", risk 7 of 11.2; phase P5):
// a set of scenes, each with its own targets (blending with constants and channel masks, depth and stencil with a depth bias, two integer attachments, culling, wireframe,
// scissor, viewport, lines, a logic op, per-instance vertex input), drawn five times over on one device: fast-linked pipelines of level A (no optimized relink), the optimized
// relinks of level A (drawn once, the background relinks waited for, drawn again: every draw then binds an optimized pipeline), libraries with the blend and raster state baked
// (level B), monolithic pipelines with the state dynamic (level C) and monolithic pipelines with every state baked (level D). Each run's targets are read back and compared with
// the fast-linked run's. A difference is a quirk of the driver: it passes only where the backend's quirk table (vk_gpl.c) has a row for the device that turns that pipeline kind
// off, and the test prints which kind and scene differ. On a device without pipeline libraries only C and D are compared. Under the validation layer, 0 messages
#include "t_vk_harness.h"
#include "vk_pso.h"
#include "shaders/gen/gpl_vary_vert.h"
#include "shaders/gen/gpl_mix_frag.h"
#include "shaders/gen/gpl_uint_frag.h"

typedef struct { float x, y, z, w; } v4;
// stream 0: positions; stream 1: colours (RGBA8)
static const v4 k_pos[] = {
    {-0.9f, -0.9f, 0.3f, 1}, {0.8f, -0.7f, 0.5f, 1}, {-0.6f, 0.9f, 0.7f, 1},                // 0: triangle a (counter-clockwise on screen)
    {-0.7f, 0.8f, 0.2f, 1}, {0.2f, -0.9f, 0.4f, 1}, {0.9f, 0.9f, 0.6f, 1},                  // 3: triangle b (the other winding)
    {-1.6f, -1.6f, 1.2f, 2}, {0.8f, -0.7f, 0.5f, 1}, {-0.9f, 1.35f, 1.35f, 1.5f},           // 6: triangle w, perspective (w 1 to 2)
    {-1, -1, 0.5f, 1}, {1, -1, 0.5f, 1}, {-1, 1, 0.5f, 1}, {1, 1, 0.5f, 1},                  // 9: a quad as a strip
    {-0.8f, -0.5f, 0, 1}, {0.7f, 0.6f, 0, 1}, {-0.5f, 0.7f, 0, 1}, {0.6f, -0.8f, 0, 1}, {0.0f, 0.9f, 0, 1},   // 13: lines
};
static const uint8_t k_col[18][4] = {{255, 0, 0, 200}, {0, 255, 0, 128}, {0, 0, 255, 255}, {255, 255, 0, 64}, {0, 255, 255, 220}, {255, 0, 255, 30}, {200, 100, 50, 255},
    {50, 100, 200, 180}, {10, 240, 130, 90}, {255, 255, 255, 255}, {128, 64, 32, 160}, {32, 64, 128, 96}, {0, 0, 0, 255}, {90, 180, 30, 250}, {250, 30, 90, 10}, {7, 77, 177, 77},
    {200, 200, 20, 140}, {33, 66, 99, 199}};

static hv_cmds cmds; static NVNqueue queue; static NVNprogram p_mix, p_uint; static uint8_t *vb0, *vb1;
static tex_t ta, tb, tbd, tc0, tc1, td, te, tf;
static void vp(hv_cmds *c, float x, float y, float w, float h) { p_viewports v; memset(&v, 0, sizeof v); v.first = 0; v.n = 1; v.v[0][0] = x; v.v[0][1] = y; v.v[0][2] = w; v.v[0][3] = h; v.v[0][4] = 0; v.v[0][5] = 1; hv_emit(c, OP_VIEWPORTS, &v, sizeof v); }
static void blend(hv_cmds *c, int target, int src, int dst, int eq, int src_a, int dst_a, int eq_a) {
    blend_st b; memset(&b, 0, sizeof b); b.target = (unsigned)target; b.src_rgb = (unsigned)src; b.dst_rgb = (unsigned)dst; b.eq_rgb = (unsigned)eq; b.src_a = (unsigned)src_a; b.dst_a = (unsigned)dst_a; b.eq_a = (unsigned)eq_a;
    hv_state(c, 0, &b, sizeof b);
}
static void color(hv_cmds *c, unsigned blend_en, unsigned logic) { color_st cs; memset(&cs, 0, sizeof cs); cs.blend_en = blend_en; cs.logic_op = logic; hv_state(c, 1, &cs, sizeof cs); }
static void chmask(hv_cmds *c, uint32_t m) { chmask_st s = {m}; hv_state(c, 2, &s, sizeof s); }
static void poly(hv_cmds *c, int cull, int front, int mode, int offset_en) { poly_st p; memset(&p, 0, sizeof p); p.cull = (unsigned)cull; p.front = (unsigned)front; p.mode = (unsigned)mode; p.offset_en = (unsigned)offset_en; hv_state(c, 4, &p, sizeof p); }
static void stencil(hv_cmds *c, int which, int v) { p_stencil s = {3, v, v, v, which}; hv_emit(c, OP_STENCIL, &s, sizeof s); }
static void clear_ds(hv_cmds *c, float depth, int st) { p_clear_ds d = {depth, 1, st, 0xff}; hv_emit(c, OP_CLEAR_DS, &d, sizeof d); }
static void vinput(hv_cmds *c, int divisor1) {
    va_st a[2]; memset(a, 0, sizeof a); a[0].fmt = NVN_FORMAT_RGBA32F; a[0].stream = 0; a[1].fmt = NVN_FORMAT_RGBA8; a[1].stream = 1;
    vs_st s[2] = {{16, 0}, {4, (uint32_t)divisor1}};
    hv_attrs(c, 2, a); hv_streams(c, 2, s); hv_vbuf(c, 0, vb0, sizeof k_pos); hv_vbuf(c, 1, vb1, sizeof k_col);
}
static void defaults(hv_cmds *c, int w, int h) {   // every state a scene relies on, set (the runs start from the same state)
    vp(c, 0, 0, (float)w, (float)h); hv_scissor(c, 0, 0, w, h);
    color(c, 0, NVN_LOGIC_OP_COPY); chmask(c, 0xffffffffu); poly(c, NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0);
    hv_depth(c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); stencil(c, 0, 0xff); stencil(c, 1, 0xff); stencil(c, 2, 0);
    p_poly_offset po = {0, 0, 0}; hv_emit(c, OP_POLY_OFFSET, &po, sizeof po);
    p_blend_color bc = {{0.25f, 0.5f, 0.75f, 1.0f}}; hv_emit(c, OP_BLEND_COLOR, &bc, sizeof bc);
    vinput(c, 0);
}
static int g_draws;
static void draw(hv_cmds *c, int prim, int first, int count, int inst) { hv_draw(c, prim, first, count, inst); g_draws++; }
static void scenes(void) {
    hv_cmds *c = &cmds;
    // A: blending with alpha, with MAX and a constant, a channel mask
    const tex_t *a[1] = {&ta}; hv_rt(c, 1, a, NULL); defaults(c, 64, 64);
    hv_clear_color(c, 0, 15, 0.2f, 0.3f, 0.4f, 0.5f);
    hv_bind(c, &p_mix, 3);
    blend(c, 0, NVN_BLEND_FUNC_SRC_ALPHA, NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, NVN_BLEND_EQUATION_ADD, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, NVN_BLEND_EQUATION_ADD);
    color(c, 1, NVN_LOGIC_OP_COPY);
    draw(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    blend(c, 0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_EQUATION_MAX, NVN_BLEND_FUNC_CONSTANT_COLOR, NVN_BLEND_FUNC_ONE, NVN_BLEND_EQUATION_ADD);
    chmask(c, 0x3);
    draw(c, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, 9, 4, 1);
    hv_submit(&queue, c);
    // B: depth and stencil, a depth bias
    const tex_t *b[1] = {&tb}; hv_rt(c, 1, b, &tbd); defaults(c, 64, 64);
    hv_clear_color(c, 0, 15, 0, 0, 0, 0); clear_ds(c, 1.0f, 0);
    hv_bind(c, &p_mix, 3);
    ds_st ds; memset(&ds, 0, sizeof ds); ds.dt = 1; ds.dw = 1; ds.df = NVN_DEPTH_FUNC_LESS; ds.st = 1; ds.sf_f = ds.sf_b = NVN_STENCIL_FUNC_ALWAYS;
    ds.fail_f = ds.fail_b = ds.zfail_f = ds.zfail_b = NVN_STENCIL_OP_KEEP; ds.zpass_f = ds.zpass_b = NVN_STENCIL_OP_REPLACE; hv_state(c, 5, &ds, sizeof ds);
    stencil(c, 2, 0x5a);
    draw(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    ds.df = NVN_DEPTH_FUNC_LEQUAL; ds.sf_f = ds.sf_b = NVN_STENCIL_FUNC_EQUAL; ds.zpass_f = ds.zpass_b = NVN_STENCIL_OP_INCR; hv_state(c, 5, &ds, sizeof ds);
    poly(c, NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, NVN_POLYGON_OFFSET_ENABLE_FILL);
    p_poly_offset po = {1.0f, 2.0f, 0}; hv_emit(c, OP_POLY_OFFSET, &po, sizeof po);
    draw(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 3, 3, 1);
    hv_submit(&queue, c);
    // C: two integer attachments (blending asked for: integer attachments ignore it)
    const tex_t *ci[2] = {&tc0, &tc1}; hv_rt(c, 2, ci, NULL); defaults(c, 32, 32);
    hv_clear_color(c, 0, 15, 0, 0, 0, 0); hv_clear_color(c, 1, 15, 0, 0, 0, 0);
    hv_bind(c, &p_uint, 3);
    blend(c, 0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_EQUATION_ADD, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_EQUATION_ADD); color(c, 3, NVN_LOGIC_OP_COPY);
    draw(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 6, 3, 1);
    draw(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    hv_submit(&queue, c);
    // D: culling, wireframe, scissor, viewport, lines
    const tex_t *dd[1] = {&td}; hv_rt(c, 1, dd, NULL); defaults(c, 64, 64);
    hv_clear_color(c, 0, 15, 0, 0, 0, 1);
    hv_bind(c, &p_mix, 3);
    poly(c, NVN_FACE_BACK, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0);
    draw(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1); draw(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 3, 3, 1);   // one of the two is culled
    poly(c, NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_LINE, 0);
    draw(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 6, 3, 1);
    poly(c, NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0);
    hv_scissor(c, 8, 8, 40, 24); draw(c, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, 9, 4, 1);
    hv_scissor(c, 0, 0, 64, 64); vp(c, 16, 16, 32, 32); draw(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 3, 3, 1);
    vp(c, 0, 0, 64, 64); draw(c, NVN_DRAW_PRIMITIVE_LINES, 13, 4, 1); draw(c, NVN_DRAW_PRIMITIVE_LINE_STRIP, 13, 5, 1);
    hv_submit(&queue, c);
    // E: a logic op (XOR) on a normalized target
    const tex_t *e[1] = {&te}; hv_rt(c, 1, e, NULL); defaults(c, 32, 32);
    hv_clear_color(c, 0, 15, 0.5f, 0.25f, 0.75f, 1);
    hv_bind(c, &p_mix, 3); color(c, 0, NVN_LOGIC_OP_XOR);
    draw(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    hv_submit(&queue, c);
    // F: per-instance vertex input (a divisor on the colour stream)
    const tex_t *f[1] = {&tf}; hv_rt(c, 1, f, NULL); defaults(c, 32, 32);
    hv_clear_color(c, 0, 15, 0, 0, 0, 0);
    hv_bind(c, &p_mix, 3); vinput(c, 1);
    blend(c, 0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_EQUATION_ADD, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_EQUATION_ADD); color(c, 1, NVN_LOGIC_OP_COPY);
    draw(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 3);
    hv_submit(&queue, c);
}

// The targets read back: their bytes, by name
enum { NT = 9 };
static const char *const k_tname[NT] = {"A blend (RGBA8)", "B colour (RGBA16F)", "B depth", "B stencil", "C R32UI", "C RGBA8UI", "D raster (RGBA8)", "E logic op (RGBA8)", "F instanced (RGBA8)"};
typedef struct { uint8_t *b[NT]; size_t n[NT]; } result;
static int read_all(result *r) {
    VkFormat f; VkImageAspectFlags as; hvk_test_image(&tbd, &f, &as);
    uint32_t dmask = f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32 ? 0x00ffffffu : 0xffffffffu;   // the top byte of a D24 copy is undefined
    struct { const tex_t *t; VkImageAspectFlags a; uint32_t w, bpp; } k[NT] = {{&ta, VK_IMAGE_ASPECT_COLOR_BIT, 64, 4}, {&tb, VK_IMAGE_ASPECT_COLOR_BIT, 64, 8}, {&tbd, VK_IMAGE_ASPECT_DEPTH_BIT, 64, 4},
        {&tbd, VK_IMAGE_ASPECT_STENCIL_BIT, 64, 1}, {&tc0, VK_IMAGE_ASPECT_COLOR_BIT, 32, 4}, {&tc1, VK_IMAGE_ASPECT_COLOR_BIT, 32, 4}, {&td, VK_IMAGE_ASPECT_COLOR_BIT, 64, 4},
        {&te, VK_IMAGE_ASPECT_COLOR_BIT, 32, 4}, {&tf, VK_IMAGE_ASPECT_COLOR_BIT, 32, 4}};
    int ok = 1;
    for (int i = 0; i < NT; i++) {
        r->n[i] = (size_t)k[i].w * k[i].w * k[i].bpp; r->b[i] = calloc(1, r->n[i]);
        ok &= r->b[i] && hv_read(k[i].t, k[i].a, 0, 0, 0, 0, k[i].w, k[i].w, k[i].bpp, r->b[i]);
        if (i == 2 && r->b[i]) for (size_t p = 0; p < r->n[i] / 4; p++) { uint32_t v; memcpy(&v, r->b[i] + 4 * p, 4); v &= dmask; memcpy(r->b[i] + 4 * p, &v, 4); }
    }
    return ok;
}
static size_t texels_differ(const result *a, const result *b, int i, uint32_t bpp) {
    size_t n = 0; for (size_t p = 0; p < a->n[i] / bpp; p++) n += memcmp(a->b[i] + p * bpp, b->b[i] + p * bpp, bpp) != 0;
    return n;
}
static size_t texels_set(const result *a, int i, uint32_t bpp) {   // texels the draws changed from zero (the scene is not empty)
    size_t n = 0; static const uint8_t z[8];
    for (size_t p = 0; p < a->n[i] / bpp; p++) n += memcmp(a->b[i] + p * bpp, z, bpp) != 0;
    return n;
}

int main(void) {
    const hostvk_dev *dev = hv_up("t_vk_gpl_identity");
    hv_device(0);
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&queue, &qb);
    hv_pool mp; if (!hv_pool_make(&mp, 1 << 18)) { t("memory pool allocated", 0); return hv_finish(); }
    cmds = (hv_cmds){mp.mem + 65536, 0, 65536};
    vb0 = mp.mem + 131072; vb1 = mp.mem + 131072 + 4096;
    memcpy(vb0, k_pos, sizeof k_pos); memcpy(vb1, k_col, sizeof k_col);
    static const char vin[] = "[{\"location\":0,\"type\":\"float\"},{\"location\":1,\"type\":\"float\"}]";
    const hv_shader smix[2] = {{"gpl_vary", 0, HV_SPV(gpl_vary_vert_spv), NULL, vin}, {"gpl_mix", 1, HV_SPV(gpl_mix_frag_spv), NULL, NULL}};
    const hv_shader suint[2] = {{"gpl_vary", 0, HV_SPV(gpl_vary_vert_spv), NULL, vin}, {"gpl_uint", 1, HV_SPV(gpl_uint_frag_spv), NULL, NULL}};
    hv_program(&p_mix, &mp, 0, smix, 2); hv_program(&p_uint, &mp, 1024, suint, 2);
    ta = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 64, 64, 1, 1, 0); tb = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA16F, 64, 64, 1, 1, 0);
    tbd = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH24_STENCIL8, 64, 64, 1, 1, 0);
    tc0 = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R32UI, 32, 32, 1, 1, 0); tc1 = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8UI, 32, 32, 1, 1, 0);
    td = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 64, 64, 1, 1, 0); te = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 32, 32, 1, 1, 0);
    tf = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 32, 32, 1, 1, 0);
    const char *qwhy = NULL; uint32_t quirks = hvk_quirks(&qwhy);
    printf("device level %c (pipelines level %c), graphicsPipelineLibraryFastLinking %u, independent interpolation %u, one pipeline for every topology class %u; quirk table: %s\n",
           dev->caps.device_level, hvk_pso_level(), dev->caps.gpl_fast_link, dev->caps.gpl_indep_interp, dev->x.topology_unrestricted,
           quirks ? qwhy : "no row for this device");
    struct { const char *name; char level; int opt, twice, gpl; } modes[] = {
        {"fast-linked, level A", 'A', 0, 0, 1}, {"optimized relinks, level A", 'A', 1, 1, 1}, {"libraries with the state baked, level B", 'B', 0, 0, 1},
        {"monolithic, dynamic state, level C", 'C', 0, 0, 0}, {"monolithic, state baked, level D", 'D', 0, 0, 0}};
    enum { NM = sizeof modes / sizeof *modes };
    result res[NM]; memset(res, 0, sizeof res); int ran[NM] = {0}, ref = -1;
    int gpl = dev->caps.device_level <= 'B' && dev->caps.gpl_fast_link && dev->caps.gpl_indep_interp;
    char name[260]; int total_draws = 0;
    for (int m = 0; m < NM; m++) {
        if (modes[m].gpl && !gpl) { printf("note: %s: the device has no usable pipeline libraries, not run\n", modes[m].name); continue; }
        hvk_pso_test_optimize(modes[m].opt); hvk_pso_test_level(modes[m].level);
        nvn_state_reset();
        hvk_pso_stats s0, s1; hvk_pso_stats_get(&s0);
        uint64_t t0 = plat_now_ns(); g_draws = 0;
        scenes();
        if (modes[m].twice) { hvk_work_drain(HVK_Q_N); hvk_pso_stats_get(&s0); g_draws = 0; scenes(); }
        uint64_t t1 = plat_now_ns();
        hvk_pso_stats_get(&s1); total_draws += g_draws * (modes[m].twice ? 2 : 1);
        uint64_t waits = 0, wait_ns = 0; for (int i = 0; i < HVK_W_N; i++) { waits += s1.wait_n[i] - s0.wait_n[i]; wait_ns += s1.wait_ns[i] - s0.wait_ns[i]; }
        printf("%s: %d draws in %.1f ms; pipelines bound: %llu fast-linked, %llu optimized, %llu monolithic; made: %llu fast links, %llu optimized links (%llu swapped in), %llu monolithic, "
               "%llu libraries; %llu draws waited, %.2f ms in all\n", modes[m].name, g_draws, (t1 - t0) / 1e6,
               (unsigned long long)(s1.draws_fast - s0.draws_fast), (unsigned long long)(s1.draws_opt - s0.draws_opt), (unsigned long long)(s1.draws_mono - s0.draws_mono),
               (unsigned long long)(s1.fast - s0.fast), (unsigned long long)(s1.opt - s0.opt), (unsigned long long)(s1.swapped - s0.swapped), (unsigned long long)(s1.mono - s0.mono),
               (unsigned long long)(s1.lib[0] + s1.lib[1] + s1.lib[2] + s1.lib[3] - s0.lib[0] - s0.lib[1] - s0.lib[2] - s0.lib[3]), (unsigned long long)waits, wait_ns / 1e6);
        int kind_ok = modes[m].level >= 'C' ? s1.draws_mono - s0.draws_mono == (uint64_t)g_draws : modes[m].opt ? s1.draws_opt - s0.draws_opt == (uint64_t)g_draws : s1.draws_fast - s0.draws_fast == (uint64_t)g_draws;
        snprintf(name, sizeof name, "%s: every draw bound a pipeline of that kind", modes[m].name); t(name, kind_ok);
        if (modes[m].level >= 'C') { snprintf(name, sizeof name, "%s: each pipeline a draw was the first to use was waited for and counted", modes[m].name); t(name, waits == s1.mono - s0.mono && waits > 0); }
        snprintf(name, sizeof name, "%s: no draw failed", modes[m].name); t(name, s1.failed == s0.failed);
        snprintf(name, sizeof name, "%s: the targets read back", modes[m].name); t(name, read_all(&res[m]));
        ran[m] = 1; if (ref < 0) ref = m;
    }
    // The comparison: every run against the first one that ran
    int differ = 0; uint32_t need = 0;
    if (ref >= 0) {
        static const uint32_t bpp[NT] = {4, 8, 4, 1, 4, 4, 4, 4, 4};
        for (int i = 0; i < NT; i++) { snprintf(name, sizeof name, "scene %s: the draws changed %zu texels (not an empty scene)", k_tname[i], texels_set(&res[ref], i, bpp[i])); t(name, texels_set(&res[ref], i, bpp[i]) > 0 || i == 3); }
        for (int m = 0; m < NM; m++) if (ran[m] && m != ref) {
            size_t all = 0;
            for (int i = 0; i < NT; i++) { size_t d = texels_differ(&res[ref], &res[m], i, bpp[i]); all += d; if (d) printf("difference: %s against %s: %s, %zu texels\n", modes[m].name, modes[ref].name, k_tname[i], d); }
            if (all) { differ = 1; if (modes[m].opt) need |= HVK_QUIRK_NO_OPT; else need |= 0x80000000u; }
            snprintf(name, sizeof name, "%s: the same pixels as %s, bit for bit (%zu texels differ)", modes[m].name, modes[ref].name, all);
            t(name, !all || (modes[m].opt && (quirks & HVK_QUIRK_NO_OPT)));
        }
    }
    if (differ) printf("note: a difference that is not the driver's recorded quirk needs a row in vk_gpl.c's k_quirks for vendor %#x driver id %u version %#x (flags %#x)\n",
                       dev->props.vendorID, dev->driver_id, dev->props.driverVersion, need);
    else printf("identity: bit-exact across every kind of pipeline that ran, %d draws in all (quirk table: %s)\n", total_draws, quirks ? qwhy : "no row for this device");
    // what the pipelines cost on this device, over every run (lavapipe: LLVM on the CPU, not a GPU driver's compiler)
    hvk_pso_stats a; hvk_pso_stats_get(&a);
    static const char *const part[4] = {"vertex input", "pre-rasterization", "fragment shader", "fragment output"};
    for (int i = 0; i < 4; i++) printf("measured: %s libraries %llu, %.3f ms each\n", part[i], (unsigned long long)a.lib[i], a.lib[i] ? a.lib_ns[i] / 1e6 / (double)a.lib[i] : 0);
    printf("measured: fast links %llu, %.3f ms each; optimized links %llu, %.3f ms each; monolithic pipelines %llu, %.3f ms each; the longest wait of a draw %.3f ms, %llu over 2 ms\n",
           (unsigned long long)a.fast, a.fast ? a.fast_ns / 1e6 / (double)a.fast : 0, (unsigned long long)a.opt, a.opt ? a.opt_ns / 1e6 / (double)a.opt : 0,
           (unsigned long long)a.mono, a.mono ? a.mono_ns / 1e6 / (double)a.mono : 0, a.wait_max_ns / 1e6, (unsigned long long)a.over_budget);
    char line[2048]; be_pso_line(line, sizeof line); printf("be_pso_line (the log's pipeline line): %s\n", line);
    for (int m = 0; m < NM; m++) for (int i = 0; i < NT; i++) free(res[m].b[i]);
    hvk_pso_test_optimize(1); hvk_pso_test_level('A');
    hv_pool_free(&mp);
    return hv_finish();
}
