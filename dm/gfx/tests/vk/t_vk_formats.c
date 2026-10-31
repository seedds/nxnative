// Every row of the NVN → Vulkan format table that has a path (dm/gfx/tex/nvn_fmt_vk.c; docs/design/vulkan-backend.md 5.1, step P7), on the device: a texel pattern in the NVN
// format (4 x 2 texels, 8 x 8 for the BC formats) is put in a packaged texture's storage and uploaded, then
//   raw: the image read back holds the pattern as the row's path says: the guest's bytes for the row's own format and for a substitute read through a mapping, each texel expanded
//        to four components (alpha one) or converted to the floats it reads as where the path converts; bit for bit;
//   sampled: a fragment shader fetches every texel (texelFetch into RGBA32F, RGBA32UI or RGBA32I by the format's class) and gets what NVN gives for it, decoded here from the
//        pattern by the format's name (unorm, snorm, float, integer, sRGB, packed fields in the order the table assumes, integers read as float, alpha one where the format has
//        none): integers exactly, floats within 2^-20 (sRGB within half an 8-bit unit: lavapipe's sRGB decode is an approximation, up to 0.3 of a unit off the formula);
//   drawn: where the device can render the image's format and the row holds its own format, the fetched texels are drawn into a texture of the same NVN format, which must then
//        hold the pattern bit for bit (the write path); rows held in a substitute read through a mapping or converted at upload are declared by the backend (a red line).
// Two passes: the formats the device has, then every row that may need a device feature forced onto its substitute (alt, conv: the paths a device without the format takes).
// The depth and stencil rows are t_vk_depth24's, the ASTC rows t_vk_astc's. Prints, per row, the VkFormat the image got and which checks ran.
// Checks: raw, sampled and drawn per row and pass where they apply; the validation check
#include "t_vk_harness.h"
#include "shaders/gen/fullscreen_vert.h"
#include "shaders/gen/fetch_frag.h"
#include "shaders/gen/fetch_u_frag.h"
#include "shaders/gen/fetch_i_frag.h"
#include "nvn_fmt_vk.h"
#include <math.h>

#define FETCH_JSON(cls) "[{\"kind\":\"tex\",\"reg\":0,\"name\":\"t\",\"class\":\"combined\",\"set\":2,\"binding\":2048,\"dim\":\"2d\",\"sampled\":\"" cls "\"}," \
                        "{\"kind\":\"samp_tex\",\"reg\":0,\"name\":\"t\",\"class\":\"combined\",\"set\":2,\"binding\":2048,\"dim\":\"2d\",\"sampled\":\"" cls "\"}]"
static hv_pool g_p; static size_t g_off; static hv_cmds g_c; static NVNqueue g_q;
static NVNtexturePool g_tp; static NVNsamplerPool g_sp; static NVNprogram g_prog[3]; static int g_id = 1;
static uint64_t g_rng = 0x1234567;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)g_rng; }

