// NVN's advanced blend modes on the Vulkan backend (dm/gfx/hostvk/vk_gpl.c, "Advanced blend modes"; docs/reference/rendering.md, "Advanced blend modes on the Vulkan backend"). A CPU reference of
// NV_blend_equation_advanced as VK_EXT_blend_operation_advanced specifies it (the Vulkan specification's "Advanced Blend Operations": the f / X / Y / Z table, the HSL modes and the
// additional RGB operations; the uncorrelated, disjoint and conjoint overlaps; a source premultiplied or not, the destination premultiplied as NVN's), and draws through the
// backend checked against it, a full-screen triangle of one colour (ucol.frag) over a target cleared to another:
// 1. every mode reduced exactly to fixed-function factors, on an RGBA8 target, for opaque, translucent and transparent sources over opaque and translucent destinations, the
//    source premultiplied and, where the reduction takes it, not; on level A (dynamic blend state) and level D (the blend baked into the pipeline). SCREEN, EXCLUSION, MINUS,
//    MINUS_CLAMPED, RED, GREEN and BLUE are new: before them they kept the state's functions (here ONE, ZERO), so these checks fail on the earlier code;
// 2. the conditional reductions (MULTIPLY, INVERT_RGB, LIGHTEN): exact where their condition holds, and where it does not the error the table states (the pixel is the
//    fixed-function blend's, and it differs from the reference by the stated amount in the stated direction); the clamped modes on an RGBA16F target, where the fixed-function
//    blend does not clamp; LINEARDODGE drawn with the state's own functions on every path (vk_gpl.c adv_kept: neither reduced nor drawn through the extension, open), a
//    negative source on an RGBA16F target with Mario vs. Donkey Kong's SRC_ALPHA, ONE scaled by As (ONE, ONE and the mode's equation add it in full, so these checks fail on
//    the code that reduced it), and on RGBA8 the state's ONE, ZERO as a replace, premultiplied or not, on levels A and D;
//    and an advanced draw, a plain one and an advanced one again in one pass (the dynamic state shadow of vk_commands.c dyn_apply); the reductions' exactness by target format
//    (MULTIPLY and INVERT_RGB exact on a target without alpha, whose destination is always opaque);
// 3. on a device with VK_EXT_blend_operation_advanced (and advancedBlendCoherentOperations): every mode its limits allow, drawn through the extension against the reference, on
//    level A (the advanced state dynamic, with extendedDynamicState3ColorBlendAdvanced) and with the blend baked (levels B and D), and the alternation of advanced and plain
//    draws in one pass through it. Lavapipe has not the extension: the part says it is skipped and why, and does not fail.
// Checks: the texels of each case within 2/255 on RGBA8 (lavapipe blends 8-bit targets in 8-bit arithmetic) and 1/512 on RGBA16F, and the validation check
#include "t_vk_harness.h"
#include <math.h>
#include "shaders/gen/ucol_frag.h"
#include "shaders/gen/fullscreen_vert.h"

void hvk_pso_test_level(char level);
int hvk_adv_exact_on(unsigned mode, VkFormat f);
static NVNqueue g_q; static hv_cmds g_c; static hv_pool g_p; static NVNprogram g_ucol; static float *g_ubo;

