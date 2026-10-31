// Depth 24 with and without stencil on the Vulkan backend (docs/design/vulkan-backend.md 5.1, 5.5, decision D10): DEPTH24 and DEPTH24_STENCIL8 held as X8_D24 / D24_UNORM_S8_UINT
// where the device has them and, forced (DM_VK_DEPTH24=d32f), as D32_SFLOAT / D32_SFLOAT_S8_UINT, the format of a device without them (AMD). In each mode:
//   uploads: packaged DEPTH24_STENCIL8, DEPTH24, DEPTH32F_STENCIL8 and STENCIL8 textures whose guest texels pack depth and stencil as the table assumes (depth in the low 24 bits,
//            stencil in the top byte; float depth then the stencil byte) read back aspect by aspect: the depth as the image's format holds it, the stencil byte by byte;
//   draws:   a triangle at a known depth writing depth and stencil (REPLACE) into a cleared DEPTH24_STENCIL8 target; a texture → buffer copy and ReadTexels give the guest's packing,
//            and both modes give the same words;
//   buffer → texture: guest words copied into a texture and back out: the same words;
//   clears:  a texture clear of part of a level (step P7; it was declared before) changes the region only, in both aspects;
//   stencil views: a view in stencil mode sampled with an unsigned integer declaration reads the stencil written;
//   bias:    the constant depth bias of 16 units drawn at depths in [0.5, 1) and in [0.25, 0.5), measured in 2^-24 steps through the copy: on D24 and on the float substitute with
//            VK_EXT_depth_bias_control's float representation it is 16 at both depths; without the extension (forced off) the bias goes as the game gives it, which is 16 steps
//            in [0.5, 1) and half that in [0.25, 0.5): the shadow-acne risk decision D10's fallback leaves, measured; a slope-scaled bias (factor 1 at a known slope of 2^-14 a
//            pixel) is 1024 steps in every mode. rendering.md has no reference values for the bias on Metal.
// Checks: per mode, every texel of each case; the bias per mode and depth; the validation check
#include "t_vk_harness.h"
#include "shaders/gen/pos_vert.h"
#include "shaders/gen/empty_frag.h"
#include "shaders/gen/fullscreen_vert.h"
#include "shaders/gen/fetch_u_frag.h"
#include <math.h>

enum { W = 4, H = 4, N = W * H };
static hv_pool g_p; static size_t g_off; static hv_cmds g_c; static NVNqueue g_q;
static NVNprogram g_draw, g_fetch; static NVNtexturePool g_tp; static NVNsamplerPool g_sp; static int g_id = 1;
static float *g_tri;   // the triangle's vertices in the pool (set per draw)
static uint32_t *g_buf;   // a guest buffer for copies
static char name[300];

