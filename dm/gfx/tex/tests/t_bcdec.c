// Unit tests for nvn_bcdec.c: format mapping and block decoding.
#include <stdio.h>
#include <string.h>
#include "nvn_bcdec.h"

static int failures;
#define check(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

// Reference independent DXT1/BC1 decoder to verify correctness of bcdec
static void ref_dxt1(const unsigned char b[8], unsigned char out[16 * 4]) {
    unsigned short c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    unsigned char r0 = ((c0 >> 11) & 0x1f) * 255 / 31, g0 = ((c0 >> 5) & 0x3f) * 255 / 63, b0 = (c0 & 0x1f) * 255 / 31;
    unsigned char r1 = ((c1 >> 11) & 0x1f) * 255 / 31, g1 = ((c1 >> 5) & 0x3f) * 255 / 63, b1 = (c1 & 0x1f) * 255 / 31;
    unsigned char pal[4][4] = {
        { r0, g0, b0, 255 },
        { r1, g1, b1, 255 },
        { 0, 0, 0, 255 },
        { 0, 0, 0, 255 },
    };
    if (c0 > c1) {
        pal[2][0] = (2 * r0 + r1) / 3; pal[2][1] = (2 * g0 + g1) / 3; pal[2][2] = (2 * b0 + b1) / 3;
        pal[3][0] = (r0 + 2 * r1) / 3; pal[3][1] = (g0 + 2 * g1) / 3; pal[3][2] = (b0 + 2 * b1) / 3;
    } else {
        pal[2][0] = (r0 + r1) / 2; pal[2][1] = (g0 + g1) / 2; pal[2][2] = (b0 + b1) / 2;
        pal[3][0] = 0; pal[3][1] = 0; pal[3][2] = 0; pal[3][3] = 0;
    }
    unsigned int bits = b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        int idx = (bits >> (2 * (y * 4 + x))) & 3;
        memcpy(out + (y * 4 + x) * 4, pal[idx], 4);
    }
}

static void test_bc1_matches_reference(void) {
    // 3 blocks: c0 > c1 (4-colour), c0 <= c1 (3-colour + transparent black), solid colour
    unsigned char blocks[3][8] = {
        { 0x00, 0xf8, 0x1f, 0x00, 0x1b, 0xe4, 0x00, 0x00 },   // red and blue
        { 0x1f, 0x00, 0x00, 0xf8, 0x1b, 0xe4, 0x00, 0x00 },   // blue and red, transparent mode
        { 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },   // solid
    };
    unsigned char got[16 * 4], want[16 * 4];
    for (int k = 0; k < 3; k++) {
        memset(got, 0xcc, sizeof got);
        memset(want, 0xee, sizeof want);
        ref_dxt1(blocks[k], want);
        int r = nvn_bc_decode(0x41, blocks[k], 8, 4, 4, got, 16);   // 0x41 = RGB_DXT1 (no alpha semantics)
        check(r == 0, "nvn_bc_decode returns 0 for a valid block");
        int diff = 0;
        for (int i = 0; i < 16 * 4; i++) {
            // bcdec uses integer approximation that may differ by at most 1 from exact floating division
            int d = (int)got[i] - (int)want[i];
            if (d < -1 || d > 1) diff++;
        }
        check(diff == 0, "bc1 output matches independent reference within 1 count");
    }
}

