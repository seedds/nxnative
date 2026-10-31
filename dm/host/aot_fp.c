// The run-time helpers recompiled code calls for the floating-point and cryptographic operations whose exact Arm semantics are not one x86 sequence
// (docs/design/aot-recompiler.md 7.1 to 7.3; their interface is dm/tools/aot/src/lift/runtime.h): the reciprocal and reciprocal square root estimates and
// steps, the half-precision compare, fused multiply-add where FPCR.FZ needs Arm's flush before rounding, rounding to integral and conversions to fixed point in
// the modes x86 has no instruction for, conversions between precisions, and AES rounds on a CPU without AES-NI. A pack's library imports them from the host.
// dm/tools/aot/src/lift/runtime.cpp defines them for nxaot's own tools by wrapping dynarmic's common/fp and common/crypto (C++20); the host is C and its build
// image has no C++ standard library, so this file ports exactly the functions those wrappers call (dynarmic, dm/third_party/dynarmic, 0BSD: fp/unpacked.cpp,
// fp/process_nan.cpp, fp/fused.cpp, fp/op/*.cpp, math_util.cpp, crypto/aes.cpp), keeping their order of exception side effects. The differential test
// dm/host/tests/aot_fp_diff.sh compiles this file beside runtime.cpp and compares every helper on random and special operands (the nxnative-aot image).
// Arguments as runtime.h: raw bits, the FPCR the code is specialised for, the guest's FPSR, into which the cumulative bits the Arm pseudocode raises are ORed.
// FPCR's trap enables (IOE, DZE, OFE, UFE, IXE, IDE) are read as dynarmic reads them, but no exception is ever taken: the Cortex-A57 does not trap them
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>

#if defined(DM_AOT_FP_TEST)   // the differential test links this file beside runtime.cpp: its names get a prefix
#define FPNAME(n) c_##n
#define FPEXPORT
#else
#if defined(_WIN32)
#define FPEXPORT __declspec(dllexport)
#else
#define FPEXPORT __attribute__((visibility("default")))
#endif
#define FPNAME(n) n
#endif

typedef unsigned __int128 u128;

// FPCR (dynarmic common/fp/fpcr.h) and FPSR (fpsr.h) bits
#define FPCR_MASK 0x07FF9F00u
#define FPCR_AHP (1u << 26)
#define FPCR_DN (1u << 25)
#define FPCR_FZ (1u << 24)
#define FPCR_FZ16 (1u << 19)
#define FPCR_UFE (1u << 11)
#define FPSR_IOC 0x01u
#define FPSR_DZC 0x02u
#define FPSR_OFC 0x04u
#define FPSR_UFC 0x08u
#define FPSR_IXC 0x10u
#define FPSR_IDC 0x80u
static unsigned rmode(uint32_t fpcr) { return (fpcr >> 22) & 3; }

// dynarmic's RoundingMode
enum { RM_TIE_EVEN = 0, RM_PLUS_INF = 1, RM_MINUS_INF = 2, RM_ZERO = 3, RM_TIE_AWAY = 4, RM_ODD = 5 };
enum { T_NONZERO, T_ZERO, T_INF, T_QNAN, T_SNAN };   // FPType
enum { RE_ZERO, RE_LESS_HALF, RE_HALF, RE_MORE_HALF };   // ResidualError

// FPInfo of a precision
typedef struct {
    int total, ew, mw;   // total width, exponent width, explicit mantissa width
    uint64_t sign, emask, mmask, implicit, width;
    int emin, bias;
} fpinfo;
static const fpinfo F16 = {16, 5, 10, 0x8000u, 0x7C00u, 0x3FFu, 0x400u, 0xFFFFu, -14, 15};
static const fpinfo F32 = {32, 8, 23, 0x80000000u, 0x7F800000u, 0x7FFFFFu, 0x800000u, 0xFFFFFFFFu, -126, 127};
static const fpinfo F64 = {64, 11, 52, 0x8000000000000000ull, 0x7FF0000000000000ull, 0xFFFFFFFFFFFFFull, 0x10000000000000ull, ~0ull, -1022, 1023};
static const fpinfo *info(uint32_t fsize) { return fsize == 16 ? &F16 : fsize == 32 ? &F32 : &F64; }   // runtime.cpp's BySize: anything else is 64
static uint64_t zero_of(const fpinfo *I, int sign) { return sign ? I->sign : 0; }
static uint64_t inf_of(const fpinfo *I, int sign) { return I->emask | zero_of(I, sign); }
static uint64_t max_normal(const fpinfo *I, int sign) { return (I->emask - 1) | zero_of(I, sign); }
static uint64_t default_nan(const fpinfo *I) { return I->emask | (1ull << (I->mw - 1)); }
static uint64_t ones(int n) { return n <= 0 ? 0 : n >= 64 ? ~0ull : (1ull << n) - 1; }
static int hsb(uint64_t v) { return 63 - __builtin_clzll(v); }   // mcl::bit::highest_set_bit, v != 0

