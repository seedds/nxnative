#!/usr/bin/env python3
"""Writes src/fp_cases.h, the cases of the corpus's floating-point program (src/fp.c): the operations of design section 7.2 over special values,
in single and double precision, with the results the Arm ARM's pseudocode gives with FPCR = 0 (round to nearest even, FZ = 0, DN = 0, AH = 0).

  python3 dm/tools/aot/corpus/gen_fp.py           write src/fp_cases.h
  python3 dm/tools/aot/corpus/gen_fp.py --check   exit 1 when src/fp_cases.h differs from what this writes

The model follows the pseudocode functions by name (FPAdd, FPMulAdd, FPProcessNaNs3, FPRecipEstimate, FPToFixed, FPRoundInt, ...): exact
arithmetic on fractions, one rounding, Arm's NaN rules. It is an oracle written from the specification, independent of dynarmic and of the
lifter; the native run on ARM64 hardware is the final judge (README.md, "The corpus").
"""
import argparse
import math
import os
import sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'src', 'fp_cases.h')


class Fmt:
    def __init__(self, name, ebits, fbits):
        self.name, self.ebits, self.fbits = name, ebits, fbits
        self.n = 1 + ebits + fbits
        self.bias = (1 << (ebits - 1)) - 1
        self.emax_field = (1 << ebits) - 1

    def unpack(self, bits):
        """(kind, sign, value): kind is 'zero', 'denormal'/'normal' as 'number', 'inf', 'qnan', 'snan'; value a Fraction for numbers"""
        sign = bits >> (self.n - 1)
        exp = (bits >> self.fbits) & self.emax_field
        frac = bits & ((1 << self.fbits) - 1)
        if exp == self.emax_field:
            if frac == 0:
                return 'inf', sign, None
            return ('qnan' if frac >> (self.fbits - 1) else 'snan'), sign, None
        if exp == 0:
            if frac == 0:
                return 'zero', sign, Fraction(0)
            v = Fraction(frac, 1 << self.fbits) * Fraction(2) ** (1 - self.bias)
        else:
            v = Fraction((1 << self.fbits) | frac, 1 << self.fbits) * Fraction(2) ** (exp - self.bias)
        return 'number', sign, -v if sign else v

    def zero(self, sign):
        return sign << (self.n - 1)

    def inf(self, sign):
        return (sign << (self.n - 1)) | (self.emax_field << self.fbits)

    def default_nan(self):
        return (self.emax_field << self.fbits) | (1 << (self.fbits - 1))

    def quiet(self, bits):
        return bits | (1 << (self.fbits - 1))

    def neg(self, bits):
        return bits ^ (1 << (self.n - 1))

    def round(self, v, mode='RN'):
        """FPRound: v a nonzero Fraction, rounded once into this format (FZ = 0: denormals kept); overflow to infinity in RN"""
        sign = 1 if v < 0 else 0
        mag = -v if sign else v
        e = mag.numerator.bit_length() - mag.denominator.bit_length()
        if Fraction(2) ** e > mag:
            e -= 1
        while Fraction(2) ** (e + 1) <= mag:
            e += 1
        emin = 1 - self.bias
        e = max(e, emin)
        scaled = mag * Fraction(2) ** (self.fbits - e)
        m = scaled.numerator // scaled.denominator
        rem = scaled - m
        if mode == 'RN':
            if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and m & 1):
                m += 1
        elif mode == 'RZ':
            pass
        elif mode == 'RP':
            if rem and not sign:
                m += 1
        elif mode == 'RM':
            if rem and sign:
                m += 1
        if m >> (self.fbits + 1):  # rounding carried into a new bit
            m >>= 1
            e += 1
        if m >> self.fbits:  # a normal number (a denormal that rounded up to the smallest normal included)
            field = e + self.bias
            if field >= self.emax_field:
                return self.inf(sign)  # overflow: infinity in round to nearest
            return (sign << (self.n - 1)) | (field << self.fbits) | (m & ((1 << self.fbits) - 1))
        return (sign << (self.n - 1)) | m  # a denormal: e is the minimum exponent, the field is 0

    def value_bits(self, v, mode='RN'):
        """A result value with Arm's zero sign rule for exact zeros in RN: +0"""
        if v == 0:
            return self.zero(1 if mode == 'RM' else 0)
        return self.round(v, mode)


