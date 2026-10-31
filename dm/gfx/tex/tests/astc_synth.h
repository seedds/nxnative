// Synthetic ASTC blocks for the tests (dm/gfx/tex/tests/t_astc.c, dm/gfx/tests/vk/t_vk_astc.c): a writer of the physical block layout from a block's parameters (weight grid,
// weight and colour ranges, dual plane, partitions, colour endpoint modes, the ISE values of its colours and weights), void-extent blocks, and a random generator of legal blocks
// of every kind. The block mode is found by searching the decoder's own mode table (astc_block_mode); the trit and quint packings by searching its unpacking, under the rule that
// bits past a sequence's end are zero. Header-only: static functions.
#pragma once
#include "../astc_decode.h"
#include <stdint.h>
#include <string.h>

typedef struct {
    int parts, dual, ccs, seed;
    int cem[4];
    int gw, gh, wr;
    int mode, cr, nvals, cstart, cem_pos, ccs_pos, wbits;   // filled by astc_synth_layout
    int cv[18];   // the colour values as the sequence holds them (below astc_levels(cr))
    int wv[64];   // the weights as the sequence holds them (below astc_levels(wr)), plane-interleaved when dual
} astc_synth;

static void sb_put(uint8_t *b, int pos, int n, uint32_t v) {
    for (int i = 0; i < n; i++) {
        int bit = pos + i;
        if (bit < 0 || bit >= 128) continue;
        if (v >> i & 1) b[bit >> 3] |= (uint8_t)(1u << (bit & 7)); else b[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
    }
}
// count values v of range r as an integer sequence at bit start (upward); 0 when a packing cannot be found (does not happen for legal input)
static int sb_put_ise(uint8_t *b, int start, int r, int count, const int *v) {
    int kind, bits = astc_range_bits(r, &kind), end = start + astc_ise_bits(count, r), mask = (1 << bits) - 1;
    if (kind == 0) { for (int i = 0; i < count; i++) sb_put(b, start + i * bits, bits, (uint32_t)v[i]); return 1; }
    int per = kind == 1 ? 5 : 3, nb = kind == 1 ? 8 : 7, blk_bits = per * bits + nb;
    for (int blk = 0; blk * per < count; blk++) {
        int base = start + blk * blk_bits, hi[5] = {0}, lo[5] = {0}, n = count - blk * per < per ? count - blk * per : per;
        for (int j = 0; j < n; j++) { hi[j] = v[blk * per + j] >> bits; lo[j] = v[blk * per + j] & mask; }
        int tpos[8] = {base + bits, base + bits + 1, base + 2 * bits + 2, base + 2 * bits + 3, base + 3 * bits + 4, base + 4 * bits + 5, base + 4 * bits + 6, base + 5 * bits + 7};
        int qpos[7] = {base + bits, base + bits + 1, base + bits + 2, base + 2 * bits + 3, base + 2 * bits + 4, base + 3 * bits + 5, base + 3 * bits + 6};
        const int *pos = kind == 1 ? tpos : qpos;
        static const int tm[5] = {0, 2, 4, 5, 7}, qm[3] = {0, 3, 5};
        int found = -1;
        for (int T = 0; T < (1 << nb) && found < 0; T++) {
            int ok = 1, d[5];
            for (int k = 0; k < nb; k++) if (pos[k] >= end && (T >> k & 1)) ok = 0;
            if (kind == 1) astc_trits(T, d); else astc_quints(T, d);
            for (int j = 0; j < n; j++) if (d[j] != hi[j]) ok = 0;
            if (ok) found = T;
        }
        if (found < 0) return 0;
        for (int k = 0; k < nb; k++) if (pos[k] < end) sb_put(b, pos[k], 1, (uint32_t)(found >> k & 1));
        for (int j = 0; j < n; j++) sb_put(b, base + j * bits + (kind == 1 ? tm[j] : qm[j]), bits, (uint32_t)lo[j]);
    }
    return 1;
}
// The layout of s for a bw x bh block: its mode, its colour range and where its fields go; 0 when no legal block has these parameters
static int astc_synth_layout(astc_synth *s, int bw, int bh) {
    s->mode = -1;
    for (int m = 0; m < 2048 && s->mode < 0; m++) {
        int gw, gh, d, wr;
        if (astc_block_mode(m, &gw, &gh, &d, &wr) && gw == s->gw && gh == s->gh && d == s->dual && wr == s->wr) s->mode = m;
    }
    if (s->mode < 0 || s->gw > bw || s->gh > bh || s->parts < 1 || s->parts > 4 || (s->dual && s->parts == 4)) return 0;
    int count = s->gw * s->gh * (s->dual + 1);
    s->wbits = astc_ise_bits(count, s->wr);
    if (count > 64 || s->wbits < 24 || s->wbits > 96) return 0;
    int below = 128 - s->wbits, same = 1, minc = 4, maxc = -1;
    s->nvals = 0;
    for (int i = 0; i < s->parts; i++) {
        same &= s->cem[i] == s->cem[0]; s->nvals += 2 * ((s->cem[i] >> 2) + 1);
        if (s->cem[i] >> 2 < minc) minc = s->cem[i] >> 2;
        if (s->cem[i] >> 2 > maxc) maxc = s->cem[i] >> 2;
    }
    s->cem_pos = -1; s->ccs_pos = -1;
    if (s->parts > 1 && !same) { if (maxc - minc > 1 || minc > 2 + (maxc == minc)) return 0; below -= 3 * s->parts - 4; s->cem_pos = below; }
    if (s->dual) { below -= 2; s->ccs_pos = below; }
    if (s->nvals > 18) return 0;
    s->cstart = s->parts == 1 ? 17 : 29;
    s->cr = -1;
    for (int r = 20; r >= 0 && s->cr < 0; r--) if (astc_ise_bits(s->nvals, r) <= below - s->cstart) s->cr = r;
    return s->cr >= 4;
}
// The block of a laid-out s
static int astc_synth_pack(const astc_synth *s, uint8_t out[16]) {
    memset(out, 0, 16);
    sb_put(out, 0, 11, (uint32_t)s->mode); sb_put(out, 11, 2, (uint32_t)(s->parts - 1));
    if (s->parts == 1) sb_put(out, 13, 4, (uint32_t)s->cem[0]);
    else {
        sb_put(out, 13, 10, (uint32_t)s->seed);
        if (s->cem_pos < 0) sb_put(out, 23, 6, (uint32_t)s->cem[0] << 2);
        else {
            int base = 4;
            for (int i = 0; i < s->parts; i++) if (s->cem[i] >> 2 < base) base = s->cem[i] >> 2;
            if (base == 3) base = 2;   // a selector names base classes 0..2; class 3 is base 2 with its class bit set
            uint32_t enc = (uint32_t)(base + 1);
            for (int i = 0; i < s->parts; i++) enc |= (uint32_t)((s->cem[i] >> 2) - base) << (2 + i);
            for (int i = 0; i < s->parts; i++) enc |= (uint32_t)(s->cem[i] & 3) << (2 + s->parts + 2 * i);
            sb_put(out, 23, 6, enc & 63); sb_put(out, s->cem_pos, 3 * s->parts - 4, enc >> 6);
        }
    }
    if (s->dual) sb_put(out, s->ccs_pos, 2, (uint32_t)s->ccs);
    if (!sb_put_ise(out, s->cstart, s->cr, s->nvals, s->cv)) return 0;
    uint8_t rb[16]; memset(rb, 0, sizeof rb);
    if (!sb_put_ise(rb, 0, s->wr, s->gw * s->gh * (s->dual + 1), s->wv)) return 0;
    for (int i = 0; i < s->wbits; i++) sb_put(out, 127 - i, 1, (uint32_t)(rb[i >> 3] >> (i & 7) & 1));   // the weights read from bit 127 down
    return 1;
}
// A void-extent block: hdr sets the dynamic range bit, reserved the two reserved bits, ext the four 13-bit coordinates (min s, max s, min t, max t), c the colour
static void astc_synth_void(uint8_t out[16], int hdr, int reserved, const int ext[4], const uint16_t c[4]) {
    memset(out, 0, 16);
    sb_put(out, 0, 9, 0x1FC); sb_put(out, 9, 1, (uint32_t)hdr); sb_put(out, 10, 2, (uint32_t)reserved);
    for (int i = 0; i < 4; i++) sb_put(out, 12 + 13 * i, 13, (uint32_t)ext[i]);
    for (int i = 0; i < 4; i++) sb_put(out, 64 + 16 * i, 16, c[i]);
}

static uint32_t astc_synth_rnd(uint64_t *s) { *s = *s * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(*s >> 33); }
// A random legal block of bw x bh: its kind by k (0..15: 0 a void-extent block, 1 an illegal block, 2 a block with an HDR endpoint mode in one partition, else a normal LDR
// block of random partitions, dual plane, grid, ranges and values); *s_out (may be NULL) its parameters for the normal kinds. Returns the kind made (ASTC_*, 3 for HDR)
static int astc_synth_random(uint64_t *rng, int bw, int bh, int k, uint8_t out[16], astc_synth *s_out) {
    static const int ldr[10] = {0, 1, 4, 5, 6, 8, 9, 10, 12, 13}, hdr[6] = {2, 3, 7, 11, 14, 15};
    if (k == 0) {
        uint16_t c[4]; for (int i = 0; i < 4; i++) c[i] = (uint16_t)astc_synth_rnd(rng);
        int ones = astc_synth_rnd(rng) & 1, ext[4] = {0x1FFF, 0x1FFF, 0x1FFF, 0x1FFF};
        if (!ones) { ext[0] = (int)(astc_synth_rnd(rng) % 4000); ext[1] = ext[0] + 1 + (int)(astc_synth_rnd(rng) % 4000); ext[2] = (int)(astc_synth_rnd(rng) % 4000); ext[3] = ext[2] + 1 + (int)(astc_synth_rnd(rng) % 4000); }
        astc_synth_void(out, 0, 3, ext, c);
        return ASTC_VOID_EXTENT;
    }
    if (k == 1) {   // illegal: one of the rules of the specification's list, at random
        for (;;) {
            for (int i = 0; i < 16; i++) out[i] = (uint8_t)astc_synth_rnd(rng);
            astc_hdr h; if (astc_header(out, bw, bh, &h) == ASTC_ERROR) return ASTC_ERROR;
        }
    }
    for (int tries = 0; tries < 100000; tries++) {
        astc_synth s; memset(&s, 0, sizeof s);
        s.parts = 1 + (int)(astc_synth_rnd(rng) % 4); s.dual = s.parts < 4 && astc_synth_rnd(rng) % 3 == 0; s.ccs = (int)(astc_synth_rnd(rng) % 4);
        s.seed = (int)(astc_synth_rnd(rng) % 1024);
        int c0 = ldr[astc_synth_rnd(rng) % 10];
        for (int i = 0; i < s.parts; i++) {
            int c = c0;
            if (astc_synth_rnd(rng) & 1) { int cls = (c0 >> 2) + (int)(astc_synth_rnd(rng) % 2); if (cls > 3) cls = 3; c = cls << 2 | (int)(astc_synth_rnd(rng) % 4); }
            s.cem[i] = c;
        }
        if (k == 2) s.cem[astc_synth_rnd(rng) % (unsigned)s.parts] = hdr[astc_synth_rnd(rng) % 6];
        int any_hdr = 0;
        for (int i = 0; i < s.parts; i++) for (int j = 0; j < 6; j++) any_hdr |= s.cem[i] == hdr[j];
        if (any_hdr != (k == 2)) continue;
        s.gw = 2 + (int)(astc_synth_rnd(rng) % (unsigned)(bw - 1)); s.gh = 2 + (int)(astc_synth_rnd(rng) % (unsigned)(bh - 1)); s.wr = (int)(astc_synth_rnd(rng) % 12);
        if (!astc_synth_layout(&s, bw, bh)) continue;
        for (int i = 0; i < s.nvals; i++) s.cv[i] = (int)(astc_synth_rnd(rng) % (unsigned)astc_levels(s.cr));
        for (int i = 0; i < s.gw * s.gh * (s.dual + 1); i++) s.wv[i] = (int)(astc_synth_rnd(rng) % (unsigned)astc_levels(s.wr));
        if (!astc_synth_pack(&s, out)) continue;
        if (s_out) *s_out = s;
        return k == 2 ? 3 : ASTC_NORMAL;
    }
    return -1;
}