// Safe::LogicalShiftLeft / Right and ArithmeticShiftLeft / Right on u64: a shift of 64 or more gives 0 (or the sign), a negative one shifts the other way
static uint64_t lsr(uint64_t v, int s);
static uint64_t lsl(uint64_t v, int s) { return s >= 64 ? 0 : s < 0 ? lsr(v, -s) : v << s; }
static uint64_t lsr(uint64_t v, int s) { return s >= 64 ? 0 : s < 0 ? lsl(v, -s) : v >> s; }
static uint64_t asr(uint64_t v, int s);
static uint64_t asl(uint64_t v, int s) { return s >= 64 ? 0 : s < 0 ? asr(v, -s) : v << s; }
static uint64_t asr(uint64_t v, int s) { return s >= 64 ? ((v >> 63) ? ~0ull : 0) : s < 0 ? asl(v, -s) : (uint64_t)((int64_t)v >> s); }

static int residual(uint64_t m, int shift) {   // ResidualErrorOnRightShift
    if (shift <= 0 || m == 0) return RE_ZERO;
    if (shift > 64) return (m >> 63) ? RE_MORE_HALF : RE_LESS_HALF;
    const uint64_t half = 1ull << (shift - 1), e = m & ones(shift);
    return !e ? RE_ZERO : e < half ? RE_LESS_HALF : e == half ? RE_HALF : RE_MORE_HALF;
}

// FPUnpacked: the mantissa normalized with its leading one at bit 62 (normalized_point_position)
typedef struct { int sign, exponent; uint64_t mantissa; } fpu;
static fpu to_normalized(int sign, int exponent, uint64_t v) {
    if (!v) return (fpu){sign, 0, 0};
    const int offset = 62 - hsb(v);
    return (fpu){sign, exponent - (offset - 62), v << offset};
}

// FPUnpackBase: the type, and the value (the sign is in it)
static int unpack_base(const fpinfo *I, uint64_t op, uint32_t fpcr, uint32_t *fl, fpu *v) {
    const int sign = (int)((op >> (I->ew + I->mw)) & 1), half = I->total == 16;
    const uint64_t exp_raw = (op >> I->mw) & ones(I->ew), frac_raw = op & I->mmask;
    if (exp_raw == 0) {
        if (half) {
            if (frac_raw == 0 || (fpcr & FPCR_FZ16)) { *v = (fpu){sign, 0, 0}; return T_ZERO; }
        } else if (frac_raw == 0 || (fpcr & FPCR_FZ)) {
            if (frac_raw) *fl |= FPSR_IDC;
            *v = (fpu){sign, 0, 0}; return T_ZERO;
        }
        *v = to_normalized(sign, I->emin - I->mw, frac_raw);
        return T_NONZERO;
    }
    if (exp_raw == ones(I->ew) && (!half || !(fpcr & FPCR_AHP))) {
        if (frac_raw == 0) { *v = to_normalized(sign, 1000000, 1); return T_INF; }
        *v = (fpu){sign, 0, 0};
        return (frac_raw >> (I->mw - 1)) & 1 ? T_QNAN : T_SNAN;
    }
    *v = (fpu){sign, (int)exp_raw - I->bias, (frac_raw | I->implicit) << (62 - I->mw)};
    return T_NONZERO;
}
static int unpack(const fpinfo *I, uint64_t op, uint32_t fpcr, uint32_t *fl, fpu *v) { return unpack_base(I, op, fpcr & ~FPCR_AHP, fl, v); }      // FPUnpack
static int unpack_cv(const fpinfo *I, uint64_t op, uint32_t fpcr, uint32_t *fl, fpu *v) { return unpack_base(I, op, fpcr & ~FPCR_FZ16, fl, v); }  // FPUnpackCV

// Normalize<F>: sign, exponent, mantissa with F + 1 bits and the residual error
static void normalize(int F, fpu op, int extra, int *exponent, uint64_t *mantissa, int *error) {
    const int top = hsb(op.mantissa), shift = top - F + extra;
    *mantissa = lsr(op.mantissa, shift);
    *error = residual(op.mantissa, shift);
    *exponent = op.exponent + top - 62;
}