S, D = Fmt('s', 8, 23), Fmt('d', 11, 52)


def is_nan(kind):
    return kind in ('qnan', 'snan')


def process_nans(fmt, ops):
    """FPProcessNaNs / FPProcessNaNs3: the first signalling NaN in operand order, quieted; else the first quiet NaN; None when no NaN"""
    for bits in ops:
        if fmt.unpack(bits)[0] == 'snan':
            return fmt.quiet(bits)
    for bits in ops:
        if fmt.unpack(bits)[0] == 'qnan':
            return bits
    return None


def fp_add(fmt, a, b, sub=False):
    if sub:
        b2 = fmt.neg(b)
    r = process_nans(fmt, [a, b])
    if r is not None:
        return r
    ka, sa, va = fmt.unpack(a)
    kb, sb, vb = fmt.unpack(b if not sub else b2)
    if ka == 'inf' and kb == 'inf' and sa != sb:
        return fmt.default_nan()
    if ka == 'inf':
        return fmt.inf(sa)
    if kb == 'inf':
        return fmt.inf(sb)
    if ka == 'zero' and kb == 'zero' and sa == sb:
        return fmt.zero(sa)
    return fmt.value_bits(va + vb)


def fp_mul(fmt, a, b):
    r = process_nans(fmt, [a, b])
    if r is not None:
        return r
    ka, sa, va = fmt.unpack(a)
    kb, sb, vb = fmt.unpack(b)
    if (ka == 'inf' and kb == 'zero') or (ka == 'zero' and kb == 'inf'):
        return fmt.default_nan()
    if ka == 'inf' or kb == 'inf':
        return fmt.inf(sa ^ sb)
    if ka == 'zero' or kb == 'zero':
        return fmt.zero(sa ^ sb)
    return fmt.round(va * vb)


def fp_mulx(fmt, a, b):
    r = process_nans(fmt, [a, b])
    if r is not None:
        return r
    ka, sa, _ = fmt.unpack(a)
    kb, sb, _ = fmt.unpack(b)
    if (ka == 'inf' and kb == 'zero') or (ka == 'zero' and kb == 'inf'):
        return fmt.round(Fraction(-2 if sa ^ sb else 2))
    return fp_mul(fmt, a, b)


def fp_div(fmt, a, b):
    r = process_nans(fmt, [a, b])
    if r is not None:
        return r
    ka, sa, va = fmt.unpack(a)
    kb, sb, vb = fmt.unpack(b)
    if (ka == 'inf' and kb == 'inf') or (ka == 'zero' and kb == 'zero'):
        return fmt.default_nan()
    if ka == 'inf' or kb == 'zero':
        return fmt.inf(sa ^ sb)
    if ka == 'zero' or kb == 'inf':
        return fmt.zero(sa ^ sb)
    return fmt.round(va / vb)


def exact_sqrt_round(fmt, v):
    """sqrt(v) of a positive Fraction, rounded once: an integer square root with 200 extra bits and a sticky midpoint"""
    n, d = v.numerator, v.denominator
    k = 200 + fmt.fbits
    big = n * d * (4 ** k)
    s = math.isqrt(big)
    if s * s == big:
        return fmt.round(Fraction(s, d * 2 ** k))
    return fmt.round(Fraction(2 * s + 1, 2 * d * 2 ** k))


def fp_sqrt(fmt, a):
    ka, sa, va = fmt.unpack(a)
    if is_nan(ka):
        return process_nans(fmt, [a])
    if ka == 'zero':
        return fmt.zero(sa)
    if sa:
        return fmt.default_nan()
    if ka == 'inf':
        return fmt.inf(0)
    return exact_sqrt_round(fmt, va)