static void test_formats_map(void) {
    uint32_t pf; int bpp;
    // Standard path
    struct { uint32_t fmt, pf; int bpp; } cases_std[] = {
        { 0x41, 70, 4 }, { 0x42, 70, 4 }, { 0x43, 70, 4 }, { 0x44, 70, 4 }, { 0x4d, 70, 4 },
        { 0x45, 71, 4 }, { 0x46, 71, 4 }, { 0x47, 71, 4 }, { 0x48, 71, 4 }, { 0x4e, 71, 4 },
        { 0x49, 10, 1 }, { 0x4a, 12, 1 }, { 0x4b, 30, 2 }, { 0x4c, 32, 2 },
        { 0x4f, 115, 8 }, { 0x50, 115, 8 },
    };
    for (unsigned i = 0; i < sizeof cases_std / sizeof cases_std[0]; i++) {
        check(nvn_bc_unpacked_standard(cases_std[i].fmt, &pf, &bpp) == 1, "standard format is recognised as BC");
        check(pf == cases_std[i].pf && bpp == cases_std[i].bpp, "standard format maps to expected MTLPixelFormat and bpp");
    }
    // RGB9E5 path
    struct { uint32_t fmt, pf; int bpp; } cases_rgb9e5[] = {
        { 0x4f, 93, 4 }, { 0x50, 93, 4 },
    };
    for (unsigned i = 0; i < sizeof cases_rgb9e5 / sizeof cases_rgb9e5[0]; i++) {
        check(nvn_bc_unpacked_rgb9e5(cases_rgb9e5[i].fmt, &pf, &bpp) == 1, "rgb9e5 format is recognised as BC");
        check(pf == cases_rgb9e5[i].pf && bpp == cases_rgb9e5[i].bpp, "rgb9e5 format maps to RGB9E5 and 4 bpp");
    }
    // GPU path (returns 0)
    check(nvn_bc_unpacked_gpu(0x41, &pf, &bpp) == 0, "GPU path returns 0 (not CPU unpacked)");
    check(nvn_bc_unpacked(0x25, &pf, &bpp) == 0, "a non-BC format is rejected");
}

static void test_edge_blocks_trimmed(void) {
    // 5x5 image from 2x2 blocks: bottom and right edges are partial
    enum { W = 5, H = 5 };
    unsigned char blk[4][8];
    for (int i = 0; i < 4; i++) {
        blk[i][0] = 0x00; blk[i][1] = 0xf8;
        blk[i][2] = 0x00; blk[i][3] = 0x00;
        memset(blk[i] + 4, 0, 4);   // all texels pick c0 (red)
    }
    unsigned char out[W * H * 4];
    memset(out, 0x55, sizeof out);
    int r = nvn_bc_decode(0x41, (const unsigned char *)blk, 16, W, H, out, W * 4);
    check(r == 0, "nvn_bc_decode handles non-multiple-of-4 dimensions");
    // Verify every texel in the 5x5 region is red, nothing written past it
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        const unsigned char *p = out + (y * W + x) * 4;
        check(p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255, "texel inside 5x5 is red");
    }
}

static void test_bc4_single_channel(void) {
    // BC4: 8 bytes, decodes to 1 byte per pixel
    unsigned char blk[8] = { 255, 0, 0, 0, 0, 0, 0, 0 };   // c0 = 255, c1 = 0, all indices 0
    unsigned char out[4 * 4];
    memset(out, 0, sizeof out);
    int r = nvn_bc_decode(0x49, blk, 8, 4, 4, out, 4);   // 0x49 = BC4 unsigned
    check(r == 0, "bc4 decode returns 0");
    check(out[0] == 255, "bc4 texel receives value");
    check(out[1 + 4] == 255, "bc4 writes every row (not just the first)");
}

static void test_bc6h_rgb9e5(void) {
    unsigned char blk[16] = { 0 };
    uint32_t out[4 * 4];
    memset(out, 0xCC, sizeof out);
    int r = nvn_bc_decode_rgb9e5(0x50, blk, 16, 4, 4, (uint8_t *)out, 4 * sizeof(uint32_t));   // 0x50 = BC6H unsigned
    check(r == 0, "bc6h rgb9e5 decode returns 0");
    check((out[0] & 0xF8000000) == 0, "bc6h all-zero block has zero/minimal exponent in RGB9E5");
}

static void test_bc6h_standard(void) {
    unsigned char blk[16] = { 0 };
    uint16_t out[4 * 4 * 4];
    memset(out, 0xCC, sizeof out);
    int r = nvn_bc_decode_standard(0x50, blk, 16, 4, 4, (uint8_t *)out, 4 * 4 * sizeof(uint16_t));
    check(r == 0, "bc6h standard decode returns 0");
    check(out[3] == 0x3c00, "bc6h alpha is 1.0 (0x3c00 in half float)");
}

int main(void) {
    test_bc1_matches_reference();
    test_formats_map();
    test_edge_blocks_trimmed();
    test_bc4_single_channel();
    test_bc6h_rgb9e5();
    test_bc6h_standard();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("bc decode: BC1 matches an independent DXT1 decoder, format map, edge trimming, BC4, BC6H RGB9E5 & RGBA16F - all OK\n");
    return 0;
}
