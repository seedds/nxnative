// ASTC LDR decoder (astc_decode.h): the procedure of the Khronos Data Format Specification's ASTC chapter, step by step. dm/gfx/hostvk/shaders/astc_decode.comp has the same
// steps in GLSL, function for function (the names match), so that both give the same bytes; a change here needs the same change there (t_vk_astc compares them bit for bit).
#include "astc_decode.h"
#include <string.h>

// ---------- Bits ----------
static uint32_t getbits(const uint32_t w[4], int pos, int n) {   // bits [pos, pos + n) of the 128-bit block, n <= 32
    if (n <= 0 || pos < 0 || pos >= 128) return 0;
    int i = pos >> 5, s = pos & 31;
    uint32_t v = w[i] >> s;
    if (s && i < 3) v |= w[i + 1] << (32 - s);
    return n >= 32 ? v : v & ((1u << n) - 1);
}
static uint32_t getbits_lim(const uint32_t w[4], int pos, int n, int end) {   // the same, with the bits at end and past it read as zero (a sequence's last, partial block)
    if (pos >= end) return 0;
    if (pos + n > end) n = end - pos;
    return getbits(w, pos, n);
}
static uint32_t rev32(uint32_t x) {
    x = (x >> 1 & 0x55555555u) | (x & 0x55555555u) << 1; x = (x >> 2 & 0x33333333u) | (x & 0x33333333u) << 2;
    x = (x >> 4 & 0x0F0F0F0Fu) | (x & 0x0F0F0F0Fu) << 4; x = (x >> 8 & 0x00FF00FFu) | (x & 0x00FF00FFu) << 8;
    return x >> 16 | x << 16;
}
static void load(const uint8_t b[16], uint32_t w[4]) {
    for (int i = 0; i < 4; i++) w[i] = (uint32_t)b[4 * i] | (uint32_t)b[4 * i + 1] << 8 | (uint32_t)b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24;
}
static int asr1(int x) { return x >= 0 ? x >> 1 : ~(~x >> 1); }   // x >> 1 rounding toward minus infinity (GLSL's int >>), without C's implementation-defined shift

// ---------- The integer sequence encoding ----------
static const uint16_t R_LEVELS[21] = {2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64, 80, 96, 128, 160, 192, 256};
static const uint8_t R_BITS[21] = {1, 0, 2, 0, 1, 3, 1, 2, 4, 2, 3, 5, 3, 4, 6, 4, 5, 7, 5, 6, 8};
static const uint8_t R_KIND[21] = {0, 1, 0, 2, 1, 0, 2, 1, 0, 2, 1, 0, 2, 1, 0, 2, 1, 0, 2, 1, 0};   // 1 a trit, 2 a quint
int astc_levels(int r) { return r >= 0 && r < 21 ? R_LEVELS[r] : 0; }
int astc_range_bits(int r, int *kind) { if (kind) *kind = r >= 0 && r < 21 ? R_KIND[r] : 0; return r >= 0 && r < 21 ? R_BITS[r] : 0; }
int astc_ise_bits(int n, int r) {
    int b = R_BITS[r], k = R_KIND[r];
    return b * n + (k == 1 ? (8 * n + 4) / 5 : k == 2 ? (7 * n + 2) / 3 : 0);
}
void astc_trits(int T, int t[5]) {
    int C, t3, t4;
    if ((T >> 2 & 7) == 7) { C = (T >> 5 & 7) << 2 | (T & 3); t4 = 2; t3 = 2; }
    else {
        C = T & 0x1F;
        if ((T >> 5 & 3) == 3) { t4 = 2; t3 = T >> 7 & 1; } else { t4 = T >> 7 & 1; t3 = T >> 5 & 3; }
    }
    if ((C & 3) == 3) { t[2] = 2; t[1] = C >> 4 & 1; t[0] = (C >> 3 & 1) << 1 | ((C >> 2 & 1) & ((C >> 3 & 1) ^ 1)); }
    else if ((C >> 2 & 3) == 3) { t[2] = 2; t[1] = 2; t[0] = C & 3; }
    else { t[2] = C >> 4 & 1; t[1] = C >> 2 & 3; t[0] = (C >> 1 & 1) << 1 | ((C & 1) & ((C >> 1 & 1) ^ 1)); }
    t[3] = t3; t[4] = t4;
}
void astc_quints(int Q, int q[3]) {
    if ((Q >> 1 & 3) == 3 && (Q >> 5 & 3) == 0) {
        q[2] = (Q & 1) << 2 | ((Q >> 4 & 1) & ((Q & 1) ^ 1)) << 1 | ((Q >> 3 & 1) & ((Q & 1) ^ 1)); q[1] = 4; q[0] = 4;
        return;
    }
    int C;
    if ((Q >> 1 & 3) == 3) { q[2] = 4; C = (Q >> 3 & 3) << 3 | ((Q >> 5 & 3) ^ 3) << 1 | (Q & 1); }
    else { q[2] = Q >> 5 & 3; C = Q & 0x1F; }
    if ((C & 7) == 5) { q[1] = 4; q[0] = C >> 3 & 3; } else { q[1] = C >> 3 & 3; q[0] = C & 7; }
}
// Value i: plain bits; five values a trit block (m0 T1:0 m1 T3:2 m2 T4 m3 T6:5 m4 T7); three values a quint block (m0 Q2:0 m1 Q4:3 m2 Q6:5); the trit or quint is the high part
int astc_ise_value(const uint32_t w[4], int start, int end, int r, int i) {
    int b = R_BITS[r], k = R_KIND[r];
    if (k == 0) return (int)getbits_lim(w, start + i * b, b, end);
    if (k == 1) {
        static const int mpos[5] = {0, 2, 4, 5, 7};
        int base = start + i / 5 * (5 * b + 8), j = i % 5, t[5];
        int T = (int)(getbits_lim(w, base + b, 2, end) | getbits_lim(w, base + 2 * b + 2, 2, end) << 2 | getbits_lim(w, base + 3 * b + 4, 1, end) << 4 |
                      getbits_lim(w, base + 4 * b + 5, 2, end) << 5 | getbits_lim(w, base + 5 * b + 7, 1, end) << 7);
        astc_trits(T, t);
        return t[j] << b | (int)getbits_lim(w, base + j * b + mpos[j], b, end);
    }
    static const int mpos[3] = {0, 3, 5};
    int base = start + i / 3 * (3 * b + 7), j = i % 3, q[3];
    int Q = (int)(getbits_lim(w, base + b, 3, end) | getbits_lim(w, base + 2 * b + 3, 2, end) << 3 | getbits_lim(w, base + 3 * b + 5, 2, end) << 5);
    astc_quints(Q, q);
    return q[j] << b | (int)getbits_lim(w, base + j * b + mpos[j], b, end);
}

