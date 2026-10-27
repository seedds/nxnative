// BC textures decoded on the CPU (nvn_bcdec.c): known blocks decode to the texels computed here by hand (BC1 against an independent decoder), the format map covers every BC format,
// edge blocks write only the texels inside the image, a padded destination row keeps its padding, and the decision between the GPU and the CPU path follows the device's capability.
#include "../nvn_bcdec.h"
#include <stdio.h>
#include <string.h>

static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; printf("FAIL: %s\n", label); } } while (0)

// ---- BC1 reference decoder written without bcdec: 565 endpoints expanded with the D3D10 rounding, the two blended colours of the four-colour mode ----
static void expand565(unsigned v, unsigned char o[4]) {
    unsigned r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
    o[0] = (unsigned char)((r * 255 + 15) / 31); o[1] = (unsigned char)((g * 255 + 31) / 63); o[2] = (unsigned char)((b * 255 + 15) / 31); o[3] = 255;
}
static void ref_bc1(const unsigned char *blk, unsigned char *out, int pitch) {
    unsigned c0 = blk[0] | blk[1] << 8, c1 = blk[2] | blk[3] << 8;
    unsigned char pal[4][4];
    expand565(c0, pal[0]); expand565(c1, pal[1]);
    for (int i = 0; i < 3; i++) {
        if (c0 > c1) { pal[2][i] = (unsigned char)((2 * pal[0][i] + pal[1][i] + 1) / 3); pal[3][i] = (unsigned char)((pal[0][i] + 2 * pal[1][i] + 1) / 3); }
        else { pal[2][i] = (unsigned char)((pal[0][i] + pal[1][i] + 1) / 2); pal[3][i] = 0; }
    }
    pal[2][3] = 255; pal[3][3] = c0 > c1 ? 255 : 0;   // three-colour mode: the fourth colour is transparent black (RGB_DXT1 reads alpha 1 through its view swizzle)
    unsigned idx = blk[4] | blk[5] << 8 | blk[6] << 16 | (unsigned)blk[7] << 24;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) memcpy(out + y * pitch + x * 4, pal[idx >> (2 * (y * 4 + x)) & 3], 4);
}

static void test_bc1_against_reference(void) {
    // c0 > c1 (four colours), c0 < c1 (three colours and transparent black), c0 == c1; indices that use every palette entry
    unsigned char blocks[4][8] = {
        { 0x00, 0xF8, 0xE0, 0x07, 0x1B, 0x36, 0x6C, 0xD8 },
        { 0xE0, 0x07, 0x00, 0xF8, 0xE4, 0xE4, 0xE4, 0xE4 },
        { 0x34, 0x12, 0x34, 0x12, 0xFF, 0x00, 0xAA, 0x55 },
        { 0x1F, 0x00, 0xFF, 0xFF, 0x1B, 0xE4, 0x4E, 0xB1 },
    };
    for (int k = 0; k < 4; k++) {
        unsigned char got[4 * 16], want[4 * 16];
        CHECK(nvn_bc_decode(0x42, blocks[k], 8, 4, 4, got, 16) == 0, "BC1 decode returns 0");
        ref_bc1(blocks[k], want, 16);
        CHECK(!memcmp(got, want, sizeof want), "BC1 block equals the independent decoder");
        unsigned char srgb[4 * 16];   // the sRGB formats decode the same bytes (the texture's format applies the transfer curve)
        CHECK(nvn_bc_decode(0x46, blocks[k], 8, 4, 4, srgb, 16) == 0 && !memcmp(srgb, got, sizeof got), "BC1 sRGB decodes the same bytes");
    }
}