def fp_muladd(fmt, addend, op1, op2):
    """FPMulAdd(addend, op1, op2) = addend + op1 * op2, one rounding"""
    kA, sA, vA = fmt.unpack(addend)
    k1, s1, v1 = fmt.unpack(op1)
    k2, s2, v2 = fmt.unpack(op2)
    inf1, inf2, zero1, zero2 = k1 == 'inf', k2 == 'inf', k1 == 'zero', k2 == 'zero'
    r = process_nans(fmt, [addend, op1, op2])
    if kA == 'qnan' and ((inf1 and zero2) or (zero1 and inf2)):
        return fmt.default_nan()
    if r is not None:
        return r
    infA, zeroA = kA == 'inf', kA == 'zero'
    infP, zeroP, sP = inf1 or inf2, zero1 or zero2, s1 ^ s2
    if (inf1 and zero2) or (zero1 and inf2) or (infA and infP and sA != sP):
        return fmt.default_nan()
    if (infA and sA == 0) or (infP and sP == 0):
        return fmt.inf(0)
    if (infA and sA == 1) or (infP and sP == 1):
        return fmt.inf(1)
    if zeroA and zeroP and sA == sP:
        return fmt.zero(sA)
    return fmt.value_bits(vA + v1 * v2)


def fp_max_min(fmt, a, b, is_max, numeric):
    """FPMax / FPMin, and FPMaxNum / FPMinNum (numeric): a single quiet NaN loses to a number"""
    ka, sa, va = fmt.unpack(a)
    kb, sb, vb = fmt.unpack(b)
    if numeric:
        if ka == 'qnan' and not is_nan(kb):
            a = fmt.inf(1 if is_max else 0)
        elif kb == 'qnan' and not is_nan(ka):
            b = fmt.inf(1 if is_max else 0)
        ka, sa, va = fmt.unpack(a)
        kb, sb, vb = fmt.unpack(b)
    r = process_nans(fmt, [a, b])
    if r is not None:
        return r

    def val(k, s, v):
        return Fraction(10) ** 400 * (-1 if s else 1) if k == 'inf' else v
    x, y = val(ka, sa, va), val(kb, sb, vb)
    if ka == 'zero' and kb == 'zero':
        return fmt.zero(sa & sb if is_max else sa | sb)
    if (x > y) == is_max and x != y:
        return a
    if x == y:
        return a
    return b


def fp_recip_step(fmt, a, b, rsqrt):
    """FPRecipStepFused (2 - a*b) and FPRSqrtStepFused ((3 - a*b) / 2): op1 negated first, NaN signs included"""
    a = fmt.neg(a)
    r = process_nans(fmt, [a, b])
    if r is not None:
        return r
    ka, sa, va = fmt.unpack(a)
    kb, sb, vb = fmt.unpack(b)
    if (ka == 'inf' and kb == 'zero') or (ka == 'zero' and kb == 'inf'):
        return fmt.round(Fraction(3, 2) if rsqrt else Fraction(2))
    if ka == 'inf' or kb == 'inf':
        return fmt.inf(sa ^ sb)
    v = (3 + va * vb) / 2 if rsqrt else 2 + va * vb
    return fmt.value_bits(v)


def recip_estimate(a):
    a = a * 2 + 1
    b = (1 << 19) // a
    return (b + 1) // 2


