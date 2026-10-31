// What the P3 core declared for step P8 besides the native stages (docs/design/vulkan-backend.md section 6, 2.7, 4.8): NVN's aliased lines as Vulkan's Bresenham lines
// (VK_EXT_line_rasterization, dynamic on level A and baked into the pipeline on level D: an x-major line through pixel boundaries covers one pixel a column, where Vulkan's
// rectangular lines cover two in every other column), advanced blend modes reduced to fixed-function factors (SRC_OVER premultiplied and not, XOR; the Metal backend's
// adv_blend), indirect quads converted on the GPU (array and indexed, the record written by a copy earlier in the submission), an array of descriptor handles (TEXA: the
// driver table's words read at a run-time index, element k the combined slot k), and draw-based copies for what a blit cannot do (DEPTH32F into DEPTH16, DEPTH24_STENCIL8
// into DEPTH32F_STENCIL8 with the stencil exported, and a scaled mirrored colour copy drawn equal to the blit). No Metal test covers these but adv_blend's reduction (its
// smoke routes). Checks: the pixels and texels of each case, and the validation check
#include "t_vk_harness.h"
#include <math.h>
#include "shaders/gen/pos_vert.h"
#include "shaders/gen/red_frag.h"
#include "shaders/gen/ucol_frag.h"
#include "shaders/gen/fullscreen_vert.h"
#include "shaders/gen/arr_frag.h"