static void test_format_map(void) {
    uint32_t pf; int bpp;
    struct { uint32_t fmt; unsigned pf; int bpp; } cases[] = {
        { 0x41, 70, 4 }, { 0x42, 70, 4 }, { 0x43, 70, 4 }, { 0x44, 70, 4 }, { 0x4d, 70, 4 },
        { 0x45, 71, 4 }, { 0x46, 71, 4 }, { 0x47, 71, 4 }, { 0x48, 71, 4 }, { 0x4e, 71, 4 },
        { 0x49, 10, 1 }, { 0x4a, 12, 1 }, { 0x4b, 30, 2 }, { 0x4c, 32, 2 },
        { 0x4f, 115, 8 }, { 0x50, 115, 8 },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        CHECK(nvn_bc_unpacked(cases[i].fmt, &pf, &bpp) == 1 && pf == cases[i].pf && bpp == cases[i].bpp, "every BC format maps to its decoded Metal format");
        unsigned char blk[16] = {0}, out[16 * 8]; memset(out, 0, sizeof out);
        CHECK(nvn_bc_decode(cases[i].fmt, blk, 16, 4, 4, out, (size_t)bpp * 4) == 0, "every BC format decodes");
    }
    pf = 7; bpp = 7;
    CHECK(nvn_bc_unpacked(0x25, &pf, &bpp) == 0 && pf == 7 && bpp == 7, "a non-BC format is rejected and the outputs stay");
    CHECK(nvn_bc_unpacked(0x40, &pf, &bpp) == 0 && nvn_bc_unpacked(0x51, &pf, &bpp) == 0, "the neighbours of the BC range are not BC");
    CHECK(nvn_bc_decode(0x25, (const uint8_t *)"", 0, 4, 4, (uint8_t[16]){0}, 16) == -1, "decoding a non-BC format fails");
}

static void test_bc2_bc3_alpha(void) {
    // a solid colour block (c0 == c1, indices 0) under the alpha block: BC2 gives 4-bit alpha scaled by 17, BC3 gives the interpolated alpha
    unsigned char colour[8] = { 0x1F, 0x00, 0x1F, 0x00, 0, 0, 0, 0 };   // blue 0x1F -> (0, 0, 255)
    unsigned char b2[16] = { 0x50, 0x0F, 0xFF, 0xFF, 0x00, 0x00, 0x55, 0x55 }, b3[16] = { 200, 200, 0, 0, 0, 0, 0, 0 }, out[64];
    memcpy(b2 + 8, colour, 8); memcpy(b3 + 8, colour, 8);
    CHECK(nvn_bc_decode(0x43, b2, 16, 4, 4, out, 16) == 0, "BC2 decodes");
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 255 && out[3] == 0, "BC2: texel 0 has the blue and alpha nibble 0 -> 0");
    CHECK(out[4 + 3] == 5 * 17 && out[8 + 3] == 0xF * 17 && out[12 + 3] == 0, "BC2: the nibbles 5, F, 0 scale by 17");
    CHECK(out[3 * 16 + 3] == 5 * 17, "BC2: the last row is written");
    CHECK(nvn_bc_decode(0x44, b3, 16, 4, 4, out, 16) == 0, "BC3 decodes");
    int ok = 1; for (int i = 0; i < 16; i++) ok &= out[i * 4] == 0 && out[i * 4 + 2] == 255 && out[i * 4 + 3] == 200;
    CHECK(ok, "BC3: equal alpha endpoints give that alpha everywhere");
}