// ---------- What NVN gives for a texel, from the format's name and the table's packing ----------
enum { UN, SN, UI, SI, FL };
typedef struct { int n, size, type, bgr, x, srgb; } plain_t;
static int plain_of(int f, plain_t *p) {
    if (!((f >= 0x01 && f <= 0x30) || (f >= 0x6D && f <= 0x78) || f == 0x37 || f == 0x38 || (f >= 0x99 && f <= 0x9C))) return 0;
    const char *s = nvn_fmt_name(f);
    memset(p, 0, sizeof *p);
    if (!strncmp(s, "BGRA", 4) || !strncmp(s, "BGRX", 4)) { p->n = 4; p->bgr = 1; p->x = s[3] == 'X'; s += 4; }
    else if (!strncmp(s, "RGBA", 4) || !strncmp(s, "RGBX", 4)) { p->n = 4; p->x = s[3] == 'X'; s += 4; }
    else if (!strncmp(s, "RGB", 3)) { p->n = 3; s += 3; }
    else if (!strncmp(s, "RG", 2)) { p->n = 2; s += 2; }
    else { p->n = 1; s += 1; }
    p->size = atoi(s); while (*s >= '0' && *s <= '9') s++;
    p->type = !strcmp(s, "F") ? FL : !strcmp(s, "SN") ? SN : !strcmp(s, "UI") ? UI : !strcmp(s, "I") ? SI : UN;
    p->srgb = !strcmp(s, "_SRGB");
    return 1;
}
static double half(uint32_t h) {
    int e = (int)(h >> 10 & 31), m = (int)(h & 1023); double v = e ? ldexp(1 + m / 1024.0, e - 15) : ldexp(m / 1024.0, -14);
    return h & 0x8000 ? -v : v;
}
static double small_float(uint32_t v, int mbits) { int e = (int)(v >> mbits), m = (int)(v & ((1u << mbits) - 1)); return e ? ldexp(1 + m / (double)(1 << mbits), e - 15) : ldexp(m / (double)(1 << mbits), -14); }
static double srgb_lin(double c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
static int32_t sx(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }
static uint32_t field(const uint8_t *p, int off, int bits) { uint32_t w = 0; memcpy(&w, p, 4); return w >> off & (bits == 32 ? 0xffffffffu : (1u << bits) - 1); }
// v: what a read of texel p of NVN format f gives (floats as doubles, integers exactly); *cls NVN_VK_FLOAT / UINT / SINT; *srgb the colour channels are sRGB-decoded. 0: not a format
// this test decodes (BC: raw only)
static int nvn_read(int f, const uint8_t *p, double v[4], int *cls, int *srgb) {
    plain_t pl; *srgb = 0; *cls = NVN_VK_FLOAT; v[0] = v[1] = v[2] = 0; v[3] = 1;
    if (plain_of(f, &pl)) {
        int bytes = pl.size / 8;
        *cls = pl.type == UI ? NVN_VK_UINT : pl.type == SI ? NVN_VK_SINT : NVN_VK_FLOAT; *srgb = pl.srgb;
        for (int i = 0; i < pl.n; i++) {
            uint32_t u = 0; memcpy(&u, p + i * bytes, (size_t)bytes);
            double c;
            switch (pl.type) {
                case UN: c = u / (double)((1ull << pl.size) - 1); if (pl.srgb && i < 3) c = srgb_lin(c); break;
                case SN: c = fmax(sx(u, pl.size) / (double)((1u << (pl.size - 1)) - 1), -1); break;
                case UI: c = u; break;
                case SI: c = sx(u, pl.size); break;
                default: if (pl.size == 16) c = half(u); else { float fl; memcpy(&fl, &u, 4); c = fl; } break;
            }
            v[pl.bgr && i < 3 ? 2 - i : i] = c;
        }
        if (pl.x) v[3] = 1;
        return 1;
    }
    if (f >= 0x51 && f <= 0x68) {   // integers read as float
        int comps = (f - 0x51) / 6 + 1, k = (f - 0x51) % 6, size = k < 2 ? 1 : k < 4 ? 2 : 4, sgn = k & 1;
        for (int i = 0; i < comps; i++) { uint32_t u = 0; memcpy(&u, p + i * size, (size_t)size); v[i] = sgn ? (double)(float)sx(u, 8 * size) : (double)(float)u; }
        return 1;
    }
    uint32_t r, g, b, a = 1;
    switch (f) {
        case 0x39: v[0] = field(p, 0, 4) / 15.0; v[1] = field(p, 4, 4) / 15.0; v[2] = field(p, 8, 4) / 15.0; v[3] = field(p, 12, 4) / 15.0; return 1;
        case 0x3A: case 0x3B: v[0] = field(p, 0, 5) / 31.0; v[1] = field(p, 5, 5) / 31.0; v[2] = field(p, 10, 5) / 31.0; v[3] = f == 0x3B ? field(p, 15, 1) : 1; return 1;
        case 0x3C: v[0] = field(p, 0, 5) / 31.0; v[1] = field(p, 5, 6) / 63.0; v[2] = field(p, 11, 5) / 31.0; return 1;
        case 0x95: v[2] = field(p, 0, 5) / 31.0; v[1] = field(p, 5, 6) / 63.0; v[0] = field(p, 11, 5) / 31.0; return 1;
        case 0x96: case 0x97: v[2] = field(p, 0, 5) / 31.0; v[1] = field(p, 5, 5) / 31.0; v[0] = field(p, 10, 5) / 31.0; v[3] = f == 0x97 ? field(p, 15, 1) : 1; return 1;
        case 0x98: v[3] = field(p, 0, 1); v[2] = field(p, 1, 5) / 31.0; v[1] = field(p, 6, 5) / 31.0; v[0] = field(p, 11, 5) / 31.0; return 1;
        case 0x3D: case 0x3E: case 0x69: case 0x6A: case 0x6B: case 0x6C:
            r = field(p, 0, 10); g = field(p, 10, 10); b = field(p, 20, 10); a = field(p, 30, 2);
            if (f == 0x3D) { v[0] = r / 1023.0; v[1] = g / 1023.0; v[2] = b / 1023.0; v[3] = a / 3.0; }
            else if (f == 0x3E) { *cls = NVN_VK_UINT; v[0] = r; v[1] = g; v[2] = b; v[3] = a; }
            else if (f == 0x69) { v[0] = fmax(sx(r, 10) / 511.0, -1); v[1] = fmax(sx(g, 10) / 511.0, -1); v[2] = fmax(sx(b, 10) / 511.0, -1); v[3] = fmax(sx(a, 2), -1); }
            else if (f == 0x6A) { *cls = NVN_VK_SINT; v[0] = sx(r, 10); v[1] = sx(g, 10); v[2] = sx(b, 10); v[3] = sx(a, 2); }
            else if (f == 0x6B) { v[0] = r; v[1] = g; v[2] = b; v[3] = a; }
            else { v[0] = sx(r, 10); v[1] = sx(g, 10); v[2] = sx(b, 10); v[3] = sx(a, 2); }
            return 1;
        case 0x3F: v[0] = small_float(field(p, 0, 11), 6); v[1] = small_float(field(p, 11, 11), 6); v[2] = small_float(field(p, 22, 10), 5); return 1;
        case 0x40: { int e = (int)field(p, 27, 5) - 15 - 9; v[0] = ldexp(field(p, 0, 9), e); v[1] = ldexp(field(p, 9, 9), e); v[2] = ldexp(field(p, 18, 9), e); return 1; }
        default: return 0;
    }
}
// A texel pattern for NVN format f (bpb bytes a texel) whose values every path keeps exactly: no snorm minimum (two encodings of -1), no float NaN, infinity or denormal
static void pattern(int f, uint8_t *p, int bpb) {
    for (int i = 0; i < bpb; i++) p[i] = (uint8_t)rnd();
    plain_t pl;
    if (plain_of(f, &pl)) {
        int bytes = pl.size / 8;
        static const uint16_t h[8] = {0x0000, 0x3C00, 0xBC00, 0x3800, 0x4000, 0x5640, 0x3555, 0xC500};
        static const float fl[8] = {0, 1, -1, 0.5f, 2.25f, -3.75f, 1000.5f, 0.001f};
        for (int i = 0; i < pl.n; i++) {
            uint8_t *c = p + i * bytes;
            if (pl.type == FL) { if (pl.size == 16) memcpy(c, &h[rnd() % 8], 2); else memcpy(c, &fl[rnd() % 8], 4); }
            if (pl.type == SN) { uint32_t u = 0; memcpy(&u, c, (size_t)bytes); if (u == 1u << (pl.size - 1)) { u++; memcpy(c, &u, (size_t)bytes); } }
        }
        return;
    }
    uint32_t w; memcpy(&w, p, 4);
    if (f == 0x69) { for (int k = 0; k < 3; k++) if ((w >> (10 * k) & 0x3FF) == 0x200) w ^= 1u << (10 * k); if ((w >> 30) == 2) w ^= 1u << 30; }
    if (f == 0x3F) {   // exponents 1..30
        uint32_t e0 = 1 + rnd() % 30, e1 = 1 + rnd() % 30, e2 = 1 + rnd() % 30;
        w = (w & ~(0x1Fu << 6 | 0x1Fu << 17 | 0x1Fu << 27)) | e0 << 6 | e1 << 17 | e2 << 27;
    }
    if (bpb == 4) memcpy(p, &w, 4);
}
static int bc_format(int f) { return f >= 0x41 && f <= 0x50; }
static int skip_format(int f) { return (f >= 0x31 && f <= 0x36) || (f >= 0x79 && f <= 0x94); }   // t_vk_depth24's and t_vk_astc's
static int vk_texel_bytes(VkFormat f) {   // the sizes of the formats the backend may give these rows
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R32_SFLOAT: return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R32G32_SFLOAT: return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return 16;
        default: return 0;
    }
}
// What the image of a row whose texels the upload expands or converts holds for texel p: written again here from the table's paths
static int raw_expected(int f, const nvn_vk_fmt *row, VkFormat img, const uint8_t *p, uint8_t *out) {
    int bpb = nvn_fmt_bpb(f, NULL, NULL);
    if (img == (VkFormat)row->vk || (img == (VkFormat)row->alt && !(row->flags & NVN_VK_EXPAND3))) { memcpy(out, p, (size_t)bpb); return bpb; }
    if (row->flags & NVN_VK_EXPAND3) {   // alpha one in the four-component format of the same kind
        int cb = bpb / 3; memcpy(out, p, (size_t)bpb);
        uint8_t *a = out + bpb; memset(a, 0, (size_t)cb);
        if (row->cls != NVN_VK_FLOAT) a[0] = 1;
        else if (img == VK_FORMAT_R32G32B32A32_SFLOAT) { float one = 1; memcpy(a, &one, 4); }
        else if (img == VK_FORMAT_R16G16B16A16_SFLOAT) { uint16_t one = 0x3c00; memcpy(a, &one, 2); }
        else if (img == VK_FORMAT_R8G8B8A8_SNORM) a[0] = 0x7f;
        else if (img == VK_FORMAT_R16G16B16A16_SNORM) { uint16_t one = 0x7fff; memcpy(a, &one, 2); }
        else memset(a, 0xff, (size_t)cb);
        return bpb + cb;
    }
    if (img == (VkFormat)row->conv) {   // converted: the values a read gives, as floats (or 16-bit integers for RGB10A2I)
        double v[4]; int cls, srgb; nvn_read(f, p, v, &cls, &srgb);
        int n = vk_texel_bytes(img);
        if (img == VK_FORMAT_R16G16B16A16_SINT) { int16_t o[4]; for (int i = 0; i < 4; i++) o[i] = (int16_t)v[i]; memcpy(out, o, 8); return 8; }
        float o[4]; for (int i = 0; i < 4; i++) o[i] = (float)v[i];
        memcpy(out, o, (size_t)n); return n;
    }
    return 0;
}

