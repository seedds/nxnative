// Unit test of the NVN → Vulkan format table (dm/gfx/tex/nvn_fmt_vk.c; plain C, every host, no Vulkan): every NVN format 0x01-0x9C has a named row and a path (step P7: none
// without one); a row that is not a direct exact one says why (what it converts, substitutes or needs); the Vulkan format's bytes per block match NVN's (dm/gfx/nvn/nvn_tex.c
// nvn_fmt_bpb; depth-stencil formats, whose packing differs, apart); the row's numeric class, sRGB flag, block flag and aspect agree with the format's name; three-component rows
// name a four-component alternative; a converting row names the format it converts into, of the row's numeric class and at least its components; ASTC rows decode into RGBA8
// (sRGB for the sRGB formats); a row that may need a device feature names its substitute or says what happens without it; the mapping is a component mapping; RGBX formats read
// alpha one; formats outside the list map to nothing. Prints the rows by path
#include "../nvn_fmt_vk.h"
#include "../../nvn/nvn_tex.h"
#include "../../../third_party/vulkan-headers/include/vulkan/vulkan_core.h"
#include <stdio.h>
#include <string.h>

static int fail, total;
static void t(const char *name, int ok) { total++; fail += !ok; if (!ok) printf("FAIL %s\n", name); }
// Bytes per texel or block of the VkFormats the table uses (vulkan_core.h's enum order); 0 for a format this test does not know
static int vk_bytes(uint32_t f, int *components) {
    static const struct { uint32_t lo, hi; int bytes, comps; } r[] = {
        {1, 1, 1, 2}, {2, 3, 2, 4}, {4, 5, 2, 3}, {6, 8, 2, 4}, {9, 15, 1, 1}, {16, 22, 2, 2}, {23, 36, 3, 3}, {37, 50, 4, 4}, {51, 57, 4, 4}, {58, 69, 4, 4},
        {70, 76, 2, 1}, {77, 83, 4, 2}, {84, 90, 6, 3}, {91, 97, 8, 4}, {98, 100, 4, 1}, {101, 103, 8, 2}, {104, 106, 12, 3}, {107, 109, 16, 4},
        {122, 122, 4, 3}, {123, 123, 4, 3}, {124, 124, 2, 1}, {125, 125, 4, 1}, {126, 126, 4, 1}, {127, 127, 1, 1}, {129, 129, 4, 2}, {130, 130, 5, 2},
        {131, 134, 8, 4}, {135, 138, 16, 4}, {139, 140, 8, 1}, {141, 142, 16, 2}, {143, 144, 16, 3}, {145, 146, 16, 4}, {157, 184, 16, 4},
        {1000340000, 1000340001, 2, 4}, {1000470000, 1000470000, 2, 4}, {1000470001, 1000470001, 1, 1},
    };
    for (size_t i = 0; i < sizeof r / sizeof *r; i++) if (f >= r[i].lo && f <= r[i].hi) { if (components) *components = r[i].comps; return r[i].bytes; }
    return 0;
}
static int vk_is_sint(uint32_t f) { return f == VK_FORMAT_R16G16B16A16_SINT || f == VK_FORMAT_R32G32B32A32_SINT || f == VK_FORMAT_R8G8B8A8_SINT; }
static int vk_is_float(uint32_t f) { return f == VK_FORMAT_R32_SFLOAT || f == VK_FORMAT_R32G32_SFLOAT || f == VK_FORMAT_R32G32B32A32_SFLOAT || f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB; }
static int has(const char *s, const char *sub) { return strstr(s, sub) != NULL; }
static int ends(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && !strcmp(s + a - b, suf); }

