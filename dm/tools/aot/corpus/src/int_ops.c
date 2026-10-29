// Integer arithmetic and bit manipulation, in the manner of the compiler torture tests: every operand comes from volatile storage, each operation
// the compilers lower to one A64 instruction (or a short idiom) is checked against a reference written another way (loops, smaller pieces,
// constants of the Arm ARM), and the corner cases C leaves undefined (division by zero, INT_MIN / -1, shifts by the width or more) run through
// inline assembly, whose results the Arm ARM defines. Built without unwind tables (build_corpus.sh), so discovery finds its functions without FDEs.
#include <arm_acle.h>

#include "rt.h"

static volatile uint64_t vin[] = {
    0, 1, 2, 3, 5, 7, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000, 0x7fffffff, 0x80000000, 0xffffffff, 0x100000000ull,
    0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull, 0x123456789abcdef0ull, 0xfedcba9876543210ull, 0x5555555555555555ull,
    0xaaaaaaaaaaaaaaaaull, 0x0f0f0f0f0f0f0f0full, 0x8000000000000001ull, 0xfffffffffffffffeull, 37, 1000003, 0xdeadbeefcafebabeull,
};
#define NIN (sizeof vin / sizeof vin[0])

static uint64_t in(unsigned i) {
    return vin[i % NIN];
}

// ---- References ----

static RT_NOINLINE unsigned ref_clz64(uint64_t x) {
    unsigned n = 0;
    for (int i = 63; i >= 0 && !((x >> i) & 1); --i) {
        ++n;
    }
    return n;
}

static RT_NOINLINE unsigned ref_cls64(uint64_t x) {
    const unsigned top = x >> 63;
    unsigned n = 0;
    for (int i = 62; i >= 0 && ((x >> i) & 1) == top; --i) {
        ++n;
    }
    return n;
}

static RT_NOINLINE uint64_t ref_rbit64(uint64_t x) {
    uint64_t r = 0;
    for (int i = 0; i < 64; ++i) {
        r |= ((x >> i) & 1) << (63 - i);
    }
    return r;
}

static RT_NOINLINE uint64_t ref_rev64(uint64_t x) {
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) {
        r |= ((x >> (8 * i)) & 0xff) << (8 * (7 - i));
    }
    return r;
}

static RT_NOINLINE unsigned ref_popcount64(uint64_t x) {
    unsigned n = 0;
    for (int i = 0; i < 64; ++i) {
        n += (x >> i) & 1;
    }
    return n;
}

// The high 64 bits of a 64 x 64 multiply from 32-bit pieces
static RT_NOINLINE uint64_t ref_umulh(uint64_t a, uint64_t b) {
    const uint64_t al = (uint32_t)a, ah = a >> 32, bl = (uint32_t)b, bh = b >> 32;
    const uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    const uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

static RT_NOINLINE uint64_t ref_smulh(uint64_t a, uint64_t b) {
    uint64_t h = ref_umulh(a, b);
    if ((int64_t)a < 0) {
        h -= b;
    }
    if ((int64_t)b < 0) {
        h -= a;
    }
    return h;
}

// Bitwise CRC-32 (reflected polynomial 0xEDB88320) and CRC-32C (0x82F63B78) over the low `bytes` bytes of data
static RT_NOINLINE uint32_t ref_crc(uint32_t crc, uint64_t data, unsigned bytes, uint32_t poly) {
    for (unsigned i = 0; i < bytes; ++i) {
        crc ^= (uint8_t)(data >> (8 * i));
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ ((crc & 1) ? poly : 0);
        }
    }
    return crc;
}

// Unsigned division by shift and subtract
static RT_NOINLINE uint64_t ref_udiv(uint64_t n, uint64_t d) {
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; --i) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= 1ull << i;
        }
    }
    return q;
}

static RT_NOINLINE uint64_t ref_ror(uint64_t x, unsigned n) {
    uint64_t r = x;
    for (unsigned i = 0; i < (n & 63); ++i) {
        r = (r >> 1) | ((r & 1) << 63);
    }
    return r;
}

// ---- The operations under test, each behind noinline so that it stays one instruction (or the compiler's idiom) on its operands ----