static void run_format(const hostvk_dev *dev, int f, int forced) {
    nvn_vk_fmt row; nvn_fmt_to_vk(f, &row);
    char name[300]; const char *pass = forced ? "substitute" : "device";
    int bw, bh, bpb = nvn_fmt_bpb(f, &bw, &bh), bc = bc_format(f);
    uint32_t W = bc ? 8 : 4, H = bc ? 8 : 2, n = W * H / (uint32_t)(bw * bh);
    uint8_t guest[64 * 16];
    for (uint32_t i = 0; i < n; i++) pattern(f, guest + (size_t)i * (size_t)bpb, bpb);
    NVNtexture tex; hv_texture(&tex, &g_p, &g_off, NVN_TEXTURE_TARGET_2D, f, W, H, 1, 1, 0, 0, 0);
    if (!hv_tex_fill(&tex, 0, 0, guest, (size_t)(W / (uint32_t)bw) * (size_t)bpb)) { t("texture storage filled", 0); return; }
    VkFormat img = 0; VkImageAspectFlags asp;
    if (!hvk_test_image((const tex_t *)(const void *)&tex, &img, &asp)) {
        snprintf(name, sizeof name, "%s (%s pass): the device has a format for it", nvn_fmt_name(f), pass); t(name, 0); return;
    }
    const char *held = img == (VkFormat)row.vk ? "its own" : img == (VkFormat)row.alt ? ((row.flags & NVN_VK_EXPAND3) ? "alt, expanded" : "alt, mapped") : img == (VkFormat)row.conv ? "conv, converted" : "?";
    // raw
    uint8_t want[64 * 16 * 2], got[64 * 16 * 2]; int tb = 0;
    if (bc) { memcpy(want, guest, (size_t)n * (size_t)bpb); tb = bpb; }
    else { uint8_t tmp[32]; tb = raw_expected(f, &row, img, guest, tmp); for (uint32_t i = 0; tb && i < n; i++) raw_expected(f, &row, img, guest + (size_t)i * (size_t)bpb, want + (size_t)i * (size_t)tb); }
    int raw_ok = tb && hv_read_blocks((const tex_t *)(const void *)&tex, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, W, H, 1, bw, bh, tb, got) && !memcmp(got, want, (size_t)n * (size_t)tb);
    snprintf(name, sizeof name, "%s (%s pass, VkFormat %u, %s): the image holds the texels as the path says, bit for bit", nvn_fmt_name(f), pass, (unsigned)img, held);
    t(name, raw_ok);
    if (bc) { printf("  %-14s %-10s VkFormat %-4u %-16s raw\n", nvn_fmt_name(f), pass, (unsigned)img, held); return; }
    // sampled
    double v[4]; int cls, srgb;
    int id = g_id++; h_nvnTexturePoolRegisterTexture(&g_tp, id, &tex, NULL);
    nvn_read(f, guest, v, &cls, &srgb);
    int ti = cls == NVN_VK_UINT ? 1 : cls == NVN_VK_SINT ? 2 : 0;
    union { float f[32]; uint32_t u[32]; int32_t i[32]; } s;
    int read = hv_fetch(&g_q, &g_c, &g_prog[ti], &g_tp, &g_sp, id, ti == 1 ? NVN_FORMAT_RGBA32UI : ti == 2 ? NVN_FORMAT_RGBA32I : NVN_FORMAT_RGBA32F, W, H, s.f);
    double worst = 0; int same = read;
    for (uint32_t i = 0; read && i < n; i++) {
        nvn_read(f, guest + (size_t)i * (size_t)bpb, v, &cls, &srgb);
        for (int c = 0; c < 4; c++) {
            if (ti == 1) same &= s.u[4 * i + c] == (uint32_t)(int64_t)v[c];
            else if (ti == 2) same &= s.i[4 * i + c] == (int32_t)v[c];
            else { double d = fabs((double)s.f[4 * i + c] - v[c]), tol = srgb && c < 3 ? 0.5 / 255 : ldexp(fmax(1.0, fabs(v[c])), -20); if (d > worst) worst = d; same &= d <= tol;
                   if (d > tol && !getenv("T_VK_FORMATS_QUIET")) printf("  %s texel %u component %d: %.9g, NVN reads %.9g\n", nvn_fmt_name(f), i, c, s.f[4 * i + c], v[c]); }
        }
    }
    snprintf(name, sizeof name, "%s (%s pass): a shader reads what NVN gives for every texel (%s)", nvn_fmt_name(f), pass, ti == 1 ? "uint" : ti == 2 ? "int" : "float");
    t(name, same);
    // drawn: the fetched texels into a texture of the same NVN format
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(dev->phys, img, &fp);
    int own = img == (VkFormat)row.vk, can = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
    const char *drawn = "-";
    if (own && can) {
        tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, f, W, H, 1, 1, 0);
        const tex_t *rt[1] = {&target};
        hv_rt(&g_c, 1, rt, NULL);
        p_set_pool tpp = {(const dpool_t *)&g_tp}, spp = {(const dpool_t *)&g_sp}; hv_emit(&g_c, OP_SET_TEX_POOL, &tpp, sizeof tpp); hv_emit(&g_c, OP_SET_SAMP_POOL, &spp, sizeof spp);
        hv_bind(&g_c, &g_prog[ti], 3); hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&g_c, NVN_FACE_NONE);
        p_bind_tex1 b = {1, 0, (uint64_t)(uint32_t)id | (uint64_t)1 << 20, nvn_dpool_base(&g_tp, 1), nvn_dpool_base(&g_sp, 0)}; hv_emit(&g_c, OP_BIND_TEX1, &b, sizeof b);
        hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
        hv_submit(&g_q, &g_c);
        int ok = hv_read_blocks(&target, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, W, H, 1, 1, 1, bpb, got);
        // what the format does not hold is not compared: the X component of RGBX / BGRX formats, bit 15 of RGB5 / BGR5
        plain_t pl; int xbytes = plain_of(f, &pl) && pl.x ? pl.size / 8 : 0;
        for (uint32_t i = 0; ok && i < n; i++) {
            uint8_t a[16], b2[16]; memcpy(a, got + (size_t)i * (size_t)bpb, (size_t)bpb); memcpy(b2, guest + (size_t)i * (size_t)bpb, (size_t)bpb);
            if (xbytes) { memset(a + bpb - xbytes, 0, (size_t)xbytes); memset(b2 + bpb - xbytes, 0, (size_t)xbytes); }
            if (f == 0x3A || f == 0x96) { a[1] &= 0x7f; b2[1] &= 0x7f; }
            if (memcmp(a, b2, (size_t)bpb)) {
                printf("  %s drawn: texel %u", nvn_fmt_name(f), i); for (int k = 0; k < bpb; k++) printf(" %02x", a[k]); printf(", written"); for (int k = 0; k < bpb; k++) printf(" %02x", b2[k]); printf("\n");
                ok = 0;
            }
        }
        // A device's own conversion that does not round-trip is a finding about the device, kept here by driver and format (the backend cannot change how a device writes a format)
        int known = dev->driver_id == VK_DRIVER_ID_MESA_LLVMPIPE && f == NVN_FORMAT_RGB10A2SN;
        if (known) printf("  finding (lavapipe): %s drawn %s: lavapipe's float to A2B10G10R10_SNORM conversion %s; not counted\n", nvn_fmt_name(f), ok ? "round-trips now" : "does not round-trip",
                          ok ? "was fixed" : "(one 10-bit component one unit off, the 2-bit alpha -1 stored as 0)");
        else { snprintf(name, sizeof name, "%s (%s pass): drawn into a texture of the same format, the texels written back bit for bit", nvn_fmt_name(f), pass); t(name, ok); }
        drawn = known && !ok ? "drawn: lavapipe's conversion differs (finding)" : "drawn";
    } else drawn = !own ? "drawn: declared (substitute)" : "drawn: not a render target on this device";
    printf("  %-14s %-10s VkFormat %-4u %-16s raw, sampled (max error %.2g), %s\n", nvn_fmt_name(f), pass, (unsigned)img, held, worst, drawn);
}