int main(void) {
    char name[220]; int rows = 0, exact = 0, subst = 0, none = 0, by_path[NVN_VK_PATH_N] = {0};
    char list[NVN_VK_PATH_N][4096]; memset(list, 0, sizeof list);
    for (int f = 0x01; f <= 0x9C; f++) {
        nvn_vk_fmt r; int kind = nvn_fmt_to_vk(f, &r);
        const char *n = nvn_fmt_name(f);
        snprintf(name, sizeof name, "format %#x has a named row", f); t(name, n && strcmp(n, "?") && kind == r.kind);
        if (!n || !strcmp(n, "?")) continue;
        rows++; exact += kind == 1; subst += kind == 2; none += kind == 0;
        if (r.path < NVN_VK_PATH_N) {
            by_path[r.path]++;
            size_t k = strlen(list[r.path]);
            if (r.path != NVN_VK_PATH_DIRECT || kind != 1 || (r.flags & NVN_VK_NEEDS_EXT)) snprintf(list[r.path] + k, sizeof list[0] - k, "%s%s%s", k ? ", " : "", n, kind == 2 ? " (substitute)" : "");
        }
        snprintf(name, sizeof name, "%s: has a path", n); t(name, r.path != NVN_VK_PATH_NONE && r.path < NVN_VK_PATH_N);
        // the header's contract: a sentence for every row that is not a direct exact one (a conversion, a substitute, a device feature), none for the others
        int plain = kind == 1 && r.path == NVN_VK_PATH_DIRECT && !(r.flags & NVN_VK_NEEDS_EXT);
        snprintf(name, sizeof name, "%s: a row that converts, substitutes or needs a device feature says why; a direct exact one does not", n);
        t(name, plain ? r.why == NULL : r.why && *r.why);
        snprintf(name, sizeof name, "%s: kind 0 has no format, the others a Vulkan format or one they convert into", n); t(name, kind == 0 ? r.vk == 0 : r.vk != 0 || r.conv != 0);
        int conv_path = r.path == NVN_VK_PATH_CONVERT || r.path == NVN_VK_PATH_ASTC;
        snprintf(name, sizeof name, "%s: a converting or decoding path names the format it writes, the others none", n); t(name, conv_path ? r.conv != 0 : r.conv == 0);
        int comps = 0, vb = r.vk ? vk_bytes(r.vk, &comps) : 0;
        if (r.conv) {
            int cc = 0, cb = vk_bytes(r.conv, &cc), want = 0;
            if (r.vk) vk_bytes(r.vk, &want); else if (r.alt) vk_bytes(r.alt, &want);
            snprintf(name, sizeof name, "%s: its conversion format %u is one this test knows, with at least its %d components", n, r.conv, want); t(name, cb > 0 && cc >= want);
            snprintf(name, sizeof name, "%s: its conversion format reads the row's numeric class", n); t(name, r.cls == NVN_VK_SINT ? vk_is_sint(r.conv) : vk_is_float(r.conv));
        }
        if (r.path == NVN_VK_PATH_ASTC) {
            snprintf(name, sizeof name, "%s: decoded into RGBA8, sRGB for an sRGB format", n); t(name, r.conv == (has(n, "SRGB") ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM));
        }
        snprintf(name, sizeof name, "%s: the expansion path and flag agree", n); t(name, (r.path == NVN_VK_PATH_EXPAND3) == !!(r.flags & NVN_VK_EXPAND3));
        if (!r.vk) continue;
        int is_ds = has(n, "DEPTH") || !strcmp(n, "STENCIL8");
        snprintf(name, sizeof name, "%s: VkFormat %u is one this test knows", n, r.vk); t(name, vb > 0);
        int bw, bh, nb = nvn_fmt_bpb(f, &bw, &bh);
        if (!is_ds && vb) { snprintf(name, sizeof name, "%s: %d bytes per block in NVN and Vulkan (%d)", n, nb, vb); t(name, nb == vb); }
        snprintf(name, sizeof name, "%s: aspect from the name", n);
        t(name, is_ds ? (r.aspect & (NVN_VK_DEPTH | NVN_VK_STENCIL)) && !(r.aspect & NVN_VK_COLOR) && r.cls == NVN_VK_DS : r.aspect == NVN_VK_COLOR && r.cls != NVN_VK_DS);
        if (r.path == NVN_VK_PATH_DEPTH) { snprintf(name, sizeof name, "%s: the depth path is a depth format's", n); t(name, is_ds && (r.aspect & NVN_VK_DEPTH)); }
        int sint = !is_ds && (ends(n, "I") && !ends(n, "UI")), uint = !is_ds && ends(n, "UI");
        snprintf(name, sizeof name, "%s: numeric class from the name", n);
        t(name, is_ds || (uint ? r.cls == NVN_VK_UINT : sint ? r.cls == NVN_VK_SINT : r.cls == NVN_VK_FLOAT));
        snprintf(name, sizeof name, "%s: sRGB flag from the name", n); t(name, !!(r.flags & NVN_VK_SRGB) == !!has(n, "SRGB"));
        snprintf(name, sizeof name, "%s: block flag from the name", n); t(name, !!(r.flags & NVN_VK_BLOCK) == (has(n, "BPTC") || has(n, "RGTC") || has(n, "DXT") || has(n, "ASTC")));
        if (r.flags & NVN_VK_EXPAND3) { int ac = 0; vk_bytes(r.alt, &ac); snprintf(name, sizeof name, "%s: three components, a four-component alternative", n); t(name, comps == 3 && r.alt && ac == 4); }
        if (r.flags & NVN_VK_NEEDS_EXT) { snprintf(name, sizeof name, "%s: may need a device feature: names its substitute or says what happens without it", n); t(name, r.alt != 0 || r.conv != 0 || (r.why && *r.why)); }
        int ok = 1; for (int c = 0; c < 4; c++) ok &= r.swz[c] <= NVN_SWZ_A;   // zero, one, or a component (one the format lacks reads 0, alpha 1, as in Vulkan)
        snprintf(name, sizeof name, "%s: the mapping is a component mapping", n); t(name, ok);
        if (has(n, "RGBX") || has(n, "BGRX") || (r.flags & NVN_VK_EXPAND3)) { snprintf(name, sizeof name, "%s: no alpha in the format: alpha reads one", n); t(name, r.swz[3] == NVN_SWZ_ONE); }
    }
    nvn_vk_fmt r; t("format 0 is no NVN format", nvn_fmt_to_vk(0, &r) == 0 && r.vk == 0 && r.why && has(r.why, "not an NVN format"));
    t("format 0x9D is no NVN format", nvn_fmt_to_vk(0x9D, &r) == 0 && r.vk == 0);
    t("every row has a path (none without one)", by_path[NVN_VK_PATH_NONE] == 0 && none == 0);
    printf("NVN to Vulkan format table: %d rows, %d exact, %d substitutes, %d without a path\n", rows, exact, subst, none);
    printf("paths: direct %d, expand3 %d, convert %d, astc %d, depth %d\n", by_path[NVN_VK_PATH_DIRECT], by_path[NVN_VK_PATH_EXPAND3], by_path[NVN_VK_PATH_CONVERT],
           by_path[NVN_VK_PATH_ASTC], by_path[NVN_VK_PATH_DEPTH]);
    for (int p = 1; p < NVN_VK_PATH_N; p++) if (list[p][0]) printf("  %s%s: %s\n", nvn_fmt_vk_path_name(p), p == NVN_VK_PATH_DIRECT ? " (through a substitute or a device feature)" : "", list[p]);
    printf("== t_fmt_vk: %d checks, %d failed ==\n", total, fail);
    return fail != 0;
}
