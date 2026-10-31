// Unit test of the ASTC LDR decoder (dm/gfx/tex/astc_decode.c; plain C, every host): blocks the test writes itself (tests/astc_synth.h) and whose texels it can predict from the
// specification's rules, written again here where a check needs them (the endpoint formulas of a mode, the 8-bit interpolation, the weight infill), and the specification's
// tables where it gives values:
//   A the unquantization tables: every colour range 0..5 to 0..255 and weight range 0..1 to 0..31 spans its scale evenly, with the values the specification's examples list;
//   B the trit and quint packings: every tuple of trits (243) and quints (125) has one;
//   C the integer sequence encoding: every range and length round trips, with garbage past the sequence's end ignored;
//   D the block modes: every weight grid, range and plane count a block size allows is found and decoded back, reserved modes are errors;
//   E void-extent blocks, linear and sRGB, and the illegal ones;
//   F the other illegal encodings of the specification's list, every texel the error colour;
//   G each LDR colour endpoint mode, its endpoints and the interpolated texels at weights 0, 21, 43 and 64, linear and sRGB, with blue contraction and clamping;
//   H dual plane, for each of the four components;
//   I partitions 2 to 4: each texel takes its partition's colour, every partition reachable;
//   J weight grids smaller than the block, against the infill written again here;
//   K an HDR endpoint mode in one partition: the error colour in that partition's texels only;
//   L random legal blocks of every kind and block size: the per-texel decode (the GPU's shape) equals the whole-block decode.
// The general case against an independent decoder is the GPU test's (dm/gfx/tests/vk/t_vk_astc.c: lavapipe's own ASTC decoder).
#include "../astc_decode.h"
#include "astc_synth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail, total;
static void t(const char *name, int ok) { total++; fail += !ok; if (!ok) printf("FAIL %s\n", name); }
static const int BS[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
static int interp8(int e0, int e1, int w, int srgb) {   // the specification's interpolation and 8-bit result, written again
    int c0 = srgb ? e0 << 8 | 0x80 : e0 * 257, c1 = srgb ? e1 << 8 | 0x80 : e1 * 257;
    return ((c0 * (64 - w) + c1 * w + 32) >> 6) >> 8;
}
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static void words(const uint8_t b[16], uint32_t w[4]) { for (int i = 0; i < 4; i++) w[i] = (uint32_t)b[4 * i] | (uint32_t)b[4 * i + 1] << 8 | (uint32_t)b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24; }
// A one-partition 4x4 block of mode cem with its colour values as given (range 0..255: they unquantize to themselves) and the weight pattern 0, 21, 43, 64 (range 0..3) along x
static int mode_block(int cem, const int *v, int srgb, int e0[4], int e1[4], uint8_t texels[16][4], int *range) {
    astc_synth s; memset(&s, 0, sizeof s);
    s.parts = 1; s.cem[0] = cem; s.gw = 4; s.gh = 4; s.wr = 2;
    if (!astc_synth_layout(&s, 4, 4)) return 0;
    *range = s.cr;
    for (int i = 0; i < s.nvals; i++) s.cv[i] = v[i];
    for (int i = 0; i < 16; i++) s.wv[i] = i % 4;
    uint8_t b[16]; if (!astc_synth_pack(&s, b)) return 0;
    astc_hdr h; astc_header(b, 4, 4, &h);
    uint8_t px[16 * 4]; astc_decode_block(b, 4, 4, srgb, px, 16);
    memcpy(texels, px, sizeof px);
    int uv[8] = {0}; for (int i = 0; i < s.nvals; i++) uv[i] = astc_unq_color(s.cr, v[i]);
    astc_endpoints(cem, uv, e0, e1);
    return h.kind == ASTC_NORMAL;
}

int main(void) {
    char name[256];
    // ---- A: unquantization ----
    {
        static const int c6[6] = {0, 255, 51, 204, 102, 153}, c12[12] = {0, 255, 69, 186, 23, 232, 92, 163, 46, 209, 116, 139};
        static const int c24[24] = {0, 255, 33, 222, 66, 189, 99, 156, 11, 244, 44, 211, 77, 178, 110, 145, 22, 233, 55, 200, 88, 167, 121, 134};
        static const int w6[6] = {0, 64, 12, 52, 25, 39}, w12[12] = {0, 64, 17, 47, 5, 59, 23, 41, 11, 53, 28, 36};
        static const int w24[24] = {0, 64, 8, 56, 16, 48, 24, 40, 2, 62, 11, 53, 19, 45, 27, 37, 5, 59, 13, 51, 22, 42, 30, 34};
        int ok = 1; for (int i = 0; i < 6; i++) ok &= astc_unq_color(4, i) == c6[i] && astc_unq_weight(4, i) == w6[i];
        t("A colour and weight unquantization of range 0..5 as the specification lists it", ok);
        ok = 1; for (int i = 0; i < 12; i++) ok &= astc_unq_color(7, i) == c12[i] && astc_unq_weight(7, i) == w12[i];
        t("A colour and weight unquantization of range 0..11 as the specification lists it", ok);
        ok = 1; for (int i = 0; i < 24; i++) ok &= astc_unq_color(10, i) == c24[i] && astc_unq_weight(10, i) == w24[i];
        t("A colour and weight unquantization of range 0..23 as the specification lists it", ok);
        for (int r = 0; r < 21; r++) {
            int L = astc_levels(r), vals[256], even = 1, distinct = 1;
            if (r >= 4) {
                for (int i = 0; i < L; i++) vals[i] = astc_unq_color(r, i);
                qsort(vals, (size_t)L, sizeof *vals, cmp_int);
                for (int i = 0; i < L; i++) { double ideal = 255.0 * i / (L - 1); even &= vals[i] >= ideal - 1.5 && vals[i] <= ideal + 1.5; if (i) distinct &= vals[i] > vals[i - 1]; }
                snprintf(name, sizeof name, "A colour range of %d levels: distinct values spread evenly over 0..255", L); t(name, even && distinct && vals[0] == 0 && vals[L - 1] == 255);
            }
            if (r < 12) {
                even = 1; distinct = 1;
                for (int i = 0; i < L; i++) vals[i] = astc_unq_weight(r, i);
                qsort(vals, (size_t)L, sizeof *vals, cmp_int);
                for (int i = 0; i < L; i++) { double ideal = 64.0 * i / (L - 1); even &= vals[i] >= ideal - 1.5 && vals[i] <= ideal + 1.5; if (i) distinct &= vals[i] > vals[i - 1]; }
                snprintf(name, sizeof name, "A weight range of %d levels: distinct values spread evenly over 0..64", L); t(name, even && distinct && vals[0] == 0 && vals[L - 1] == 64);
            }
        }
        t("A sequence lengths: 5 values of a trit range of 1 bit take 13 bits, 3 of a quint range of 1 bit 10, 7 of 8 bits 56",
          astc_ise_bits(5, 4) == 13 && astc_ise_bits(3, 6) == 10 && astc_ise_bits(7, 20) == 56 && astc_ise_bits(1, 4) == 3 && astc_ise_bits(2, 6) == 7);
    }
    // ---- B: trits and quints ----
    {
        int seen[243] = {0}, qseen[125] = {0}, inrange = 1;
        for (int T = 0; T < 256; T++) { int d[5]; astc_trits(T, d); int k = 0; for (int j = 4; j >= 0; j--) { inrange &= d[j] >= 0 && d[j] <= 2; k = k * 3 + d[j]; } if (k >= 0 && k < 243) seen[k] = 1; }
        for (int Q = 0; Q < 128; Q++) { int d[3]; astc_quints(Q, d); int k = 0; for (int j = 2; j >= 0; j--) { inrange &= d[j] >= 0 && d[j] <= 4; k = k * 5 + d[j]; } if (k >= 0 && k < 125) qseen[k] = 1; }
        int all = 1; for (int k = 0; k < 243; k++) all &= seen[k];
        t("B every tuple of five trits has a packing, and every packing gives trits", all && inrange);
        all = 1; for (int k = 0; k < 125; k++) all &= qseen[k];
        t("B every tuple of three quints has a packing, and every packing gives quints", all);
    }
    // ---- C: the integer sequence encoding ----
    {
        uint64_t rng = 1; int ok = 1, runs = 0;
        for (int r = 0; r < 21; r++)
            for (int n = 1; n <= 64; n++) {
                int start = 3 + (int)(astc_synth_rnd(&rng) % 8);
                if (start + astc_ise_bits(n, r) > 128) break;
                int v[64]; for (int i = 0; i < n; i++) v[i] = (int)(astc_synth_rnd(&rng) % (unsigned)astc_levels(r));
                uint8_t b[16]; memset(b, 0xff, sizeof b);   // garbage around the sequence: ignored
                if (!sb_put_ise(b, start, r, n, v)) { ok = 0; continue; }
                uint32_t w[4]; words(b, w);
                for (int i = 0; i < n; i++) ok &= astc_ise_value(w, start, start + astc_ise_bits(n, r), r, i) == v[i];
                runs++;
            }
        snprintf(name, sizeof name, "C %d sequences of every range and length round trip, garbage past the end ignored", runs); t(name, ok);
    }
    // ---- D: block modes ----
    {
        int reserved = 0, reserved_err = 1;
        for (int m = 0; m < 2048; m++) {
            int gw, gh, d, wr;
            if ((m & 0x1FF) == 0x1FC || astc_block_mode(m, &gw, &gh, &d, &wr)) continue;
            reserved++;
            uint8_t b[16]; memset(b, 0, sizeof b); sb_put(b, 0, 11, (uint32_t)m);
            astc_hdr h; reserved_err &= astc_header(b, 12, 12, &h) == ASTC_ERROR;
        }
        snprintf(name, sizeof name, "D the %d reserved block modes are errors", reserved); t(name, reserved > 0 && reserved_err);
        for (int k = 0; k < 14; k++) {
            int bw = BS[k][0], bh = BS[k][1], found = 0, ok = 1;
            for (int gw = 2; gw <= bw; gw++) for (int gh = 2; gh <= bh; gh++) for (int d = 0; d < 2; d++) for (int wr = 0; wr < 12; wr++) {
                int count = gw * gh * (d + 1), bits = astc_ise_bits(count, wr);
                if (count > 64 || bits < 24 || bits > 96) continue;
                astc_synth s; memset(&s, 0, sizeof s); s.parts = 1; s.cem[0] = 0; s.gw = gw; s.gh = gh; s.dual = d; s.wr = wr;
                if (!astc_synth_layout(&s, bw, bh)) continue;   // no block mode has this grid (the mode table's shapes), or no colour bits are left
                uint8_t b[16]; if (!astc_synth_pack(&s, b)) { ok = 0; continue; }
                astc_hdr h; astc_header(b, bw, bh, &h);
                ok &= h.kind == ASTC_NORMAL && h.gw == gw && h.gh == gh && h.dual == d && h.wr == wr;
                found++;
            }
            snprintf(name, sizeof name, "D block %dx%d: %d grids, ranges and plane counts encoded and decoded back", bw, bh, found); t(name, ok && found > 0);
        }
    }
    // ---- E: void-extent blocks ----
    {
        const uint16_t c[4] = {0x1234, 0xabcd, 0x80ff, 0xff00}; uint8_t b[16], px[16 * 16 * 4];
        const int ones[4] = {0x1FFF, 0x1FFF, 0x1FFF, 0x1FFF}, ext[4] = {10, 20, 30, 4000}, bad[4] = {20, 20, 30, 4000};
        for (int srgb = 0; srgb < 2; srgb++) {
            astc_synth_void(b, 0, 3, ones, c);
            int kind = astc_decode_block(b, 6, 6, srgb, px, 6 * 4), ok = kind == ASTC_VOID_EXTENT;
            for (int i = 0; i < 36; i++) ok &= px[4 * i] == 0x12 && px[4 * i + 1] == 0xab && px[4 * i + 2] == 0x80 && px[4 * i + 3] == 0xff;
            snprintf(name, sizeof name, "E void-extent block, every coordinate one (%s): the top 8 bits of its UNORM16 colour", srgb ? "sRGB" : "linear"); t(name, ok);
            astc_synth_void(b, 0, 3, ext, c);
            ok = astc_decode_block(b, 6, 6, srgb, px, 6 * 4) == ASTC_VOID_EXTENT && px[0] == 0x12 && px[35 * 4 + 1] == 0xab;
            snprintf(name, sizeof name, "E void-extent block with an extent (%s)", srgb ? "sRGB" : "linear"); t(name, ok);
        }
        struct { int hdr, res; const int *e; const char *what; } errs[] = {{1, 3, ones, "the HDR bit (LDR profile)"}, {0, 1, ones, "a reserved bit 0"}, {0, 3, bad, "a low coordinate not below the high one"}};
        for (int i = 0; i < 3; i++) {
            astc_synth_void(b, errs[i].hdr, errs[i].res, errs[i].e, c);
            int ok = astc_decode_block(b, 4, 4, 0, px, 16) == ASTC_ERROR;
            for (int j = 0; j < 16; j++) ok &= px[4 * j] == 255 && px[4 * j + 1] == 0 && px[4 * j + 2] == 255 && px[4 * j + 3] == 255;
            snprintf(name, sizeof name, "E void-extent block with %s: an error block (magenta)", errs[i].what); t(name, ok);
        }
    }
    // ---- F: illegal encodings ----
    {
        struct { int bw, bh, gw, gh, dual, wr, parts, cem; const char *what; } cases[] = {
            {4, 4, 5, 4, 0, 2, 1, 8, "a weight grid wider than the block"},
            {6, 6, 6, 8, 0, 0, 1, 8, "a weight grid taller than the block"},
            {12, 12, 9, 9, 0, 0, 1, 0, "more than 64 weights (9x9)"},
            {4, 4, 3, 3, 0, 0, 1, 0, "fewer than 24 weight bits (3x3 of 1 bit)"},
            {12, 12, 8, 8, 0, 5, 1, 0, "more than 96 weight bits (8x8 of 3 bits)"},
            {8, 8, 4, 4, 1, 0, 4, 0, "dual plane with four partitions"},
            {8, 8, 4, 4, 0, 4, 4, 12, "more than 18 colour values (four RGBA partitions)"},
            {10, 10, 6, 10, 0, 0, 3, 8, "fewer colour bits than range 0..5 needs (three RGB partitions, 60 weight bits)"},
        };
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
            // written by hand: the mode for the grid when one exists, the partition count and a shared mode; no colour or weight data is needed for the decoder to refuse it
            uint8_t b[16]; memset(b, 0, sizeof b);
            int mode = -1;
            for (int m = 0; m < 2048 && mode < 0; m++) { int gw, gh, d, wr; if (astc_block_mode(m, &gw, &gh, &d, &wr) && gw == cases[i].gw && gh == cases[i].gh && d == cases[i].dual && wr == cases[i].wr) mode = m; }
            if (mode < 0) { snprintf(name, sizeof name, "F %s: a block mode exists for it", cases[i].what); t(name, 0); continue; }
            sb_put(b, 0, 11, (uint32_t)mode); sb_put(b, 11, 2, (uint32_t)(cases[i].parts - 1));
            if (cases[i].parts == 1) sb_put(b, 13, 4, (uint32_t)cases[i].cem); else sb_put(b, 23, 6, (uint32_t)cases[i].cem << 2);
            uint8_t px[12 * 12 * 4]; astc_hdr h; astc_header(b, cases[i].bw, cases[i].bh, &h);
            int ok = astc_decode_block(b, cases[i].bw, cases[i].bh, 0, px, (size_t)cases[i].bw * 4) == ASTC_ERROR;
            for (int j = 0; j < cases[i].bw * cases[i].bh; j++) ok &= px[4 * j] == 255 && px[4 * j + 1] == 0 && px[4 * j + 2] == 255 && px[4 * j + 3] == 255;
            snprintf(name, sizeof name, "F %s: an error block (%s)", cases[i].what, h.why ? h.why : "-"); t(name, ok);
        }
    }
    // ---- G: the LDR colour endpoint modes ----
    {
        struct { int cem; int v[8]; int e0[4], e1[4]; const char *what; } cases[] = {
            {0, {40, 200}, {40, 40, 40, 255}, {200, 200, 200, 255}, "luminance, direct"},
            {1, {0x84, 0xFF}, {0xE1, 0xE1, 0xE1, 255}, {0xFF, 0xFF, 0xFF, 255}, "luminance, base + offset (clamped)"},
            {1, {0x40, 0x05}, {0x10, 0x10, 0x10, 255}, {0x15, 0x15, 0x15, 255}, "luminance, base + offset"},
            {4, {10, 250, 30, 220}, {10, 10, 10, 30}, {250, 250, 250, 220}, "luminance and alpha, direct"},
            {5, {100, 0x3C, 200, 0xBE}, {50, 50, 50, 228}, {80, 80, 80, 255}, "luminance and alpha, base + offset (clamped alpha)"},
            {6, {200, 100, 50, 128}, {100, 50, 25, 255}, {200, 100, 50, 255}, "RGB, base + scale"},
            {8, {10, 200, 20, 190, 30, 180}, {10, 20, 30, 255}, {200, 190, 180, 255}, "RGB, direct"},
            {8, {200, 10, 190, 20, 180, 30}, {20, 25, 30, 255}, {190, 185, 180, 255}, "RGB, direct, blue contraction"},
            {9, {100, 20, 100, 20, 100, 20}, {50, 50, 50, 255}, {60, 60, 60, 255}, "RGB, base + offset"},
            {9, {100, 0x7E, 100, 0x7E, 100, 0x7E}, {49, 49, 49, 255}, {50, 50, 50, 255}, "RGB, base + offset, negative: blue contraction"},
            {10, {200, 100, 50, 128, 30, 220}, {100, 50, 25, 30}, {200, 100, 50, 220}, "RGB base + scale, two alphas"},
            {12, {10, 200, 20, 190, 30, 180, 40, 170}, {10, 20, 30, 40}, {200, 190, 180, 170}, "RGBA, direct"},
            {12, {200, 10, 190, 20, 180, 30, 170, 40}, {20, 25, 30, 40}, {190, 185, 180, 170}, "RGBA, direct, blue contraction"},
            {13, {100, 20, 100, 20, 100, 20, 100, 20}, {50, 50, 50, 50}, {60, 60, 60, 60}, "RGBA, base + offset"},
        };
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
            for (int srgb = 0; srgb < 2; srgb++) {
                int e0[4], e1[4], r = 0; uint8_t px[16][4];
                int ok = mode_block(cases[i].cem, cases[i].v, srgb, e0, e1, px, &r) && r == 20;
                for (int c = 0; c < 4; c++) ok &= e0[c] == cases[i].e0[c] && e1[c] == cases[i].e1[c];
                static const int wq[4] = {0, 21, 43, 64};
                for (int p = 0; p < 16; p++) for (int c = 0; c < 4; c++) ok &= px[p][c] == interp8(cases[i].e0[c], cases[i].e1[c], wq[p % 4], srgb);
                snprintf(name, sizeof name, "G mode %d (%s), %s: endpoints and the texels at weights 0, 21, 43, 64", cases[i].cem, cases[i].what, srgb ? "sRGB" : "linear");
                t(name, ok);
                if (!ok) printf("  e0 %d %d %d %d e1 %d %d %d %d (range %d) texel 1: %d %d %d %d\n", e0[0], e0[1], e0[2], e0[3], e1[0], e1[1], e1[2], e1[3], r, px[1][0], px[1][1], px[1][2], px[1][3]);
            }
    }
    // ---- H: dual plane ----
    for (int ccs = 0; ccs < 4; ccs++) {
        astc_synth s; memset(&s, 0, sizeof s);
        s.parts = 1; s.cem[0] = 12; s.gw = 4; s.gh = 4; s.wr = 2; s.dual = 1; s.ccs = ccs;
        int ok = astc_synth_layout(&s, 4, 4);
        static const int v[8] = {10, 200, 20, 190, 30, 180, 40, 170}, e0[4] = {10, 20, 30, 40}, e1[4] = {200, 190, 180, 170};
        for (int i = 0; i < s.nvals; i++) s.cv[i] = v[i] * (astc_levels(s.cr) - 1) / 255;   // the range the layout leaves; endpoints from their unquantized values
        int uv[8]; for (int i = 0; i < 8; i++) uv[i] = astc_unq_color(s.cr, s.cv[i]);
        int ee0[4], ee1[4]; astc_endpoints(12, uv, ee0, ee1);
        for (int i = 0; i < 16; i++) { s.wv[2 * i] = 0; s.wv[2 * i + 1] = 3; }   // plane 0 weight 0, plane 1 weight 64
        uint8_t b[16], px[16 * 4]; ok = ok && astc_synth_pack(&s, b) && astc_decode_block(b, 4, 4, 0, px, 16) == ASTC_NORMAL;
        for (int p = 0; p < 16; p++) for (int c = 0; c < 4; c++) ok &= px[4 * p + c] == (c == ccs ? ee1[c] : ee0[c]);
        (void)e0; (void)e1;
        snprintf(name, sizeof name, "H dual plane, component %d on the second plane: that component from endpoint 1, the others from endpoint 0", ccs); t(name, ok);
    }
    // ---- I: partitions ----
    for (int parts = 2; parts <= 4; parts++)
        for (int k = 0; k < 14; k += 13) {
            int bw = BS[k][0], bh = BS[k][1], ok = 1, seen[4] = {0};
            for (int seed = 0; seed < 1024; seed += 97) {
                astc_synth s; memset(&s, 0, sizeof s);
                s.parts = parts; s.seed = seed; s.gw = 4; s.gh = 4; s.wr = 2;
                for (int p = 0; p < parts; p++) s.cem[p] = 0;
                if (!astc_synth_layout(&s, bw, bh)) { ok = 0; continue; }
                int lum[4] = {20, 90, 160, 230};
                for (int p = 0; p < parts; p++) { int q = lum[p] * (astc_levels(s.cr) - 1) / 255; s.cv[2 * p] = q; s.cv[2 * p + 1] = q; }
                for (int i = 0; i < 16; i++) s.wv[i] = (int)(i * 7 % 4);
                uint8_t b[16], px[12 * 12 * 4]; ok &= astc_synth_pack(&s, b) && astc_decode_block(b, bw, bh, 0, px, (size_t)bw * 4) == ASTC_NORMAL;
                for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
                    int p = astc_partition(seed, parts, x, y, bw * bh < 31); seen[p] = 1;
                    int want = astc_unq_color(s.cr, s.cv[2 * p]);
                    ok &= px[(y * bw + x) * 4] == want && px[(y * bw + x) * 4 + 3] == 255;
                }
            }
            int all = 1; for (int p = 0; p < parts; p++) all &= seen[p];
            snprintf(name, sizeof name, "I %d partitions, block %dx%d, 11 patterns: each texel its partition's colour, every partition reached", parts, bw, bh); t(name, ok && all);
        }
    // ---- J: weight infill, written again from the specification ----
    {
        uint64_t rng = 7; int ok = 1, n = 0;
        for (int k = 0; k < 14; k++) {
            int bw = BS[k][0], bh = BS[k][1];
            for (int rep = 0; rep < 6; rep++) {
                astc_synth s; memset(&s, 0, sizeof s);
                s.parts = 1; s.cem[0] = 0; s.gw = 2 + (int)(astc_synth_rnd(&rng) % (unsigned)(bw - 1)); s.gh = 2 + (int)(astc_synth_rnd(&rng) % (unsigned)(bh - 1)); s.wr = (int)(astc_synth_rnd(&rng) % 12);
                if (!astc_synth_layout(&s, bw, bh)) { rep--; continue; }
                s.cv[0] = 0;   // luminance 0 to 255: the texel is the interpolation of 0 and 65535 by the weight (the value that unquantizes to 255: the order is permuted)
                for (int v = 0; v < astc_levels(s.cr); v++) if (astc_unq_color(s.cr, v) == 255) s.cv[1] = v;
                int wq[64];
                for (int i = 0; i < s.gw * s.gh; i++) { s.wv[i] = (int)(astc_synth_rnd(&rng) % (unsigned)astc_levels(s.wr)); wq[i] = astc_unq_weight(s.wr, s.wv[i]); }
                uint8_t b[16], px[12 * 12 * 4]; ok &= astc_synth_pack(&s, b) && astc_decode_block(b, bw, bh, 0, px, (size_t)bw * 4) == ASTC_NORMAL;
                int Ds = (1024 + bw / 2) / (bw - 1), Dt = (1024 + bh / 2) / (bh - 1);
                for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
                    int gs = (Ds * x * (s.gw - 1) + 32) >> 6, gt = (Dt * y * (s.gh - 1) + 32) >> 6, js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
                    int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11, v0 = js + jt * s.gw, N = s.gw * s.gh;
                    int p00 = wq[v0], p01 = v0 + 1 < N ? wq[v0 + 1] : 0, p10 = v0 + s.gw < N ? wq[v0 + s.gw] : 0, p11 = v0 + s.gw + 1 < N ? wq[v0 + s.gw + 1] : 0;
                    int w = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
                    ok &= px[(y * bw + x) * 4] == interp8(0, 255, w, 0);
                }
                n++;
            }
        }
        snprintf(name, sizeof name, "J %d weight grids smaller than their block, every block size: the infill's texels", n); t(name, ok);
    }
    // ---- K: an HDR endpoint mode in one partition ----
    {
        astc_synth s; memset(&s, 0, sizeof s);
        s.parts = 2; s.seed = 33; s.gw = 4; s.gh = 4; s.wr = 2; s.cem[0] = 0; s.cem[1] = 2;   // luminance direct; HDR luminance, large range
        int ok = astc_synth_layout(&s, 8, 8), m = 0, l = 0;
        for (int i = 0; i < s.nvals; i++) s.cv[i] = astc_levels(s.cr) / 2;
        uint8_t b[16], px[64 * 4]; ok = ok && astc_synth_pack(&s, b) && astc_decode_block(b, 8, 8, 0, px, 32) == ASTC_NORMAL;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            const uint8_t *p = px + (y * 8 + x) * 4; int magenta = p[0] == 255 && p[1] == 0 && p[2] == 255 && p[3] == 255;
            if (astc_partition(33, 2, x, y, 0) == 1) { ok &= magenta; m++; } else { ok &= !magenta && p[0] == p[1]; l++; }
        }
        snprintf(name, sizeof name, "K an HDR mode in partition 1 of 2: its %d texels the error colour, the other %d decoded", m, l); t(name, ok && m && l);
    }
    // ---- L: random blocks ----
    {
        uint64_t rng = 12345; long texels = 0; int kinds[4] = {0}, ok = 1;
        for (int k = 0; k < 14; k++) {
            int bw = BS[k][0], bh = BS[k][1];
            for (int i = 0; i < 400; i++) {
                uint8_t b[16]; int kind = astc_synth_random(&rng, bw, bh, i % 16, b, NULL);
                if (kind < 0) { ok = 0; continue; }
                kinds[kind]++;
                astc_hdr h; int hk = astc_header(b, bw, bh, &h);
                ok &= kind == 3 ? hk == ASTC_NORMAL : hk == kind;
                uint8_t px[12 * 12 * 4]; astc_decode_block(b, bw, bh, i & 1, px, (size_t)bw * 4);
                for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) { uint8_t q[4]; astc_texel(b, &h, bw, bh, i & 1, x, y, q); ok &= !memcmp(q, px + (y * bw + x) * 4, 4); texels++; }
            }
        }
        snprintf(name, sizeof name, "L %d random blocks (%d normal, %d void-extent, %d illegal, %d with an HDR partition), %ld texels: per-texel decode = block decode",
                 kinds[0] + kinds[1] + kinds[2] + kinds[3], kinds[0], kinds[1], kinds[2], kinds[3], texels);
        t(name, ok);
        printf("%s\n", name);
    }
    printf("== t_astc: %d checks, %d failed ==\n", total, fail);
    return fail != 0;
}