def fp_recip_estimate(fmt, op):
    k, sign, v = fmt.unpack(op)
    if is_nan(k):
        return process_nans(fmt, [op])
    if k == 'inf':
        return fmt.zero(sign)
    if k == 'zero':
        return fmt.inf(sign)
    if abs(v) < Fraction(2) ** (-128 if fmt is S else -1024):
        return fmt.inf(sign)  # overflow, RN
    fraction = (op & ((1 << fmt.fbits) - 1)) << (52 - fmt.fbits)
    exp = (op >> fmt.fbits) & fmt.emax_field
    if exp == 0:
        if not (fraction >> 51) & 1:
            exp = -1
            fraction = (fraction & ((1 << 50) - 1)) << 2
        else:
            fraction = (fraction & ((1 << 51) - 1)) << 1
    scaled = (1 << 8) | ((fraction >> 44) & 0xFF)
    result_exp = (253 if fmt is S else 2045) - exp
    estimate = recip_estimate(scaled)
    fraction = (estimate & 0xFF) << 44
    if result_exp == 0:
        fraction = (1 << 51) | (fraction >> 1)
    elif result_exp == -1:
        fraction = (1 << 50) | (fraction >> 2)
        result_exp = 0
    return (sign << (fmt.n - 1)) | ((result_exp & fmt.emax_field) << fmt.fbits) | (fraction >> (52 - fmt.fbits))


def recip_sqrt_estimate(a):
    if a < 256:
        a = a * 2 + 1
    else:
        a = (a >> 1) << 1
        a = (a + 1) * 2
    b = 512
    while a * (b + 1) * (b + 1) < (1 << 28):
        b += 1
    return (b + 1) // 2


def fp_rsqrt_estimate(fmt, op):
    k, sign, v = fmt.unpack(op)
    if is_nan(k):
        return process_nans(fmt, [op])
    if k == 'zero':
        return fmt.inf(sign)
    if sign:
        return fmt.default_nan()
    if k == 'inf':
        return fmt.zero(0)
    fraction = (op & ((1 << fmt.fbits) - 1)) << (52 - fmt.fbits)
    exp = (op >> fmt.fbits) & fmt.emax_field
    if exp == 0:
        while not (fraction >> 51) & 1:
            fraction = (fraction << 1) & ((1 << 52) - 1)
            exp -= 1
        fraction = (fraction << 1) & ((1 << 52) - 1)
    if exp & 1 == 0:
        scaled = (1 << 8) | ((fraction >> 44) & 0xFF)
    else:
        scaled = (1 << 7) | ((fraction >> 45) & 0x7F)
    result_exp = ((380 if fmt is S else 3068) - exp) // 2
    estimate = recip_sqrt_estimate(scaled)
    return ((result_exp & fmt.emax_field) << fmt.fbits) | ((estimate & 0xFF) << (fmt.fbits - 8))


def fp_recpx(fmt, op):
    k, sign, _ = fmt.unpack(op)
    if is_nan(k):
        return process_nans(fmt, [op])
    exp = (op >> fmt.fbits) & fmt.emax_field
    new_exp = fmt.emax_field - 1 if exp == 0 else (~exp) & fmt.emax_field
    return (sign << (fmt.n - 1)) | (new_exp << fmt.fbits)


def round_integer(v, mode):
    """A Fraction rounded to an integer: RN (ties to even), RA (ties away), RP, RM, RZ"""
    f = math.floor(v)
    rem = v - f
    if mode == 'RN':
        return f + 1 if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and f & 1) else f
    if mode == 'RA':
        if rem == Fraction(1, 2):
            return f + 1 if v > 0 else f
        return f + 1 if rem > Fraction(1, 2) else f
    if mode == 'RP':
        return f + 1 if rem else f
    if mode == 'RM':
        return f
    return f + 1 if rem and v < 0 else f  # RZ


def fp_round_int(fmt, op, mode):
    k, sign, v = fmt.unpack(op)
    if is_nan(k):
        return process_nans(fmt, [op])
    if k in ('inf', 'zero'):
        return op
    r = round_integer(v, mode)
    if r == 0:
        return fmt.zero(sign)
    return fmt.round(Fraction(r))