static RT_NOINLINE uint64_t op_clz(uint64_t x) { return x ? (uint64_t)__builtin_clzll(x) : 64; }
static RT_NOINLINE uint64_t op_cls(uint64_t x) { return (uint64_t)__builtin_clrsbll((long long)x); }
static RT_NOINLINE uint64_t op_rbit(uint64_t x) { return __rbitll(x); }
static RT_NOINLINE uint64_t op_rev(uint64_t x) { return __builtin_bswap64(x); }
static RT_NOINLINE uint64_t op_rev16(uint64_t x) { return __rev16ll(x); }
static RT_NOINLINE uint64_t op_rev32(uint64_t x) { return ((uint64_t)__builtin_bswap32((uint32_t)(x >> 32)) << 32) | __builtin_bswap32((uint32_t)x); }
static RT_NOINLINE uint64_t op_popcount(uint64_t x) { return (uint64_t)__builtin_popcountll(x); }
static RT_NOINLINE uint64_t op_umulh(uint64_t a, uint64_t b) { return (uint64_t)(((unsigned __int128)a * b) >> 64); }
static RT_NOINLINE uint64_t op_smulh(uint64_t a, uint64_t b) { return (uint64_t)(((__int128)(int64_t)a * (int64_t)b) >> 64); }
static RT_NOINLINE uint64_t op_madd(uint64_t a, uint64_t b, uint64_t c) { return a * b + c; }
static RT_NOINLINE uint64_t op_msub(uint64_t a, uint64_t b, uint64_t c) { return c - a * b; }
static RT_NOINLINE uint64_t op_umull(uint32_t a, uint32_t b) { return (uint64_t)a * b; }
static RT_NOINLINE int64_t op_smull(int32_t a, int32_t b) { return (int64_t)a * b; }
static RT_NOINLINE uint64_t op_udiv(uint64_t a, uint64_t b) { return a / b; }
static RT_NOINLINE int64_t op_sdiv(int64_t a, int64_t b) { return a / b; }
static RT_NOINLINE uint64_t op_ror(uint64_t x, unsigned n) { return (x >> (n & 63)) | (x << ((64 - n) & 63)); }
static RT_NOINLINE uint32_t op_ror32(uint32_t x, unsigned n) { return (x >> (n & 31)) | (x << ((32 - n) & 31)); }