static uint32_t z24_round(double d) { return (uint32_t)floor(d * 16777215.0 + 0.5); }
static float depth_of(uint32_t z) { return (float)(z & 0xFFFFFFu) / 16777215.0f; }
static int d24_native(const hostvk_dev *dev) {
    VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(dev->phys, VK_FORMAT_D24_UNORM_S8_UINT, &p);
    return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
}
// A triangle covering the target at window depth d (GL depth mode: z = 2d - 1) and d_right at its far right vertex (x = 3 in clip space: a slope along x), depth always written,
// stencil replaced by ref when ref >= 0, polygon offset units and slope factor
static void draw_ex(tex_t *target, double d, double d_right, int ref, float units, float factor) {
    hv_rt(&g_c, 0, NULL, target);
    float z = (float)(2 * d - 1), zr = (float)(2 * d_right - 1), v[12] = {-1, -1, z, 1, 3, -1, zr, 1, -1, 3, z, 1};
    memcpy(g_tri, v, sizeof v);
    hv_bind(&g_c, &g_draw, 3);
    ds_st ds; memset(&ds, 0, sizeof ds); ds.dt = 1; ds.dw = 1; ds.df = NVN_DEPTH_FUNC_ALWAYS;
    if (ref >= 0) { ds.st = 1; ds.sf_f = ds.sf_b = NVN_STENCIL_FUNC_ALWAYS; ds.fail_f = ds.zfail_f = ds.fail_b = ds.zfail_b = NVN_STENCIL_OP_KEEP; ds.zpass_f = ds.zpass_b = NVN_STENCIL_OP_REPLACE; }
    hv_state(&g_c, 5, &ds, sizeof ds);
    if (ref >= 0) {
        p_stencil s = {3, 0xff, 0xff, ref, 2}; hv_emit(&g_c, OP_STENCIL, &s, sizeof s);
        s.which = 1; hv_emit(&g_c, OP_STENCIL, &s, sizeof s); s.which = 0; hv_emit(&g_c, OP_STENCIL, &s, sizeof s);
    }
    poly_st po; memset(&po, 0, sizeof po); po.cull = NVN_FACE_NONE; po.front = NVN_FRONT_FACE_CCW; po.mode = NVN_POLYGON_MODE_FILL; po.offset_en = units != 0 || factor != 0 ? NVN_POLYGON_OFFSET_ENABLE_FILL : 0;
    hv_state(&g_c, 4, &po, sizeof po);
    p_poly_offset off = {factor, units, 0}; hv_emit(&g_c, OP_POLY_OFFSET, &off, sizeof off);
    const va_st attr[1] = {{.fmt = NVN_FORMAT_RGBA32F, .stream = 0}}; const vs_st st[1] = {{16, 0}};
    hv_attrs(&g_c, 1, attr); hv_streams(&g_c, 1, st); hv_vbuf(&g_c, 0, g_tri, sizeof v);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
}
static void draw_at(tex_t *target, double d, int ref, float units) { draw_ex(target, d, d, ref, units, 0); }
static void clear_ds(tex_t *target, float d, int s) { hv_rt(&g_c, 0, NULL, target); hv_scissor(&g_c, 0, 0, W, H); p_clear_ds c = {d, 1, s, 0xff}; hv_emit(&g_c, OP_CLEAR_DS, &c, sizeof c); }
// The guest words of a depth-stencil texture's level 0 (W x H) through a texture → buffer copy
static void t2b(const tex_t *t, uint32_t *out, int bpt) {
    memset(g_buf, 0x77, (size_t)N * (size_t)bpt);
    hv_t2b(&g_c, t, NULL, g_buf, 0, 0, 0, W, H, 1);
    hv_submit(&g_q, &g_c);
    memcpy(out, g_buf, (size_t)N * (size_t)bpt);
}
static NVNtexture *packaged(int fmt, const void *texels, int bpt) {
    NVNtexture *t = calloc(1, sizeof *t);
    hv_texture(t, &g_p, &g_off, NVN_TEXTURE_TARGET_2D, fmt, W, H, 1, 1, 0, 0, 0);
    hv_tex_fill(t, 0, 0, texels, (size_t)W * (size_t)bpt);
    return t;
}