def fp_to_int(fmt, op, mode, bits, unsigned):
    k, sign, v = fmt.unpack(op)
    if is_nan(k):
        return 0
    if k == 'inf':
        v = Fraction(-(1 << 200) if sign else (1 << 200))
    r = round_integer(v, mode)
    lo, hi = (0, (1 << bits) - 1) if unsigned else (-(1 << (bits - 1)), (1 << (bits - 1)) - 1)
    r = min(max(r, lo), hi)
    return r & ((1 << bits) - 1)


def int_to_fp(fmt, i, unsigned):
    v = i if unsigned else (i - (1 << 64) if i >> 63 else i)
    return fmt.value_bits(Fraction(v))


def fp_convert(src, dst, op):
    """FPConvert between single and double: NaNs keep their sign and the top of their payload, quieted (FPConvertNaN)"""
    k, sign, v = src.unpack(op)
    if is_nan(k):
        frac = op & ((1 << src.fbits) - 1)
        frac = frac << (dst.fbits - src.fbits) if dst.fbits > src.fbits else frac >> (src.fbits - dst.fbits)
        return (sign << (dst.n - 1)) | (dst.emax_field << dst.fbits) | (1 << (dst.fbits - 1)) | frac
    if k == 'inf':
        return dst.inf(sign)
    if k == 'zero':
        return dst.zero(sign)
    return dst.round(v)


def fp_cmp(fmt, a, b):
    """FPCompare as NZCV: unordered 0011, equal 0110, less 1000, greater 0010"""
    ka, sa, va = fmt.unpack(a)
    kb, sb, vb = fmt.unpack(b)
    if is_nan(ka) or is_nan(kb):
        return 0x3
    big = Fraction(10) ** 400
    x = (-big if sa else big) if ka == 'inf' else va
    y = (-big if sb else big) if kb == 'inf' else vb
    return 0x6 if x == y else 0x8 if x < y else 0x2


# ---- The values ----

VALUES_S = [
    0x00000000, 0x80000000, 0x3f800000, 0xbfc00000, 0x3dcccccd, 0x7f800000, 0xff800000, 0x7fc12345, 0x7fa00007, 0xffc00042, 0x00000001,
    0x7f7fffff, 0x00800000, 0x40200000, 0xc0200000, 0x3f000000, 0x4f000000, 0xcf000000, 0x5f000000, 0x5f800000, 0x501502f9, 0x007fffff,
    0x80400000, 0x3eaaaaab, 0x40400000, 0xc0e00000, 0x4b7fffff, 0x3fc00000,
]
VALUES_D = [
    0x0000000000000000, 0x8000000000000000, 0x3ff0000000000000, 0xbff8000000000000, 0x3fb999999999999a, 0x7ff0000000000000,
    0xfff0000000000000, 0x7ff8000000012345, 0x7ff4000000000007, 0xfff8000000000042, 0x0000000000000001, 0x7fefffffffffffff,
    0x0010000000000000, 0x4004000000000000, 0xc004000000000000, 0x3fe0000000000000, 0x41e0000000000000, 0xc1e0000000000000,
    0x43e0000000000000, 0x43f0000000000000, 0x4202a05f20000000, 0x000fffffffffffff, 0x8008000000000000, 0x3fd5555555555555,
    0x4008000000000000, 0xc01c000000000000, 0x433fffffffffffff, 0x3ff8000000000000,
]
INTS = [0, 1, 0xffffffffffffffff, 0x7fffffffffffffff, 0x8000000000000000, 0x0000000000ffffff, 0x0000000001000001, 0x20000000000003,
        0x7fffffffffffff01, 0xfffffffffffffc01, 0x123456789abcdef1, 0x8000000000000001, 1000003, 0xffffffffff000000]
BINARY = 10  # the first ten values form the pairs of the binary operations (every pair); the unary operations take every value
EXTRA_PAIRS = [(11, 13), (11, 11), (12, 15), (10, 15), (4, 23), (23, 24), (21, 12), (13, 14), (9, 9), (11, 3)]  # overflow, underflow, rounding
FMA_VALUES = 9  # the pairs of the fused multiply-adds: the first nine values (a signalling NaN included)