// ---------- Unquantization ----------
static int replicate(int v, int b, int n) {   // a b-bit value repeated over n bits, most significant first
    int out = 0;
    for (int s = n - b; s > -b; s -= b) out |= s >= 0 ? v << s : v >> -s;
    return out;
}
int astc_unq_color(int r, int v) {
    int b = R_BITS[r];
    if (R_KIND[r] == 0) return replicate(v, b, 8);
    int D = v >> b, m = v & ((1 << b) - 1), A = m & 1 ? 0x1FF : 0;
    int b1 = m >> 1 & 1, c = m >> 2 & 1, d = m >> 3 & 1, e = m >> 4 & 1, f = m >> 5 & 1, B = 0, C;
    switch (r) {
        case 4: C = 204; break;                                                         // trit, 1 bit
        case 6: C = 113; break;                                                         // quint, 1 bit
        case 7: B = b1 * 0x116; C = 93; break;                                          // trit, 2 bits: b000b0bb0
        case 9: B = b1 * 0x10C; C = 54; break;                                          // quint, 2 bits: b0000bb00
        case 10: B = c * 0x10A + b1 * 0x85; C = 44; break;                              // trit, 3 bits: cb000cbcb
        case 12: B = c * 0x105 + b1 * 0x82; C = 26; break;                              // quint, 3 bits: cb0000cbc
        case 13: B = d * 0x104 + c * 0x82 + b1 * 0x41; C = 22; break;                   // trit, 4 bits: dcb000dcb
        case 15: B = d * 0x102 + c * 0x81 + b1 * 0x40; C = 13; break;                   // quint, 4 bits: dcb0000dc
        case 16: B = e * 0x102 + d * 0x81 + c * 0x40 + b1 * 0x20; C = 11; break;        // trit, 5 bits: edcb000ed
        case 18: B = e * 0x101 + d * 0x80 + c * 0x40 + b1 * 0x20; C = 6; break;         // quint, 5 bits: edcb0000e
        case 19: B = f * 0x101 + e * 0x80 + d * 0x40 + c * 0x20 + b1 * 0x10; C = 5; break;   // trit, 6 bits: fedcb000f
        default: return 0;                                                              // ranges 3 and 5 levels: not colour ranges
    }
    int T = (D * C + B) ^ A;
    return (A & 0x80) | T >> 2;
}
int astc_unq_weight(int r, int v) {
    int b = R_BITS[r], T;
    if (R_KIND[r] == 0) T = replicate(v, b, 6);
    else if (b == 0) { static const int t3[3] = {0, 32, 63}, q5[5] = {0, 16, 32, 47, 63}; T = R_KIND[r] == 1 ? t3[v % 3] : q5[v % 5]; }
    else {
        int D = v >> b, m = v & ((1 << b) - 1), A = m & 1 ? 0x7F : 0, b1 = m >> 1 & 1, c = m >> 2 & 1, B = 0, C;
        switch (r) {
            case 4: C = 50; break;                               // trit, 1 bit
            case 6: C = 28; break;                               // quint, 1 bit
            case 7: B = b1 * 0x45; C = 23; break;                // trit, 2 bits: b000b0b
            case 9: B = b1 * 0x42; C = 13; break;                // quint, 2 bits: b0000b0
            case 10: B = c * 0x42 + b1 * 0x21; C = 11; break;    // trit, 3 bits: cb000cb
            default: return 0;
        }
        T = (D * C + B) ^ A;
        T = (A & 0x20) | T >> 2;
    }
    return T > 32 ? T + 1 : T;
}