int main(void) {
    const hostvk_dev *dev = hv_up("t_vk_formats");
    hv_device(0);
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&g_q, &qb);
    if (!hv_pool_make(&g_p, 8u << 20)) { t("memory pool allocated", 0); return hv_finish(); }
    g_c = (hv_cmds){g_p.mem + 0x40000, 0, 0x10000};
    static const char jf[] = FETCH_JSON("float"), ju[] = FETCH_JSON("uint"), ji[] = FETCH_JSON("int");
    const hv_shader s0[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"fetch", 1, HV_SPV(fetch_frag_spv), jf, NULL}};
    const hv_shader s1[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"fetch_u", 1, HV_SPV(fetch_u_frag_spv), ju, NULL}};
    const hv_shader s2[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"fetch_i", 1, HV_SPV(fetch_i_frag_spv), ji, NULL}};
    hv_program(&g_prog[0], &g_p, 0, s0, 2); hv_program(&g_prog[1], &g_p, 1024, s1, 2); hv_program(&g_prog[2], &g_p, 2048, s2, 2);
    h_nvnTexturePoolInitialize(&g_tp, &g_p.pool, 0x10000, 512); h_nvnSamplerPoolInitialize(&g_sp, &g_p.pool, 0x30000, 16);
    NVNsamplerBuilder sb; NVNsampler samp; h_nvnSamplerBuilderSetDefaults(&sb); h_nvnSamplerInitialize(&samp, &sb); h_nvnSamplerPoolRegisterSampler(&g_sp, 1, &samp);
    g_off = 0x50000;
    int rows = 0, forced_rows = 0;
    printf("pass 1: the formats the device has\n");
    for (int f = 0x01; f <= 0x9C; f++) { nvn_vk_fmt r; nvn_fmt_to_vk(f, &r); if (!r.path || skip_format(f)) continue; run_format(dev, f, 0); rows++; }
    printf("pass 2: every row that may need a device feature, on its substitute\n");
    hvk_test_tex_opts(-1, -1, -1, 1);
    for (int f = 0x01; f <= 0x9C; f++) { nvn_vk_fmt r; nvn_fmt_to_vk(f, &r); if (!r.path || skip_format(f) || !(r.flags & NVN_VK_NEEDS_EXT)) continue; run_format(dev, f, 1); forced_rows++; }
    hvk_test_tex_opts(-1, -1, -1, 0);
    printf("format rows: %d on the device's formats, %d on their substitutes (the depth and stencil rows: t_vk_depth24; ASTC: t_vk_astc)\n", rows, forced_rows);
    hv_pool_free(&g_p);
    return hv_finish();
}