static void run_mode(const hostvk_dev *dev, int d32f, uint32_t ref_words[N]) {
    const char *mode = d32f ? "as 32-bit float" : "as D24";
    hvk_test_tex_opts(-1, -1, d32f, -1);
    // ---- uploads ----
    uint32_t w24s8[N], w24[N]; uint8_t w32s8[N * 8], s8[N];
    for (int i = 0; i < N; i++) {
        uint32_t z = (uint32_t)(i * 1048573u + 12345u) & 0xFFFFFFu; w24s8[i] = z | (uint32_t)(0x30 + i) << 24; w24[i] = z | 0xAB000000u;
        float f = (float)i / 15.0f; uint32_t st = (uint32_t)(0xC0 + i) | 0xDEAD0000u; memcpy(w32s8 + 8 * i, &f, 4); memcpy(w32s8 + 8 * i + 4, &st, 4); s8[i] = (uint8_t)(7 * i + 1);
    }
    struct { int fmt; const void *texels; int bpt; const char *what; } up[] = {
        {NVN_FORMAT_DEPTH24_STENCIL8, w24s8, 4, "DEPTH24_STENCIL8"}, {NVN_FORMAT_DEPTH24, w24, 4, "DEPTH24 (top byte ignored)"},
        {NVN_FORMAT_DEPTH32F_STENCIL8, w32s8, 8, "DEPTH32F_STENCIL8 (eight bytes a texel)"}, {NVN_FORMAT_STENCIL8, s8, 1, "STENCIL8"}};
    for (int k = 0; k < 4; k++) {
        NVNtexture *tx = packaged(up[k].fmt, up[k].texels, up[k].bpt);
        const tex_t *tt = (const tex_t *)(const void *)tx;
        VkFormat f; VkImageAspectFlags a; hvk_test_image(tt, &f, &a);
        uint32_t dep[N]; uint8_t st[N]; int ok = 1;
        int has_d = up[k].fmt != NVN_FORMAT_STENCIL8, has_s = up[k].fmt != NVN_FORMAT_DEPTH24;
        if (has_d) ok &= hv_read_blocks(tt, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, W, H, 1, 1, 1, 4, dep);
        if (has_s) ok &= hv_read_blocks(tt, VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, W, H, 1, 1, 1, 1, st);
        int float_depth = f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
        for (int i = 0; ok && i < N; i++) {
            if (up[k].fmt == NVN_FORMAT_DEPTH32F_STENCIL8) { ok &= !memcmp(&dep[i], w32s8 + 8 * i, 4) && st[i] == w32s8[8 * i + 4]; continue; }
            if (up[k].fmt == NVN_FORMAT_STENCIL8) { ok &= st[i] == s8[i]; continue; }
            uint32_t g = up[k].fmt == NVN_FORMAT_DEPTH24 ? w24[i] : w24s8[i];
            if (float_depth) { float e = depth_of(g); ok &= !memcmp(&dep[i], &e, 4); } else ok &= (dep[i] & 0xFFFFFFu) == (g & 0xFFFFFFu);
            if (has_s) ok &= st[i] == g >> 24;
        }
        snprintf(name, sizeof name, "%s, %s: an upload of %s holds its depth and stencil (VkFormat %u)", mode, up[k].what, up[k].what, (unsigned)f); t(name, ok);
    }
    // ---- draws, the copy out and ReadTexels ----
    tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH24_STENCIL8, W, H, 1, 1, 0);
    clear_ds(&target, 1.0f, 0);
    draw_at(&target, 0.6, 0x5a, 0);
    hv_submit(&g_q, &g_c);
    uint32_t words[N]; t2b(&target, words, 4);
    int ok = 1; for (int i = 0; i < N; i++) ok &= (words[i] >> 24) == 0x5a && (words[i] & 0xFFFFFFu) == z24_round(0.6);
    snprintf(name, sizeof name, "%s: depth 0.6 and stencil 0x5a drawn, copied out as the guest packs them (%#x, expected %#x)", mode, words[0], z24_round(0.6) | 0x5a000000u); t(name, ok);
    if (ref_words[0] == 0) memcpy(ref_words, words, sizeof words);
    else { snprintf(name, sizeof name, "%s: the words copied out equal those of the D24 mode", mode); t(name, !memcmp(ref_words, words, sizeof words)); }
    // ReadTexels of the same GPU-written texture: a texture object of the frontend over the target's image is needed; a drawn NVNtexture instead
    NVNtexture *rt = packaged(NVN_FORMAT_DEPTH24_STENCIL8, w24s8, 4);
    const tex_t *rtt = (const tex_t *)(const void *)rt;
    clear_ds((tex_t *)rtt, 1.0f, 0); draw_at((tex_t *)rtt, 0.6, 0x5a, 0); hv_submit(&g_q, &g_c);
    uint32_t rd[N]; memset(rd, 0, sizeof rd);
    NVNcopyRegion reg = {0, 0, 0, W, H, 1}; h_nvnTextureReadTexels(rt, NULL, &reg, rd);
    snprintf(name, sizeof name, "%s: ReadTexels of a drawn DEPTH24_STENCIL8 texture gives the guest's packing", mode); t(name, !memcmp(rd, words, sizeof words));
    // ---- buffer → texture → buffer ----
    tex_t b2t = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH24_STENCIL8, W, H, 1, 1, 0);
    memcpy(g_buf, w24s8, sizeof w24s8);
    p_copy_b2t cb; memset(&cb, 0, sizeof cb); cb.src = (uint64_t)(uintptr_t)g_buf; cb.tex = &b2t; cb.r = (NVNcopyRegion){0, 0, 0, W, H, 1};
    hv_emit(&g_c, OP_COPY_B2T, &cb, sizeof cb); hv_submit(&g_q, &g_c);
    uint32_t back[N]; t2b(&b2t, back, 4);
    snprintf(name, sizeof name, "%s: guest words copied into a DEPTH24_STENCIL8 texture and back out are the same words", mode); t(name, !memcmp(back, w24s8, sizeof back));
    tex_t b2t32 = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH32F_STENCIL8, W, H, 1, 1, 0);
    memcpy(g_buf, w32s8, sizeof w32s8);
    cb.tex = &b2t32; hv_emit(&g_c, OP_COPY_B2T, &cb, sizeof cb); hv_submit(&g_q, &g_c);
    uint8_t back32[N * 8]; t2b(&b2t32, (uint32_t *)(void *)back32, 8);
    ok = 1; for (int i = 0; i < N; i++) ok &= !memcmp(back32 + 8 * i, w32s8 + 8 * i, 5);
    snprintf(name, sizeof name, "%s: DEPTH32F_STENCIL8 texels copied in and back out keep depth and stencil", mode); t(name, ok);
    // ---- a texture clear of part of a level ----
    tex_t ct = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH24_STENCIL8, W, H, 1, 1, 0);
    clear_ds(&ct, 0.75f, 0xa5); hv_submit(&g_q, &g_c);
    p_clear_tex pc; memset(&pc, 0, sizeof pc); pc.tex = &ct; pc.mask = 15; pc.r = (NVNcopyRegion){1, 1, 0, 2, 2, 1}; pc.c.f[0] = 0.25f; pc.c.u[1] = 0x3c;
    hv_emit(&g_c, OP_CLEAR_TEX, &pc, sizeof pc); hv_submit(&g_q, &g_c);
    uint32_t cw[N]; t2b(&ct, cw, 4); ok = 1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int in = x >= 1 && x < 3 && y >= 1 && y < 3; uint32_t e = in ? z24_round(0.25) | 0x3cu << 24 : z24_round(0.75) | 0xa5u << 24;
        ok &= cw[y * W + x] == e;
    }
    snprintf(name, sizeof name, "%s: a texture clear of a 2x2 region changes that region's depth and stencil only", mode); t(name, ok);
    // ---- a stencil-mode view, sampled as unsigned integers ----
    NVNtextureView view; memset(&view, 0, sizeof view); view_t *v = (view_t *)(void *)&view; v->set_dsmode = 1; v->dsmode = NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL;
    int id = g_id++; h_nvnTexturePoolRegisterTexture(&g_tp, id, rt, &view);
    uint32_t sv[N * 4]; int read = hv_fetch(&g_q, &g_c, &g_fetch, &g_tp, &g_sp, id, NVN_FORMAT_RGBA32UI, W, H, sv);
    ok = read; for (int i = 0; ok && i < N; i++) ok &= sv[4 * i] == 0x5a;
    snprintf(name, sizeof name, "%s: a stencil-mode view read with an unsigned integer declaration gives the stencil drawn (%u)", mode, read ? sv[0] : 0); t(name, ok);
    (void)dev;
}