OPS = [
    # name, arity, kind
    ('fadd', 2), ('fsub', 2), ('fmul', 2), ('fdiv', 2), ('fmax', 2), ('fmin', 2), ('fmaxnm', 2), ('fminnm', 2), ('fmulx', 2),
    ('frecps', 2), ('frsqrts', 2), ('fcmp', 2), ('fmadd', 3), ('fmsub', 3), ('fnmadd', 3), ('fnmsub', 3),
    ('fsqrt', 1), ('fabs', 1), ('fneg', 1), ('frintn', 1), ('frinta', 1), ('frintm', 1), ('frintp', 1), ('frintz', 1), ('frinti', 1),
    ('frintx', 1), ('frecpe', 1), ('frsqrte', 1), ('frecpx', 1), ('fcvt', 1), ('fcvtzs_w', 1), ('fcvtzs_x', 1), ('fcvtzu_w', 1),
    ('fcvtzu_x', 1), ('fcvtas_x', 1), ('fcvtau_w', 1), ('fcvtns_x', 1), ('fcvtms_w', 1), ('fcvtps_x', 1), ('scvtf', 0), ('ucvtf', 0),
]
ADDENDS = [2, 7, 5]  # the addends of the fused multiply-adds: 1.0, a quiet NaN, +infinity (indices into the values)


def compute(name, fmt, a, b, c):
    other = D if fmt is S else S
    if name == 'fadd': return fp_add(fmt, a, b)
    if name == 'fsub': return fp_add(fmt, a, b, sub=True)
    if name == 'fmul': return fp_mul(fmt, a, b)
    if name == 'fdiv': return fp_div(fmt, a, b)
    if name == 'fmax': return fp_max_min(fmt, a, b, True, False)
    if name == 'fmin': return fp_max_min(fmt, a, b, False, False)
    if name == 'fmaxnm': return fp_max_min(fmt, a, b, True, True)
    if name == 'fminnm': return fp_max_min(fmt, a, b, False, True)
    if name == 'fmulx': return fp_mulx(fmt, a, b)
    if name == 'frecps': return fp_recip_step(fmt, a, b, False)
    if name == 'frsqrts': return fp_recip_step(fmt, a, b, True)
    if name == 'fcmp': return fp_cmp(fmt, a, b)
    # fmadd d, n, m, a: a + n * m; fmsub: a - n * m; fnmadd: -a - n * m; fnmsub: -a + n * m (FPMulAdd with the negations of the pseudocode)
    if name == 'fmadd': return fp_muladd(fmt, c, a, b)
    if name == 'fmsub': return fp_muladd(fmt, c, fmt.neg(a), b)
    if name == 'fnmadd': return fp_muladd(fmt, fmt.neg(c), fmt.neg(a), b)
    if name == 'fnmsub': return fp_muladd(fmt, fmt.neg(c), a, b)
    if name == 'fsqrt': return fp_sqrt(fmt, a)
    if name == 'fabs': return a & ~(1 << (fmt.n - 1))
    if name == 'fneg': return fmt.neg(a)
    if name.startswith('frint'):
        mode = {'n': 'RN', 'a': 'RA', 'm': 'RM', 'p': 'RP', 'z': 'RZ', 'i': 'RN', 'x': 'RN'}[name[5]]
        return fp_round_int(fmt, a, mode)
    if name == 'frecpe': return fp_recip_estimate(fmt, a)
    if name == 'frsqrte': return fp_rsqrt_estimate(fmt, a)
    if name == 'frecpx': return fp_recpx(fmt, a)
    if name == 'fcvt': return fp_convert(fmt, other, a)
    if name.startswith('fcvt'):
        base, width = name.split('_')
        mode = {'z': 'RZ', 'a': 'RA', 'n': 'RN', 'm': 'RM', 'p': 'RP'}[base[4]]
        return fp_to_int(fmt, a, mode, 32 if width == 'w' else 64, base[5] == 'u')
    if name == 'scvtf': return int_to_fp(fmt, a, False)
    if name == 'ucvtf': return int_to_fp(fmt, a, True)
    raise ValueError(name)