// FPRoundBase
static uint64_t round_base(const fpinfo *I, fpu op, uint32_t fpcr, int rounding, uint32_t *fl) {
    const int E = I->ew, F = I->mw, half = I->total == 16, sign = op.sign;
    int exponent, error;
    uint64_t mantissa;
    normalize(F, op, 0, &exponent, &mantissa, &error);
    if (((!half && (fpcr & FPCR_FZ)) || (half && (fpcr & FPCR_FZ16))) && exponent < I->emin) { *fl |= FPSR_UFC; return zero_of(I, sign); }
    int biased_exp = exponent - I->emin + 1;
    if (biased_exp < 0) biased_exp = 0;
    if (biased_exp == 0) normalize(F, op, I->emin - exponent, &exponent, &mantissa, &error);
    if (biased_exp == 0 && (error != RE_ZERO || (fpcr & FPCR_UFE))) *fl |= FPSR_UFC;
    int round_up = 0, overflow_to_inf = 0;
    switch (rounding) {
    case RM_TIE_EVEN: round_up = error > RE_HALF || (error == RE_HALF && (mantissa & 1)); overflow_to_inf = 1; break;
    case RM_PLUS_INF: round_up = error != RE_ZERO && !sign; overflow_to_inf = !sign; break;
    case RM_MINUS_INF: round_up = error != RE_ZERO && sign; overflow_to_inf = sign; break;
    default: break;
    }
    if (round_up) {
        if ((mantissa & I->mmask) == I->mmask) {
            if (mantissa == I->mmask) mantissa++;
            else mantissa = (mantissa + 1) / 2;
            biased_exp++;
        } else mantissa++;
    }
    if (error != RE_ZERO && rounding == RM_ODD) mantissa |= 1;
    uint64_t result;
    const int ieee = !half || !(fpcr & FPCR_AHP);
    if (biased_exp >= (ieee ? (1 << E) - 1 : 1 << E)) {
        if (ieee) { result = overflow_to_inf ? inf_of(I, sign) : max_normal(I, sign); *fl |= FPSR_OFC | FPSR_IXC; }
        else { result = sign ? 0xFFFF : 0x7FFF; *fl |= FPSR_IOC; }   // the alternative half precision has no infinity
    } else {
        result = (((((uint64_t)sign << E) + (uint64_t)biased_exp) << F) | (mantissa & I->mmask)) & I->width;
        if (error != RE_ZERO) *fl |= FPSR_IXC;
    }
    return result;
}
static uint64_t fpround(const fpinfo *I, fpu op, uint32_t fpcr, int rounding, uint32_t *fl) { return round_base(I, op, fpcr & ~FPCR_AHP, rounding, fl); }      // FPRound
static uint64_t fpround_cv(const fpinfo *I, fpu op, uint32_t fpcr, int rounding, uint32_t *fl) { return round_base(I, op, fpcr & ~FPCR_FZ16, rounding, fl); }  // FPRoundCV

// FPProcessNaN, FPProcessNaNs, FPProcessNaNs3: 1 with *r set when an operand is a NaN
static uint64_t process_nan(const fpinfo *I, int type, uint64_t op, uint32_t fpcr, uint32_t *fl) {
    uint64_t r = op;
    if (type == T_SNAN) { r = op | (1ull << (I->mw - 1)); *fl |= FPSR_IOC; }
    return (fpcr & FPCR_DN) ? default_nan(I) : r;
}
static int process_nans(const fpinfo *I, int t1, int t2, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fl, uint64_t *r) {
    if (t1 == T_SNAN) { *r = process_nan(I, t1, a, fpcr, fl); return 1; }
    if (t2 == T_SNAN) { *r = process_nan(I, t2, b, fpcr, fl); return 1; }
    if (t1 == T_QNAN) { *r = process_nan(I, t1, a, fpcr, fl); return 1; }
    if (t2 == T_QNAN) { *r = process_nan(I, t2, b, fpcr, fl); return 1; }
    return 0;
}
static int process_nans3(const fpinfo *I, int t1, int t2, int t3, uint64_t a, uint64_t b, uint64_t c, uint32_t fpcr, uint32_t *fl, uint64_t *r) {
    if (t1 == T_SNAN) { *r = process_nan(I, t1, a, fpcr, fl); return 1; }
    if (t2 == T_SNAN) { *r = process_nan(I, t2, b, fpcr, fl); return 1; }
    if (t3 == T_SNAN) { *r = process_nan(I, t3, c, fpcr, fl); return 1; }
    if (t1 == T_QNAN) { *r = process_nan(I, t1, a, fpcr, fl); return 1; }
    if (t2 == T_QNAN) { *r = process_nan(I, t2, b, fpcr, fl); return 1; }
    if (t3 == T_QNAN) { *r = process_nan(I, t3, c, fpcr, fl); return 1; }
    return 0;
}