// The constant bias of `units` at window depth d, in 2^-24 steps, measured through the copy out (the guest's 24-bit depth)
static double bias_steps(double d, float units) {
    tex_t a = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH24, W, H, 1, 1, 0), b = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH24, W, H, 1, 1, 0);
    clear_ds(&a, 1.0f, 0); draw_at(&a, d, -1, 0); clear_ds(&b, 1.0f, 0); draw_at(&b, d, -1, units); hv_submit(&g_q, &g_c);
    uint32_t wa[N], wb[N]; t2b(&a, wa, 4); t2b(&b, wb, 4);
    return (double)(wb[0] & 0xFFFFFFu) - (double)(wa[0] & 0xFFFFFFu);
}
// The slope-scaled bias of factor 1 on a triangle whose depth rises by 2^-14 a pixel along x from 0.6 (none along y): in 2^-24 steps, at texel 0; the slope term does not
// depend on the depth format, so every mode should give 1024 steps
static double slope_steps(void) {
    tex_t a = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH24, W, H, 1, 1, 0), b = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH24, W, H, 1, 1, 0);
    double d0 = 0.6, d1 = d0 + 2 * W * ldexp(1, -14);   // the right vertex is 2W pixels away
    clear_ds(&a, 1.0f, 0); draw_ex(&a, d0, d1, -1, 0, 0); clear_ds(&b, 1.0f, 0); draw_ex(&b, d0, d1, -1, 0, 1); hv_submit(&g_q, &g_c);
    uint32_t wa[N], wb[N]; t2b(&a, wa, 4); t2b(&b, wb, 4);
    return (double)(wb[0] & 0xFFFFFFu) - (double)(wa[0] & 0xFFFFFFu);
}

