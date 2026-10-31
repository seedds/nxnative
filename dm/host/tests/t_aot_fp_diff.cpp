// Differential test of dm/host/aot_fp.c, the host's C port of the floating-point and AES helpers that recompiled code calls, against
// dm/tools/aot/src/lift/runtime.cpp, nxaot's own definitions over dynarmic's common/fp and common/crypto (docs/design/aot-recompiler.md 7.1 to 7.3). Every helper,
// every precision, on special operands (zeros, infinities, quiet and signalling NaNs with payloads, denormals, the extreme normals, ones, twos), on random bit
// patterns and on random values near rounding boundaries, with FPCR's mode bits swept (AHP, DN, FZ, RMode, FZ16; never a trap enable, which dynarmic asserts on
// and the Cortex-A57 does not implement), and exhaustively over every half-precision operand for the unary operations, round to integral and conversions from
// half precision. The result and the cumulative FPSR bits must be equal. Built and run by dm/host/tests/aot_fp_diff.sh in the nxnative-aot image.
//   t_aot_fp_diff [random cases per operation, default 300000] [seed]
// Prints the cases per operation and the mismatches (the first ten in full); exit 1 on a mismatch
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "lift/runtime.h"   // the reference: nxaot_rt_* (runtime.cpp)

extern "C" {   // the port, compiled with DM_AOT_FP_TEST: the same names with a c_ prefix
uint64_t c_nxaot_rt_fp_unary(uint32_t which, uint32_t fsize, uint64_t a, uint32_t fpcr, uint32_t *fpsr);
uint64_t c_nxaot_rt_fp_binary(uint32_t which, uint32_t fsize, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr);
uint64_t c_nxaot_rt_fp_muladd(uint32_t fsize, uint32_t negate, uint64_t addend, uint64_t op1, uint64_t op2, uint32_t fpcr, uint32_t *fpsr);
uint64_t c_nxaot_rt_fp_round_int(uint32_t fsize, uint64_t op, uint32_t rounding, uint32_t exact, uint32_t fpcr, uint32_t *fpsr);
uint64_t c_nxaot_rt_fp_to_fixed(uint32_t fsize, uint32_t isize, uint32_t is_unsigned, uint32_t fbits, uint32_t rounding, uint64_t op, uint32_t fpcr, uint32_t *fpsr);
uint64_t c_nxaot_rt_fp_convert(uint32_t to_size, uint32_t from_size, uint64_t op, uint32_t rounding, uint32_t fpcr, uint32_t *fpsr);
void c_nxaot_rt_aes(uint32_t which, uint8_t *out, const uint8_t *in);
}

namespace {

uint64_t g_state = 0x243F6A8885A308D3ull;
uint64_t Rand() {   // xorshift64*
    g_state ^= g_state >> 12; g_state ^= g_state << 25; g_state ^= g_state >> 27;
    return g_state * 0x2545F4914F6CDD1Dull;
}

struct Info { int mw, ew; uint64_t width; };
Info InfoOf(unsigned fsize) {
    return fsize == 16 ? Info{10, 5, 0xFFFFu} : fsize == 32 ? Info{23, 8, 0xFFFFFFFFu} : Info{52, 11, ~0ull};
}

// An operand of a precision: a special value, random bits, or a random value near a rounding boundary (random exponent, mantissa with few bits set)
uint64_t Operand(unsigned fsize) {
    const Info I = InfoOf(fsize);
    const uint64_t sign = (Rand() & 1) << (fsize - 1), emax = (1ull << I.ew) - 1, mmask = (1ull << I.mw) - 1;
    switch (Rand() % 16) {
    case 0: return sign;                                                         // zero
    case 1: return sign | emax << I.mw;                                          // infinity
    case 2: return sign | emax << I.mw | (1ull << (I.mw - 1)) | (Rand() & (mmask >> 1));   // quiet NaN
    case 3: return sign | emax << I.mw | ((Rand() & (mmask >> 1)) | 1);          // signalling NaN
    case 4: return sign | (Rand() & mmask) | 1;                                  // denormal
    case 5: return sign | 1ull << I.mw;                                          // the smallest normal
    case 6: return sign | (emax - 1) << I.mw | mmask;                            // the largest normal
    case 7: return sign | ((emax >> 1) << I.mw);                                 // one
    case 8: return sign | (((emax >> 1) + 1) << I.mw);                           // two
    case 9: case 10: {                                                           // a value with few mantissa bits: halfway cases of rounding
        const uint64_t e = Rand() % (emax + 1);
        uint64_t m = 0;
        for (int k = Rand() % 3; k >= 0; --k) m |= 1ull << (Rand() % I.mw);
        return sign | e << I.mw | m;
    }
    case 11: {                                                                   // an exponent near the integer range of a conversion
        const uint64_t e = (emax >> 1) + Rand() % (fsize == 16 ? 17 : 66);
        return sign | (e > emax ? emax - 1 : e) << I.mw | (Rand() & mmask);
    }
    default: return Rand() & I.width;                                            // random bits
    }
}

uint32_t Fpcr() {   // the mode bits only: AHP 26, DN 25, FZ 24, RMode 22-23, FZ16 19
    const uint64_t r = Rand();
    return (uint32_t)((r & 1) << 26 | (r >> 1 & 1) << 25 | (r >> 2 & 1) << 24 | (r >> 3 & 3) << 22 | (r >> 5 & 1) << 19);
}

struct Counter { const char *name; uint64_t cases = 0, bad = 0; };
uint64_t g_bad_total = 0;

template<typename F, typename G>
void Check(Counter &c, F ref, G port, const char *fmt, uint64_t a, uint64_t b, uint64_t d, uint64_t e) {
    uint32_t fr = 0, fp = 0;
    const uint64_t r = ref(&fr), p = port(&fp);
    ++c.cases;
    if (r != p || fr != fp) {
        ++c.bad;
        if (g_bad_total++ < 10) {
            std::printf("MISMATCH %s: ", c.name);
            std::printf(fmt, a, b, d, e);
            std::printf(" -> reference %#" PRIx64 " fpsr %#x, port %#" PRIx64 " fpsr %#x\n", r, fr, p, fp);
        }
    }
}

}  // namespace