// ---------- FusedMulAdd (fp/fused.cpp) on 128-bit products ----------
static u128 shr128(u128 v, int s);
static u128 shl128(u128 v, int s) { return s < 0 ? shr128(v, -s) : s >= 128 ? 0 : v << s; }
static u128 shr128(u128 v, int s) { return s < 0 ? shl128(v, -s) : s >= 128 ? 0 : v >> s; }
static u128 sticky_lsr(u128 v, int amount) {   // StickyLogicalShiftRight: bit 0 set when a set bit was shifted out
    if (amount < 0) return shl128(v, -amount);
    if (amount == 0) return v;
    if (amount < 128) { u128 r = v >> amount; if (v << (128 - amount)) r |= 1; return r; }
    return v != 0;
}
static fpu reduce_mantissa(int sign, int exponent, u128 m) {   // the product's point at 124 back to 62
    return (fpu){sign, exponent + 2, (uint64_t)(m >> 64) | (uint64_t)((uint64_t)m != 0)};
}
static fpu fused_mul_add(fpu addend, fpu op1, fpu op2) {
    const int product_sign = op1.sign != op2.sign;
    int product_exponent = op1.exponent + op2.exponent;
    u128 product = (u128)op1.mantissa * op2.mantissa;
    if ((product >> 125) & 1) { product >>= 1; product_exponent++; }
    if (product == 0) return addend;
    if (addend.mantissa == 0) return reduce_mantissa(product_sign, product_exponent, product);
    const int exp_diff = product_exponent - addend.exponent;
    if (product_sign == addend.sign) {
        if (exp_diff <= 0) return (fpu){addend.sign, addend.exponent, addend.mantissa + (uint64_t)sticky_lsr(product, 62 - exp_diff)};
        return reduce_mantissa(product_sign, product_exponent, product + sticky_lsr((u128)addend.mantissa, exp_diff - 62));
    }
    const u128 addend_long = (u128)addend.mantissa << 62;
    int result_sign, result_exponent;
    u128 result;
    if (exp_diff == 0 && product > addend_long) { result_sign = product_sign; result_exponent = product_exponent; result = product - addend_long; }
    else if (exp_diff <= 0) { result_sign = !product_sign; result_exponent = addend.exponent; result = addend_long - sticky_lsr(product, -exp_diff); }
    else { result_sign = product_sign; result_exponent = product_exponent; result = product - sticky_lsr(addend_long, exp_diff); }
    if ((uint64_t)(result >> 64) == 0) return (fpu){result_sign, result_exponent, (uint64_t)result};
    const int required_shift = 62 - hsb((uint64_t)(result >> 64));
    result = shl128(result, required_shift);
    return reduce_mantissa(result_sign, result_exponent - required_shift, result);
}

// ---------- The estimate tables (math_util.cpp) ----------
static uint8_t recip_estimate(uint64_t a) {   // RecipEstimate: a in [256, 511]
    const uint64_t x = a * 2 + 1, b = (1u << 19) / x;
    return (uint8_t)((b + 1) / 2);
}
static uint8_t g_rsqrt[512];
static pthread_once_t g_rsqrt_once = PTHREAD_ONCE_INIT;
static void rsqrt_init(void) {   // RecipSqrtEstimate's table: entries 128 to 511 (0 below)
    for (uint64_t i = 128; i < 512; i++) {
        uint64_t a = i < 256 ? i * 2 + 1 : (i | 1) * 2, b = 512;
        while (a * (b + 1) * (b + 1) < (1u << 28)) b++;
        g_rsqrt[i] = (uint8_t)((b + 1) / 2);
    }
}
static uint8_t rsqrt_estimate(uint64_t a) { pthread_once(&g_rsqrt_once, rsqrt_init); return g_rsqrt[a & 0x1FF]; }

// ---------- The operations (fp/op/*.cpp) ----------
static uint64_t recip_estimate_op(const fpinfo *I, uint64_t op, uint32_t fpcr, uint32_t *fl) {   // FPRecipEstimate
    fpu v; const int type = unpack(I, op, fpcr, fl, &v), sign = v.sign;
    if (type == T_SNAN || type == T_QNAN) return process_nan(I, type, op, fpcr, fl);
    if (type == T_INF) return zero_of(I, sign);
    if (type == T_ZERO) { *fl |= FPSR_DZC; return inf_of(I, sign); }
    if (v.exponent < I->emin - 2) {
        const unsigned rm = rmode(fpcr);
        const int to_inf = rm == RM_TIE_EVEN ? 1 : rm == RM_PLUS_INF ? !sign : rm == RM_MINUS_INF ? sign : 0;
        *fl |= FPSR_OFC | FPSR_IXC;
        return to_inf ? inf_of(I, sign) : max_normal(I, sign);
    }
    if (((fpcr & FPCR_FZ) && I->total != 16) || ((fpcr & FPCR_FZ16) && I->total == 16))
        if (v.exponent >= -I->emin) { *fl |= FPSR_UFC; return zero_of(I, sign); }
    uint64_t estimate = (uint64_t)recip_estimate(v.mantissa >> (62 - 8)) << (I->mw - 8);
    int result_exponent = -(v.exponent + 1);
    if (result_exponent < I->emin) {
        if (result_exponent == I->emin - 1) { estimate |= I->implicit; estimate >>= 1; }
        else if (result_exponent == I->emin - 2) { estimate |= I->implicit; estimate >>= 2; result_exponent++; }
        else abort();   // UNREACHABLE in dynarmic: the overflow above takes every smaller exponent
    }
    return ((((uint64_t)(result_exponent + I->bias) & I->width) << I->mw) | (estimate & I->mmask) | zero_of(I, sign)) & I->width;
}