int main(void) {
    const hostvk_dev *dev = hv_up("t_vk_depth24");
    hv_device(0);
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&g_q, &qb);
    if (!hv_pool_make(&g_p, 4u << 20)) { t("memory pool allocated", 0); return hv_finish(); }
    g_c = (hv_cmds){g_p.mem + 0x40000, 0, 0x10000};
    g_tri = (float *)(void *)(g_p.mem + 0x38000); g_buf = (uint32_t *)(void *)(g_p.mem + 0x39000);
    static const char vin[] = "[{\"location\":0,\"type\":\"float\",\"n\":4}]";
    static const char ju[] = "[{\"kind\":\"tex\",\"reg\":0,\"name\":\"t\",\"class\":\"combined\",\"set\":2,\"binding\":2048,\"dim\":\"2d\",\"sampled\":\"uint\"},"
                             "{\"kind\":\"samp_tex\",\"reg\":0,\"name\":\"t\",\"class\":\"combined\",\"set\":2,\"binding\":2048,\"dim\":\"2d\",\"sampled\":\"uint\"}]";
    const hv_shader sd[2] = {{"pos", 0, HV_SPV(pos_vert_spv), NULL, vin}, {"empty", 1, HV_SPV(empty_frag_spv), NULL, NULL}};
    const hv_shader sf[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"fetch_u", 1, HV_SPV(fetch_u_frag_spv), ju, NULL}};
    hv_program(&g_draw, &g_p, 0, sd, 2); hv_program(&g_fetch, &g_p, 1024, sf, 2);
    h_nvnTexturePoolInitialize(&g_tp, &g_p.pool, 0x10000, 64); h_nvnSamplerPoolInitialize(&g_sp, &g_p.pool, 0x30000, 16);
    NVNsamplerBuilder sb; NVNsampler samp; h_nvnSamplerBuilderSetDefaults(&sb); h_nvnSamplerInitialize(&samp, &sb); h_nvnSamplerPoolRegisterSampler(&g_sp, 1, &samp);
    g_off = 0x50000;
    static const VkFormat probe[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT, VK_FORMAT_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT};
    static const char *const pn[] = {"D24_UNORM_S8_UINT", "X8_D24_UNORM_PACK32", "D32_SFLOAT_S8_UINT", "D32_SFLOAT", "S8_UINT", "D16_UNORM_S8_UINT"};
    for (int i = 0; i < 6; i++) {
        VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(dev->phys, probe[i], &p);
        printf("device format %-20s depth-stencil attachment %s, sampled %s\n", pn[i], p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? "yes" : "no",
               p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ? "yes" : "no");
    }
    printf("VK_EXT_depth_bias_control: %s (float representation %s, force unorm %s, exact %s)\n", dev->x.depth_bias_control ? "enabled" : "absent", dev->x.dbc_float ? "yes" : "no",
           dev->x.dbc_force_unorm ? "yes" : "no", dev->x.dbc_exact ? "yes" : "no");
    uint32_t ref[N]; memset(ref, 0, sizeof ref);
    int native = d24_native(dev);
    if (native) run_mode(dev, 0, ref); else printf("note: the device has no D24_UNORM_S8_UINT: the D24 mode is the substitute's\n");
    run_mode(dev, 1, ref);
    // ---- the bias (decision D10) ----
    struct { int d32f, no_dbc; const char *what; double want_hi, want_lo; } bm[] = {
        {0, 0, "D24", 16, 16}, {1, 0, "32-bit float, VK_EXT_depth_bias_control's float representation", 16, 16}, {1, 1, "32-bit float without depth bias control", 16, 8}};
    for (int m = 0; m < 3; m++) {
        if (m == 0 && !native) continue;
        if (m == 1 && !dev->x.dbc_float) { printf("note: no float representation of the depth bias: case skipped\n"); continue; }
        hvk_test_tex_opts(-1, -1, bm[m].d32f, -1); hvk_test_no_depth_bias_control(bm[m].no_dbc);
        double hi = bias_steps(0.7, 16), lo = bias_steps(0.3, 16);
        printf("constant depth bias of 16 units, %s: %.0f steps of 2^-24 at depth 0.7, %.0f at depth 0.3\n", bm[m].what, hi, lo);
        snprintf(name, sizeof name, "bias on %s: %.0f and %.0f steps at depths 0.7 and 0.3 (expected %.0f and %.0f, within one)", bm[m].what, hi, lo, bm[m].want_hi, bm[m].want_lo);
        t(name, fabs(hi - bm[m].want_hi) <= 1 && fabs(lo - bm[m].want_lo) <= 1);
        double sl = slope_steps();
        printf("slope-scaled depth bias, factor 1 at a slope of 2^-14 a pixel, %s: %.0f steps of 2^-24\n", bm[m].what, sl);
        snprintf(name, sizeof name, "slope bias on %s: %.0f steps (expected 1024, within one)", bm[m].what, sl);
        t(name, fabs(sl - 1024) <= 1);
    }
    hvk_test_no_depth_bias_control(0); hvk_test_tex_opts(-1, -1, 0, -1);
    hv_pool_free(&g_p);
    return hv_finish();
}
