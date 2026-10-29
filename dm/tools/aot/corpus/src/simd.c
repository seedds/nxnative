// The Advanced SIMD set: NEON intrinsics over vectors of special lanes, each checked lane by lane against a scalar reference (a noinline
// function per lane, so that no compiler vectorises the reference back into the instruction under test), and the vectors printed as hex digests:
// integer arithmetic of every lane size, saturating arithmetic, doubling multiplies, shifts by immediate and by register (negative amounts shift
// right), narrowing and widening, pairwise and across-vector reductions, table lookups, permutes, bit selects, counts, compares, interleaving
// loads and stores, and the floating-point vector forms against the scalar instruction of the same operation (fp.c checks the scalar ones).
#include <arm_neon.h>

#include "rt.h"

static volatile uint8_t seed_bytes[64] = {
    0x00, 0x01, 0x7f, 0x80, 0xff, 0xfe, 0x55, 0xaa, 0x0f, 0xf0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc,
    0xde, 0xf1, 0x80, 0x7f, 0x01, 0x00, 0xff, 0x81, 0x40, 0xc0, 0x3c, 0xc3, 0x99, 0x66, 0x24, 0xdb,
    0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0x00,
    0xff, 0x7f, 0x00, 0x80, 0x01, 0xfe, 0x02, 0xfd, 0x03, 0xfc, 0x04, 0xfb, 0x05, 0xfa, 0x06, 0xf9,
};

typedef union {
    uint8_t u8[16];
    int8_t s8[16];
    uint16_t u16[8];
    int16_t s16[8];
    uint32_t u32[4];
    int32_t s32[4];
    uint64_t u64[2];
    int64_t s64[2];
    float f32[4];
    double f64[2];
} lanes;

static lanes vec_at(unsigned i) {
    lanes l;
    for (unsigned k = 0; k < 16; ++k) {
        l.u8[k] = seed_bytes[(i * 7 + k * 3) % 64];
    }
    return l;
}
static uint8x16_t as_u8(lanes l) { return vld1q_u8(l.u8); }
static lanes from_u8(uint8x16_t v) { lanes l; vst1q_u8(l.u8, v); return l; }

static unsigned bad_total;
static uint64_t digest;
static void expect(const char *name, unsigned bad, uint64_t h) {
    bad_total += bad;
    RT_CHECK32(name, bad, 0);
    rt_note("  digest", h, 16);
    digest = rt_mix(digest, h);
}
static uint64_t mix_lanes(uint64_t h, lanes l) {
    return rt_mix(rt_mix(h, l.u64[0]), l.u64[1]);
}