static uint64_t rsqrt_estimate_op(const fpinfo *I, uint64_t op, uint32_t fpcr, uint32_t *fl) {   // FPRSqrtEstimate
    fpu v; const int type = unpack(I, op, fpcr, fl, &v), sign = v.sign;
    if (type == T_SNAN || type == T_QNAN) return process_nan(I, type, op, fpcr, fl);
    if (type == T_ZERO) { *fl |= FPSR_DZC; return inf_of(I, sign); }
    if (sign) { *fl |= FPSR_IOC; return default_nan(I); }
    if (type == T_INF) return zero_of(I, 0);
    const int result_exponent = (-(v.exponent + 1)) >> 1;   // arithmetic shift, as C++20 defines it (clang and GCC do it in C too)
    const int odd = v.exponent % 2 == 0;
    const uint64_t estimate = rsqrt_estimate(lsr(v.mantissa, 62 - (odd ? 7 : 8)));
    return ((((uint64_t)(result_exponent + I->bias) & I->width) << I->mw) | ((estimate << (I->mw - 8)) & I->mmask)) & I->width;
}

static uint64_t recip_exponent_op(const fpinfo *I, uint64_t op, uint32_t fpcr, uint32_t *fl) {   // FPRecipExponent
    fpu v; const int type = unpack(I, op, fpcr, fl, &v);
    if (type == T_SNAN || type == T_QNAN) return process_nan(I, type, op, fpcr, fl);
    const uint64_t sign_bits = zero_of(I, v.sign), exponent = (op >> I->mw) & ones(I->ew);
    if (exponent == 0) return sign_bits | ((ones(I->ew) - 1) << I->mw);
    return sign_bits | (((~exponent & I->width) << I->mw) & I->emask);
}

// FPRecipStepFused (rsqrt = 0) and FPRSqrtStepFused (rsqrt = 1): 2 - op1 * op2, and (3 - op1 * op2) / 2, rounded once
static uint64_t step_fused_op(const fpinfo *I, int rsqrt, uint64_t op1, uint64_t op2, uint32_t fpcr, uint32_t *fl) {
    op1 ^= I->sign;   // FPNeg
    fpu v1, v2;
    const int t1 = unpack(I, op1, fpcr, fl, &v1), t2 = unpack(I, op2, fpcr, fl, &v2);
    uint64_t nan;
    if (process_nans(I, t1, t2, op1, op2, fpcr, fl, &nan)) return nan;
    const int inf1 = t1 == T_INF, inf2 = t2 == T_INF, zero1 = t1 == T_ZERO, zero2 = t2 == T_ZERO;
    if ((inf1 && zero2) || (zero1 && inf2)) {   // FPValue 2.0, or 1.5
        if (!rsqrt) return (uint64_t)(1 + I->bias) << I->mw;
        return ((uint64_t)I->bias << I->mw) | (1ull << (I->mw - 1));
    }
    if (inf1 || inf2) return inf_of(I, v1.sign != v2.sign);
    fpu r = fused_mul_add(to_normalized(0, 0, rsqrt ? 3 : 2), v1, v2);
    if (rsqrt) r.exponent--;
    if (r.mantissa == 0) return zero_of(I, rmode(fpcr) == RM_MINUS_INF);
    return fpround(I, r, fpcr, (int)rmode(fpcr), fl);
}

static uint64_t compare_eq_op(const fpinfo *I, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fl) {   // FPCompareEQ
    fpu v1, v2;
    const int t1 = unpack(I, a, fpcr, fl, &v1), t2 = unpack(I, b, fpcr, fl, &v2);
    if (t1 == T_QNAN || t1 == T_SNAN || t2 == T_QNAN || t2 == T_SNAN) {
        if (t1 == T_SNAN || t2 == T_SNAN) *fl |= FPSR_IOC;
        return 0;
    }
    return (v1.sign == v2.sign && v1.exponent == v2.exponent && v1.mantissa == v2.mantissa) || (t1 == T_ZERO && t2 == T_ZERO);
}