int main(int argc, char **argv) {
    const uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 0) : 300000;
    if (argc > 2) g_state = std::strtoull(argv[2], nullptr, 0) | 1;
    static const unsigned sizes[3] = {16, 32, 64};
    Counter unary{"unary (FPRecipEstimate, FPRSqrtEstimate, FPRecipExponent)"}, binary{"binary (FPRecipStepFused, FPRSqrtStepFused, FPCompareEQ)"};
    Counter muladd{"muladd (FPMulAdd, FPMulSub)"}, round_int{"round_int (FPRoundInt)"}, to_fixed{"to_fixed (FPToFixed)"}, convert{"convert (FPConvert)"};
    Counter aes{"aes (single rounds, MixColumns, InverseMixColumns)"};

    // Exhaustive over half precision: every operand of the unary operations, round to integral and the conversions from it, under a set of FPCR values
    static const uint32_t fpcrs[] = {0, 1u << 24, 1u << 25, 1u << 26, 1u << 19, 1u << 22, 2u << 22, 3u << 22, (1u << 25) | (1u << 19) | (1u << 26), (1u << 24) | (3u << 22)};
    for (uint32_t fpcr : fpcrs) {
        for (uint64_t a = 0; a < 0x10000; ++a) {
            for (uint32_t which = 0; which < 3; ++which)
                Check(unary, [&](uint32_t *f) { return nxaot_rt_fp_unary(which, 16, a, fpcr, f); }, [&](uint32_t *f) { return c_nxaot_rt_fp_unary(which, 16, a, fpcr, f); },
                      "which %" PRIu64 " half %#" PRIx64 " fpcr %#" PRIx64 "%.0" PRIu64, which, a, fpcr, 0);
            for (uint32_t rounding = 0; rounding < 5; ++rounding)
                Check(round_int, [&](uint32_t *f) { return nxaot_rt_fp_round_int(16, a, rounding, 1, fpcr, f); },
                      [&](uint32_t *f) { return c_nxaot_rt_fp_round_int(16, a, rounding, 1, fpcr, f); }, "half %#" PRIx64 " rounding %" PRIu64 " fpcr %#" PRIx64 "%.0" PRIu64, a, rounding, fpcr, 0);
            for (uint32_t to : {32u, 64u})
                Check(convert, [&](uint32_t *f) { return nxaot_rt_fp_convert(to, 16, a, (fpcr >> 22) & 3, fpcr, f); },
                      [&](uint32_t *f) { return c_nxaot_rt_fp_convert(to, 16, a, (fpcr >> 22) & 3, fpcr, f); }, "to %" PRIu64 " from half %#" PRIx64 " fpcr %#" PRIx64 "%.0" PRIu64, to, a, fpcr, 0);
        }
    }

    // Random operands in every precision
    for (uint64_t i = 0; i < n; ++i) {
        const unsigned fsize = sizes[Rand() % 3];
        const uint32_t fpcr = Fpcr(), rm = (fpcr >> 22) & 3;
        const uint64_t a = Operand(fsize), b = Operand(fsize), c = Operand(fsize);
        const uint32_t which = (uint32_t)(Rand() % 3);
        Check(unary, [&](uint32_t *f) { return nxaot_rt_fp_unary(which, fsize, a, fpcr, f); }, [&](uint32_t *f) { return c_nxaot_rt_fp_unary(which, fsize, a, fpcr, f); },
              "which %" PRIu64 " op %#" PRIx64 " fsize %" PRIu64 " fpcr %#" PRIx64, which, a, fsize, fpcr);
        const uint32_t bw = 3 + (uint32_t)(Rand() % 3);
        Check(binary, [&](uint32_t *f) { return nxaot_rt_fp_binary(bw, fsize, a, b, fpcr, f); }, [&](uint32_t *f) { return c_nxaot_rt_fp_binary(bw, fsize, a, b, fpcr, f); },
              "which %" PRIu64 " a %#" PRIx64 " b %#" PRIx64 " fsize/fpcr %#" PRIx64, bw, a, b, (uint64_t)fsize << 32 | fpcr);
        const uint32_t neg = (uint32_t)(Rand() & 1);
        Check(muladd, [&](uint32_t *f) { return nxaot_rt_fp_muladd(fsize, neg, a, b, c, fpcr, f); }, [&](uint32_t *f) { return c_nxaot_rt_fp_muladd(fsize, neg, a, b, c, fpcr, f); },
              "addend %#" PRIx64 " op1 %#" PRIx64 " op2 %#" PRIx64 " negate/fsize/fpcr %#" PRIx64, a, b, c, (uint64_t)neg << 48 | (uint64_t)fsize << 32 | fpcr);
        const uint32_t rounding = (uint32_t)(Rand() % 5), exact = (uint32_t)(Rand() & 1);
        Check(round_int, [&](uint32_t *f) { return nxaot_rt_fp_round_int(fsize, a, rounding, exact, fpcr, f); },
              [&](uint32_t *f) { return c_nxaot_rt_fp_round_int(fsize, a, rounding, exact, fpcr, f); },
              "op %#" PRIx64 " rounding %" PRIu64 " exact %" PRIu64 " fsize/fpcr %#" PRIx64, a, rounding, exact, (uint64_t)fsize << 32 | fpcr);
        const uint32_t isize = Rand() & 1 ? 64 : 32, uns = (uint32_t)(Rand() & 1), fbits = (uint32_t)(Rand() % 4 ? 0 : Rand() % (isize + 1));
        Check(to_fixed, [&](uint32_t *f) { return nxaot_rt_fp_to_fixed(fsize, isize, uns, fbits, rounding, a, fpcr, f); },
              [&](uint32_t *f) { return c_nxaot_rt_fp_to_fixed(fsize, isize, uns, fbits, rounding, a, fpcr, f); },
              "op %#" PRIx64 " isize/unsigned %#" PRIx64 " fbits %" PRIu64 " rounding/fsize/fpcr %#" PRIx64, a, (uint64_t)isize << 8 | uns, fbits,
              (uint64_t)rounding << 48 | (uint64_t)fsize << 32 | fpcr);
        static const unsigned pairs[6][2] = {{16, 32}, {16, 64}, {32, 16}, {32, 64}, {64, 16}, {64, 32}};
        const unsigned *pr = pairs[Rand() % 6];
        const uint64_t op = Operand(pr[1]);
        const uint32_t cr = Rand() % 6 == 0 ? 5u : rm;   // ToOdd (FCVTXN) or FPCR's mode; dynarmic asserts on the tie-away mode here
        Check(convert, [&](uint32_t *f) { return nxaot_rt_fp_convert(pr[0], pr[1], op, cr, fpcr, f); },
              [&](uint32_t *f) { return c_nxaot_rt_fp_convert(pr[0], pr[1], op, cr, fpcr, f); },
              "to/from %#" PRIx64 " op %#" PRIx64 " rounding %" PRIu64 " fpcr %#" PRIx64, (uint64_t)pr[0] << 8 | pr[1], op, cr, fpcr);
        if (i % 8 == 0) {
            uint8_t in[16], o1[16], o2[16];
            for (auto &x : in) x = (uint8_t)Rand();
            const uint32_t w = (uint32_t)(Rand() % 4);
            nxaot_rt_aes(w, o1, in);
            c_nxaot_rt_aes(w, o2, in);
            ++aes.cases;
            if (std::memcmp(o1, o2, 16)) { ++aes.bad; if (g_bad_total++ < 10) std::printf("MISMATCH aes which %u\n", w); }
        }
    }
    uint64_t total = 0, bad = 0;
    for (const Counter *c : {&unary, &binary, &muladd, &round_int, &to_fixed, &convert, &aes}) {
        std::printf("%-62s %10" PRIu64 " cases %6" PRIu64 " mismatches\n", c->name, c->cases, c->bad);
        total += c->cases; bad += c->bad;
    }
    std::printf("t_aot_fp_diff: %" PRIu64 " cases, %" PRIu64 " mismatches\n", total, bad);
    return bad != 0;
}