def cases():
    out = []
    for oi, (name, arity) in enumerate(OPS):
        for prec, fmt, values in ((0, S, VALUES_S), (1, D, VALUES_D)):
            if arity == 0:
                for i, v in enumerate(INTS):
                    out.append((oi, prec, i, 0, 0, compute(name, fmt, v, 0, 0)))
            elif arity == 1:
                for i, v in enumerate(values):
                    out.append((oi, prec, i, 0, 0, compute(name, fmt, v, 0, 0)))
            elif arity == 2:
                pairs = [(i, j) for i in range(BINARY) for j in range(BINARY)] + EXTRA_PAIRS
                for i, j in pairs:
                    out.append((oi, prec, i, j, 0, compute(name, fmt, values[i], values[j], 0)))
            else:
                for i in range(FMA_VALUES):
                    for j in range(FMA_VALUES):
                        for k in ADDENDS:
                            out.append((oi, prec, i, j, k, compute(name, fmt, values[i], values[j], values[k])))
    return out


def text():
    rows = cases()
    lines = ['// Written by dm/tools/aot/corpus/gen_fp.py from the Arm ARM\'s pseudocode (FPCR = 0); do not edit. The cases of src/fp.c.', '#pragma once', '',
             '#include <stdint.h>', '']
    lines.append('static const uint32_t fp_values_s[] = {' + ', '.join(f'{v:#010x}' for v in VALUES_S) + '};')
    lines.append('static const uint64_t fp_values_d[] = {' + ', '.join(f'{v:#018x}' for v in VALUES_D) + '};')
    lines.append('static const uint64_t fp_ints[] = {' + ', '.join(f'{v:#x}' for v in INTS) + '};')
    lines.append('enum fp_op {' + ', '.join(f'FP_{n.upper()}' for n, _ in OPS) + ', FP_NOPS};')
    lines.append('static const char fp_op_names[][10] = {' + ', '.join(f'"{n}"' for n, _ in OPS) + '};')
    results = sorted(set(r[5] for r in rows))
    index = {v: i for i, v in enumerate(results)}
    lines.append('// The distinct results (fcmp: NZCV; conversions to integers: the integer; else the result bits), which the cases name by index')
    lines.append(f'static const uint64_t fp_results[{len(results)}] = {{')
    for k in range(0, len(results), 6):
        lines.append('    ' + ', '.join(f'{v:#x}' for v in results[k:k + 6]) + ',')
    lines.append('};')
    lines.append('typedef struct {')
    lines.append('    uint8_t op, prec, a, b, c;  // prec: 0 single, 1 double; a, b, c: indices into the values (fp_ints for scvtf, ucvtf)')
    lines.append('    uint16_t want;              // index into fp_results')
    lines.append('} fp_case;')
    lines.append(f'#define FP_NCASES {len(rows)}')
    lines.append('static const fp_case fp_cases[FP_NCASES] = {')
    for k in range(0, len(rows), 4):
        lines.append('    ' + ' '.join(f'{{{r[0]}, {r[1]}, {r[2]}, {r[3]}, {r[4]}, {index[r[5]]}}},' for r in rows[k:k + 4]))
    lines.append('};')
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    t = text()
    if a.check:
        cur = open(OUT).read() if os.path.exists(OUT) else ''
        if cur != t:
            print(f'{OUT} is stale: run python3 dm/tools/aot/corpus/gen_fp.py')
            return 1
        print(f'gen_fp.py --check: {OUT} is current ({t.count(chr(10))} lines)')
        return 0
    with open(OUT, 'w', newline='\n') as f:
        f.write(t)
    print(f'gen_fp.py: wrote {OUT}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