static uint64_t muladd_op(const fpinfo *I, uint64_t addend, uint64_t op1, uint64_t op2, uint32_t fpcr, uint32_t *fl) {   // FPMulAdd
    const int rounding = (int)rmode(fpcr);
    fpu vA, v1, v2;
    const int tA = unpack(I, addend, fpcr, fl, &vA), t1 = unpack(I, op1, fpcr, fl, &v1), t2 = unpack(I, op2, fpcr, fl, &v2);
    const int infA = tA == T_INF, inf1 = t1 == T_INF, inf2 = t2 == T_INF, zeroA = tA == T_ZERO, zero1 = t1 == T_ZERO, zero2 = t2 == T_ZERO;
    uint64_t nan;
    const int is_nan = process_nans3(I, tA, t1, t2, addend, op1, op2, fpcr, fl, &nan);   // first: its exception side effects come before the check below
    if (tA == T_QNAN && ((inf1 && zero2) || (zero1 && inf2))) { *fl |= FPSR_IOC; return default_nan(I); }
    if (is_nan) return nan;
    const int signP = v1.sign != v2.sign, infP = inf1 || inf2, zeroP = zero1 || zero2;
    if ((inf1 && zero2) || (zero1 && inf2) || (infA && infP && vA.sign != signP)) { *fl |= FPSR_IOC; return default_nan(I); }
    if ((infA && !vA.sign) || (infP && !signP)) return inf_of(I, 0);
    if ((infA && vA.sign) || (infP && signP)) return inf_of(I, 1);
    if (zeroA && zeroP && vA.sign == signP) return zero_of(I, vA.sign);
    const fpu r = fused_mul_add(vA, v1, v2);
    if (r.mantissa == 0) return zero_of(I, rounding == RM_MINUS_INF);
    return fpround(I, r, fpcr, rounding, fl);
}

// The rounding decision of FPRoundInt and FPToFixed for an integer (two's complement) with its residual error
static int int_round_up(int rounding, int error, uint64_t v) {
    switch (rounding) {
    case RM_TIE_EVEN: return error > RE_HALF || (error == RE_HALF && (v & 1));
    case RM_PLUS_INF: return error != RE_ZERO;
    case RM_MINUS_INF: return 0;
    case RM_ZERO: return error != RE_ZERO && (v >> 63);
    case RM_TIE_AWAY: return error > RE_HALF || (error == RE_HALF && !(v >> 63));
    default: abort();   // ToOdd: UNREACHABLE in dynarmic
    }
}

static uint64_t round_int_op(const fpinfo *I, uint64_t op, int rounding, int exact, uint32_t fpcr, uint32_t *fl) {   // FPRoundInt
    if (rounding == RM_ODD) abort();
    fpu v; const int type = unpack(I, op, fpcr, fl, &v), sign = v.sign;
    if (type == T_SNAN || type == T_QNAN) return process_nan(I, type, op, fpcr, fl);
    if (type == T_INF) return inf_of(I, sign);
    if (type == T_ZERO) return zero_of(I, sign);
    const int exponent = v.exponent - 62;
    if (exponent >= 0) return op;
    uint64_t r = sign ? 0 - v.mantissa : v.mantissa;
    const int error = residual(r, -exponent);
    r = asl(r, exponent);
    if (int_round_up(rounding, error, r)) r++;
    const int new_sign = (int)(r >> 63);
    const uint64_t abs_r = new_sign ? 0 - r : r;
    const uint64_t result = r == 0 ? zero_of(I, sign) : fpround(I, (fpu){new_sign, 62, abs_r}, fpcr, RM_ZERO, fl);
    if (error != RE_ZERO && exact) *fl |= FPSR_IXC;
    return result;
}

static uint64_t to_fixed_op(const fpinfo *I, int ibits, uint64_t op, int fbits, int is_unsigned, int rounding, uint32_t fpcr, uint32_t *fl) {   // FPToFixed
    if (rounding == RM_ODD || ibits > 64 || fbits > ibits) abort();
    fpu v; const int type = unpack(I, op, fpcr, fl, &v), sign = v.sign;
    if (type == T_SNAN || type == T_QNAN) *fl |= FPSR_IOC;
    if (v.mantissa == 0) return 0;
    if (sign && is_unsigned) { *fl |= FPSR_IOC; return 0; }
    const int exponent = v.exponent + fbits - 62;
    uint64_t r = sign ? 0 - v.mantissa : v.mantissa;
    const int error = residual(r, -exponent);
    r = asl(r, exponent);
    const int up = int_round_up(rounding, error, r);
    if (up) r++;
    const int min_exponent_for_overflow = ibits - hsb(v.mantissa + (up ? lsr(1, exponent) : 0)) - (is_unsigned ? 0 : 1);
    if (exponent >= min_exponent_for_overflow) {
        if (is_unsigned || !sign) { *fl |= FPSR_IOC; return ones(ibits - (is_unsigned ? 0 : 1)); }
        const uint64_t min_value = 0 - (1ull << (ibits - 1));
        if (!(exponent == min_exponent_for_overflow && r == min_value)) { *fl |= FPSR_IOC; return 1ull << (ibits - 1); }
    }
    if (error != RE_ZERO) *fl |= FPSR_IXC;
    return r & ones(ibits);
}