// ---- Scalar references, one lane each ----
static RT_NOINLINE int16_t ref_qadd_s16(int16_t a, int16_t b) { int v = a + b; return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static RT_NOINLINE uint8_t ref_qsub_u8(uint8_t a, uint8_t b) { return (uint8_t)(a > b ? a - b : 0); }
static RT_NOINLINE int16_t ref_qdmulh_s16(int16_t a, int16_t b) {
    int64_t p = 2 * (int64_t)a * b;
    p >>= 16;
    return (int16_t)(p > 32767 ? 32767 : p);
}
static RT_NOINLINE int32_t ref_qrdmulh_s32(int32_t a, int32_t b) {
    __int128 p = 2 * (__int128)a * b + ((__int128)1 << 31);
    p >>= 32;
    return (int32_t)(p > 0x7fffffff ? 0x7fffffff : p);
}
static RT_NOINLINE uint8_t ref_abd_u8(uint8_t a, uint8_t b) { return (uint8_t)(a > b ? a - b : b - a); }
static RT_NOINLINE int8_t ref_qabs_s8(int8_t a) { return (int8_t)(a == -128 ? 127 : a < 0 ? -a : a); }
static RT_NOINLINE int16_t ref_sshl_s16(int16_t a, int8_t s) {
    if (s >= 0) return (int16_t)(s >= 16 ? 0 : (uint16_t)a << s);
    return (int16_t)(-s >= 16 ? (a < 0 ? -1 : 0) : a >> -s);
}
static RT_NOINLINE uint32_t ref_urshr_u32(uint32_t a, unsigned n) { return (uint32_t)(((uint64_t)a + (1ull << (n - 1))) >> n); }
static RT_NOINLINE int8_t ref_sqxtn_s16(int16_t a) { return (int8_t)(a > 127 ? 127 : a < -128 ? -128 : a); }
static RT_NOINLINE uint16_t ref_sqxtun_s32(int32_t a) { return (uint16_t)(a > 65535 ? 65535 : a < 0 ? 0 : a); }
static RT_NOINLINE uint8_t ref_rshrn_u16(uint16_t a, unsigned n) { return (uint8_t)((a + (1u << (n - 1))) >> n); }
static RT_NOINLINE uint8_t ref_cnt(uint8_t a) { unsigned n = 0; for (int i = 0; i < 8; ++i) n += (a >> i) & 1; return (uint8_t)n; }
static RT_NOINLINE uint8_t ref_clz8(uint8_t a) { unsigned n = 0; for (int i = 7; i >= 0 && !((a >> i) & 1); --i) ++n; return (uint8_t)n; }
static RT_NOINLINE uint8_t ref_rbit8(uint8_t a) { uint8_t r = 0; for (int i = 0; i < 8; ++i) r |= (uint8_t)(((a >> i) & 1) << (7 - i)); return r; }
static RT_NOINLINE uint32_t ref_fadd(uint32_t a, uint32_t b) {
    uint32_t r;
    __asm__ volatile("fmov s1, %w1\n fmov s2, %w2\n fadd s0, s1, s2\n fmov %w0, s0" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
    return r;
}
static RT_NOINLINE uint32_t ref_fmaxnm(uint32_t a, uint32_t b) {
    uint32_t r;
    __asm__ volatile("fmov s1, %w1\n fmov s2, %w2\n fmaxnm s0, s1, s2\n fmov %w0, s0" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
    return r;
}
static RT_NOINLINE uint32_t ref_fmla(uint32_t acc, uint32_t a, uint32_t b) {
    uint32_t r;
    __asm__ volatile("fmov s0, %w1\n fmov s1, %w2\n fmov s2, %w3\n fmadd s0, s1, s2, s0\n fmov %w0, s0" : "=r"(r) : "r"(acc), "r"(a), "r"(b) : "v0", "v1", "v2");
    return r;
}
static RT_NOINLINE uint32_t ref_frecpe(uint32_t a) {
    uint32_t r;
    __asm__ volatile("fmov s1, %w1\n frecpe s0, s1\n fmov %w0, s0" : "=r"(r) : "r"(a) : "v0", "v1");
    return r;
}
static RT_NOINLINE uint32_t ref_fcvtzs(uint32_t a) {
    uint32_t r;
    __asm__ volatile("fmov s1, %w1\n fcvtzs %w0, s1" : "=r"(r) : "r"(a) : "v1");
    return r;
}
static RT_NOINLINE uint64_t ref_dfmul(uint64_t a, uint64_t b) {
    uint64_t r;
    __asm__ volatile("fmov d1, %1\n fmov d2, %2\n fmul d0, d1, d2\n fmov %0, d0" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
    return r;
}
static RT_NOINLINE uint64_t ref_dfrinta(uint64_t a) {
    uint64_t r;
    __asm__ volatile("fmov d1, %1\n frinta d0, d1\n fmov %0, d0" : "=r"(r) : "r"(a) : "v0", "v1");
    return r;
}

static const uint32_t fspecial[] = {0x00000000, 0x80000000, 0x3f800000, 0xbfc00000, 0x7f800000, 0xff800000, 0x7fc12345, 0x7fa00007,
                                    0x00000001, 0x7f7fffff, 0x4f000000, 0xcf000001, 0x40200000, 0x3dcccccd, 0xffc00042, 0x00800000};
static const uint64_t dspecial[] = {0x0000000000000000, 0x8000000000000000, 0x3ff0000000000000, 0x7ff0000000000000, 0x7ff8000000012345,
                                    0x7ff4000000000007, 0x0000000000000001, 0x7fefffffffffffff, 0x4004000000000000, 0xc004000000000000};

int main(void) {
    digest = RT_MIX_INIT;
    unsigned bad;
    uint64_t h;

    // Integer arithmetic, every lane size
    bad = 0; h = RT_MIX_INIT;
    for (unsigned i = 0; i < 16; ++i) {
        const lanes a = vec_at(i), b = vec_at(i + 5);
        lanes r;
        vst1q_u8(r.u8, vaddq_u8(as_u8(a), as_u8(b)));
        for (int k = 0; k < 16; ++k) bad += r.u8[k] != (uint8_t)(a.u8[k] + b.u8[k]);
        h = mix_lanes(h, r);
        vst1q_u16(r.u16, vmulq_u16(vld1q_u16(a.u16), vld1q_u16(b.u16)));
        for (int k = 0; k < 8; ++k) bad += r.u16[k] != (uint16_t)(a.u16[k] * b.u16[k]);
        h = mix_lanes(h, r);
        vst1q_s32(r.s32, vmlsq_s32(vld1q_s32(a.s32), vld1q_s32(b.s32), vld1q_s32(a.s32)));
        for (int k = 0; k < 4; ++k) bad += r.u32[k] != (uint32_t)(a.u32[k] - b.u32[k] * a.u32[k]);
        h = mix_lanes(h, r);
        vst1q_u64(r.u64, vsubq_u64(vld1q_u64(a.u64), vld1q_u64(b.u64)));
        for (int k = 0; k < 2; ++k) bad += r.u64[k] != a.u64[k] - b.u64[k];
        h = mix_lanes(h, r);
        vst1q_s8(r.s8, vnegq_s8(vld1q_s8(a.s8)));
        for (int k = 0; k < 16; ++k) bad += r.s8[k] != (int8_t)(0 - a.s8[k]);
        h = mix_lanes(h, r);
        vst1q_u8(r.u8, vabdq_u8(as_u8(a), as_u8(b)));
        for (int k = 0; k < 16; ++k) bad += r.u8[k] != ref_abd_u8(a.u8[k], b.u8[k]);
        h = mix_lanes(h, r);
        vst1q_s16(r.s16, vmaxq_s16(vld1q_s16(a.s16), vld1q_s16(b.s16)));
        for (int k = 0; k < 8; ++k) bad += r.s16[k] != (a.s16[k] > b.s16[k] ? a.s16[k] : b.s16[k]);
        h = mix_lanes(h, r);
        vst1q_u32(r.u32, vminq_u32(vld1q_u32(a.u32), vld1q_u32(b.u32)));
        for (int k = 0; k < 4; ++k) bad += r.u32[k] != (a.u32[k] < b.u32[k] ? a.u32[k] : b.u32[k]);
        h = mix_lanes(h, r);
    }
    expect("simd.int_arith", bad, h);

    // Saturating and doubling arithmetic
    bad = 0; h = RT_MIX_INIT;
    for (unsigned i = 0; i < 16; ++i) {
        const lanes a = vec_at(i * 3), b = vec_at(i * 3 + 1);
        lanes r;
        vst1q_s16(r.s16, vqaddq_s16(vld1q_s16(a.s16), vld1q_s16(b.s16)));
        for (int k = 0; k < 8; ++k) bad += r.s16[k] != ref_qadd_s16(a.s16[k], b.s16[k]);
        h = mix_lanes(h, r);
        vst1q_u8(r.u8, vqsubq_u8(as_u8(a), as_u8(b)));
        for (int k = 0; k < 16; ++k) bad += r.u8[k] != ref_qsub_u8(a.u8[k], b.u8[k]);
        h = mix_lanes(h, r);
        vst1q_s16(r.s16, vqdmulhq_s16(vld1q_s16(a.s16), vld1q_s16(b.s16)));
        for (int k = 0; k < 8; ++k) bad += r.s16[k] != ref_qdmulh_s16(a.s16[k], b.s16[k]);
        h = mix_lanes(h, r);
        vst1q_s32(r.s32, vqrdmulhq_s32(vld1q_s32(a.s32), vld1q_s32(b.s32)));
        for (int k = 0; k < 4; ++k) bad += r.s32[k] != ref_qrdmulh_s32(a.s32[k], b.s32[k]);
        h = mix_lanes(h, r);
        vst1q_s8(r.s8, vqabsq_s8(vld1q_s8(a.s8)));
        for (int k = 0; k < 16; ++k) bad += r.s8[k] != ref_qabs_s8(a.s8[k]);
        h = mix_lanes(h, r);
    }
    expect("simd.saturating", bad, h);

    // Shifts: by immediate, rounding, by register with negative amounts, narrowing, widening
    bad = 0; h = RT_MIX_INIT;
    for (unsigned i = 0; i < 16; ++i) {
        const lanes a = vec_at(i * 5), b = vec_at(i * 5 + 2);
        lanes r, sh;
        for (int k = 0; k < 16; ++k) sh.s8[k] = (int8_t)((int)(b.u8[k] % 41) - 20);
        vst1q_s16(r.s16, vshlq_s16(vld1q_s16(a.s16), vreinterpretq_s16_s8(vld1q_s8(sh.s8))));
        lanes shh;
        vst1q_s8(shh.s8, vld1q_s8(sh.s8));
        for (int k = 0; k < 8; ++k) bad += r.s16[k] != ref_sshl_s16(a.s16[k], (int8_t)shh.u16[k]);
        h = mix_lanes(h, r);
        vst1q_u32(r.u32, vrshrq_n_u32(vld1q_u32(a.u32), 7));
        for (int k = 0; k < 4; ++k) bad += r.u32[k] != ref_urshr_u32(a.u32[k], 7);
        h = mix_lanes(h, r);
        vst1q_s64(r.s64, vsraq_n_s64(vld1q_s64(a.s64), vld1q_s64(b.s64), 13));
        for (int k = 0; k < 2; ++k) bad += r.u64[k] != a.u64[k] + (uint64_t)(b.s64[k] >> 13);
        h = mix_lanes(h, r);
        vst1_s8(r.s8, vqmovn_s16(vld1q_s16(a.s16)));
        for (int k = 0; k < 8; ++k) bad += r.s8[k] != ref_sqxtn_s16(a.s16[k]);
        vst1_u16(r.u16, vqmovun_s32(vld1q_s32(b.s32)));
        for (int k = 0; k < 4; ++k) bad += r.u16[k] != ref_sqxtun_s32(b.s32[k]);
        h = mix_lanes(h, r);
        vst1_u8(r.u8, vrshrn_n_u16(vld1q_u16(a.u16), 5));
        for (int k = 0; k < 8; ++k) bad += r.u8[k] != ref_rshrn_u16(a.u16[k], 5);
        h = rt_mix(h, r.u64[0]);
        vst1q_s16(r.s16, vmovl_s8(vld1_s8(a.s8)));
        for (int k = 0; k < 8; ++k) bad += r.s16[k] != a.s8[k];
        h = mix_lanes(h, r);
        vst1q_u32(r.u32, vmull_u16(vld1_u16(a.u16), vld1_u16(b.u16)));
        for (int k = 0; k < 4; ++k) bad += r.u32[k] != (uint32_t)a.u16[k] * b.u16[k];
        h = mix_lanes(h, r);
        vst1q_s64(r.s64, vmull_high_s32(vld1q_s32(a.s32), vld1q_s32(b.s32)));
        for (int k = 0; k < 2; ++k) bad += r.s64[k] != (int64_t)a.s32[k + 2] * b.s32[k + 2];
        h = mix_lanes(h, r);
    }
    expect("simd.shift_narrow_widen", bad, h);

    // Reductions, counts, bit operations
    bad = 0; h = RT_MIX_INIT;
    for (unsigned i = 0; i < 16; ++i) {
        const lanes a = vec_at(i * 11), b = vec_at(i * 11 + 3), m = vec_at(i * 11 + 6);
        lanes r;
        unsigned sum = 0, umax = 0;
        int smin = 127;
        for (int k = 0; k < 16; ++k) {
            sum += a.u8[k];
            umax = a.u8[k] > umax ? a.u8[k] : umax;
            smin = a.s8[k] < smin ? a.s8[k] : smin;
        }
        bad += vaddlvq_u8(as_u8(a)) != sum;
        bad += vmaxvq_u8(as_u8(a)) != umax;
        bad += vminvq_s8(vld1q_s8(a.s8)) != smin;
        bad += vaddvq_u32(vld1q_u32(a.u32)) != a.u32[0] + a.u32[1] + a.u32[2] + a.u32[3];
        h = rt_mix(h, (uint64_t)vaddlvq_u8(as_u8(a)) << 32 | vmaxvq_u8(as_u8(a)));
        vst1q_u16(r.u16, vpaddq_u16(vld1q_u16(a.u16), vld1q_u16(b.u16)));
        for (int k = 0; k < 4; ++k) bad += r.u16[k] != (uint16_t)(a.u16[2 * k] + a.u16[2 * k + 1]);
        for (int k = 0; k < 4; ++k) bad += r.u16[k + 4] != (uint16_t)(b.u16[2 * k] + b.u16[2 * k + 1]);
        h = mix_lanes(h, r);
        vst1q_u8(r.u8, vcntq_u8(as_u8(a)));
        for (int k = 0; k < 16; ++k) bad += r.u8[k] != ref_cnt(a.u8[k]);
        h = mix_lanes(h, r);
        vst1q_u8(r.u8, vclzq_u8(as_u8(b)));
        for (int k = 0; k < 16; ++k) bad += r.u8[k] != ref_clz8(b.u8[k]);
        h = mix_lanes(h, r);
        vst1q_u8(r.u8, vrbitq_u8(as_u8(b)));
        for (int k = 0; k < 16; ++k) bad += r.u8[k] != ref_rbit8(b.u8[k]);
        h = mix_lanes(h, r);
        vst1q_u8(r.u8, vbslq_u8(as_u8(m), as_u8(a), as_u8(b)));
        for (int k = 0; k < 16; ++k) bad += r.u8[k] != (uint8_t)((m.u8[k] & a.u8[k]) | (~m.u8[k] & b.u8[k]));
        h = mix_lanes(h, r);
        vst1q_u8(r.u8, veorq_u8(vbicq_u8(as_u8(a), as_u8(m)), vornq_u8(as_u8(b), as_u8(m))));
        for (int k = 0; k < 16; ++k) bad += r.u8[k] != (uint8_t)((a.u8[k] & ~m.u8[k]) ^ (b.u8[k] | (uint8_t)~m.u8[k]));
        h = mix_lanes(h, r);
        vst1q_u16(r.u16, vcgtq_s16(vld1q_s16(a.s16), vld1q_s16(b.s16)));
        for (int k = 0; k < 8; ++k) bad += r.u16[k] != (a.s16[k] > b.s16[k] ? 0xffff : 0);
        vst1q_u32(r.u32, vtstq_u32(vld1q_u32(a.u32), vld1q_u32(m.u32)));
        for (int k = 0; k < 4; ++k) bad += r.u32[k] != ((a.u32[k] & m.u32[k]) ? 0xffffffffu : 0);
        h = mix_lanes(h, r);
    }
    expect("simd.reduce_count_bits", bad, h);

    // Table lookups and permutes
    bad = 0; h = RT_MIX_INIT;
    for (unsigned i = 0; i < 16; ++i) {
        const lanes t0 = vec_at(i), t1 = vec_at(i + 1), idx = vec_at(i * 13 + 4), a = vec_at(i + 9), b = vec_at(i + 10);
        lanes r, ix;
        for (int k = 0; k < 16; ++k) ix.u8[k] = idx.u8[k] % 40;  // in range and out of range of one and two tables
        vst1q_u8(r.u8, vqtbl1q_u8(as_u8(t0), as_u8(ix)));
        for (int k = 0; k < 16; ++k) bad += r.u8[k] != (ix.u8[k] < 16 ? t0.u8[ix.u8[k]] : 0);
        h = mix_lanes(h, r);
        uint8x16x2_t tt = {{as_u8(t0), as_u8(t1)}};
        vst1q_u8(r.u8, vqtbx2q_u8(as_u8(a), tt, as_u8(ix)));
        for (int k = 0; k < 16; ++k) bad += r.u8[k] != (ix.u8[k] < 16 ? t0.u8[ix.u8[k]] : ix.u8[k] < 32 ? t1.u8[ix.u8[k] - 16] : a.u8[k]);
        h = mix_lanes(h, r);
        vst1q_u8(r.u8, vzip1q_u8(as_u8(a), as_u8(b)));
        for (int k = 0; k < 8; ++k) bad += r.u8[2 * k] != a.u8[k] || r.u8[2 * k + 1] != b.u8[k];
        h = mix_lanes(h, r);
        vst1q_u16(r.u16, vuzp2q_u16(vld1q_u16(a.u16), vld1q_u16(b.u16)));
        for (int k = 0; k < 4; ++k) bad += r.u16[k] != a.u16[2 * k + 1] || r.u16[k + 4] != b.u16[2 * k + 1];
        h = mix_lanes(h, r);
        vst1q_u32(r.u32, vtrn1q_u32(vld1q_u32(a.u32), vld1q_u32(b.u32)));
        bad += r.u32[0] != a.u32[0] || r.u32[1] != b.u32[0] || r.u32[2] != a.u32[2] || r.u32[3] != b.u32[2];
        h = mix_lanes(h, r);
        vst1q_u8(r.u8, vextq_u8(as_u8(a), as_u8(b), 5));
        for (int k = 0; k < 16; ++k) bad += r.u8[k] != (k + 5 < 16 ? a.u8[k + 5] : b.u8[k - 11]);
        h = mix_lanes(h, r);
        vst1q_u8(r.u8, vrev64q_u8(as_u8(a)));
        for (int k = 0; k < 16; ++k) bad += r.u8[k] != a.u8[(k & 8) + 7 - (k & 7)];
        h = mix_lanes(h, r);
        vst1q_u16(r.u16, vdupq_laneq_u16(vld1q_u16(b.u16), 5));
        for (int k = 0; k < 8; ++k) bad += r.u16[k] != b.u16[5];
        vst1q_u32(r.u32, vsetq_lane_u32(0xdeadbeefu, vld1q_u32(a.u32), 2));
        bad += r.u32[2] != 0xdeadbeefu || r.u32[0] != a.u32[0] || r.u32[3] != a.u32[3];
        h = mix_lanes(h, r);
    }
    expect("simd.table_permute", bad, h);

    // Interleaving loads and stores
    bad = 0; h = RT_MIX_INIT;
    {
        uint8_t buf[64], out[64];
        for (int k = 0; k < 64; ++k) buf[k] = seed_bytes[k] ^ (uint8_t)(k * 37);
        const uint8x16x2_t two = vld2q_u8(buf);
        const uint8x16x4_t four = vld4q_u8(buf);
        lanes l;
        for (int j = 0; j < 2; ++j) {
            l = from_u8(two.val[j]);
            for (int k = 0; k < 16; ++k) bad += l.u8[k] != buf[2 * k + j];
            h = mix_lanes(h, l);
        }
        for (int j = 0; j < 4; ++j) {
            l = from_u8(four.val[j]);
            for (int k = 0; k < 16; ++k) bad += l.u8[k] != buf[4 * k + j];
            h = mix_lanes(h, l);
        }
        const uint16x8x3_t three = vld3q_u16((const uint16_t *)buf);
        vst3q_u16((uint16_t *)out, three);
        bad += memcmp(out, buf, 48) != 0;
        vst4q_u8(out, four);
        bad += memcmp(out, buf, 64) != 0;
        const uint32x4_t lane = vld1q_lane_u32((const uint32_t *)(buf + 20), vdupq_n_u32(7), 3);
        l.u32[0] = vgetq_lane_u32(lane, 0);
        l.u32[3] = vgetq_lane_u32(lane, 3);
        uint32_t w;
        memcpy(&w, buf + 20, 4);
        bad += l.u32[0] != 7 || l.u32[3] != w;
    }
    expect("simd.load_store", bad, h);

    // Floating-point vectors against the scalar instruction, lane by lane
    bad = 0; h = RT_MIX_INIT;
    for (unsigned i = 0; i < 16; ++i) {
        lanes a, b, c, r;
        for (int k = 0; k < 4; ++k) {
            a.u32[k] = fspecial[(i + k) % 16];
            b.u32[k] = fspecial[(i * 3 + k * 5) % 16];
            c.u32[k] = fspecial[(i * 7 + k) % 16];
        }
        vst1q_f32(r.f32, vaddq_f32(vld1q_f32(a.f32), vld1q_f32(b.f32)));
        for (int k = 0; k < 4; ++k) bad += r.u32[k] != ref_fadd(a.u32[k], b.u32[k]);
        h = mix_lanes(h, r);
        vst1q_f32(r.f32, vmaxnmq_f32(vld1q_f32(a.f32), vld1q_f32(b.f32)));
        for (int k = 0; k < 4; ++k) bad += r.u32[k] != ref_fmaxnm(a.u32[k], b.u32[k]);
        h = mix_lanes(h, r);
        vst1q_f32(r.f32, vfmaq_f32(vld1q_f32(c.f32), vld1q_f32(a.f32), vld1q_f32(b.f32)));
        for (int k = 0; k < 4; ++k) bad += r.u32[k] != ref_fmla(c.u32[k], a.u32[k], b.u32[k]);
        h = mix_lanes(h, r);
        vst1q_f32(r.f32, vrecpeq_f32(vld1q_f32(a.f32)));
        for (int k = 0; k < 4; ++k) bad += r.u32[k] != ref_frecpe(a.u32[k]);
        h = mix_lanes(h, r);
        vst1q_s32(r.s32, vcvtq_s32_f32(vld1q_f32(b.f32)));
        for (int k = 0; k < 4; ++k) bad += r.u32[k] != ref_fcvtzs(b.u32[k]);
        h = mix_lanes(h, r);
        lanes da, db;
        for (int k = 0; k < 2; ++k) {
            da.u64[k] = dspecial[(i + k) % 10];
            db.u64[k] = dspecial[(i * 3 + k * 7) % 10];
        }
        vst1q_f64(r.f64, vmulq_f64(vld1q_f64(da.f64), vld1q_f64(db.f64)));
        for (int k = 0; k < 2; ++k) bad += r.u64[k] != ref_dfmul(da.u64[k], db.u64[k]);
        h = mix_lanes(h, r);
        vst1q_f64(r.f64, vrndaq_f64(vld1q_f64(db.f64)));
        for (int k = 0; k < 2; ++k) bad += r.u64[k] != ref_dfrinta(db.u64[k]);
        h = mix_lanes(h, r);
    }
    expect("simd.float_vectors", bad, h);

    // A few lanes the Arm ARM fixes outright
    RT_CHECK64("simd.qadd_s16.saturates", (uint64_t)vgetq_lane_s16(vqaddq_s16(vdupq_n_s16(RT_OPAQUE((int16_t)0x7000)), vdupq_n_s16(0x2000)), 3), 0x7fff);
    RT_CHECK64("simd.qdmulh.min", (uint64_t)(uint16_t)vgetq_lane_s16(vqdmulhq_s16(vdupq_n_s16(RT_OPAQUE((int16_t)-32768)), vdupq_n_s16(-32768)), 0), 0x7fff);
    RT_CHECK64("simd.addv", (uint64_t)vaddvq_u8(vdupq_n_u8(RT_OPAQUE((uint8_t)0x11))), 0x10);
    RT_CHECK64("simd.tbl.out_of_range", vgetq_lane_u64(vreinterpretq_u64_u8(vqtbl1q_u8(vdupq_n_u8(0xab), vdupq_n_u8(RT_OPAQUE((uint8_t)16)))), 1), 0);
    rt_note("simd.digest", digest, 16);
    (void)bad_total;
    return rt_finish();
}