// ---------- Block modes, partitions, endpoints, weights ----------
int astc_block_mode(int mode, int *gw, int *gh, int *dual, int *wr) {
    int R = mode >> 4 & 1, H = mode >> 9 & 1, D = mode >> 10 & 1, A = mode >> 5 & 3, w, h;
    if ((mode & 0x1FF) == 0x1FC) return 0;
    if (mode & 3) {
        R |= (mode & 3) << 1;
        int B = mode >> 7 & 3;
        switch (mode >> 2 & 3) {
            case 0: w = B + 4; h = A + 2; break;
            case 1: w = B + 8; h = A + 2; break;
            case 2: w = A + 2; h = B + 8; break;
            default: B &= 1; if (mode & 0x100) { w = B + 2; h = A + 2; } else { w = A + 2; h = B + 6; } break;
        }
    } else {
        R |= (mode >> 2 & 3) << 1;
        if (!(mode >> 2 & 3)) return 0;
        int B = mode >> 9 & 3;
        switch (mode >> 7 & 3) {
            case 0: w = 12; h = A + 2; break;
            case 1: w = A + 2; h = 12; break;
            case 2: w = A + 6; h = B + 6; D = 0; H = 0; break;
            default:
                if ((mode >> 5 & 3) == 0) { w = 6; h = 10; } else if ((mode >> 5 & 3) == 1) { w = 10; h = 6; } else return 0;
                break;
        }
    }
    *gw = w; *gh = h; *dual = D; *wr = R - 2 + 6 * H;
    return 1;
}
static uint32_t hash52(uint32_t p) {
    p ^= p >> 15; p -= p << 17; p += p << 7; p += p << 4;
    p ^= p >> 5; p += p << 16; p ^= p >> 7; p ^= p >> 3;
    p ^= p << 6; p ^= p >> 17;
    return p;
}
int astc_partition(int seed, int parts, int x, int y, int small_block) {
    if (parts <= 1) return 0;
    if (small_block) { x <<= 1; y <<= 1; }
    seed += (parts - 1) * 1024;
    uint32_t rnum = hash52((uint32_t)seed), s[12];
    for (int i = 0; i < 8; i++) s[i] = rnum >> (4 * i) & 0xF;
    s[8] = rnum >> 18 & 0xF; s[9] = rnum >> 22 & 0xF; s[10] = rnum >> 26 & 0xF; s[11] = (rnum >> 30 | rnum << 2) & 0xF;
    for (int i = 0; i < 12; i++) s[i] *= s[i];
    int sh1, sh2;
    if (seed & 1) { sh1 = seed & 2 ? 4 : 5; sh2 = parts == 3 ? 6 : 5; } else { sh1 = parts == 3 ? 6 : 5; sh2 = seed & 2 ? 4 : 5; }
    int sh3 = seed & 0x10 ? sh1 : sh2;
    for (int i = 0; i < 8; i++) s[i] >>= i & 1 ? sh2 : sh1;
    for (int i = 8; i < 12; i++) s[i] >>= sh3;
    uint32_t ux = (uint32_t)x, uy = (uint32_t)y;   // z = 0: 2D blocks
    uint32_t a = (s[0] * ux + s[1] * uy + (rnum >> 14)) & 0x3F, b = (s[2] * ux + s[3] * uy + (rnum >> 10)) & 0x3F;
    uint32_t c = (s[4] * ux + s[5] * uy + (rnum >> 6)) & 0x3F, d = (s[6] * ux + s[7] * uy + (rnum >> 2)) & 0x3F;
    if (parts < 4) d = 0;
    if (parts < 3) c = 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    if (c >= d) return 2;
    return 3;
}
static void bit_transfer_signed(int *a, int *b) {
    *b = *b >> 1 | (*a & 0x80);
    *a = *a >> 1 & 0x3F;
    if (*a & 0x20) *a -= 0x40;
}
static void set4(int e[4], int r, int g, int b, int a) { e[0] = r; e[1] = g; e[2] = b; e[3] = a; }
static void blue_contract(int e[4], int r, int g, int b, int a) { set4(e, asr1(r + b), asr1(g + b), b, a); }
static void clamp4(int e[4]) { for (int i = 0; i < 4; i++) e[i] = e[i] < 0 ? 0 : e[i] > 255 ? 255 : e[i]; }
int astc_endpoints(int cem, const int v[8], int e0[4], int e1[4]) {
    int a[8]; memcpy(a, v, sizeof a);
    switch (cem) {
        case 0: set4(e0, a[0], a[0], a[0], 255); set4(e1, a[1], a[1], a[1], 255); return 1;   // luminance, direct
        case 1: {                                                                                // luminance, base + offset
            int l0 = a[0] >> 2 | (a[1] & 0xC0), l1 = l0 + (a[1] & 0x3F);
            if (l1 > 255) l1 = 255;
            set4(e0, l0, l0, l0, 255); set4(e1, l1, l1, l1, 255); return 1;
        }
        case 4: set4(e0, a[0], a[0], a[0], a[2]); set4(e1, a[1], a[1], a[1], a[3]); return 1;   // luminance and alpha, direct
        case 5:                                                                                  // luminance and alpha, base + offset
            bit_transfer_signed(&a[1], &a[0]); bit_transfer_signed(&a[3], &a[2]);
            set4(e0, a[0], a[0], a[0], a[2]); set4(e1, a[0] + a[1], a[0] + a[1], a[0] + a[1], a[2] + a[3]); clamp4(e0); clamp4(e1); return 1;
        case 6: set4(e0, a[0] * a[3] >> 8, a[1] * a[3] >> 8, a[2] * a[3] >> 8, 255); set4(e1, a[0], a[1], a[2], 255); return 1;   // RGB, base + scale
        case 8: case 12: {                                                                       // RGB / RGBA, direct
            int al0 = cem == 12 ? a[6] : 255, al1 = cem == 12 ? a[7] : 255;
            if (a[1] + a[3] + a[5] >= a[0] + a[2] + a[4]) { set4(e0, a[0], a[2], a[4], al0); set4(e1, a[1], a[3], a[5], al1); }
            else { blue_contract(e0, a[1], a[3], a[5], al1); blue_contract(e1, a[0], a[2], a[4], al0); }
            return 1;
        }
        case 9: case 13: {                                                                       // RGB / RGBA, base + offset
            bit_transfer_signed(&a[1], &a[0]); bit_transfer_signed(&a[3], &a[2]); bit_transfer_signed(&a[5], &a[4]);
            int al0 = 255, al1 = 255;
            if (cem == 13) { bit_transfer_signed(&a[7], &a[6]); al0 = a[6]; al1 = a[6] + a[7]; }
            if (a[1] + a[3] + a[5] >= 0) { set4(e0, a[0], a[2], a[4], al0); set4(e1, a[0] + a[1], a[2] + a[3], a[4] + a[5], al1); }
            else { blue_contract(e0, a[0] + a[1], a[2] + a[3], a[4] + a[5], al1); blue_contract(e1, a[0], a[2], a[4], al0); }
            clamp4(e0); clamp4(e1); return 1;
        }
        case 10: set4(e0, a[0] * a[3] >> 8, a[1] * a[3] >> 8, a[2] * a[3] >> 8, a[4]); set4(e1, a[0], a[1], a[2], a[5]); return 1;   // RGB base + scale, two alphas
        default: set4(e0, 0, 0, 0, 0); set4(e1, 0, 0, 0, 0); return 0;                           // 2, 3, 7, 11, 14, 15: HDR modes
    }
}
int astc_infill(const int *wq, int gw, int gh, int dual, int plane, int bw, int bh, int x, int y) {
    int ds = (1024 + bw / 2) / (bw - 1), dt = (1024 + bh / 2) / (bh - 1);
    int gs = (ds * x * (gw - 1) + 32) >> 6, gt = (dt * y * (gh - 1) + 32) >> 6;
    int js = gs >> 4, fs = gs & 0xF, jt = gt >> 4, ft = gt & 0xF;
    int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
    int v0 = js + jt * gw, n = gw * gh, st = dual + 1;
#define WQ(i) ((i) < n ? wq[(i) * st + plane] : 0)   // a grid point past the last one has a zero factor (fs or ft is 0 there)
    int p = WQ(v0) * w00 + WQ(v0 + 1) * w01 + WQ(v0 + gw) * w10 + WQ(v0 + gw + 1) * w11;
#undef WQ
    return (p + 8) >> 4;
}