// Divisions and shifts whose C forms are undefined: the instructions themselves
static RT_NOINLINE uint64_t asm_udiv(uint64_t a, uint64_t b) {
    uint64_t r;
    __asm__("udiv %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}
static RT_NOINLINE int64_t asm_sdiv(int64_t a, int64_t b) {
    int64_t r;
    __asm__("sdiv %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}
static RT_NOINLINE uint32_t asm_sdiv32(uint32_t a, uint32_t b) {
    uint32_t r;
    __asm__("sdiv %w0, %w1, %w2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}
static RT_NOINLINE uint64_t asm_lslv(uint64_t x, uint64_t n) {
    uint64_t r;
    __asm__("lslv %0, %1, %2" : "=r"(r) : "r"(x), "r"(n));
    return r;
}
static RT_NOINLINE uint64_t asm_asrv(uint64_t x, uint64_t n) {
    uint64_t r;
    __asm__("asrv %0, %1, %2" : "=r"(r) : "r"(x), "r"(n));
    return r;
}
static RT_NOINLINE uint32_t asm_lsrv32(uint32_t x, uint32_t n) {
    uint32_t r;
    __asm__("lsrv %w0, %w1, %w2" : "=r"(r) : "r"(x), "r"(n));
    return r;
}

// Add with carry across 128 bits (adds / adcs / adc), and the flags of the overflow builtins (adds / subs and a condition)
static RT_NOINLINE unsigned __int128 op_add128(unsigned __int128 a, unsigned __int128 b) { return a + b; }
static RT_NOINLINE unsigned __int128 op_sub128(unsigned __int128 a, unsigned __int128 b) { return a - b; }
static RT_NOINLINE int op_sadd_overflow(int64_t a, int64_t b) { int64_t r; return __builtin_add_overflow(a, b, &r); }
static RT_NOINLINE int op_usub_overflow(uint64_t a, uint64_t b) { uint64_t r; return __builtin_sub_overflow(a, b, &r); }
static RT_NOINLINE int op_smul_overflow(int64_t a, int64_t b) { int64_t r; return __builtin_mul_overflow(a, b, &r); }

// Conditional selects and compare chains (csel, csinc, csinv, csneg, cset, ccmp)
static RT_NOINLINE uint64_t op_select(uint64_t a, uint64_t b) {
    const uint64_t s1 = (int64_t)a < (int64_t)b ? a : b;
    const uint64_t s2 = a == b ? a + 1 : b;
    const uint64_t s3 = a > b ? ~a : b;
    const uint64_t s4 = (int64_t)a >= 0 ? a : -a;
    const uint64_t s5 = (a & 0xff) == 7 && b > 100 ? 1 : 0;
    const uint64_t s6 = (a > 3 || (int64_t)b < -3) ? 11 : 22;
    return s1 ^ (s2 << 1) ^ (s3 << 2) ^ (s4 << 3) ^ (s5 << 4) ^ (s6 << 5);
}

static uint64_t ref_select(uint64_t a, uint64_t b) {
    uint64_t s1, s2, s3, s4, s5, s6;
    if ((int64_t)a < (int64_t)b) s1 = a; else s1 = b;
    if (a == b) s2 = a + 1; else s2 = b;
    if (a > b) s3 = ~a; else s3 = b;
    if ((a >> 63) == 0) s4 = a; else s4 = 0 - a;
    s5 = 0;
    if ((a & 0xff) == 7) { if (b > 100) s5 = 1; }
    s6 = 22;
    if (a > 3) s6 = 11;
    if ((b >> 63) && b < (uint64_t)-3) s6 = 11;
    return s1 ^ (s2 << 1) ^ (s3 << 2) ^ (s4 << 3) ^ (s5 << 4) ^ (s6 << 5);
}

// Bit fields: ubfx, sbfx, bfi, bfxil, extr
struct fields {
    uint64_t a : 5, b : 13, c : 1, d : 27, e : 18;
};
static RT_NOINLINE uint64_t op_fields(uint64_t x, uint64_t y) {
    struct fields f;
    memcpy(&f, &x, 8);
    f.b = (unsigned)y;
    f.d += f.a;
    const int64_t s = (int64_t)(x << 20) >> 41;  // sbfx: bits 21..43, sign-extended
    uint64_t out;
    memcpy(&out, &f, 8);
    return out ^ (uint64_t)s ^ ((x >> 17) | (y << 47));
}
static uint64_t ref_fields(uint64_t x, uint64_t y) {
    uint64_t a = x & 0x1f, b = (x >> 5) & 0x1fff, c = (x >> 18) & 1, d = (x >> 19) & 0x7ffffff, e = (x >> 46) & 0x3ffff;
    b = y & 0x1fff;
    d = (d + a) & 0x7ffffff;
    const uint64_t out = a | (b << 5) | (c << 18) | (d << 19) | (e << 46);
    uint64_t s = (x >> 21) & 0x7fffff;
    if (s & 0x400000) {
        s |= ~0x7fffffull;
    }
    return out ^ s ^ ((x >> 17) | (y << 47));
}

// Loads of every size and sign, unaligned, through a packed structure
struct __attribute__((packed)) unaligned {
    uint8_t pad;
    int16_t h;
    int32_t w;
    int8_t b;
    uint64_t x;
};
static volatile struct unaligned vua = {0x11, -2, -300000, -5, 0x8877665544332211ull};
static RT_NOINLINE int64_t op_loads(void) {
    return vua.h + vua.w + vua.b + (int64_t)(vua.x >> 8);
}

// A thread-local counter: tpidr_el0 plus a local-exec offset
static __thread uint64_t tls_counter __attribute__((tls_model("local-exec")));
static RT_NOINLINE uint64_t op_tls(uint64_t add) {
    tls_counter += add;
    return tls_counter;
}

int main(void) {
    uint64_t h_clz = RT_MIX_INIT, h_bits = RT_MIX_INIT, h_mul = RT_MIX_INIT, h_div = RT_MIX_INIT, h_misc = RT_MIX_INIT;
    unsigned bad_clz = 0, bad_cls = 0, bad_rbit = 0, bad_rev = 0, bad_pop = 0, bad_umulh = 0, bad_smulh = 0, bad_div = 0, bad_ror = 0;
    unsigned bad_crc = 0, bad_sel = 0, bad_fields = 0, bad_wide = 0;
    for (unsigned i = 0; i < NIN; ++i) {
        const uint64_t x = in(i);
        h_clz = rt_mix(h_clz, op_clz(x));
        bad_clz += op_clz(x) != ref_clz64(x);
        bad_cls += op_cls(x) != ref_cls64(x);
        bad_rbit += op_rbit(x) != ref_rbit64(x);
        bad_rev += op_rev(x) != ref_rev64(x);
        bad_rev += op_rev16(x) != (((x & 0x00ff00ff00ff00ffull) << 8) | ((x >> 8) & 0x00ff00ff00ff00ffull));
        bad_rev += op_rev32(x) != ((ref_rev64(x) >> 32) | (ref_rev64(x) << 32));
        bad_pop += op_popcount(x) != ref_popcount64(x);
        h_bits = rt_mix(rt_mix(rt_mix(h_bits, op_rbit(x)), op_rev(x)), op_popcount(x));
        for (unsigned j = 0; j < NIN; ++j) {
            const uint64_t y = in(j * 7 + 3);
            bad_umulh += op_umulh(x, y) != ref_umulh(x, y);
            bad_smulh += op_smulh(x, y) != ref_smulh(x, y);
            h_mul = rt_mix(rt_mix(h_mul, op_madd(x, y, i)), op_msub(x, y, j));
            bad_wide += op_umull((uint32_t)x, (uint32_t)y) != (uint64_t)(uint32_t)x * (uint32_t)y;
            bad_wide += (uint64_t)op_smull((int32_t)x, (int32_t)y) != (uint64_t)((int64_t)(int32_t)x * (int64_t)(int32_t)y);
            if (y) {
                bad_div += op_udiv(x, y) != ref_udiv(x, y);
                bad_div += asm_udiv(x, y) != op_udiv(x, y);
                if (!(x == 0x8000000000000000ull && y == ~0ull)) {
                    bad_div += (uint64_t)asm_sdiv((int64_t)x, (int64_t)y) != (uint64_t)op_sdiv((int64_t)x, (int64_t)y);
                }
                h_div = rt_mix(h_div, op_udiv(x, y));
            }
            bad_sel += op_select(x, y) != ref_select(x, y);
            bad_fields += op_fields(x, y) != ref_fields(x, y);
            bad_crc += __crc32d((uint32_t)y, x) != ref_crc((uint32_t)y, x, 8, 0xEDB88320u);
            bad_crc += __crc32cw((uint32_t)y, (uint32_t)x) != ref_crc((uint32_t)y, x, 4, 0x82F63B78u);
            bad_crc += __crc32b((uint32_t)y, (uint8_t)x) != ref_crc((uint32_t)y, x, 1, 0xEDB88320u);
            bad_crc += __crc32ch((uint32_t)y, (uint16_t)x) != ref_crc((uint32_t)y, x, 2, 0x82F63B78u);
        }
        for (unsigned n = 0; n < 130; n += 13) {
            bad_ror += op_ror(x, n) != ref_ror(x, n);
            bad_ror += op_ror32((uint32_t)x, n) != (uint32_t)((((uint64_t)(uint32_t)x << 32 | (uint32_t)x) >> (n & 31)));
            h_misc = rt_mix(h_misc, asm_lslv(x, n) ^ asm_asrv(x, n + 64));
            bad_ror += asm_lslv(x, n) != (x << (n & 63));
            bad_ror += asm_asrv(x, n) != (uint64_t)((int64_t)x >> (n & 63));
        }
    }
    RT_CHECK32("clz.ref", bad_clz, 0);
    RT_CHECK32("cls.ref", bad_cls, 0);
    RT_CHECK32("rbit.ref", bad_rbit, 0);
    RT_CHECK32("rev.ref", bad_rev, 0);
    RT_CHECK32("popcount.ref", bad_pop, 0);
    RT_CHECK32("umulh.ref", bad_umulh, 0);
    RT_CHECK32("smulh.ref", bad_smulh, 0);
    RT_CHECK32("umull_smull.ref", bad_wide, 0);
    RT_CHECK32("div.ref", bad_div, 0);
    RT_CHECK32("ror_shiftv.ref", bad_ror, 0);
    RT_CHECK32("crc32.ref", bad_crc, 0);
    RT_CHECK32("csel_ccmp.ref", bad_sel, 0);
    RT_CHECK32("bitfield.ref", bad_fields, 0);
    rt_note("clz.digest", h_clz, 16);
    rt_note("bits.digest", h_bits, 16);
    rt_note("mul.digest", h_mul, 16);
    rt_note("div.digest", h_div, 16);
    rt_note("shift.digest", h_misc, 16);

    // Constants of the Arm ARM
    RT_CHECK64("clz.1", op_clz(RT_OPAQUE(1ull)), 63);
    RT_CHECK64("cls.-1", op_cls(RT_OPAQUE(~0ull)), 63);
    RT_CHECK64("cls.0x4000", op_cls(RT_OPAQUE(0x0000400000000000ull)), 16);
    RT_CHECK64("rbit.1", op_rbit(RT_OPAQUE(1ull)), 0x8000000000000000ull);
    RT_CHECK64("rev16", op_rev16(RT_OPAQUE(0x0123456789abcdefull)), 0x23016745ab89efcdull);
    RT_CHECK64("rev32", op_rev32(RT_OPAQUE(0x0123456789abcdefull)), 0x67452301efcdab89ull);
    RT_CHECK64("udiv.by0", asm_udiv(RT_OPAQUE(12345ull), RT_OPAQUE(0ull)), 0);
    RT_CHECK64("sdiv.by0", asm_sdiv(RT_OPAQUE(-12345ll), RT_OPAQUE(0ll)), 0);
    RT_CHECK64("sdiv.min_by_m1", asm_sdiv(RT_OPAQUE((int64_t)0x8000000000000000ull), RT_OPAQUE(-1ll)), 0x8000000000000000ull);
    RT_CHECK32("sdiv32.min_by_m1", asm_sdiv32(RT_OPAQUE(0x80000000u), RT_OPAQUE(0xffffffffu)), 0x80000000u);
    RT_CHECK64("lslv.64", asm_lslv(RT_OPAQUE(0x1234ull), RT_OPAQUE(64ull)), 0x1234);
    RT_CHECK64("lslv.65", asm_lslv(RT_OPAQUE(0x1234ull), RT_OPAQUE(65ull)), 0x2468);
    RT_CHECK64("asrv.127", asm_asrv(RT_OPAQUE(0x8000000000000000ull), RT_OPAQUE(127ull)), ~0ull);
    RT_CHECK32("lsrv32.33", asm_lsrv32(RT_OPAQUE(0x80000000u), RT_OPAQUE(33u)), 0x40000000u);
    RT_CHECK64("umulh.max", op_umulh(RT_OPAQUE(~0ull), RT_OPAQUE(~0ull)), 0xfffffffffffffffeull);
    RT_CHECK64("smulh.min", op_smulh(RT_OPAQUE(0x8000000000000000ull), RT_OPAQUE(0x8000000000000000ull)), 0x4000000000000000ull);
    RT_CHECK32("crc32.check", __crc32d(~0u, RT_OPAQUE(0x3837363534333231ull)) ^ ~0u, 0x9ae0daafu);  // CRC-32 of "12345678"

    const unsigned __int128 a = ((unsigned __int128)RT_OPAQUE(0xffffffffffffffffull) << 64) | RT_OPAQUE(0xffffffffffffffffull);
    const unsigned __int128 s = op_add128(a, RT_OPAQUE(2ull));
    rt_check128("add128.carry", (uint64_t)s, (uint64_t)(s >> 64), 1, 0);
    const unsigned __int128 d = op_sub128(RT_OPAQUE(0ull), RT_OPAQUE(1ull));
    rt_check128("sub128.borrow", (uint64_t)d, (uint64_t)(d >> 64), ~0ull, ~0ull);
    RT_CHECK32("sadd.overflow", op_sadd_overflow(RT_OPAQUE(0x7fffffffffffffffll), RT_OPAQUE(1ll)), 1);
    RT_CHECK32("sadd.no_overflow", op_sadd_overflow(RT_OPAQUE(-5ll), RT_OPAQUE(3ll)), 0);
    RT_CHECK32("usub.borrow", op_usub_overflow(RT_OPAQUE(3ull), RT_OPAQUE(5ull)), 1);
    RT_CHECK32("smul.overflow", op_smul_overflow(RT_OPAQUE(0x100000000ll), RT_OPAQUE(0x80000000ll)), 1);
    RT_CHECK32("smul.no_overflow", op_smul_overflow(RT_OPAQUE(-0x100000000ll), RT_OPAQUE(0x7fffffffll)), 0);
    RT_CHECK64("loads.unaligned", (uint64_t)op_loads(), (uint64_t)(-2 - 300000 - 5 + 0x0088776655443322ll));
    op_tls(5);
    RT_CHECK64("tls.counter", op_tls(RT_OPAQUE(7ull)), 12);
    return rt_finish();
}
