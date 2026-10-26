// The code of the BC → ASTC transcoder, shared between C and Metal: bc2astc_core.h includes it as it is (BCA_BODY passes the code through) and bc2astc_msl.h turns it into the text of the
// Metal kernel (BCA_BODY stringifies it). Must contain no preprocessor directives and no string or character literals. Rules for editing: see bc2astc_core.h
BCA_BODY(
// Layout ids returned by bca_transcode (statistics and tests)
enum { BCA_L0_SOLID = 0, BCA_L1A_RGB = 1, BCA_L1B_RGBA_UNIFORM = 2, BCA_L2_BC1_PUNCH = 3, BCA_L3_ALPHA = 4, BCA_SWAPPED = 0x100 };   // the return value is a layout id, or'ed with BCA_SWAPPED when the endpoints were swapped

// ASTC integer sequence encoding of five trits: index t0 + 3*t1 + 9*t2 + 27*t3 + 81*t4 → the 8-bit T value of the spec (C.2.12); the inverse of the spec's decode (checked in tests)
BCA_K u8 bca_trit_enc[243] = {
    0,1,2,4,5,6,8,9,10,16,17,18,20,21,22,24,25,26,3,7,11,19,23,27,12,13,14,
    32,33,34,36,37,38,40,41,42,48,49,50,52,53,54,56,57,58,35,39,43,51,55,59,44,45,46,
    64,65,66,68,69,70,72,73,74,80,81,82,84,85,86,88,89,90,67,71,75,83,87,91,76,77,78,
    128,129,130,132,133,134,136,137,138,144,145,146,148,149,150,152,153,154,131,135,139,147,151,155,140,141,142,
    160,161,162,164,165,166,168,169,170,176,177,178,180,181,182,184,185,186,163,167,171,179,183,187,172,173,174,
    192,193,194,196,197,198,200,201,202,208,209,210,212,213,214,216,217,218,195,199,203,211,215,219,204,205,206,
    96,97,98,100,101,102,104,105,106,112,113,114,116,117,118,120,121,122,99,103,107,115,119,123,108,109,110,
    224,225,226,228,229,230,232,233,234,240,241,242,244,245,246,248,249,250,227,231,235,243,247,251,236,237,238,
    28,29,30,60,61,62,92,93,94,156,157,158,188,189,190,220,221,222,31,63,95,159,191,223,124,125,126,
};
// The 48-level endpoint range (trit + 4 bits). The spec scrambles the order of the raw code v = trit * 16 + m against the unquantized value, so the fitting works on levels L = 0..47
// sorted by value: bca_unq48[L] is the 8-bit value of level L and bca_raw48[L] the code that is written to the block (both read out of astcenc's decode, checked in the tests)
BCA_K u8 bca_unq48[48] = { 0,5,11,16,21,27,32,38,43,48,54,59,65,70,76,81,86,92,97,103,108,113,119,124,131,136,142,147,152,158,163,169,174,179,185,190,196,201,207,212,217,223,228,234,239,244,250,255 };
BCA_K u8 bca_raw48[48] = { 0,16,32,2,18,34,4,20,36,6,22,38,8,24,40,10,26,42,12,28,44,14,30,46,47,31,15,45,29,13,43,27,11,41,25,9,39,23,7,37,21,5,35,19,3,33,17,1 };
// The four 2-bit weights as the decoder sees them (of 64)
BCA_K u8 bca_w4[4] = { 0, 21, 43, 64 };

// ---- bit helpers on a 128-bit block held as four words ----
BCA_FN void bca_putl(BCA_T u32 *w, u32 pos, u32 lim, u32 n, u32 v) {   // n bits of v at bit pos; bits at or past lim (and past 127) are dropped
    for (u32 i = 0; i < n; i++) {
        u32 p = pos + i;
        if (p < lim && p < 128u && ((v >> i) & 1u)) w[p >> 5] |= 1u << (p & 31u);
    }
}
BCA_FN u32 bca_getb(BCA_T const u32 *w, u32 p) { return (w[p >> 5] >> (p & 31u)) & 1u; }
// Weights are stored from bit 127 downwards: stream bit i of tmp goes to block bit 127 - i
BCA_FN void bca_rev(BCA_T u32 *out, BCA_T const u32 *tmp, u32 n) {
    for (u32 i = 0; i < n; i++) if (bca_getb(tmp, i)) out[(127u - i) >> 5] |= 1u << ((127u - i) & 31u);
}
// Integer sequence encoding with trits: value v = trit * 2^nb + m. Writes the stream at bit pos, returns its length n*nb + ceil(8n/5). The last group is cut after the length.
BCA_FN u32 bca_ise_trit(BCA_T u32 *dst, u32 pos, BCA_T const u32 *vals, u32 n, u32 nb) {
    u32 total = n * nb + (n * 8u + 4u) / 5u, lim = pos + total, p = pos, mask = (1u << nb) - 1u;
    for (u32 g = 0; g < n; g += 5) {
        u32 m[5], t[5];
        for (u32 k = 0; k < 5; k++) { u32 v = (g + k < n) ? vals[g + k] : 0u; m[k] = v & mask; t[k] = v >> nb; }
        u32 T = bca_trit_enc[t[0] + 3u * t[1] + 9u * t[2] + 27u * t[3] + 81u * t[4]];
        bca_putl(dst, p, lim, nb, m[0]); p += nb;
        bca_putl(dst, p, lim, 2, T & 3u); p += 2;
        bca_putl(dst, p, lim, nb, m[1]); p += nb;
        bca_putl(dst, p, lim, 2, (T >> 2) & 3u); p += 2;
        bca_putl(dst, p, lim, nb, m[2]); p += nb;
        bca_putl(dst, p, lim, 1, (T >> 4) & 1u); p += 1;
        bca_putl(dst, p, lim, nb, m[3]); p += nb;
        bca_putl(dst, p, lim, 2, (T >> 5) & 3u); p += 2;
        bca_putl(dst, p, lim, nb, m[4]); p += nb;
        bca_putl(dst, p, lim, 1, (T >> 7) & 1u); p += 1;
    }
    return total;
}

// ---- BC decode (same arithmetic as dm/third_party/bcdec, which the tests compare against) ----
BCA_FN void bca_col(u32 c0, u32 c1, u32 three, BCA_T s32 *ref) {   // ref[4 colours][4 = r g b a]; three: BC1 three-colour mode (index 3 = transparent black)
    s32 r0 = (s32)((c0 >> 11) & 31u), g0 = (s32)((c0 >> 5) & 63u), b0 = (s32)(c0 & 31u);
    s32 r1 = (s32)((c1 >> 11) & 31u), g1 = (s32)((c1 >> 5) & 63u), b1 = (s32)(c1 & 31u);
    ref[0] = (r0 * 527 + 23) >> 6; ref[1] = (g0 * 259 + 33) >> 6; ref[2] = (b0 * 527 + 23) >> 6; ref[3] = 255;
    ref[4] = (r1 * 527 + 23) >> 6; ref[5] = (g1 * 259 + 33) >> 6; ref[6] = (b1 * 527 + 23) >> 6; ref[7] = 255;
    if (!three) {
        ref[8]  = ((2 * r0 + r1) * 351 + 61) >> 7; ref[9]  = ((2 * g0 + g1) * 2763 + 1039) >> 11; ref[10] = ((2 * b0 + b1) * 351 + 61) >> 7; ref[11] = 255;
        ref[12] = ((r0 + r1 * 2) * 351 + 61) >> 7; ref[13] = ((g0 + g1 * 2) * 2763 + 1039) >> 11; ref[14] = ((b0 + b1 * 2) * 351 + 61) >> 7; ref[15] = 255;
    } else {
        ref[8]  = ((r0 + r1) * 1053 + 125) >> 8; ref[9]  = ((g0 + g1) * 4145 + 1019) >> 11; ref[10] = ((b0 + b1) * 1053 + 125) >> 8; ref[11] = 255;
        ref[12] = 0; ref[13] = 0; ref[14] = 0; ref[15] = 0;
    }
}

// ---- endpoint fitting ----
BCA_FN s32 bca_lerp(s32 e0, s32 e1, s32 w) { return ((((e0 * (64 - w) + e1 * w) * 257 + 32) >> 6) >> 8); }   // the decoder's interpolation, reduced to 8 bits
BCA_FN s32 bca_near48(s32 x) {   // index of the unquantized value nearest to x (the lower index wins a tie)
    s32 best = 0, bd = 1 << 20;
    for (s32 i = 0; i < 48; i++) { s32 d = (s32)bca_unq48[i] - x; d = d < 0 ? -d : d; if (d < bd) { bd = d; best = i; } }
    return best;
}
// Least-squares endpoints for fixed weights wt (0..64), quantized to the 48-value range; the nearest index and its neighbours are tried and the smallest squared error wins
BCA_FN void bca_fit48(BCA_T const s32 *vals, BCA_T const s32 *wt, BCA_T s32 *q0, BCA_T s32 *q1) {
    s64 saa = 0, sab = 0, sbb = 0, sac = 0, sbc = 0; s32 sum = 0;
    for (u32 i = 0; i < 16; i++) {
        s64 a = 64 - wt[i], b = wt[i], c = vals[i];
        saa += a * a; sab += a * b; sbb += b * b; sac += a * c; sbc += b * c; sum += vals[i];
    }
    s64 det = saa * sbb - sab * sab;
    s32 e0, e1;
    if (det <= 0) { e0 = (sum + 8) >> 4; e1 = e0; }
    else {
        s64 n0 = sac * sbb - sbc * sab, n1 = saa * sbc - sab * sac;
        s64 x0 = (n0 * 64 + det / 2) / det, x1 = (n1 * 64 + det / 2) / det;
        e0 = (s32)(x0 < 0 ? 0 : (x0 > 255 ? 255 : x0)); e1 = (s32)(x1 < 0 ? 0 : (x1 > 255 ? 255 : x1));
    }
    s32 c0 = bca_near48(e0), c1 = bca_near48(e1), best = 1 << 30, b0 = c0, b1 = c1;
    for (s32 d0 = -1; d0 <= 1; d0++) for (s32 d1 = -1; d1 <= 1; d1++) {
        s32 i0 = c0 + d0, i1 = c1 + d1;
        if (i0 < 0 || i0 > 47 || i1 < 0 || i1 > 47) continue;
        s32 err = 0;
        for (u32 i = 0; i < 16; i++) { s32 d = bca_lerp((s32)bca_unq48[i0], (s32)bca_unq48[i1], wt[i]) - vals[i]; err += d * d; }
        if (err < best) { best = err; b0 = i0; b1 = i1; }
    }
    *q0 = b0; *q1 = b1;
}
BCA_FN s32 bca_near7(s32 x) {   // 7-bit endpoint index whose bit-replicated 8-bit value is nearest to x
    s32 q = x >> 1, best = q, bd = 1 << 20;
    for (s32 c = q; c <= q + 1 && c < 128; c++) {
        s32 u = (c << 1) | (c >> 6), d = u - x; d = d < 0 ? -d : d;
        if (d < bd) { bd = d; best = c; }
    }
    return best;
}

// bc: the BC block as four little-endian words (BC1 uses words 0..1). kind 1 = BC1, 2 = BC2, 3 = BC3. out: the ASTC block as four words (zeroed here). Returns the layout id (| BCA_SWAPPED).
BCA_FN u32 bca_transcode(BCA_T const u32 *bc, u32 kind, BCA_T u32 *out) {
    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
    u32 co = kind == 1 ? 0u : 2u, c0 = bc[co] & 0xffffu, c1 = bc[co] >> 16, ix = bc[co + 1];
    u32 three = (kind == 1 && c0 <= c1) ? 1u : 0u;
    s32 ref[16]; bca_col(c0, c1, three, ref);
    s32 ci[16], px[16][4], al[16];
    for (u32 i = 0; i < 16; i++) {
        ci[i] = (s32)((ix >> (2 * i)) & 3u);
        for (u32 c = 0; c < 4; c++) px[i][c] = ref[ci[i] * 4 + (s32)c];
    }
    if (kind == 2) { for (u32 i = 0; i < 16; i++) al[i] = (s32)((bc[i >> 3] >> ((i & 7u) * 4u)) & 15u) * 17; }
    else if (kind == 3) {
        s32 a0 = (s32)(bc[0] & 255u), a1 = (s32)((bc[0] >> 8) & 255u), at[8];
        at[0] = a0; at[1] = a1;
        if (a0 > a1) { for (s32 k = 1; k < 7; k++) at[k + 1] = ((7 - k) * a0 + k * a1) / 7; }
        else { for (s32 k = 1; k < 5; k++) at[k + 1] = ((5 - k) * a0 + k * a1) / 5; at[6] = 0; at[7] = 255; }
        u64 ai = ((u64)bc[0] | ((u64)bc[1] << 32)) >> 16;
        for (u32 i = 0; i < 16; i++) al[i] = at[(ai >> (3u * i)) & 7u];
    } else { for (u32 i = 0; i < 16; i++) al[i] = px[i][3]; }
    if (kind != 1) for (u32 i = 0; i < 16; i++) px[i][3] = al[i];

    u32 same = 1;
    for (u32 i = 1; i < 16; i++) for (u32 c = 0; c < 4; c++) if (px[i][c] != px[0][c]) same = 0;
    if (same) {   // void-extent block: one constant colour (exact for 8-bit values: v * 257 is the 16-bit value)
        out[0] = 0xFFFFFDFCu; out[1] = 0xFFFFFFFFu;
        out[2] = (u32)(px[0][0] * 257) | ((u32)(px[0][1] * 257) << 16);
        out[3] = (u32)(px[0][2] * 257) | ((u32)(px[0][3] * 257) << 16);
        return BCA_L0_SOLID;
    }

    u32 auni = 1;
    for (u32 i = 1; i < 16; i++) if (al[i] != al[0]) auni = 0;
    // colour weight index (0..3 of bca_w4) of each BC colour index: 0 → w0, 1 → w3, 2 → w1, 3 → w2
    u32 wmap[4]; wmap[0] = 0; wmap[1] = 3; wmap[2] = 1; wmap[3] = 2;

    if (!three && (kind == 1 || auni)) {   // L1a / L1b: one plane, 2-bit weights, 8-bit endpoints
        s32 e0[4], e1[4]; u32 w[16];
        for (u32 c = 0; c < 3; c++) { e0[c] = ref[c]; e1[c] = ref[4 + c]; }
        e0[3] = al[0]; e1[3] = al[0];
        for (u32 i = 0; i < 16; i++) w[i] = wmap[ci[i]];
        u32 sw = 0;
        if (e1[0] + e1[1] + e1[2] < e0[0] + e0[1] + e0[2]) {   // the decoder would blue-contract: swap the endpoints (all four channels) and invert the weights
            for (u32 c = 0; c < 4; c++) { s32 t = e0[c]; e0[c] = e1[c]; e1[c] = t; }
            for (u32 i = 0; i < 16; i++) w[i] = 3u - w[i];
            sw = BCA_SWAPPED;
        }
        u32 rgba = (kind != 1 && al[0] != 255) ? 1u : 0u, tmp[4] = { 0, 0, 0, 0 };
        bca_putl(out, 0, 128, 11, 0x42u); bca_putl(out, 13, 128, 4, rgba ? 12u : 8u);
        u32 pos = 17;
        for (u32 c = 0; c < (rgba ? 4u : 3u); c++) { bca_putl(out, pos, 128, 8, (u32)e0[c]); pos += 8; bca_putl(out, pos, 128, 8, (u32)e1[c]); pos += 8; }
        for (u32 i = 0; i < 16; i++) bca_putl(tmp, 2 * i, 128, 2, w[i]);
        bca_rev(out, tmp, 32);
        return (rgba ? BCA_L1B_RGBA_UNIFORM : BCA_L1A_RGB) | sw;
    }

    if (three) {   // L2: BC1 three-colour mode. Plane 0: colour, weights 0 / 32 / 64 (trit); plane 1: alpha 0 / 255
        s32 q0[3], q1[3], u0[3], u1[3];
        for (u32 c = 0; c < 3; c++) { q0[c] = bca_near7(ref[c]); q1[c] = bca_near7(ref[4 + c]); u0[c] = (q0[c] << 1) | (q0[c] >> 6); u1[c] = (q1[c] << 1) | (q1[c] >> 6); }
        u32 dark = (u0[0] + u0[1] + u0[2] <= u1[0] + u1[1] + u1[2]) ? 0u : 2u;   // transparent texels take the colour weight that lands on the endpoint closest to black
        u32 wc[16], wa[16];
        for (u32 i = 0; i < 16; i++) {
            u32 k = (u32)ci[i];
            wc[i] = k == 0 ? 0u : (k == 1 ? 2u : (k == 2 ? 1u : dark));
            wa[i] = k == 3 ? 0u : 2u;
        }
        s32 a0 = 0, a1 = 127;   // alpha endpoint indices: endpoint 0 holds alpha 0, endpoint 1 alpha 255 (index 127 unquantizes to 255)
        u32 sw = 0;
        if (u1[0] + u1[1] + u1[2] < u0[0] + u0[1] + u0[2]) {   // blue contraction would apply: swap all endpoints, invert both planes
            sw = BCA_SWAPPED;
            for (u32 c = 0; c < 3; c++) { s32 t = q0[c]; q0[c] = q1[c]; q1[c] = t; }
            s32 t = a0; a0 = a1; a1 = t;
            for (u32 i = 0; i < 16; i++) { wc[i] = 2u - wc[i]; wa[i] = 2u - wa[i]; }
        }
        u32 ev[8], wv[32], tmp[4] = { 0, 0, 0, 0 };
        for (u32 c = 0; c < 3; c++) { ev[2 * c] = (u32)q0[c]; ev[2 * c + 1] = (u32)q1[c]; }
        ev[6] = (u32)a0; ev[7] = (u32)a1;
        bca_putl(out, 0, 128, 11, 0x451u); bca_putl(out, 13, 128, 4, 12u);
        for (u32 i = 0; i < 8; i++) bca_putl(out, 17 + 7 * i, 128, 7, ev[i]);
        for (u32 i = 0; i < 16; i++) { wv[2 * i] = wc[i]; wv[2 * i + 1] = wa[i]; }
        u32 nbits = bca_ise_trit(tmp, 0, wv, 32, 0);
        bca_rev(out, tmp, nbits);
        bca_putl(out, 128u - nbits - 2u, 128, 2, 3u);
        return BCA_L2_BC1_PUNCH | sw;
    }

    // L3: BC2 / BC3 with varying alpha. Plane 0 colour, plane 1 alpha, 2-bit weights, endpoints in 48 levels
    s32 cw[16], vals[16], q0[4], q1[4];
    u32 wc[16], wa[16], sw = 0;
    for (u32 i = 0; i < 16; i++) { wc[i] = wmap[ci[i]]; cw[i] = (s32)bca_w4[wc[i]]; }
    for (u32 c = 0; c < 3; c++) {
        for (u32 i = 0; i < 16; i++) vals[i] = px[i][c];
        bca_fit48(vals, cw, &q0[c], &q1[c]);
    }
    {   // alpha: start from min / max, then reassign the four levels and refit twice
        s32 lo = 255, hi = 0, aw[16];
        for (u32 i = 0; i < 16; i++) { lo = al[i] < lo ? al[i] : lo; hi = al[i] > hi ? al[i] : hi; }
        s32 a0 = bca_near48(lo), a1 = bca_near48(hi);
        for (u32 it = 0; it < 3; it++) {
            for (u32 i = 0; i < 16; i++) {
                s32 bk = 0, bd = 1 << 20;
                for (s32 k = 0; k < 4; k++) { s32 d = bca_lerp((s32)bca_unq48[a0], (s32)bca_unq48[a1], (s32)bca_w4[k]) - al[i]; d = d < 0 ? -d : d; if (d < bd) { bd = d; bk = k; } }
                wa[i] = (u32)bk; aw[i] = (s32)bca_w4[bk];
            }
            if (it < 2) bca_fit48(al, aw, &a0, &a1);
        }
        q0[3] = a0; q1[3] = a1;
    }
    {
        s32 s0 = (s32)bca_unq48[q0[0]] + (s32)bca_unq48[q0[1]] + (s32)bca_unq48[q0[2]];
        s32 s1 = (s32)bca_unq48[q1[0]] + (s32)bca_unq48[q1[1]] + (s32)bca_unq48[q1[2]];
        if (s1 < s0) {
            sw = BCA_SWAPPED;   // swap all endpoints (alpha too) and invert both planes
            for (u32 c = 0; c < 4; c++) { s32 t = q0[c]; q0[c] = q1[c]; q1[c] = t; }
            for (u32 i = 0; i < 16; i++) { wc[i] = 3u - wc[i]; wa[i] = 3u - wa[i]; }
        }
    }
    {
        u32 ev[8], tmp[4] = { 0, 0, 0, 0 };
        for (u32 c = 0; c < 4; c++) { ev[2 * c] = bca_raw48[q0[c]]; ev[2 * c + 1] = bca_raw48[q1[c]]; }
        bca_putl(out, 0, 128, 11, 0x442u); bca_putl(out, 13, 128, 4, 12u);
        u32 n = bca_ise_trit(out, 17, ev, 8, 4);
        for (u32 i = 0; i < 16; i++) { bca_putl(tmp, 4 * i, 128, 2, wc[i]); bca_putl(tmp, 4 * i + 2, 128, 2, wa[i]); }
        bca_rev(out, tmp, 64);
        bca_putl(out, 128u - 64u - 2u, 128, 2, 3u);
        (void)n;
        return BCA_L3_ALPHA | sw;
    }
}
)