static int ref_bc4(int a0, int a1, int k, int is_signed) {   // palette entry k of a BC4 block, rounded
    if (k == 0) return a0; if (k == 1) return a1;
    if (a0 > a1) return (int)((double)((8 - k) * a0 + (k - 1) * a1) / 7 + (is_signed && ((8 - k) * a0 + (k - 1) * a1) < 0 ? -0.5 : 0.5));
    if (k == 6) return is_signed ? -127 : 0; if (k == 7) return is_signed ? 127 : 255;
    return (int)((double)((6 - k) * a0 + (k - 1) * a1) / 5 + (is_signed && ((6 - k) * a0 + (k - 1) * a1) < 0 ? -0.5 : 0.5));
}
static void bc4_block(unsigned char *b, int a0, int a1, int k) {   // every texel index k
    unsigned long long idx = 0; for (int i = 0; i < 16; i++) idx |= (unsigned long long)k << (3 * i);
    b[0] = (unsigned char)a0; b[1] = (unsigned char)a1; for (int i = 0; i < 6; i++) b[2 + i] = (unsigned char)(idx >> (8 * i));
}
static void test_bc4_bc5(void) {
    unsigned char b[16], out[32];
    for (int k = 0; k < 8; k++) {   // unsigned, eight-value mode (a0 > a1) and six-value mode (a0 < a1)
        bc4_block(b, 250, 20, k); memset(out, 0, 16);
        CHECK(nvn_bc_decode(0x49, b, 8, 4, 4, out, 4) == 0 && out[0] == ref_bc4(250, 20, k, 0) && out[15] == out[0], "BC4 unsigned, eight-value mode");
        bc4_block(b, 20, 250, k); memset(out, 0, 16);
        CHECK(nvn_bc_decode(0x49, b, 8, 4, 4, out, 4) == 0 && out[0] == ref_bc4(20, 250, k, 0) && out[15] == out[0], "BC4 unsigned, six-value mode");
        bc4_block(b, 100, (unsigned char)-90, k); memset(out, 0, 16);
        CHECK(nvn_bc_decode(0x4a, b, 8, 4, 4, out, 4) == 0 && (signed char)out[0] == ref_bc4(100, -90, k, 1) && out[15] == out[0], "BC4 signed");
    }
    // BC5: red from the first block, green from the second, two bytes per texel
    unsigned char b5[16]; bc4_block(b5, 255, 0, 0); bc4_block(b5 + 8, 255, 0, 1);
    memset(out, 0xCC, sizeof out);
    CHECK(nvn_bc_decode(0x4b, b5, 16, 4, 4, out, 8) == 0, "BC5 decodes");
    int ok = 1; for (int i = 0; i < 16; i++) ok &= out[i * 2] == 255 && out[i * 2 + 1] == 0;
    CHECK(ok, "BC5: red 255 from the first block, green 0 from the second");
}

static uint16_t half_at(const unsigned char *p, int i) { uint16_t v; memcpy(&v, p + i * 2, 2); return v; }
static void put_bits(unsigned char *b, int pos, int n, unsigned v) { for (int i = 0; i < n; i++) if (v >> i & 1) b[(pos + i) / 8] |= (unsigned char)(1 << ((pos + i) % 8)); }
static void test_bc6h_bc7(void) {
    // BC6H mode 0b00011 (one region, ten-bit endpoints, no delta): both endpoints (1023, 0, 1023), every index 0. An unsigned ten-bit 1023 is the largest finite half (0x7BFF), 0 is 0; alpha is 1.0 (0x3C00)
    unsigned char b6[16] = {0}, out[16 * 8]; memset(out, 0xAB, sizeof out);
    put_bits(b6, 0, 5, 3); put_bits(b6, 5, 10, 1023); put_bits(b6, 25, 10, 1023); put_bits(b6, 35, 10, 1023); put_bits(b6, 55, 10, 1023);
    CHECK(nvn_bc_decode(0x50, b6, 16, 4, 4, out, 32) == 0, "BC6H decodes");
    int ok = 1; for (int i = 0; i < 16; i++) ok &= half_at(out, i * 4) == 0x7BFF && half_at(out, i * 4 + 1) == 0 && half_at(out, i * 4 + 2) == 0x7BFF && half_at(out, i * 4 + 3) == 0x3C00;
    CHECK(ok, "BC6H: endpoint colour in half floats, alpha 1.0, every row and texel written with the right pitch");
    // BC7 mode 6 (one subset, 7-bit RGBA endpoints and a p-bit each): R = B = A = 127, G = 0, p-bits 1 -> (255, 1, 255, 255); every index 0
    unsigned char b7[16] = {0}; unsigned char o7[16 * 4];
    put_bits(b7, 6, 1, 1);
    for (int c = 0; c < 8; c++) { unsigned v = (c / 2 == 1) ? 0 : 127; put_bits(b7, 7 + 7 * c, 7, v); }
    put_bits(b7, 63, 1, 1); put_bits(b7, 64, 1, 1);
    CHECK(nvn_bc_decode(0x4d, b7, 16, 4, 4, o7, 16) == 0, "BC7 decodes");
    ok = 1; for (int i = 0; i < 16; i++) ok &= o7[i * 4] == 255 && o7[i * 4 + 1] == 1 && o7[i * 4 + 2] == 255 && o7[i * 4 + 3] == 255;
    CHECK(ok, "BC7 mode 6: the endpoint colour with its p-bit");
}

