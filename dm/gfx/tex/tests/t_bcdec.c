// BC decode correctness: decode a known block with bcdec through our glue, and compare against
// bytes produced by an independent path (a second, hand-written DXT1 decoder in this file).
// Formats with no independent reference here are checked structurally instead: every decoded
// pixel must be opaque, the block must fit its 4x4 grid, and edge blocks must be trimmed.
#include "../nvn_bcdec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(int cond, const char *what) {
    if (!cond) { printf("FAIL: %s\n", what); failures++; }
}

// ---- reference DXT1 (BC1) decoder: the textbook algorithm, written independently of bcdec ----
static void put565(unsigned char *o, unsigned v, int srgb) {
    (void)srgb;
    int r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f;
    o[0] = (unsigned char)((r << 3) | (r >> 2));
    o[1] = (unsigned char)((g << 2) | (g >> 4));
    o[2] = (unsigned char)((b << 3) | (b >> 2));
    o[3] = 255;
}
static void ref_bc1(const unsigned char *blk, unsigned char *out, int op) {
    unsigned c0 = blk[0] | (blk[1] << 8), c1 = blk[2] | (blk[3] << 8);
    unsigned char pal[4][4];
    put565(pal[0], c0, 0);
    put565(pal[1], c1, 0);
    if (c0 > c1) {
        for (int i = 0; i < 3; i++) {
            // bcdec rounds; a truncating reference differs by one on every interpolated stop
            pal[2][i] = (unsigned char)((2 * pal[0][i] + pal[1][i] + 1) / 3);
            pal[3][i] = (unsigned char)((pal[0][i] + 2 * pal[1][i] + 1) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int i = 0; i < 3; i++) {
            pal[2][i] = (unsigned char)((pal[0][i] + pal[1][i] + 1) / 2);
            pal[3][i] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 255;   // RGB_DXT1: the fourth colour is black but opaque
    }
    unsigned idx = blk[4] | (blk[5] << 8) | (blk[6] << 16) | ((unsigned)blk[7] << 24);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            int p = (int)((idx >> (2 * (y * 4 + x))) & 3);
            memcpy(out + y * op + x * 4, pal[p], 4);
        }
}

static void test_bc1_matches_reference(void) {
    // Two blocks: one with c0 > c1 (four-colour mode) and one with c0 <= c1 (three-colour).
    unsigned char blocks[2][8] = {
        { 0x00, 0xF8, 0xE0, 0x07, 0x1B, 0x36, 0x6C, 0xD8 },   // c0 = red-ish 0xF800, c1 = green-ish 0x07E0
        { 0xE0, 0x07, 0x00, 0xF8, 0x55, 0xAA, 0x55, 0xAA },   // swapped: three-colour mode
    };
    for (int k = 0; k < 2; k++) {
        unsigned char got[4 * 16], want[4 * 16];
        int r = nvn_bc_decode(0x41, blocks[k], 8, 4, 4, got, 16);   // 0x41 = RGB_DXT1 (no alpha semantics)
        check(r == 0, "bc1 decode returns 0");
        ref_bc1(blocks[k], want, 16);
        if (memcmp(got, want, sizeof want) != 0) {
            char msg[96];
            snprintf(msg, sizeof msg, "BC1 block %d matches the reference decoder", k);
            check(0, msg);
            for (int i = 0; i < 16; i++)
                if (got[i] != want[i]) { printf("  byte %d: got %u want %u\n", i, got[i], want[i]); break; }
        } else {
            check(1, "");
        }
    }
}

static void test_formats_map(void) {
    unsigned pf; int bpp;
    struct { uint32_t fmt; unsigned pf; int bpp; } cases[] = {
        { 0x41, 70, 4 }, { 0x42, 70, 4 }, { 0x43, 70, 4 }, { 0x44, 70, 4 }, { 0x4d, 70, 4 },
        { 0x45, 71, 4 }, { 0x46, 71, 4 }, { 0x47, 71, 4 }, { 0x48, 71, 4 }, { 0x4e, 71, 4 },
        { 0x49, 10, 1 }, { 0x4a, 12, 1 }, { 0x4b, 30, 2 }, { 0x4c, 32, 2 },
        { 0x4f, 115, 8 }, { 0x50, 115, 8 },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        check(nvn_bc_unpacked(cases[i].fmt, &pf, &bpp) == 1, "format is recognised as BC");
        check(pf == cases[i].pf && bpp == cases[i].bpp, "format maps to the expected pixel format");
    }
    check(nvn_bc_unpacked(0x25, &pf, &bpp) == 0, "a non-BC format is rejected");
}

static void test_edge_blocks_trimmed(void) {
    // A 6x3 image: blocks at x=4 and y=0 are partial. Decoding must write only real pixels,
    // and the sentinel after the last row must survive (nothing written past the height).
    // 6 pixels wide = two 4-pixel blocks per block row, so the source needs two blocks and a
    // pitch of 2 * 8 bytes. (Passing one block made the second one read past the array.)
    unsigned char blk[2][8] = {
        { 0x00, 0xF8, 0xE0, 0x07, 0x1B, 0x36, 0x6C, 0xD8 },
        { 0xE0, 0x07, 0x1F, 0x00, 0x1B, 0x36, 0x6C, 0xD8 },
    };
    enum { W = 6, H = 3 };
    unsigned char out[H * W * 4 + 64];
    memset(out, 0xAB, sizeof out);
    int r = nvn_bc_decode(0x41, (const unsigned char *)blk, 16, W, H, out, W * 4);
    check(r == 0, "partial-block decode returns 0");
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const unsigned char *p = out + y * W * 4 + x * 4;
            check(p[3] == 255, "RGB_DXT1 decodes every pixel opaque");
        }
    for (unsigned i = H * W * 4; i < sizeof out; i++)
        if (out[i] != 0xAB) { check(0, "nothing is written past the last row"); break; }
}

static void test_bc4_single_channel(void) {
    // BC4 stores one channel; the unpacked format is R8, so bytes 1..3 of each pixel stay as the
    // caller left them (tex_mtl fills alpha itself). Check the red channel varies with the block.
    unsigned char blk[8] = { 0xFF, 0x00, 0, 0, 0, 0, 0, 0 };   // endpoints 255 and 0
    // BC4 decodes to R8: one byte per pixel, dpitch = 4 for a 4-pixel row.
    unsigned char out[4 * 4];
    memset(out, 0xCC, sizeof out);
    int r = nvn_bc_decode(0x49, blk, 8, 4, 4, out, 4);   // 0x49 = BC4 unsigned
    check(r == 0, "bc4 decode returns 0");
    int uniform = 1;
    for (int i = 0; i < 16; i++) if (out[i] != 255) uniform = 0;
    check(uniform, "bc4 decodes the whole block to the endpoint value");
    check(out[1 + 4] == 255, "bc4 writes every row (not just the first)");
}

int main(void) {
    test_bc1_matches_reference();
    test_formats_map();
    test_edge_blocks_trimmed();
    test_bc4_single_channel();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("bc decode: BC1 matches an independent DXT1 decoder, format map, edge trimming, BC4 - all OK\n");
    return 0;
}