// ---------- The block header ----------
static int fail(astc_hdr *h, const char *why) { h->kind = ASTC_ERROR; h->why = why; return ASTC_ERROR; }
int astc_header(const uint8_t b[16], int bw, int bh, astc_hdr *h) {
    uint32_t w[4]; load(b, w);
    memset(h, 0, sizeof *h);
    int mode = (int)getbits(w, 0, 11);
    if ((mode & 0x1FF) == 0x1FC) {
        if (mode & 0x200) return fail(h, "void-extent block with HDR values (LDR profile)");
        if (getbits(w, 10, 2) != 3) return fail(h, "void-extent block whose reserved bits are not 1");
        uint32_t s0 = getbits(w, 12, 13), s1 = getbits(w, 25, 13), t0 = getbits(w, 38, 13), t1 = getbits(w, 51, 13);
        int ones = s0 == 0x1FFF && s1 == 0x1FFF && t0 == 0x1FFF && t1 == 0x1FFF;
        if (!ones && (s0 >= s1 || t0 >= t1)) return fail(h, "void-extent block whose low coordinate is not below its high one");
        for (int i = 0; i < 4; i++) h->ve[i] = (uint16_t)getbits(w, 64 + 16 * i, 16);
        h->kind = ASTC_VOID_EXTENT;
        return h->kind;
    }
    if (!astc_block_mode(mode, &h->gw, &h->gh, &h->dual, &h->wr)) return fail(h, "reserved block mode");
    if (h->gw > bw || h->gh > bh) return fail(h, "weight grid larger than the block");
    h->parts = (int)getbits(w, 11, 2) + 1;
    if (h->dual && h->parts == 4) return fail(h, "dual plane with four partitions");
    int count = h->gw * h->gh * (h->dual + 1);
    if (count > 64) return fail(h, "more than 64 weights");
    h->wbits = astc_ise_bits(count, h->wr);
    if (h->wbits < 24 || h->wbits > 96) return fail(h, "weight sequence outside 24..96 bits");
    int below = 128 - h->wbits;
    if (h->parts == 1) { h->cem[0] = (int)getbits(w, 13, 4); h->cstart = 17; }
    else {
        h->seed = (int)getbits(w, 13, 10); h->cstart = 29;
        int enc = (int)getbits(w, 23, 6);
        if (!(enc & 3)) for (int i = 0; i < h->parts; i++) h->cem[i] = enc >> 2;
        else {   // a class selector, a class bit per partition and two mode bits per partition, the rest just below the weights
            int extra = 3 * h->parts - 4;
            below -= extra;
            enc |= (int)getbits(w, below, extra) << 6;
            int base = (enc & 3) - 1;
            for (int i = 0; i < h->parts; i++) h->cem[i] = (base + (enc >> (2 + i) & 1)) << 2 | (enc >> (2 + h->parts + 2 * i) & 3);
        }
    }
    if (h->dual) { below -= 2; h->ccs = (int)getbits(w, below, 2); }
    for (int i = 0; i < h->parts; i++) h->nvals += 2 * ((h->cem[i] >> 2) + 1);
    if (h->nvals > 18) return fail(h, "more than 18 colour values");
    int avail = below - h->cstart;
    h->cr = -1;
    for (int r = 20; r >= 0 && h->cr < 0; r--) if (astc_ise_bits(h->nvals, r) <= avail) h->cr = r;
    if (h->cr < 4) return fail(h, "fewer colour bits than range 0..5 needs");
    h->cbits = astc_ise_bits(h->nvals, h->cr);
    h->kind = ASTC_NORMAL;
    return h->kind;
}