// ---------- The CPU reference (the specification's equations, in double) ----------
static double lumv(const double c[3]) { return 0.30 * c[0] + 0.59 * c[1] + 0.11 * c[2]; }
static double minv(const double c[3]) { return fmin(fmin(c[0], c[1]), c[2]); }
static double maxv(const double c[3]) { return fmax(fmax(c[0], c[1]), c[2]); }
static void clip_color(double c[3]) {
    double l = lumv(c), mn = minv(c), mx = maxv(c);
    if (mn < 0) for (int i = 0; i < 3; i++) c[i] = l + (c[i] - l) * l / (l - mn);
    if (mx > 1) for (int i = 0; i < 3; i++) c[i] = l + (c[i] - l) * (1 - l) / (mx - l);
}
static void set_lum(const double base[3], const double lum[3], double out[3]) {
    double d = lumv(lum) - lumv(base);
    for (int i = 0; i < 3; i++) out[i] = base[i] + d;
    clip_color(out);
}
static void set_lum_sat(const double base[3], const double sat[3], const double lum[3], double out[3]) {
    double mb = minv(base), sb = maxv(base) - mb, ss = maxv(sat) - minv(sat), c[3];
    for (int i = 0; i < 3; i++) c[i] = sb > 0 ? (base[i] - mb) * ss / sb : 0;
    set_lum(c, lum, out);
}
static double f_of(unsigned m, double s, double d) {
    switch (m) {
        case NVN_BLEND_ADVANCED_MODE_ZERO: case NVN_BLEND_ADVANCED_MODE_SRC_OUT: case NVN_BLEND_ADVANCED_MODE_DST_OUT: case NVN_BLEND_ADVANCED_MODE_XOR: return 0;
        case NVN_BLEND_ADVANCED_MODE_SRC: case NVN_BLEND_ADVANCED_MODE_SRC_OVER: case NVN_BLEND_ADVANCED_MODE_SRC_IN: case NVN_BLEND_ADVANCED_MODE_SRC_ATOP: return s;
        case NVN_BLEND_ADVANCED_MODE_DST: case NVN_BLEND_ADVANCED_MODE_DST_OVER: case NVN_BLEND_ADVANCED_MODE_DST_IN: case NVN_BLEND_ADVANCED_MODE_DST_ATOP: return d;
        case NVN_BLEND_ADVANCED_MODE_MULTIPLY: return s * d;
        case NVN_BLEND_ADVANCED_MODE_SCREEN: return s + d - s * d;
        case NVN_BLEND_ADVANCED_MODE_OVERLAY: return d <= 0.5 ? 2 * s * d : 1 - 2 * (1 - s) * (1 - d);
        case NVN_BLEND_ADVANCED_MODE_DARKEN: return fmin(s, d);
        case NVN_BLEND_ADVANCED_MODE_LIGHTEN: return fmax(s, d);
        case NVN_BLEND_ADVANCED_MODE_COLORDODGE: return d <= 0 ? 0 : s < 1 ? fmin(1, d / (1 - s)) : 1;
        case NVN_BLEND_ADVANCED_MODE_COLORBURN: return d >= 1 ? 1 : s > 0 ? 1 - fmin(1, (1 - d) / s) : 0;
        case NVN_BLEND_ADVANCED_MODE_HARDLIGHT: return s <= 0.5 ? 2 * s * d : 1 - 2 * (1 - s) * (1 - d);
        case NVN_BLEND_ADVANCED_MODE_SOFTLIGHT:
            if (s <= 0.5) return d - (1 - 2 * s) * d * (1 - d);
            if (d <= 0.25) return d + (2 * s - 1) * d * ((16 * d - 12) * d + 3);
            return d + (2 * s - 1) * (sqrt(d) - d);
        case NVN_BLEND_ADVANCED_MODE_DIFFERENCE: return fabs(d - s);
        case NVN_BLEND_ADVANCED_MODE_EXCLUSION: return s + d - 2 * s * d;
        case NVN_BLEND_ADVANCED_MODE_INVERT: return 1 - d;
        case NVN_BLEND_ADVANCED_MODE_INVERT_RGB: return s * (1 - d);
        case NVN_BLEND_ADVANCED_MODE_LINEARDODGE: return s + d <= 1 ? s + d : 1;
        case NVN_BLEND_ADVANCED_MODE_LINEARBURN: return s + d > 1 ? s + d - 1 : 0;
        case NVN_BLEND_ADVANCED_MODE_VIVIDLIGHT: return s <= 0 ? 0 : s < 0.5 ? 1 - fmin(1, (1 - d) / (2 * s)) : s < 1 ? fmin(1, d / (2 * (1 - s))) : 1;
        case NVN_BLEND_ADVANCED_MODE_LINEARLIGHT: return 2 * s + d > 2 ? 1 : 2 * s + d > 1 ? 2 * s + d - 1 : 0;
        case NVN_BLEND_ADVANCED_MODE_PINLIGHT: return 2 * s - 1 > d ? (s < 0.5 ? 0 : 2 * s - 1) : (s < 0.5 * d ? 2 * s : d);
        case NVN_BLEND_ADVANCED_MODE_HARDMIX: return s + d < 1 ? 0 : 1;
        default: return 0;
    }
}
static void xyz_of(unsigned m, double *x, double *y, double *z) {
    int c = 7;   // X Y Z as bits 4 2 1
    switch (m) {
        case NVN_BLEND_ADVANCED_MODE_ZERO: c = 0; break;
        case NVN_BLEND_ADVANCED_MODE_SRC: case NVN_BLEND_ADVANCED_MODE_DST_ATOP: c = 6; break;
        case NVN_BLEND_ADVANCED_MODE_DST: case NVN_BLEND_ADVANCED_MODE_SRC_ATOP: case NVN_BLEND_ADVANCED_MODE_INVERT: case NVN_BLEND_ADVANCED_MODE_INVERT_RGB: c = 5; break;
        case NVN_BLEND_ADVANCED_MODE_SRC_IN: case NVN_BLEND_ADVANCED_MODE_DST_IN: c = 4; break;
        case NVN_BLEND_ADVANCED_MODE_SRC_OUT: c = 2; break;
        case NVN_BLEND_ADVANCED_MODE_DST_OUT: c = 1; break;
        case NVN_BLEND_ADVANCED_MODE_XOR: c = 3; break;
    }
    *x = c >> 2 & 1; *y = c >> 1 & 1; *z = c & 1;
}
// The blend of a source (src: the fragment's output, premultiplied unless npm) over a premultiplied destination dst, with overlap ovl (NVN's: 0 uncorrelated, 1 disjoint,
// 2 conjoint): the premultiplied result
static void reference(unsigned m, int npm, int ovl, const double src[4], const double dst[4], double out[4]) {
    double As = src[3], Ad = dst[3], Cs[3], cs[3], cd[3];
    for (int i = 0; i < 3; i++) {
        Cs[i] = npm ? src[i] * As : src[i];
        cs[i] = npm ? src[i] : As > 0 ? src[i] / As : 0;
        cd[i] = Ad > 0 ? dst[i] / Ad : 0;
    }
    if (m >= NVN_BLEND_ADVANCED_MODE_PLUS && m <= NVN_BLEND_ADVANCED_MODE_PLUS_DARKER) {
        double a = fmin(1, As + Ad);
        for (int i = 0; i < 3; i++) {
            double s = Cs[i] + dst[i];
            out[i] = m == NVN_BLEND_ADVANCED_MODE_PLUS ? s : m == NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED ? fmin(1, s) : m == NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED_ALPHA ? fmin(a, s)
                   : fmax(0, a - ((As - Cs[i]) + (Ad - dst[i])));
        }
        out[3] = m == NVN_BLEND_ADVANCED_MODE_PLUS ? As + Ad : a;
        return;
    }
    if (m == NVN_BLEND_ADVANCED_MODE_MINUS || m == NVN_BLEND_ADVANCED_MODE_MINUS_CLAMPED) {
        int cl = m == NVN_BLEND_ADVANCED_MODE_MINUS_CLAMPED;
        for (int i = 0; i < 3; i++) out[i] = cl ? fmax(0, dst[i] - Cs[i]) : dst[i] - Cs[i];
        out[3] = cl ? fmax(0, Ad - As) : Ad - As;
        return;
    }
    if (m == NVN_BLEND_ADVANCED_MODE_CONTRAST) { for (int i = 0; i < 3; i++) out[i] = Ad / 2 + 2 * (dst[i] - Ad / 2) * (Cs[i] - As / 2); out[3] = Ad; return; }
    if (m == NVN_BLEND_ADVANCED_MODE_INVERT_OVG) { for (int i = 0; i < 3; i++) out[i] = As * (1 - dst[i]) + (1 - As) * dst[i]; out[3] = As + Ad - As * Ad; return; }
    if (m >= NVN_BLEND_ADVANCED_MODE_RED && m <= NVN_BLEND_ADVANCED_MODE_BLUE) {
        for (int i = 0; i < 3; i++) out[i] = (unsigned)i == m - NVN_BLEND_ADVANCED_MODE_RED ? Cs[i] : dst[i];
        out[3] = Ad; return;
    }
    double p0, p1, p2;
    if (ovl == NVN_BLEND_ADVANCED_OVERLAP_CONJOINT) { p0 = fmin(As, Ad); p1 = fmax(As - Ad, 0); p2 = fmax(Ad - As, 0); }
    else if (ovl == NVN_BLEND_ADVANCED_OVERLAP_DISJOINT) { p0 = fmax(As + Ad - 1, 0); p1 = fmin(As, 1 - Ad); p2 = fmin(Ad, 1 - As); }
    else { p0 = As * Ad; p1 = As * (1 - Ad); p2 = Ad * (1 - As); }
    double x, y, z, f[3]; xyz_of(m, &x, &y, &z);
    if (m == NVN_BLEND_ADVANCED_MODE_HSL_HUE) set_lum_sat(cs, cd, cd, f);
    else if (m == NVN_BLEND_ADVANCED_MODE_HSL_SATURATION) set_lum_sat(cd, cs, cd, f);
    else if (m == NVN_BLEND_ADVANCED_MODE_HSL_COLOR) set_lum(cs, cd, f);
    else if (m == NVN_BLEND_ADVANCED_MODE_HSL_LUMINOSITY) set_lum(cd, cs, f);
    else for (int i = 0; i < 3; i++) f[i] = f_of(m, cs[i], cd[i]);
    for (int i = 0; i < 3; i++) out[i] = f[i] * p0 + y * cs[i] * p1 + z * cd[i] * p2;
    out[3] = x * p0 + y * p1 + z * p2;
}

