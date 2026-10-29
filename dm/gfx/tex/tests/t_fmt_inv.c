// Unit test of nvn_fmt_from_mtl, the inverse of the NVN → Metal format table (plain C, every host): for every NVN format with a Metal counterpart (0x01-0x9C, the ASTC ranges
// included) the inverse's first candidate maps forward to the same Metal format and swizzle; every candidate does; the format itself is among them; formats without a counterpart
// are no candidate of anything. The Metal formats that more than one NVN format produces are printed and compared with the expectation below, written out from the table's
// comments in nvn_fmt_mtl.c (integer formats and their *_I2F / *_UI2F substitutes, RGBA and RGBX, sRGB and the no-alpha sRGB, DEPTH24 and DEPTH32F, the 16-bit packed families):
// a change of the forward table that adds or removes an ambiguity fails here, so the Vulkan backend's reading of old R rows (nvn_pipe.c) is looked at again
#include "../nvn_fmt_mtl.h"
#include <stdio.h>
#include <string.h>

static int fail, total;
static void t(const char *name, int ok) {
    total++; fail += !ok;
    if (!ok) printf("FAIL %s\n", name);
}

// Metal format → its NVN sources in preference order (format only: what a pipeline key holds)
static const struct { uint32_t mtl; int n; uint16_t f[4]; } AMB[] = {
    {13, 2, {0x03, 0x51}}, {14, 2, {0x04, 0x52}}, {23, 2, {0x08, 0x53}}, {24, 2, {0x09, 0x54}}, {33, 2, {0x0F, 0x57}}, {34, 2, {0x10, 0x58}},
    {40, 2, {0x95, 0x3C}}, {43, 4, {0x97, 0x3A, 0x3B, 0x96}}, {53, 2, {0x0B, 0x55}}, {54, 2, {0x0C, 0x56}}, {63, 2, {0x14, 0x59}}, {64, 2, {0x15, 0x5A}},
    {70, 2, {0x25, 0x6D}}, {71, 2, {0x38, 0x37}}, {72, 2, {0x26, 0x6E}}, {73, 3, {0x27, 0x6F, 0x63}}, {74, 3, {0x28, 0x70, 0x64}}, {80, 2, {0x9A, 0x99}},
    {81, 2, {0x9C, 0x9B}}, {91, 2, {0x3E, 0x6B}}, {103, 2, {0x17, 0x5B}}, {104, 2, {0x18, 0x5C}}, {110, 2, {0x2A, 0x72}}, {112, 2, {0x2B, 0x73}},
    {113, 3, {0x2C, 0x74, 0x65}}, {114, 3, {0x2D, 0x75, 0x66}}, {115, 2, {0x29, 0x71}}, {123, 3, {0x2F, 0x77, 0x67}}, {124, 3, {0x30, 0x78, 0x68}},
    {125, 2, {0x2E, 0x76}}, {130, 2, {0x42, 0x41}}, {131, 2, {0x46, 0x45}}, {252, 2, {0x34, 0x33}}, {260, 2, {0x36, 0x35}},
};
// Still ambiguous with the swizzle as well: an integer format and its *_I2F / *_UI2F read mode, DEPTH32F and DEPTH24, RGB10A2UI and RGB10A2_UI2F (all identity swizzles)
static const struct { uint32_t mtl; uint16_t f[2]; } AMB_SWZ[] = {
    {13, {0x03, 0x51}}, {14, {0x04, 0x52}}, {23, {0x08, 0x53}}, {24, {0x09, 0x54}}, {53, {0x0B, 0x55}}, {54, {0x0C, 0x56}}, {33, {0x0F, 0x57}},
    {34, {0x10, 0x58}}, {63, {0x14, 0x59}}, {64, {0x15, 0x5A}}, {103, {0x17, 0x5B}}, {104, {0x18, 0x5C}}, {73, {0x27, 0x63}}, {74, {0x28, 0x64}},
    {113, {0x2C, 0x65}}, {114, {0x2D, 0x66}}, {123, {0x2F, 0x67}}, {124, {0x30, 0x68}}, {252, {0x34, 0x33}}, {260, {0x36, 0x35}}, {91, {0x3E, 0x6B}},
};
#define N(a) ((int)(sizeof a / sizeof *a))