// ---------- Texels ----------
typedef struct { int e0[4][4], e1[4][4], ok[4], wq[64]; } prep_t;
static void prepare(const uint32_t w[4], const astc_hdr *h, prep_t *p) {
    int idx = 0;
    for (int q = 0; q < h->parts; q++) {
        int n = 2 * ((h->cem[q] >> 2) + 1), v[8] = {0};
        for (int k = 0; k < n; k++) v[k] = astc_unq_color(h->cr, astc_ise_value(w, h->cstart, h->cstart + h->cbits, h->cr, idx + k));
        idx += n;
        p->ok[q] = astc_endpoints(h->cem[q], v, p->e0[q], p->e1[q]);
    }
    uint32_t rw[4] = {rev32(w[3]), rev32(w[2]), rev32(w[1]), rev32(w[0])};   // the weights are read from bit 127 down
    int count = h->gw * h->gh * (h->dual + 1);
    for (int i = 0; i < count; i++) p->wq[i] = astc_unq_weight(h->wr, astc_ise_value(rw, 0, h->wbits, h->wr, i));
}
static const uint8_t MAGENTA[4] = {255, 0, 255, 255};
static int texel_of(const astc_hdr *h, const prep_t *p, int bw, int bh, int srgb, int x, int y, uint8_t out[4]) {
    if (h->kind == ASTC_ERROR) { memcpy(out, MAGENTA, 4); return 1; }
    if (h->kind == ASTC_VOID_EXTENT) { for (int c = 0; c < 4; c++) out[c] = (uint8_t)(h->ve[c] >> 8); return 0; }
    int part = astc_partition(h->seed, h->parts, x, y, bw * bh < 31);
    if (!p->ok[part]) { memcpy(out, MAGENTA, 4); return 1; }
    int w0 = astc_infill(p->wq, h->gw, h->gh, h->dual, 0, bw, bh, x, y), w1 = h->dual ? astc_infill(p->wq, h->gw, h->gh, 1, 1, bw, bh, x, y) : w0;
    for (int c = 0; c < 4; c++) {
        int c0 = srgb ? p->e0[part][c] << 8 | 0x80 : p->e0[part][c] * 257, c1 = srgb ? p->e1[part][c] << 8 | 0x80 : p->e1[part][c] * 257;
        int wt = h->dual && c == h->ccs ? w1 : w0;
        out[c] = (uint8_t)(((c0 * (64 - wt) + c1 * wt + 32) >> 6) >> 8);   // the top 8 bits of the 16-bit result
    }
    return 0;
}
int astc_texel(const uint8_t b[16], const astc_hdr *h, int bw, int bh, int srgb, int x, int y, uint8_t rgba[4]) {
    prep_t p; memset(&p, 0, sizeof p);
    if (h->kind == ASTC_NORMAL) { uint32_t w[4]; load(b, w); prepare(w, h, &p); }
    return texel_of(h, &p, bw, bh, srgb, x, y, rgba);
}
int astc_decode_block(const uint8_t b[16], int bw, int bh, int srgb, uint8_t *dst, size_t pitch) {
    astc_hdr h; astc_header(b, bw, bh, &h);
    prep_t p; memset(&p, 0, sizeof p);
    if (h.kind == ASTC_NORMAL) { uint32_t w[4]; load(b, w); prepare(w, &h, &p); }
    for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) texel_of(&h, &p, bw, bh, srgb, x, y, dst + (size_t)y * pitch + (size_t)x * 4);
    return h.kind;
}
void astc_decode_image(const uint8_t *src, size_t src_pitch, uint32_t w, uint32_t h, int bw, int bh, int srgb, uint8_t *dst, size_t dst_pitch) {
    uint8_t blk[12 * 12 * 4];
    for (uint32_t by = 0; by * (uint32_t)bh < h; by++)
        for (uint32_t bx = 0; bx * (uint32_t)bw < w; bx++) {
            astc_decode_block(src + by * src_pitch + (size_t)bx * 16, bw, bh, srgb, blk, (size_t)bw * 4);
            for (int y = 0; y < bh && by * (uint32_t)bh + (uint32_t)y < h; y++) {
                uint32_t x0 = bx * (uint32_t)bw, n = w - x0 < (uint32_t)bw ? w - x0 : (uint32_t)bw;
                memcpy(dst + (size_t)(by * (uint32_t)bh + (uint32_t)y) * dst_pitch + (size_t)x0 * 4, blk + (size_t)y * bw * 4, (size_t)n * 4);
            }
        }
}