static uint64_t convert_nan(const fpinfo *TO, const fpinfo *FROM, uint64_t op) {   // FPConvertNaN
    const uint64_t sign = (op >> (FROM->total - 1)) & 1;
    const uint64_t frac = FROM->total == 64 ? op & ones(51) : FROM->total == 32 ? (op & ones(22)) << 29 : (op & ones(9)) << 42;
    const uint64_t shifted_sign = sign << (TO->total - 1), exponent = ones(TO->total - TO->mw);
    if (TO->total == 64) return shifted_sign | exponent << 51 | frac;
    if (TO->total == 32) return shifted_sign | exponent << 22 | ((frac >> 29) & ones(22));
    return shifted_sign | exponent << 9 | ((frac >> 42) & ones(9));
}

static uint64_t convert_op(const fpinfo *TO, const fpinfo *FROM, uint64_t op, int rounding, uint32_t fpcr, uint32_t *fl) {   // FPConvert
    fpu v; const int type = unpack_cv(FROM, op, fpcr, fl, &v), sign = v.sign;
    const int althp = TO->total == 16 && (fpcr & FPCR_AHP);
    if (type == T_SNAN || type == T_QNAN) {
        const uint64_t r = althp ? zero_of(TO, sign) : (fpcr & FPCR_DN) ? default_nan(TO) : convert_nan(TO, FROM, op);
        if (type == T_SNAN || althp) *fl |= FPSR_IOC;
        return r & TO->width;
    }
    if (type == T_INF) {
        if (althp) { *fl |= FPSR_IOC; return ((uint64_t)sign << 15) | 0x7FFF; }
        return inf_of(TO, sign);
    }
    if (type == T_ZERO) return zero_of(TO, sign);
    return fpround_cv(TO, v, fpcr, rounding, fl);
}

// ---------- The entry points of runtime.h ----------
enum { RT_RECIP_ESTIMATE = 0, RT_RSQRT_ESTIMATE = 1, RT_RECIP_EXPONENT = 2, RT_RECIP_STEP = 3, RT_RSQRT_STEP = 4, RT_COMPARE_EQ = 5 };

FPEXPORT uint64_t FPNAME(nxaot_rt_fp_unary)(uint32_t which, uint32_t fsize, uint64_t a, uint32_t fpcr, uint32_t *fpsr) {
    const fpinfo *I = info(fsize);
    uint32_t fl = 0;
    fpcr &= FPCR_MASK; a &= I->width;
    const uint64_t r = which == RT_RECIP_ESTIMATE ? recip_estimate_op(I, a, fpcr, &fl) : which == RT_RSQRT_ESTIMATE ? rsqrt_estimate_op(I, a, fpcr, &fl)
                                                  : recip_exponent_op(I, a, fpcr, &fl);
    *fpsr |= fl & 0x9F;
    return r;
}

FPEXPORT uint64_t FPNAME(nxaot_rt_fp_binary)(uint32_t which, uint32_t fsize, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr) {
    const fpinfo *I = info(fsize);
    uint32_t fl = 0;
    fpcr &= FPCR_MASK; a &= I->width; b &= I->width;
    const uint64_t r = which == RT_RECIP_STEP ? step_fused_op(I, 0, a, b, fpcr, &fl) : which == RT_RSQRT_STEP ? step_fused_op(I, 1, a, b, fpcr, &fl)
                                              : compare_eq_op(I, a, b, fpcr, &fl);
    *fpsr |= fl & 0x9F;
    return r;
}

FPEXPORT uint64_t FPNAME(nxaot_rt_fp_muladd)(uint32_t fsize, uint32_t negate, uint64_t addend, uint64_t op1, uint64_t op2, uint32_t fpcr, uint32_t *fpsr) {
    const fpinfo *I = info(fsize);
    uint32_t fl = 0;
    fpcr &= FPCR_MASK; addend &= I->width; op1 &= I->width; op2 &= I->width;
    const uint64_t r = muladd_op(I, addend, negate ? op1 ^ I->sign : op1, op2, fpcr, &fl);   // FPMulSub: the first factor negated
    *fpsr |= fl & 0x9F;
    return r;
}