int main(void) {
    char name[160];
    int mapped = 0, astc = 0, none = 0;
    for (int f = 0; f <= 0x9C + 1; f++) {   // 0 and 0x9D: no format
        uint32_t m = 0; uint8_t s[4]; int kind = nvn_fmt_to_mtl(f, &m, s);
        if (!kind || !m) {
            none++;
            continue;
        }
        mapped++; astc += f >= 0x79 && f <= 0x94;
        uint16_t c[16]; int n = nvn_fmt_from_mtl(m, NULL, c, 16);
        uint32_t m2 = 0; uint8_t s2[4]; nvn_fmt_to_mtl(c[0], &m2, s2);
        snprintf(name, sizeof name, "NVN %#04x -> Metal %u -> NVN %#04x maps to the same Metal format", f, m, n ? c[0] : 0);
        t(name, n >= 1 && m2 == m);
        int found = 0, all = 1;
        for (int i = 0; i < n && i < 16; i++) { uint32_t mi = 0; nvn_fmt_to_mtl(c[i], &mi, NULL); found |= c[i] == f; all &= mi == m; }
        snprintf(name, sizeof name, "NVN %#04x is a candidate of Metal %u, and every candidate maps to it", f, m);
        t(name, found && all);
        n = nvn_fmt_from_mtl(m, s, c, 16); nvn_fmt_to_mtl(n ? c[0] : 0, &m2, s2);
        snprintf(name, sizeof name, "NVN %#04x -> Metal %u + swizzle -> NVN %#04x with the same format and swizzle", f, m, n ? c[0] : 0);
        t(name, n >= 1 && m2 == m && !memcmp(s, s2, 4));
    }
    t("format 0 has no NVN source", nvn_fmt_from_mtl(0, NULL, NULL, 0) == 0);
    t("Metal format 1 (A8Unorm) has no NVN source", nvn_fmt_from_mtl(1, NULL, NULL, 0) == 0);
    t("Metal format 999 has no NVN source", nvn_fmt_from_mtl(999, NULL, NULL, 0) == 0);
    { uint16_t c[1] = {0}; int n = nvn_fmt_from_mtl(260, NULL, c, 1); t("max bounds the output, the count is the total", n == 2 && c[0] == 0x36); }
    { uint8_t one[4] = {NVN_SWZ_R, NVN_SWZ_G, NVN_SWZ_B, NVN_SWZ_ONE}; uint16_t c[4]; int n = nvn_fmt_from_mtl(70, one, c, 4);
      t("RGBA8Unorm with alpha read as 1 is RGBX8 (0x6D) only", n == 1 && c[0] == 0x6D); }
    { uint8_t sw[4] = {NVN_SWZ_B, NVN_SWZ_G, NVN_SWZ_R, NVN_SWZ_A}; uint16_t c[4]; int n = nvn_fmt_from_mtl(40, sw, c, 4);
      t("B5G6R5Unorm with r/b swapped is RGB565 (0x3C) only", n == 1 && c[0] == 0x3C); }
    t("ASTC: 28 formats mapped", astc == 28);

    // The ambiguity list, printed, then checked against the expectation (both directions)
    int namb = 0, ok_amb = 1;
    printf("Metal formats with more than one NVN source (preferred first):\n");
    for (uint32_t m = 1; m < 1024; m++) {
        uint16_t c[16]; int n = nvn_fmt_from_mtl(m, NULL, c, 16);
        if (n < 2) continue;
        namb++;
        printf("  Metal %3u:", m);
        for (int i = 0; i < n && i < 16; i++) printf(" %#04x", c[i]);
        printf("\n");
        int hit = 0;
        for (int i = 0; i < N(AMB); i++) if (AMB[i].mtl == m) { hit = AMB[i].n == n && !memcmp(AMB[i].f, c, sizeof c[0] * (size_t)n); break; }
        if (!hit) { printf("    not as expected\n"); ok_amb = 0; }
    }
    t("the ambiguous Metal formats are the expected ones, in the expected order", ok_amb && namb == N(AMB));
    int namb_s = 0, ok_s = 1;
    for (int f = 1; f <= 0x9C; f++) {
        uint32_t m = 0; uint8_t s[4]; if (!nvn_fmt_to_mtl(f, &m, s) || !m) continue;
        uint16_t c[16]; int n = nvn_fmt_from_mtl(m, s, c, 16);
        if (n < 2 || c[0] != f) continue;   // each ambiguous pair once, at its preferred format
        namb_s++;
        int hit = 0;
        for (int i = 0; i < N(AMB_SWZ); i++) if (AMB_SWZ[i].mtl == m && AMB_SWZ[i].f[0] == c[0]) { hit = n == 2 && AMB_SWZ[i].f[1] == c[1]; break; }
        if (!hit) { printf("  Metal %u with swizzle of %#04x: %d NVN sources, not as expected\n", m, f, n); ok_s = 0; }
    }
    t("the formats still ambiguous with the swizzle are the expected ones", ok_s && namb_s == N(AMB_SWZ));
    printf("== t_fmt_inv: %d NVN formats mapped (%d ASTC), %d without a Metal format; %d ambiguous Metal formats, %d with the swizzle; %d checks, %d failed ==\n",
           mapped, astc, none, namb, namb_s, total, fail);
    return fail != 0;
}