// ---------- Drawing a case ----------
static double q8(double v) { v = v < 0 ? 0 : v > 1 ? 1 : v; return floor(v * 255 + 0.5) / 255; }   // an RGBA8 texel's value
static float half_to_float(uint16_t h) {
    int e = h >> 10 & 31, m = h & 1023; float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? INFINITY : ldexpf((float)(m | 1024), e - 25);
    return h & 0x8000 ? -v : v;
}
static uint16_t float_to_half(float f) {   // round to nearest, for the values the test clears (normal, in [0, 1])
    if (f == 0) return 0;
    int e; float m = frexpf(f, &e);   // f = m 2^e, m in [0.5, 1)
    uint32_t mant = (uint32_t)lrintf(m * 2048.0f);   // 11 bits with the leading one
    if (mant >= 2048) { mant >>= 1; e++; }
    return (uint16_t)((uint32_t)(e + 14) << 10 | (mant & 1023));
}
static double qh(double v) { return half_to_float(float_to_half((float)v)); }
// One draw with blend state b: src from the uniform buffer over a target of format fmt (RGBA8 or RGBA16F) cleared to dst; out: the texel at (0, 0) as values in [0, 1] (RGBA8)
// or the floats
static int draw_state(const blend_st *b, const double src[4], const double dst[4], int fmt, double out[4]) {
    tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, fmt, 4, 4, 1, 1, 0);
    const tex_t *rt[1] = {&target};
    for (int i = 0; i < 4; i++) g_ubo[i] = (float)src[i];
    hv_rt(&g_c, 1, rt, NULL); hv_clear_color(&g_c, 0, 15, (float)dst[0], (float)dst[1], (float)dst[2], (float)dst[3]);
    hv_bind(&g_c, &g_ucol, 0x1f); hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&g_c, NVN_FACE_NONE);
    hv_buf(&g_c, OP_BIND_UBO, 1, 0, g_ubo, 16);
    hv_state(&g_c, 0, b, sizeof *b);
    color_st cs = {1, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; hv_state(&g_c, 1, &cs, sizeof cs);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    blend_st off = {0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD, 0, 0, 0}; hv_state(&g_c, 0, &off, sizeof off);
    color_st cs0 = {0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; hv_state(&g_c, 1, &cs0, sizeof cs0);
    hv_submit(&g_q, &g_c);
    if (fmt == NVN_FORMAT_RGBA8) { uint8_t px[4]; if (!hv_read(&target, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 1, 1, 4, px)) return 0; for (int i = 0; i < 4; i++) out[i] = px[i] / 255.0; }
    else { uint16_t px[4]; if (!hv_read(&target, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 1, 1, 8, px)) return 0; for (int i = 0; i < 4; i++) out[i] = half_to_float(px[i]); }
    return 1;
}
// One draw of mode with the state's own functions ONE, ZERO (a replace), which the GPU ignores while the mode is set
static int draw_case(unsigned mode, int npm, int ovl, const double src[4], const double dst[4], int fmt, double out[4]) {
    blend_st b = {0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD, mode, (unsigned)npm, (unsigned)ovl};
    return draw_state(&b, src, dst, fmt, out);
}
static const char *mode_name(unsigned m) {
    static const char *const n[] = {[NVN_BLEND_ADVANCED_MODE_ZERO] = "ZERO", "SRC", "DST", "SRC_OVER", "DST_OVER", "SRC_IN", "DST_IN", "SRC_OUT", "DST_OUT", "SRC_ATOP", "DST_ATOP", "XOR",
        "PLUS", "PLUS_CLAMPED", "PLUS_CLAMPED_ALPHA", "PLUS_DARKER", "MULTIPLY", "SCREEN", "OVERLAY", "DARKEN", "LIGHTEN", "COLORDODGE", "COLORBURN", "HARDLIGHT", "SOFTLIGHT",
        "DIFFERENCE", "MINUS", "MINUS_CLAMPED", "EXCLUSION", "CONTRAST", "INVERT", "INVERT_RGB", "INVERT_OVG", "LINEARDODGE", "LINEARBURN", "VIVIDLIGHT", "LINEARLIGHT", "PINLIGHT",
        "HARDMIX", "RED", "GREEN", "BLUE", "HSL_HUE", "HSL_SATURATION", "HSL_COLOR", "HSL_LUMINOSITY"};
    return m < sizeof n / sizeof *n && n[m] ? n[m] : "?";
}
// The destination as the target holds it after the clear
static void dst_stored(const double dst[4], int fmt, double q[4]) { for (int i = 0; i < 4; i++) q[i] = fmt == NVN_FORMAT_RGBA8 ? q8(dst[i]) : qh(dst[i]); }
static double max_diff(const double a[4], const double b[4]) { double e = 0; for (int i = 0; i < 4; i++) e = fmax(e, fabs(a[i] - b[i])); return e; }
static void clamp01(double v[4]) { for (int i = 0; i < 4; i++) v[i] = v[i] < 0 ? 0 : v[i] > 1 ? 1 : v[i]; }
// A case checked against the reference (the result clamped as the target stores it): name, worst difference
static int check_exact(const char *what, unsigned mode, int npm, int ovl, const double src[4], const double dst[4], int fmt, double tol) {
    double got[4], dq[4], ref[4];
    if (!draw_case(mode, npm, ovl, src, dst, fmt, got)) { t("a case's texel read back", 0); return 0; }
    dst_stored(dst, fmt, dq); reference(mode, npm, ovl, src, dq, ref);
    if (fmt == NVN_FORMAT_RGBA8) clamp01(ref);
    double e = max_diff(got, ref);
    if (e > tol) {
        fprintf(stderr, "%s %s%s over (%.3f %.3f %.3f %.3f): got %.4f %.4f %.4f %.4f, reference %.4f %.4f %.4f %.4f\n", what, mode_name(mode), npm ? " (source not premultiplied)" : "",
                dq[0], dq[1], dq[2], dq[3], got[0], got[1], got[2], got[3], ref[0], ref[1], ref[2], ref[3]);
        return 0;
    }
    return 1;
}

// Three draws in one pass of one command buffer, each into its own column of a 4x4 target cleared to dst (a scissor per draw): mode, then a plain blend (ONE,
// ONE_MINUS_SRC_ALPHA, no advanced mode), then mode again. The advanced draws' own functions are the plain draw's, so where the mode is not reduced the plain draw's equation
// equals the one set before and only a correct dynamic state shadow sets it again (vk_commands.c dyn_apply). Columns 0 and 2 against the reference of mode, column 1 against
// SRC_OVER's (the plain blend); worst difference in *worst
static int alt_case(unsigned mode, const double src[4], const double dst[4], double *worst) {
    tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 4, 4, 1, 1, 0);
    const tex_t *rt[1] = {&target};
    for (int i = 0; i < 4; i++) g_ubo[i] = (float)src[i];
    hv_rt(&g_c, 1, rt, NULL); hv_scissor(&g_c, 0, 0, 4, 4); hv_clear_color(&g_c, 0, 15, (float)dst[0], (float)dst[1], (float)dst[2], (float)dst[3]);
    hv_bind(&g_c, &g_ucol, 0x1f); hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&g_c, NVN_FACE_NONE);
    hv_buf(&g_c, OP_BIND_UBO, 1, 0, g_ubo, 16);
    color_st cs = {1, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; hv_state(&g_c, 1, &cs, sizeof cs);
    for (int k = 0; k < 3; k++) {
        blend_st b = {0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD,
                      k == 1 ? 0u : mode, 0, 0};
        hv_state(&g_c, 0, &b, sizeof b); hv_scissor(&g_c, k, 0, 1, 4);
        hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    }
    hv_scissor(&g_c, 0, 0, 4, 4);
    blend_st off = {0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD, 0, 0, 0}; hv_state(&g_c, 0, &off, sizeof off);
    color_st cs0 = {0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; hv_state(&g_c, 1, &cs0, sizeof cs0);
    hv_submit(&g_q, &g_c);
    uint8_t px[16]; if (!hv_read(&target, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 4, 1, 4, px)) return 0;
    double dq[4], ref[4], plain[4]; dst_stored(dst, NVN_FORMAT_RGBA8, dq);
    reference(mode, 0, 0, src, dq, ref); clamp01(ref); reference(NVN_BLEND_ADVANCED_MODE_SRC_OVER, 0, 0, src, dq, plain); clamp01(plain);
    *worst = 0;
    for (int k = 0; k < 4; k++) {
        double got[4]; for (int i = 0; i < 4; i++) got[i] = px[4 * k + i] / 255.0;
        double e = max_diff(got, k == 1 ? plain : k == 3 ? dq : ref);
        if (e > *worst) *worst = e;
        if (e > 3.0 / 255) fprintf(stderr, "alternation %s, column %d: got %.4f %.4f %.4f %.4f\n", mode_name(mode), k, got[0], got[1], got[2], got[3]);
    }
    return 1;
}

// The device draws a mode through VK_EXT_blend_operation_advanced (vk_gpl.c adv_op), one target: its limits, and not LINEARDODGE, which keeps the state's functions (adv_kept).
// dynamic: the blend equation is dynamic (level A), which needs extendedDynamicState3ColorBlendAdvanced as well
static int ext_takes(const hostvk_dev *d, unsigned m, int npm, int ovl) {
    if (!d->x.blend_adv || d->x.blend_adv_max_att < 1 || m == NVN_BLEND_ADVANCED_MODE_LINEARDODGE) return 0;
    int basic = (m >= NVN_BLEND_ADVANCED_MODE_MULTIPLY && m <= NVN_BLEND_ADVANCED_MODE_DIFFERENCE) || m == NVN_BLEND_ADVANCED_MODE_EXCLUSION || m >= NVN_BLEND_ADVANCED_MODE_HSL_HUE;
    return (d->x.blend_adv_all || basic) && (!npm || d->x.blend_adv_npm_src) && (!ovl || d->x.blend_adv_correlated);
}

int main(void) {
    const hostvk_dev *d = hv_up("t_vk_adv_blend");
    hv_device(1);
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&g_q, &qb);
    if (!hv_pool_make(&g_p, 1 << 21)) { t("memory pool allocated", 0); return hv_finish(); }
    memset(g_p.mem, 0, 1 << 21);
    g_c = (hv_cmds){g_p.mem + 0x8000, 0, 0x8000};
    g_ubo = (float *)(void *)(g_p.mem + 0x21000);
    static const char ubo[] = "[{\"kind\":\"cbuf\",\"reg\":0,\"name\":\"C\",\"class\":\"ubo\",\"set\":0,\"binding\":76}]";
    const hv_shader su[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"ucol", 1, HV_SPV(ucol_frag_spv), ubo, NULL}};
    hv_program(&g_ucol, &g_p, 1024, su, 2);
    printf("device: VK_EXT_blend_operation_advanced %s\n", d->x.blend_adv ? "enabled (with advancedBlendCoherentOperations)" :
           d->opt_present >> HOSTVK_OPT_BLEND_ADVANCED & 1 ? "present without advancedBlendCoherentOperations (not enabled)" : "absent");

    // Sources (premultiplied: opaque, translucent, transparent) and destinations (premultiplied: opaque, translucent), as multiples of 1/255 where the target stores them
    static const double srcs[3][4] = {{0.6, 0.2, 0.4, 1.0}, {0.3, 0.1, 0.2, 0.5}, {0, 0, 0, 0}};
    static const double srcs_npm[3][4] = {{0.6, 0.2, 0.4, 1.0}, {0.6, 0.2, 0.4, 0.5}, {0.8, 0.4, 0.2, 0}};
    static const double dsts[2][4] = {{0.2, 0.8, 0.4, 1.0}, {0.1, 0.3, 0.2, 0.5}};
    const double t8 = 2.0 / 255;

    // 1. the exact reductions
    static const unsigned exact_pm[] = {NVN_BLEND_ADVANCED_MODE_ZERO, NVN_BLEND_ADVANCED_MODE_SRC, NVN_BLEND_ADVANCED_MODE_DST, NVN_BLEND_ADVANCED_MODE_SRC_OVER,
        NVN_BLEND_ADVANCED_MODE_DST_OVER, NVN_BLEND_ADVANCED_MODE_SRC_IN, NVN_BLEND_ADVANCED_MODE_DST_IN, NVN_BLEND_ADVANCED_MODE_SRC_OUT, NVN_BLEND_ADVANCED_MODE_DST_OUT,
        NVN_BLEND_ADVANCED_MODE_SRC_ATOP, NVN_BLEND_ADVANCED_MODE_DST_ATOP, NVN_BLEND_ADVANCED_MODE_XOR, NVN_BLEND_ADVANCED_MODE_PLUS, NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED,
        NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED_ALPHA,
        // new
        NVN_BLEND_ADVANCED_MODE_SCREEN, NVN_BLEND_ADVANCED_MODE_EXCLUSION, NVN_BLEND_ADVANCED_MODE_MINUS, NVN_BLEND_ADVANCED_MODE_MINUS_CLAMPED, NVN_BLEND_ADVANCED_MODE_RED,
        NVN_BLEND_ADVANCED_MODE_GREEN, NVN_BLEND_ADVANCED_MODE_BLUE};
    static const unsigned exact_npm[] = {NVN_BLEND_ADVANCED_MODE_ZERO, NVN_BLEND_ADVANCED_MODE_SRC, NVN_BLEND_ADVANCED_MODE_DST, NVN_BLEND_ADVANCED_MODE_SRC_OVER,
        NVN_BLEND_ADVANCED_MODE_DST_IN, NVN_BLEND_ADVANCED_MODE_DST_OUT, NVN_BLEND_ADVANCED_MODE_PLUS, NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED, NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED_ALPHA,
        NVN_BLEND_ADVANCED_MODE_MINUS, NVN_BLEND_ADVANCED_MODE_MINUS_CLAMPED, NVN_BLEND_ADVANCED_MODE_RED, NVN_BLEND_ADVANCED_MODE_GREEN, NVN_BLEND_ADVANCED_MODE_BLUE};
    for (int lv = 0; lv < 2; lv++) {
        if (lv) hvk_pso_test_level('D');
        const char *lvn = lv ? "level D (blend baked)" : "level A (blend dynamic)";
        for (size_t k = 0; k < sizeof exact_pm / sizeof *exact_pm; k++) {
            unsigned m = exact_pm[k]; int ok = 1;
            for (int s = 0; s < 3; s++) for (int q = 0; q < 2; q++) ok &= check_exact(lvn, m, 0, 0, srcs[s], dsts[q], NVN_FORMAT_RGBA8, t8);
            char name[200]; snprintf(name, sizeof name, "%s: %s, premultiplied source, exact on RGBA8 (3 sources x 2 destinations)%s", lvn, mode_name(m), k >= 15 ? " (new)" : "");
            t(name, ok);
        }
        for (size_t k = 0; k < sizeof exact_npm / sizeof *exact_npm; k++) {
            unsigned m = exact_npm[k]; int ok = 1;
            for (int s = 0; s < 3; s++) for (int q = 0; q < 2; q++) ok &= check_exact(lvn, m, 1, 0, srcs_npm[s], dsts[q], NVN_FORMAT_RGBA8, t8);
            char name[200]; snprintf(name, sizeof name, "%s: %s, source not premultiplied, exact on RGBA8", lvn, mode_name(m));
            t(name, ok);
        }
        if (lv) hvk_pso_test_level('A');
    }

    // 2. the conditional reductions: exact under the condition; elsewhere the fixed-function blend with the stated error. On a device whose pipelines bake the blend and that has the
    //    extension, the extension draws them exactly instead
    int ext_here = d->x.blend_adv && (!(d->eds_present >> HOSTVK_EDS_BLEND_EQUATION & 1) || d->x.blend_adv_dyn);
    {
        struct { unsigned m; const char *cond; double src[4], dst[4]; int inside; } c[] = {
            {NVN_BLEND_ADVANCED_MODE_MULTIPLY, "over an opaque destination, translucent source", {0.3, 0.1, 0.2, 0.5}, {0.2, 0.8, 0.4, 1.0}, 1},
            {NVN_BLEND_ADVANCED_MODE_MULTIPLY, "over a translucent destination", {0.6, 0.2, 0.4, 1.0}, {0.1, 0.3, 0.2, 0.5}, 0},
            {NVN_BLEND_ADVANCED_MODE_INVERT_RGB, "over an opaque destination, translucent source", {0.3, 0.1, 0.2, 0.5}, {0.2, 0.8, 0.4, 1.0}, 1},
            {NVN_BLEND_ADVANCED_MODE_INVERT_RGB, "over a translucent destination", {0.6, 0.2, 0.4, 1.0}, {0.1, 0.3, 0.2, 0.5}, 0},
            {NVN_BLEND_ADVANCED_MODE_LIGHTEN, "source and destination opaque", {0.6, 0.2, 0.4, 1.0}, {0.2, 0.8, 0.4, 1.0}, 1},
            {NVN_BLEND_ADVANCED_MODE_LIGHTEN, "transparent source", {0, 0, 0, 0}, {0.1, 0.3, 0.2, 0.5}, 1},
            {NVN_BLEND_ADVANCED_MODE_LIGHTEN, "translucent source over an opaque destination", {0.4, 0.35, 0.05, 0.5}, {0.6, 0.6, 0.6, 1.0}, 0},
        };
        for (size_t k = 0; k < sizeof c / sizeof *c; k++) {
            double got[4], dq[4], ref[4], fx[4];
            int ok = draw_case(c[k].m, 0, 0, c[k].src, c[k].dst, NVN_FORMAT_RGBA8, got);
            dst_stored(c[k].dst, NVN_FORMAT_RGBA8, dq); reference(c[k].m, 0, 0, c[k].src, dq, ref); clamp01(ref);
            // the fixed-function blend of the table (vk_gpl.c k_red), clamped as RGBA8 stores it
            const double *S = c[k].src, *D = dq; double As = S[3], Ad = D[3];
            for (int i = 0; i < 3; i++) {
                switch (c[k].m) {
                    case NVN_BLEND_ADVANCED_MODE_MULTIPLY: fx[i] = S[i] * D[i] + D[i] * (1 - As); break;
                    case NVN_BLEND_ADVANCED_MODE_INVERT_RGB: fx[i] = S[i] * (1 - D[i]) + D[i] * (1 - As); break;
                    default: fx[i] = fmax(S[i], D[i]); break;
                }
            }
            fx[3] = c[k].m == NVN_BLEND_ADVANCED_MODE_INVERT_RGB ? Ad : As + Ad * (1 - As);
            clamp01(fx);
            int ext = ext_here && ext_takes(d, c[k].m, 0, 0);
            double e_ref = max_diff(got, ref), e_fx = max_diff(got, fx), stated = max_diff(fx, ref);
            char name[300];
            if (c[k].inside || ext) {
                ok = ok && e_ref <= t8;
                snprintf(name, sizeof name, "%s %s: exact (%s; worst %.4f)", mode_name(c[k].m), c[k].cond, ext ? "through VK_EXT_blend_operation_advanced" : "fixed-function", e_ref);
            } else {
                // the pixel is the fixed-function blend's, off the reference by the amount the table states, in the stated direction (INVERT_RGB brighter, MULTIPLY and
                // LIGHTEN darker), and that amount is not zero
                int brighter = c[k].m == NVN_BLEND_ADVANCED_MODE_INVERT_RGB, dir = 1;
                for (int i = 0; i < 3; i++) if (fabs(fx[i] - ref[i]) > t8) dir &= brighter ? fx[i] > ref[i] : fx[i] < ref[i];
                double bound = c[k].m == NVN_BLEND_ADVANCED_MODE_LIGHTEN ? 0.25 : S[0] * (1 - Ad) + 1e-9;
                if (c[k].m == NVN_BLEND_ADVANCED_MODE_MULTIPLY || c[k].m == NVN_BLEND_ADVANCED_MODE_INVERT_RGB) { double b2 = 0; for (int i = 0; i < 3; i++) b2 = fmax(b2, S[i] * (1 - Ad)); bound = b2 + 1e-9; }
                ok = ok && e_fx <= t8 && stated > 4 * t8 && stated <= bound && dir;
                snprintf(name, sizeof name, "%s %s: the fixed-function blend (off by %.4f), %.4f %s than the reference as stated (bound %.4f)", mode_name(c[k].m), c[k].cond, e_fx, stated,
                         brighter ? "brighter" : "darker", bound);
            }
            if (!ok) fprintf(stderr, "%s: got %.4f %.4f %.4f %.4f, fixed %.4f %.4f %.4f %.4f, reference %.4f %.4f %.4f %.4f\n", mode_name(c[k].m), got[0], got[1], got[2], got[3],
                             fx[0], fx[1], fx[2], fx[3], ref[0], ref[1], ref[2], ref[3]);
            t(name, ok);
        }
    }
    // LINEARDODGE keeps the state's own functions on every path (vk_gpl.c adv_kept; open): Mario vs. Donkey Kong's fire bird (premultiplied source, uncorrelated, state functions
    // SRC_ALPHA, ONE for colour and ONE, ZERO for alpha) writes a negative colour into a float target. With the state's functions it is scaled by As: Cs As + Cd; ONE, ONE (the
    // reduction) and the mode's equation (the extension) give Cs + Cd where unsaturated, so the checks tell them apart. On RGBA8 the state's ONE, ZERO is a replace, the source as
    // it is, premultiplied or not (the reduction gave Cs + Cd, or Cs As + Cd for a source that is not premultiplied)
    for (int lv = 0; lv < 2; lv++) {
        if (lv) hvk_pso_test_level('D');
        const char *lvn = lv ? "level D (blend baked)" : "level A (blend dynamic)";
        static const double neg[4] = {-0.3, 0.2, -0.1, 0.5}, over[4] = {0.1, 0.3, 0.2, 1.0};
        const double th = 1.0 / 512;
        blend_st b = {0, NVN_BLEND_FUNC_SRC_ALPHA, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD,
                      NVN_BLEND_ADVANCED_MODE_LINEARDODGE, 0, 0};
        double got[4], dq[4], want[4];
        int ok = draw_state(&b, neg, over, NVN_FORMAT_RGBA16F, got);
        dst_stored(over, NVN_FORMAT_RGBA16F, dq);
        for (int i = 0; i < 3; i++) want[i] = neg[i] * neg[3] + dq[i];
        want[3] = neg[3];
        ok = ok && max_diff(got, want) <= th;
        if (!ok) fprintf(stderr, "%s LINEARDODGE, negative source on RGBA16F: got %.4f %.4f %.4f %.4f, the state's SRC_ALPHA, ONE / ONE, ZERO give %.4f %.4f %.4f %.4f\n", lvn,
                         got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
        char name[300];
        snprintf(name, sizeof name, "%s: LINEARDODGE, negative source (%.1f %.1f %.1f, alpha %.1f) on RGBA16F: the state's SRC_ALPHA, ONE scale it by As (%.4f %.4f %.4f), not added "
                 "in full", lvn, neg[0], neg[1], neg[2], neg[3], want[0], want[1], want[2]);
        t(name, ok);
        for (int npm = 0; npm < 2; npm++) {
            const double *s = npm ? srcs_npm[1] : srcs[1]; double q[4];
            ok = draw_case(NVN_BLEND_ADVANCED_MODE_LINEARDODGE, npm, 0, s, dsts[1], NVN_FORMAT_RGBA8, got);
            for (int i = 0; i < 4; i++) q[i] = q8(s[i]);
            ok = ok && max_diff(got, q) <= t8;
            if (!ok) fprintf(stderr, "%s LINEARDODGE on RGBA8 with ONE, ZERO: got %.4f %.4f %.4f %.4f, the source %.4f %.4f %.4f %.4f\n", lvn, got[0], got[1], got[2], got[3], q[0], q[1], q[2], q[3]);
            snprintf(name, sizeof name, "%s: LINEARDODGE, %spremultiplied source, on RGBA8: the state's ONE, ZERO (a replace)", lvn, npm ? "not " : "");
            t(name, ok);
        }
        if (lv) hvk_pso_test_level('A');
    }
    // The clamped modes on a float target: exact while unclamped; where the mode clamps, the fixed-function blend (which does not on a float target) passes 1 or falls below 0
    {
        const double th = 1.0 / 512;
        t("RGBA16F: SCREEN exact", check_exact("RGBA16F", NVN_BLEND_ADVANCED_MODE_SCREEN, 0, 0, srcs[1], dsts[1], NVN_FORMAT_RGBA16F, th));
        t("RGBA16F: PLUS_CLAMPED exact while no sum passes 1", check_exact("RGBA16F", NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED, 0, 0, srcs[1], dsts[1], NVN_FORMAT_RGBA16F, th));
        t("RGBA16F: MINUS exact (negative results kept)", check_exact("RGBA16F", NVN_BLEND_ADVANCED_MODE_MINUS, 0, 0, srcs[0], dsts[1], NVN_FORMAT_RGBA16F, th));
        double got[4];
        int ext = ext_here && ext_takes(d, NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED, 0, 0);
        int ok = draw_case(NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED, 0, 0, srcs[0], dsts[0], NVN_FORMAT_RGBA16F, got);   // 0.6 + 0.2, 0.2 + 0.8, 0.4 + 0.4, 1 + 1
        ok = ok && (ext ? fabs(got[3] - 1) < th : fabs(got[3] - 2) < th) && fabs(got[1] - 1) < th;
        if (!ok) fprintf(stderr, "PLUS_CLAMPED on RGBA16F: %.4f %.4f %.4f %.4f\n", got[0], got[1], got[2], got[3]);
        t(ext ? "RGBA16F: PLUS_CLAMPED over 1 clamped through the extension" : "RGBA16F: PLUS_CLAMPED over 1 is not clamped by the fixed-function blend (alpha 2 where the mode gives 1, as stated)", ok);
        ext = ext_here && ext_takes(d, NVN_BLEND_ADVANCED_MODE_MINUS_CLAMPED, 0, 0);
        ok = draw_case(NVN_BLEND_ADVANCED_MODE_MINUS_CLAMPED, 0, 0, srcs[0], dsts[1], NVN_FORMAT_RGBA16F, got);   // 0.1 - 0.6, ..., 0.5 - 1
        ok = ok && (ext ? fabs(got[0]) < th && fabs(got[3]) < th : fabs(got[0] + 0.5) < th && fabs(got[3] + 0.5) < th);
        if (!ok) fprintf(stderr, "MINUS_CLAMPED on RGBA16F: %.4f %.4f %.4f %.4f\n", got[0], got[1], got[2], got[3]);
        t(ext ? "RGBA16F: MINUS_CLAMPED below 0 clamped through the extension" : "RGBA16F: MINUS_CLAMPED below 0 is not clamped by the fixed-function blend (-0.5 where the mode gives 0, as stated)", ok);
    }

    // An advanced draw, a plain one and an advanced one again in one pass (the dynamic state shadow): MULTIPLY over an opaque destination, which every device draws exactly
    // (reduced, or through the extension); on a device with the extension also a mode no fixed-function blend gives (OVERLAY)
    {
        static const double src[4] = {0.3, 0.1, 0.2, 0.5}, dst[4] = {0.2, 0.8, 0.4, 1.0};
        double w = 1; int ok = alt_case(NVN_BLEND_ADVANCED_MODE_MULTIPLY, src, dst, &w);
        char name[200]; snprintf(name, sizeof name, "MULTIPLY, plain SRC_OVER, MULTIPLY in one pass: each column as its blend gives it (worst %.4f)", w);
        t(name, ok && w <= 3.0 / 255);
    }

    // The conditional reductions on a target without alpha (vk_gpl.c fmt_no_alpha, R_OPAQUE): blending reads its destination alpha as 1, so MULTIPLY and INVERT_RGB, exact
    // over an opaque destination, are exact for every input there (Mario vs. Donkey Kong draws MULTIPLY into its R11G11B10F scene, where the red line called it approximate:
    // these checks fail on that code); with alpha they stay conditional, and LIGHTEN, which also needs an opaque source, stays conditional everywhere
    {
        static const struct { unsigned m; VkFormat f; int want; const char *what; } c[] = {
            {NVN_BLEND_ADVANCED_MODE_MULTIPLY, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 1, "MULTIPLY on R11G11B10F: exact"},
            {NVN_BLEND_ADVANCED_MODE_MULTIPLY, VK_FORMAT_R16G16_SFLOAT, 1, "MULTIPLY on RG16F: exact"},
            {NVN_BLEND_ADVANCED_MODE_INVERT_RGB, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 1, "INVERT_RGB on R11G11B10F: exact"},
            {NVN_BLEND_ADVANCED_MODE_MULTIPLY, VK_FORMAT_R8G8B8A8_UNORM, 0, "MULTIPLY on RGBA8: exact only over an opaque destination"},
            {NVN_BLEND_ADVANCED_MODE_MULTIPLY, VK_FORMAT_R16G16B16A16_SFLOAT, 0, "MULTIPLY on RGBA16F: exact only over an opaque destination"},
            {NVN_BLEND_ADVANCED_MODE_INVERT_RGB, VK_FORMAT_R8G8B8A8_UNORM, 0, "INVERT_RGB on RGBA8: exact only over an opaque destination"},
            {NVN_BLEND_ADVANCED_MODE_LIGHTEN, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 0, "LIGHTEN on R11G11B10F: still conditional (needs an opaque source too)"},
            {NVN_BLEND_ADVANCED_MODE_SCREEN, VK_FORMAT_R8G8B8A8_UNORM, 1, "SCREEN on RGBA8: exact (unchanged)"},
        };
        for (size_t k = 0; k < sizeof c / sizeof *c; k++) {
            int got = hvk_adv_exact_on(c[k].m, c[k].f);
            if (got != c[k].want) fprintf(stderr, "%s: hvk_adv_exact_on gave %d\n", c[k].what, got);
            t(c[k].what, got == c[k].want);
        }
    }

    // 3. VK_EXT_blend_operation_advanced: every mode the device's limits allow, against the reference, on level A (the advanced state dynamic, vkCmdSetColorBlendAdvancedEXT)
    //    where the device has extendedDynamicState3ColorBlendAdvanced, and with the blend baked into the pipeline (levels B with pipeline libraries, and D)
    if (!d->x.blend_adv) {
        printf("note: the VK_EXT_blend_operation_advanced part is skipped: %s\n", d->opt_present >> HOSTVK_OPT_BLEND_ADVANCED & 1 ?
               "the device has the extension without advancedBlendCoherentOperations, which the backend needs (it places no barriers between overlapping advanced blends)" :
               "the device has not the extension (lavapipe has not)");
    } else {
        const char levels[3] = {'A', 'B', 'D'};
        for (int li = 0; li < 3; li++) {
            char lv = levels[li];
            if (lv == 'A' && !(d->x.blend_adv_dyn && (d->eds_present >> HOSTVK_EDS_BLEND_EQUATION & 1))) {
                printf("note: level A of the extension part is skipped: the blend equation is not dynamic here, or the device has not extendedDynamicState3ColorBlendAdvanced with "
                       "advancedBlendMaxColorAttachments of 8\n");
                continue;
            }
            if (lv == 'B' && !d->caps.gpl) { printf("note: level B of the extension part is skipped: no pipeline libraries\n"); continue; }
            if (lv != 'A') hvk_pso_test_level(lv);
            int n = 0, failed = 0;
            for (unsigned m = NVN_BLEND_ADVANCED_MODE_ZERO; m <= NVN_BLEND_ADVANCED_MODE_HSL_LUMINOSITY; m++)
                for (int npm = 0; npm < 2; npm++) for (int ovl = 0; ovl < 3; ovl++) {
                    if (!ext_takes(d, m, npm, ovl)) continue;
                    int ok = 1;
                    for (int s = 0; s < 3; s++) for (int q = 0; q < 2; q++) {
                        const double *sv = npm ? srcs_npm[s] : srcs[s]; double dq[4], ref[4];
                        dst_stored(dsts[q], NVN_FORMAT_RGBA8, dq); reference(m, npm, ovl, sv, dq, ref);
                        if (ref[3] < 0) continue;   // MINUS with As > Ad: a negative alpha is ill-formed, the result undefined (the specification's "Advanced Blend Operations")
                        ok &= check_exact("extension", m, npm, ovl, sv, dsts[q], NVN_FORMAT_RGBA8, 3.0 / 255);
                    }
                    n++; failed += !ok;
                    if (!ok) { char name[160]; snprintf(name, sizeof name, "extension, level %c: %s, %spremultiplied source, overlap %d, against the reference", lv, mode_name(m), npm ? "not " : "", ovl); t(name, 0); }
                }
            char name[200]; snprintf(name, sizeof name, "extension, level %c: %d cases (modes x source kinds x overlaps the device allows) against the reference, %d off by more than 3/255", lv, n, failed);
            t(name, failed == 0 && n > 0);
            // advanced, plain, advanced in one pass through the extension
            unsigned alt[1] = {NVN_BLEND_ADVANCED_MODE_OVERLAY};
            for (int a = 0; a < 1; a++) {
                if (!ext_takes(d, alt[a], 0, 0)) continue;
                static const double src[4] = {0.4, 0.35, 0.25, 0.5}, dst[4] = {0.6, 0.6, 0.6, 1.0};
                double w = 1; int ok = alt_case(alt[a], src, dst, &w);
                snprintf(name, sizeof name, "extension, level %c: %s, plain SRC_OVER, %s in one pass against the reference (worst %.4f)", lv, mode_name(alt[a]), mode_name(alt[a]), w);
                t(name, ok && w <= 3.0 / 255);
            }
            if (lv != 'A') hvk_pso_test_level('A');
        }
    }
    hv_pool_free(&g_p);
    return hv_finish();
}