static void test_edges_pitch_and_mips(void) {
    // a 10x6 image of 3x2 blocks of solid BC1 colours, source rows padded to 32 bytes, destination rows padded to 64 bytes: the texels inside are decoded, the padding and everything past the last row stay
    enum { W = 10, H = 6, DP = 64 };
    unsigned char src[2 * 32]; memset(src, 0x77, sizeof src);
    unsigned short col[2][3] = { { 0xF800, 0x07E0, 0x001F }, { 0xFFFF, 0x0000, 0x8410 } };
    for (int by = 0; by < 2; by++) for (int bx = 0; bx < 3; bx++) { unsigned char *b = src + by * 32 + bx * 8; b[0] = b[2] = (unsigned char)col[by][bx]; b[1] = b[3] = (unsigned char)(col[by][bx] >> 8); b[4] = b[5] = b[6] = b[7] = 0; }
    unsigned char out[H * DP + 64]; memset(out, 0xAB, sizeof out);
    CHECK(nvn_bc_decode(0x42, src, 32, W, H, out, DP) == 0, "10x6 decodes");
    int ok = 1, pad = 1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) { unsigned char want[4]; expand565(col[y / 4][x / 4], want); ok &= !memcmp(out + y * DP + x * 4, want, 4); }
        for (int x = W * 4; x < DP; x++) pad &= out[y * DP + x] == 0xAB;
    }
    for (unsigned i = H * DP; i < sizeof out; i++) pad &= out[i] == 0xAB;
    CHECK(ok, "texels of whole and partial edge blocks hold their block's colour");
    CHECK(pad, "the row padding and the bytes past the last row are untouched");
    // the mip chain of a 9x5 texture: 9x5, 4x2, 2x1, 1x1 -- every level's partial blocks decode to the colour of the block, only inside the level
    unsigned lw[4] = { 9, 4, 2, 1 }, lh[4] = { 5, 2, 1, 1 };
    for (int l = 0; l < 4; l++) {
        unsigned bw = (lw[l] + 3) / 4, bh = (lh[l] + 3) / 4; unsigned char s[6 * 8], o[9 * 5 * 4 + 16]; memset(s, 0, sizeof s); memset(o, 0xAB, sizeof o);
        for (unsigned i = 0; i < bw * bh; i++) { s[i * 8] = s[i * 8 + 2] = 0xE0 + (unsigned char)l; s[i * 8 + 1] = s[i * 8 + 3] = 0x07; }
        CHECK(nvn_bc_decode(0x41, s, bw * 8, lw[l], lh[l], o, lw[l] * 4) == 0, "a mip level decodes");
        unsigned char want[4]; expand565(0x07E0 + l, want); int good = 1;
        for (unsigned i = 0; i < lw[l] * lh[l]; i++) good &= !memcmp(o + i * 4, want, 4);
        for (unsigned i = lw[l] * lh[l] * 4; i < sizeof o; i++) good &= o[i] == 0xAB;
        CHECK(good, "mip level texels hold the block colour and nothing is written past the level");
    }
}

static void test_capability_decision(void) {
    CHECK(nvn_bc_cpu_wanted(0, NULL) == 1, "a device without BC support decodes on the CPU");
    CHECK(nvn_bc_cpu_wanted(1, NULL) == 0, "a device that samples BC keeps the GPU path");
    CHECK(nvn_bc_cpu_wanted(1, "") == 0 && nvn_bc_cpu_wanted(0, "") == 1, "an empty switch changes nothing");
    CHECK(nvn_bc_cpu_wanted(1, "1") == 1, "DM_TEST_NO_BC=1 forces the CPU path on a device with BC");
    CHECK(nvn_bc_cpu_wanted(1, "0") == 0 && nvn_bc_cpu_wanted(0, "0") == 1, "DM_TEST_NO_BC=0 is as unset: the device decides");
}

int main(void) {
    test_bc1_against_reference();
    test_format_map();
    test_bc2_bc3_alpha();
    test_bc4_bc5();
    test_bc6h_bc7();
    test_edges_pitch_and_mips();
    test_capability_decision();
    printf("t_bcdec: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