FPEXPORT uint64_t FPNAME(nxaot_rt_fp_round_int)(uint32_t fsize, uint64_t op, uint32_t rounding, uint32_t exact, uint32_t fpcr, uint32_t *fpsr) {
    const fpinfo *I = info(fsize);
    uint32_t fl = 0;
    const uint64_t r = round_int_op(I, op & I->width, (int)rounding, exact != 0, fpcr & FPCR_MASK, &fl);
    *fpsr |= fl & 0x9F;
    return r;
}

FPEXPORT uint64_t FPNAME(nxaot_rt_fp_to_fixed)(uint32_t fsize, uint32_t isize, uint32_t is_unsigned, uint32_t fbits, uint32_t rounding, uint64_t op, uint32_t fpcr,
                                               uint32_t *fpsr) {
    const fpinfo *I = info(fsize);
    uint32_t fl = 0;
    const uint64_t r = to_fixed_op(I, (int)isize, op & I->width, (int)fbits, is_unsigned != 0, (int)rounding, fpcr & FPCR_MASK, &fl);
    *fpsr |= fl & 0x9F;
    return r;
}

FPEXPORT uint64_t FPNAME(nxaot_rt_fp_convert)(uint32_t to_size, uint32_t from_size, uint64_t op, uint32_t rounding, uint32_t fpcr, uint32_t *fpsr) {
    // runtime.cpp's cases: 16 from 32 or 64, 32 from 16 or 64, 64 from 16, and anything else as 64 from 32
    const uint32_t k = to_size * 100 + from_size;
    const fpinfo *TO = k == 1632 || k == 1664 ? &F16 : k == 3216 || k == 3264 ? &F32 : &F64;
    const fpinfo *FROM = k == 1664 || k == 3264 ? &F64 : k == 3216 || k == 6416 ? &F16 : &F32;
    uint32_t fl = 0;
    const uint64_t r = convert_op(TO, FROM, op & FROM->width, (int)rounding, fpcr & FPCR_MASK, &fl);
    *fpsr |= fl & 0x9F;
    return r;
}

// ---------- AES (crypto/aes.cpp): single rounds and the column mixes, for a CPU without AES-NI ----------
static const uint8_t g_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};
static const uint8_t g_inv_sbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};
static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ (((x >> 7) & 1) * 0x1B)); }
static uint8_t gmul(uint8_t x, uint8_t y) {
    return (uint8_t)(((y & 1) * x) ^ ((y >> 1 & 1) * xtime(x)) ^ ((y >> 2 & 1) * xtime(xtime(x))) ^ ((y >> 3 & 1) * xtime(xtime(xtime(x)))) ^
                     ((y >> 4 & 1) * xtime(xtime(xtime(xtime(x))))));
}
FPEXPORT void FPNAME(nxaot_rt_aes)(uint32_t which, uint8_t *out, const uint8_t *in) {
    uint8_t s[16], r[16];
    memcpy(s, in, 16);
    if (which == 0 || which == 1) {   // DecryptSingleRound: InverseShiftRows then the inverse S-box; EncryptSingleRound: ShiftRows then the S-box
        static const int shift[16] = {0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11};       // out[i] = in[shift[i]] (ShiftRows)
        static const int inv_shift[16] = {0, 13, 10, 7, 4, 1, 14, 11, 8, 5, 2, 15, 12, 9, 6, 3};   // InverseShiftRows
        for (int i = 0; i < 16; i++) r[i] = which ? g_sbox[s[shift[i]]] : g_inv_sbox[s[inv_shift[i]]];
    } else if (which == 2) {          // InverseMixColumns
        for (int i = 0; i < 16; i += 4) {
            const uint8_t a = s[i], b = s[i + 1], c = s[i + 2], d = s[i + 3];
            r[i] = gmul(a, 0x0E) ^ gmul(b, 0x0B) ^ gmul(c, 0x0D) ^ gmul(d, 0x09);
            r[i + 1] = gmul(a, 0x09) ^ gmul(b, 0x0E) ^ gmul(c, 0x0B) ^ gmul(d, 0x0D);
            r[i + 2] = gmul(a, 0x0D) ^ gmul(b, 0x09) ^ gmul(c, 0x0E) ^ gmul(d, 0x0B);
            r[i + 3] = gmul(a, 0x0B) ^ gmul(b, 0x0D) ^ gmul(c, 0x09) ^ gmul(d, 0x0E);
        }
    } else {                          // MixColumns
        for (int i = 0; i < 16; i += 4) {
            const uint8_t a = s[i], b = s[i + 1], c = s[i + 2], d = s[i + 3], t = a ^ b ^ c ^ d;
            r[i] = a ^ xtime(a ^ b) ^ t;
            r[i + 1] = b ^ xtime(b ^ c) ^ t;
            r[i + 2] = c ^ xtime(c ^ d) ^ t;
            r[i + 3] = d ^ xtime(d ^ a) ^ t;
        }
    }
    memcpy(out, r, 16);
}