void hvk_pso_test_level(char level);
void hvk_test_copy_draw(int on);
static NVNqueue g_q; static hv_cmds g_c; static hv_pool g_p; static NVNtexturePool g_tp; static NVNsamplerPool g_sp; static size_t g_off = 0x40000;
static NVNprogram g_red, g_ucol, g_arr;
static float *g_vb, *g_ubo; static uint32_t *g_args, *g_stage; static uint16_t *g_ix;
static void make(NVNtexture *t, int format, int w, int h) {
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetDevice(&tb, &g_dev); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetFormat(&tb, format); h_nvnTextureBuilderSetSize2D(&tb, w, h); h_nvnTextureBuilderSetFlags(&tb, 0);
    h_nvnTextureBuilderSetStorage(&tb, &g_p.pool, (ptrdiff_t)g_off); g_off += 0x10000;
    h_nvnTextureInitialize(t, &tb);
}
static void clear_tex(const void *t, int x, int y, int w, int h, float r, float g, float b, float a, uint32_t stencil) {
    p_clear_tex c; memset(&c, 0, sizeof c); c.tex = (const tex_t *)t; c.mask = 15; c.r = (NVNcopyRegion){x, y, 0, w, h, 1}; c.c.f[0] = r; c.c.f[1] = g; c.c.f[2] = b; c.c.f[3] = a;
    if (stencil != UINT32_MAX) c.c.u[1] = stencil;
    hv_emit(&g_c, OP_CLEAR_TEX, &c, sizeof c);
}
static void copy(const void *s, const void *d, NVNcopyRegion sr, NVNcopyRegion dr, int flags) {
    p_copy_t2t c; memset(&c, 0, sizeof c); c.src = (const tex_t *)s; c.dst = (const tex_t *)d; c.sr = sr; c.dr = dr; c.flags = flags; hv_emit(&g_c, OP_COPY_T2T, &c, sizeof c);
}
static int count_red(const tex_t *t, int w, int h, int *per_col) {
    uint8_t px[64 * 4]; int n = 0;
    if (!hv_read(t, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, (uint32_t)w, (uint32_t)h, 4, px)) return -1;
    for (int i = 0; i < w * h; i++) if (px[4 * i] > 200 && px[4 * i + 1] < 50) { n++; if (per_col) per_col[i % w]++; }
    return n;
}
// 1. A line from pixel (0, 0.25) to (8, 4.25) on an 8x8 target
static void line_case(const char *level) {
    tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 8, 8, 1, 1, 0);
    const tex_t *rt[1] = {&target};
    const float v[2][4] = {{0 / 4.0f - 1, 0.25f / 4.0f - 1, 0, 1}, {8 / 4.0f - 1, 4.25f / 4.0f - 1, 0, 1}};
    memcpy(g_vb, v, sizeof v);
    hv_rt(&g_c, 1, rt, NULL); hv_clear_color(&g_c, 0, 15, 0, 0, 0, 1);
    hv_bind(&g_c, &g_red, 0x1f); hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&g_c, NVN_FACE_NONE);
    va_st a[1] = {{NVN_FORMAT_RGBA32F, 0, 0}}; hv_attrs(&g_c, 1, a); vs_st s[1] = {{16, 0}}; hv_streams(&g_c, 1, s); hv_vbuf(&g_c, 0, g_vb, sizeof v);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_LINES, 0, 2, 1);
    hv_submit(&g_q, &g_c);
    int col[8] = {0}, n = count_red(&target, 8, 8, col), one = n == 8;
    for (int i = 0; i < 8; i++) one &= col[i] == 1;
    char name[160]; snprintf(name, sizeof name, "level %s: an x-major line covers one pixel a column (Bresenham: %d pixels; rectangular lines would cover 12)", level, n);
    t(name, one);
}
// 2. An advanced blend mode over a blue target: the result's texels
static void blend_case(const char *label, unsigned mode, int npm, const float src[4], const uint8_t want[4]) {
    tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 8, 8, 1, 1, 0);
    const tex_t *rt[1] = {&target};
    memcpy(g_ubo, src, 16);
    hv_rt(&g_c, 1, rt, NULL); hv_clear_color(&g_c, 0, 15, 0, 0, 1, 1);
    hv_bind(&g_c, &g_ucol, 0x1f); hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&g_c, NVN_FACE_NONE);
    hv_buf(&g_c, OP_BIND_UBO, 1, 0, g_ubo, 16);
    blend_st b = {0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD, mode, (unsigned)npm, 0};
    hv_state(&g_c, 0, &b, sizeof b);
    color_st cs = {1, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; hv_state(&g_c, 1, &cs, sizeof cs);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    blend_st off = {0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD, 0, 0, 0}; hv_state(&g_c, 0, &off, sizeof off);
    color_st cs0 = {0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; hv_state(&g_c, 1, &cs0, sizeof cs0);
    hv_submit(&g_q, &g_c);
    uint8_t px[256]; int ok = hv_read(&target, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 8, 8, 4, px);
    for (int c = 0; ok && c < 4; c++) if (abs((int)px[c] - want[c]) > 1) { fprintf(stderr, "%s: %u %u %u %u\n", label, px[0], px[1], px[2], px[3]); ok = 0; }
    t(label, ok);
}

int main(void) {
    hv_up("t_vk_raster");
    hv_device(1);
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&g_q, &qb);
    if (!hv_pool_make(&g_p, 1 << 21)) { t("memory pool allocated", 0); return hv_finish(); }
    memset(g_p.mem, 0, 1 << 21);
    g_c = (hv_cmds){g_p.mem + 0x8000, 0, 0x8000};
    g_vb = (float *)(void *)(g_p.mem + 0x20000); g_ubo = (float *)(void *)(g_p.mem + 0x21000); g_args = (uint32_t *)(void *)(g_p.mem + 0x22000);
    g_stage = (uint32_t *)(void *)(g_p.mem + 0x23000); g_ix = (uint16_t *)(void *)(g_p.mem + 0x24000);
    const char *vin = "[{\"location\":0,\"type\":\"float\",\"n\":4}]";
    static const char ubo[] = "[{\"kind\":\"cbuf\",\"reg\":0,\"name\":\"C\",\"class\":\"ubo\",\"set\":0,\"binding\":76}]";
    static const char arr[] = "[{\"kind\":\"tex_c2a\",\"off\":32,\"n\":2,\"name\":\"t\",\"class\":\"combined\",\"set\":2,\"binding\":2048,\"dim\":\"2d\",\"sampled\":\"float\"},"
                              "{\"kind\":\"samp_c2a\",\"off\":32,\"n\":2,\"name\":\"t\",\"class\":\"combined\",\"set\":2,\"binding\":2048,\"dim\":\"2d\",\"sampled\":\"float\"}]";
    const hv_shader sr[2] = {{"pos", 0, HV_SPV(pos_vert_spv), NULL, vin}, {"red", 1, HV_SPV(red_frag_spv), NULL, NULL}};
    const hv_shader su[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"ucol", 1, HV_SPV(ucol_frag_spv), ubo, NULL}};
    const hv_shader sa[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"arr", 1, HV_SPV(arr_frag_spv), arr, NULL}};
    hv_program(&g_red, &g_p, 0, sr, 2); hv_program(&g_ucol, &g_p, 1024, su, 2); hv_program(&g_arr, &g_p, 2048, sa, 2);
    h_nvnTexturePoolInitialize(&g_tp, &g_p.pool, 0x10000, 64); h_nvnSamplerPoolInitialize(&g_sp, &g_p.pool, 0x18000, 16);
    NVNsamplerBuilder sb; NVNsampler samp; h_nvnSamplerBuilderSetDefaults(&sb); h_nvnSamplerInitialize(&samp, &sb); h_nvnSamplerPoolRegisterSampler(&g_sp, 1, &samp);

    // 1. lines: the dynamic mode (level A) and the mode baked into the pipeline (level D)
    line_case("A");
    hvk_pso_test_level('D'); line_case("D"); hvk_pso_test_level('A');

    // 2. advanced blend modes over (0, 0, 1, 1)
    const float pm[4] = {0.5f, 0, 0, 0.5f}, npm[4] = {1, 0, 0, 0.5f};
    blend_case("advanced SRC_OVER, premultiplied source: (0.5, 0, 0.5, 1)", NVN_BLEND_ADVANCED_MODE_SRC_OVER, 0, pm, (const uint8_t[4]){128, 0, 128, 255});
    blend_case("advanced SRC_OVER, source not premultiplied (multiplied by its alpha): (0.5, 0, 0.5, 1)", NVN_BLEND_ADVANCED_MODE_SRC_OVER, 1, npm, (const uint8_t[4]){128, 0, 128, 255});
    blend_case("advanced XOR: (0, 0, 0.5, 0.5)", NVN_BLEND_ADVANCED_MODE_XOR, 0, pm, (const uint8_t[4]){0, 0, 128, 128});
    blend_case("advanced DST_OVER over an opaque destination: the destination", NVN_BLEND_ADVANCED_MODE_DST_OVER, 0, pm, (const uint8_t[4]){0, 0, 255, 255});

    // 3. indirect quads: the record written by a copy earlier in the submission
    {
        const float q[8][4] = {{-1, -1, 0, 1}, {0, -1, 0, 1}, {0, 1, 0, 1}, {-1, 1, 0, 1}, {0, -1, 0, 1}, {1, -1, 0, 1}, {1, 1, 0, 1}, {0, 1, 0, 1}};
        memcpy(g_vb, q, sizeof q);
        for (int pass = 0; pass < 3; pass++) {
            tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 8, 8, 1, 1, 0);
            const tex_t *rt[1] = {&target};
            hv_rt(&g_c, 1, rt, NULL); hv_clear_color(&g_c, 0, 15, 0, 0, 0, 1);
            hv_bind(&g_c, &g_red, 0x1f); hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&g_c, NVN_FACE_NONE);
            va_st a[1] = {{NVN_FORMAT_RGBA32F, 0, 0}}; hv_attrs(&g_c, 1, a); vs_st s[1] = {{16, 0}}; hv_streams(&g_c, 1, s); hv_vbuf(&g_c, 0, g_vb, sizeof q);
            memset(g_args, 0, 32);
            if (pass < 2) {
                const uint32_t args[4] = {pass ? 4u : 8u, 1, 0, 0}; memcpy(g_stage, args, sizeof args); hv_b2b(&g_c, g_stage, g_args, 16);
                p_draw d; memset(&d, 0, sizeof d); d.prim = NVN_DRAW_PRIMITIVE_QUADS; d.instances = 1; d.ind = (uint64_t)(uintptr_t)g_args; hv_emit(&g_c, OP_DRAW, &d, sizeof d);
            } else {
                for (int i = 0; i < 8; i++) g_ix[i] = (uint16_t)i;
                const uint32_t args[5] = {8, 1, 0, 0, 0}; memcpy(g_stage, args, sizeof args); hv_b2b(&g_c, g_stage, g_args, 20);
                p_draw_idx d; memset(&d, 0, sizeof d); d.prim = NVN_DRAW_PRIMITIVE_QUADS; d.itype = 1; d.instances = 1; d.ibuf = (uint64_t)(uintptr_t)g_ix; d.ind = (uint64_t)(uintptr_t)g_args;
                hv_emit(&g_c, OP_DRAW_IDX, &d, sizeof d);
            }
            hv_submit(&g_q, &g_c);
            int n = count_red(&target, 8, 8, NULL), want = pass == 1 ? 32 : 64;
            char name[160]; snprintf(name, sizeof name, "indirect %s quads, the record written on the GPU: %d red pixels (expected %d)", pass == 2 ? "indexed" : "array", n, want);
            t(name, n == want);
        }
    }

    // 4. an array of two textures (TEXA): element k = the combined slot k (red, green)
    {
        NVNtexture r, g; make(&r, NVN_FORMAT_RGBA8, 8, 8); make(&g, NVN_FORMAT_RGBA8, 8, 8);
        clear_tex(&r, 0, 0, 8, 8, 1, 0, 0, 1, UINT32_MAX); clear_tex(&g, 0, 0, 8, 8, 0, 1, 0, 1, UINT32_MAX); hv_submit(&g_q, &g_c);
        h_nvnTexturePoolRegisterTexture(&g_tp, 1, &r, NULL); h_nvnTexturePoolRegisterTexture(&g_tp, 2, &g, NULL);
        tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 8, 8, 1, 1, 0);
        const tex_t *rt[1] = {&target};
        hv_rt(&g_c, 1, rt, NULL);
        p_set_pool tp = {(const dpool_t *)&g_tp}, sp = {(const dpool_t *)&g_sp}; hv_emit(&g_c, OP_SET_TEX_POOL, &tp, sizeof tp); hv_emit(&g_c, OP_SET_SAMP_POOL, &sp, sizeof sp);
        hv_bind(&g_c, &g_arr, 0x1f); hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&g_c, NVN_FACE_NONE);
        for (int s = 0; s < 2; s++) { p_bind_tex1 b = {1, s, (uint64_t)(uint32_t)(1 + s) | (uint64_t)1 << 20, nvn_dpool_base(&g_tp, 1), nvn_dpool_base(&g_sp, 0)}; hv_emit(&g_c, OP_BIND_TEX1, &b, sizeof b); }
        hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
        hv_submit(&g_q, &g_c);
        uint8_t px[256]; int ok = hv_read(&target, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 8, 8, 4, px);
        for (int i = 0; ok && i < 64; i++) { int x = i % 8; ok = x < 4 ? (px[4 * i] == 255 && px[4 * i + 1] == 0) : (px[4 * i] == 0 && px[4 * i + 1] == 255); if (!ok) fprintf(stderr, "array pixel %d: %u %u\n", i, px[4 * i], px[4 * i + 1]); }
        t("an array of driver-table handles: element 0 the texture of combined slot 0, element 1 of slot 1", ok);
    }

    // 5. draw-based copies
    {
        NVNtexture d32, d16; make(&d32, NVN_FORMAT_DEPTH32F, 8, 8); make(&d16, NVN_FORMAT_DEPTH16, 8, 8);
        clear_tex(&d32, 0, 0, 8, 8, 0.5f, 0, 0, 0, UINT32_MAX); clear_tex(&d16, 0, 0, 8, 8, 0, 0, 0, 0, UINT32_MAX);
        copy(&d32, &d16, (NVNcopyRegion){0, 0, 0, 8, 8, 1}, (NVNcopyRegion){0, 0, 0, 8, 8, 1}, 0);
        hv_submit(&g_q, &g_c);
        uint16_t z[64]; int ok = hv_read((const tex_t *)&d16, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 0, 0, 8, 8, 2, z);
        for (int i = 0; ok && i < 64; i++) ok = abs((int)z[i] - 32768) <= 1;
        if (!ok) fprintf(stderr, "DEPTH16 after the copy: %u\n", z[0]);
        t("DEPTH32F → DEPTH16 (no blit between two depth formats): drawn, 0.5 stored", ok);
        NVNtexture ds24, ds32; make(&ds24, NVN_FORMAT_DEPTH24_STENCIL8, 8, 8); make(&ds32, NVN_FORMAT_DEPTH32F_STENCIL8, 8, 8);
        clear_tex(&ds24, 0, 0, 8, 8, 0.25f, 0, 0, 0, 0x5a); clear_tex(&ds32, 0, 0, 8, 8, 1, 0, 0, 0, 0);
        copy(&ds24, &ds32, (NVNcopyRegion){0, 0, 0, 8, 8, 1}, (NVNcopyRegion){0, 0, 0, 8, 8, 1}, 0);
        hv_submit(&g_q, &g_c);
        float zf[64]; uint8_t st[64];
        int okd = hv_read((const tex_t *)&ds32, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 0, 0, 8, 8, 4, zf), oks = hv_read((const tex_t *)&ds32, VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 0, 0, 8, 8, 1, st);
        for (int i = 0; okd && i < 64; i++) okd = fabsf(zf[i] - 0.25f) < 1e-6f;
        for (int i = 0; oks && i < 64; i++) oks = st[i] == 0x5a;
        if (!okd || !oks) fprintf(stderr, "DEPTH32F_STENCIL8 after the copy: %g, %#x\n", zf[0], st[0]);
        t("DEPTH24_STENCIL8 → DEPTH32F_STENCIL8: the depth drawn (0.25)", okd);
        if (hvk_device()->x.stencil_export) t("... and the stencil exported (0x5a)", oks);
        else printf("note: no VK_EXT_shader_stencil_export: the stencil of a draw-based copy is not checked\n");
        // a scaled, mirrored colour copy drawn, against the same copy blitted
        NVNtexture src, viab, viad; make(&src, NVN_FORMAT_RGBA8, 8, 8); make(&viab, NVN_FORMAT_RGBA8, 4, 4); make(&viad, NVN_FORMAT_RGBA8, 4, 4);
        clear_tex(&src, 0, 0, 4, 8, 1, 0, 0, 1, UINT32_MAX); clear_tex(&src, 4, 0, 4, 8, 0, 1, 0, 1, UINT32_MAX); clear_tex(&src, 0, 0, 8, 2, 0, 0, 1, 1, UINT32_MAX);
        copy(&src, &viab, (NVNcopyRegion){0, 0, 0, 8, 8, 1}, (NVNcopyRegion){0, 0, 0, 4, 4, 1}, NVN_COPY_FLAGS_MIRROR_X_BIT);
        hv_submit(&g_q, &g_c);
        hvk_test_copy_draw(1);
        copy(&src, &viad, (NVNcopyRegion){0, 0, 0, 8, 8, 1}, (NVNcopyRegion){0, 0, 0, 4, 4, 1}, NVN_COPY_FLAGS_MIRROR_X_BIT);
        hv_submit(&g_q, &g_c);
        hvk_test_copy_draw(0);
        uint8_t pb[64], pd[64]; int okc = hv_read((const tex_t *)&viab, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 4, 4, 4, pb) && hv_read((const tex_t *)&viad, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 4, 4, 4, pd);
        okc = okc && !memcmp(pb, pd, sizeof pb) && pb[0] == 0 && pb[2] == 255 && pb[4 * 4] == 0 && pb[4 * 4 + 1] == 255 && pb[4 * 7] == 255;   // row 0 blue; row 1 green then red (mirrored)
        if (!okc) fprintf(stderr, "colour copy: blit %u %u %u / draw %u %u %u; row 1: %u %u %u .. %u %u %u\n", pb[0], pb[1], pb[2], pd[0], pd[1], pd[2], pb[16], pb[17], pb[18], pb[28], pb[29], pb[30]);
        t("a scaled, mirrored colour copy drawn gives the blit's texels", okc);
    }
    hv_pool_free(&g_p);
    return hv_finish();
}
