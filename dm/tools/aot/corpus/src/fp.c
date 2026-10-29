// The floating-point set, in the manner of a libm test suite: the operations of design section 7.2 (arithmetic, fused multiply-add, the four
// min / max forms, FMULX, the reciprocal steps and estimates, FRECPX, every rounding to integral, conversions between precisions and to and from
// integers, compares) over special values (signed zeros, infinities, quiet and signalling NaNs with payloads, denormals, the largest and
// smallest normals, ties), single and double precision. Each instruction runs alone in inline assembly; its result bits are checked against the
// result the Arm ARM's pseudocode gives with FPCR = 0, which gen_fp.py computes into fp_cases.h. One line per operation and precision: the
// mismatches, then a digest of the results; the first mismatches are printed whole.
#include "rt.h"
#include "fp_cases.h"

static uint32_t bits_s(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static uint64_t bits_d(double f) {
    uint64_t u;
    memcpy(&u, &f, 8);
    return u;
}
static float from_s(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static double from_d(uint64_t u) {
    double f;
    memcpy(&f, &u, 8);
    return f;
}

#define BIN(insn) __asm__ volatile(insn " %s0, %s1, %s2" : "=w"(r) : "w"(fa), "w"(fb)); return bits_s(r)
#define TRI(insn) __asm__ volatile(insn " %s0, %s1, %s2, %s3" : "=w"(r) : "w"(fa), "w"(fb), "w"(fc)); return bits_s(r)
#define UN(insn) __asm__ volatile(insn " %s0, %s1" : "=w"(r) : "w"(fa)); return bits_s(r)
#define TOW(insn) __asm__ volatile(insn " %w0, %s1" : "=r"(w) : "w"(fa)); return w
#define TOX(insn) __asm__ volatile(insn " %x0, %s1" : "=r"(x) : "w"(fa)); return x

static RT_NOINLINE uint64_t run_s(unsigned op, uint32_t a, uint32_t b, uint32_t c, uint64_t i) {
    const float fa = from_s(a), fb = from_s(b), fc = from_s(c);
    float r;
    uint32_t w;
    uint64_t x;
    double d;
    switch (op) {
    case FP_FADD: BIN("fadd");
    case FP_FSUB: BIN("fsub");
    case FP_FMUL: BIN("fmul");
    case FP_FDIV: BIN("fdiv");
    case FP_FMAX: BIN("fmax");
    case FP_FMIN: BIN("fmin");
    case FP_FMAXNM: BIN("fmaxnm");
    case FP_FMINNM: BIN("fminnm");
    case FP_FMULX: BIN("fmulx");
    case FP_FRECPS: BIN("frecps");
    case FP_FRSQRTS: BIN("frsqrts");
    case FP_FCMP: __asm__ volatile("fcmp %s1, %s2\n mrs %0, nzcv" : "=r"(x) : "w"(fa), "w"(fb) : "cc"); return x >> 28;
    case FP_FMADD: TRI("fmadd");
    case FP_FMSUB: TRI("fmsub");
    case FP_FNMADD: TRI("fnmadd");
    case FP_FNMSUB: TRI("fnmsub");
    case FP_FSQRT: UN("fsqrt");
    case FP_FABS: UN("fabs");
    case FP_FNEG: UN("fneg");
    case FP_FRINTN: UN("frintn");
    case FP_FRINTA: UN("frinta");
    case FP_FRINTM: UN("frintm");
    case FP_FRINTP: UN("frintp");
    case FP_FRINTZ: UN("frintz");
    case FP_FRINTI: UN("frinti");
    case FP_FRINTX: UN("frintx");
    case FP_FRECPE: UN("frecpe");
    case FP_FRSQRTE: UN("frsqrte");
    case FP_FRECPX: UN("frecpx");
    case FP_FCVT: __asm__ volatile("fcvt %d0, %s1" : "=w"(d) : "w"(fa)); return bits_d(d);
    case FP_FCVTZS_W: TOW("fcvtzs");
    case FP_FCVTZS_X: TOX("fcvtzs");
    case FP_FCVTZU_W: TOW("fcvtzu");
    case FP_FCVTZU_X: TOX("fcvtzu");
    case FP_FCVTAS_X: TOX("fcvtas");
    case FP_FCVTAU_W: TOW("fcvtau");
    case FP_FCVTNS_X: TOX("fcvtns");
    case FP_FCVTMS_W: TOW("fcvtms");
    case FP_FCVTPS_X: TOX("fcvtps");
    case FP_SCVTF: __asm__ volatile("scvtf %s0, %x1" : "=w"(r) : "r"(i)); return bits_s(r);
    case FP_UCVTF: __asm__ volatile("ucvtf %s0, %x1" : "=w"(r) : "r"(i)); return bits_s(r);
    default: return ~0ull;
    }
}

#undef BIN
#undef TRI
#undef UN
#undef TOW
#undef TOX
#define BIN(insn) __asm__ volatile(insn " %d0, %d1, %d2" : "=w"(r) : "w"(fa), "w"(fb)); return bits_d(r)
#define TRI(insn) __asm__ volatile(insn " %d0, %d1, %d2, %d3" : "=w"(r) : "w"(fa), "w"(fb), "w"(fc)); return bits_d(r)
#define UN(insn) __asm__ volatile(insn " %d0, %d1" : "=w"(r) : "w"(fa)); return bits_d(r)
#define TOW(insn) __asm__ volatile(insn " %w0, %d1" : "=r"(w) : "w"(fa)); return w
#define TOX(insn) __asm__ volatile(insn " %x0, %d1" : "=r"(x) : "w"(fa)); return x

static RT_NOINLINE uint64_t run_d(unsigned op, uint64_t a, uint64_t b, uint64_t c, uint64_t i) {
    const double fa = from_d(a), fb = from_d(b), fc = from_d(c);
    double r;
    uint32_t w;
    uint64_t x;
    float s;
    switch (op) {
    case FP_FADD: BIN("fadd");
    case FP_FSUB: BIN("fsub");
    case FP_FMUL: BIN("fmul");
    case FP_FDIV: BIN("fdiv");
    case FP_FMAX: BIN("fmax");
    case FP_FMIN: BIN("fmin");
    case FP_FMAXNM: BIN("fmaxnm");
    case FP_FMINNM: BIN("fminnm");
    case FP_FMULX: BIN("fmulx");
    case FP_FRECPS: BIN("frecps");
    case FP_FRSQRTS: BIN("frsqrts");
    case FP_FCMP: __asm__ volatile("fcmp %d1, %d2\n mrs %0, nzcv" : "=r"(x) : "w"(fa), "w"(fb) : "cc"); return x >> 28;
    case FP_FMADD: TRI("fmadd");
    case FP_FMSUB: TRI("fmsub");
    case FP_FNMADD: TRI("fnmadd");
    case FP_FNMSUB: TRI("fnmsub");
    case FP_FSQRT: UN("fsqrt");
    case FP_FABS: UN("fabs");
    case FP_FNEG: UN("fneg");
    case FP_FRINTN: UN("frintn");
    case FP_FRINTA: UN("frinta");
    case FP_FRINTM: UN("frintm");
    case FP_FRINTP: UN("frintp");
    case FP_FRINTZ: UN("frintz");
    case FP_FRINTI: UN("frinti");
    case FP_FRINTX: UN("frintx");
    case FP_FRECPE: UN("frecpe");
    case FP_FRSQRTE: UN("frsqrte");
    case FP_FRECPX: UN("frecpx");
    case FP_FCVT: __asm__ volatile("fcvt %s0, %d1" : "=w"(s) : "w"(fa)); return bits_s(s);
    case FP_FCVTZS_W: TOW("fcvtzs");
    case FP_FCVTZS_X: TOX("fcvtzs");
    case FP_FCVTZU_W: TOW("fcvtzu");
    case FP_FCVTZU_X: TOX("fcvtzu");
    case FP_FCVTAS_X: TOX("fcvtas");
    case FP_FCVTAU_W: TOW("fcvtau");
    case FP_FCVTNS_X: TOX("fcvtns");
    case FP_FCVTMS_W: TOW("fcvtms");
    case FP_FCVTPS_X: TOX("fcvtps");
    case FP_SCVTF: __asm__ volatile("scvtf %d0, %x1" : "=w"(r) : "r"(i)); return bits_d(r);
    case FP_UCVTF: __asm__ volatile("ucvtf %d0, %x1" : "=w"(r) : "r"(i)); return bits_d(r);
    default: return ~0ull;
    }
}

int main(void) {
    static unsigned bad[FP_NOPS][2];
    static uint64_t digest[FP_NOPS][2];
    unsigned printed = 0;
    for (unsigned k = 0; k < FP_NOPS; ++k) {
        digest[k][0] = digest[k][1] = RT_MIX_INIT;
    }
    for (unsigned n = 0; n < FP_NCASES; ++n) {
        const fp_case *t = &fp_cases[RT_OPAQUE(n)];
        const uint64_t want = fp_results[t->want];
        uint64_t got;
        if (t->prec == 0) {
            got = run_s(t->op, fp_values_s[t->a], fp_values_s[t->b], fp_values_s[t->c], fp_ints[t->a % (sizeof fp_ints / 8)]);
        } else {
            got = run_d(t->op, fp_values_d[t->a], fp_values_d[t->b], fp_values_d[t->c], fp_ints[t->a % (sizeof fp_ints / 8)]);
        }
        digest[t->op][t->prec] = rt_mix(digest[t->op][t->prec], got);
        if (got != want) {
            ++bad[t->op][t->prec];
            if (printed++ < 40) {
                rt_begin("fp.mismatch ");
                rt_str(fp_op_names[t->op]);
                rt_str(t->prec ? ".d" : ".s");
                rt_hex(t->a, 2);
                rt_hex(t->b, 2);
                rt_hex(t->c, 2);
                rt_str(" got");
                rt_hex(got, 16);
                rt_str(" want");
                rt_hex(want, 16);
                rt_end();
            }
        }
    }
    for (unsigned k = 0; k < FP_NOPS; ++k) {
        for (unsigned p = 0; p < 2; ++p) {
            char name[32] = "fp.";
            unsigned len = 3;
            for (const char *s = fp_op_names[k]; *s; ++s) {
                name[len++] = *s;
            }
            name[len++] = '.';
            name[len++] = p ? 'd' : 's';
            name[len] = 0;
            rt_begin(name);
            rt_str(".digest");
            rt_hex(digest[k][p], 16);
            rt_end();
            RT_CHECK32(name, bad[k][p], 0);
        }
    }
    return rt_finish();
}
