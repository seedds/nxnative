"""Compares an op's outputs on Metal with the Ampere GPU's, word by word, and names why each differing lane differs (standard library only, for the tests).

An output word is one lane of kind f (f32), u (an integer or a predicate) or two lanes of kind h (f16x2) or b (bf16x2). A differing lane gets the first cause
that fits:

  nan-bits    both NaN, different bits: Ampere writes the canonical NaN (0x7fffffff, f16 0x7fff), Metal another
  nan-one     one side NaN, the other a number
  denorm-in   an f32 input is a denormal: Metal's f32 arithmetic flushes them, Ampere keeps them without .FTZ (Metal keeps f16 denormals, so a half input
              does not count)
  denorm-out  either f32 result is a denormal, or one is zero and the other a denormal or the smallest normal (a denormal intermediate rounded up)
  zero-sign   +0 against -0
  rn-result   a directed-rounding op (name ends in _rz / _rm / _rp) where Metal gave the round-to-nearest result: the rounding mode was not applied
  other       the rest, with their ULP distances (float lanes) for the report

PLATFORM is what Metal's float arithmetic itself gives (nothing a translation of one instruction can change cheaply): the test allows it on every op."""
import struct
from collections import Counter

PLATFORM = frozenset({'nan-bits', 'denorm-in', 'denorm-out'})

_MASK = {'f': (0x7f800000, 0x007fffff, 32), 'h': (0x7c00, 0x03ff, 16), 'b': (0x7f80, 0x007f, 16)}


def is_nan(v, k):
    e, m, _ = _MASK[k]
    return (v & e) == e and (v & m) != 0


def is_denorm(v, k):
    e, m, _ = _MASK[k]
    return (v & e) == 0 and (v & m) != 0


def is_zero(v, k):
    e, m, _ = _MASK[k]
    return (v & (e | m)) == 0


def ulps(a, b, k):
    """distance in units in the last place between two float lanes of kind k"""
    w = _MASK[k][2]
    s = 1 << (w - 1)

    def o(v):
        return -(v & (s - 1)) if v & s else v
    return abs(o(a) - o(b))


def lanes(word, kind):
    if kind in 'hb':
        return [(word & 0xffff, kind), (word >> 16, kind)]
    return [(word, kind)]


def denorm_input(row, kinds):
    for v, k in zip(row, kinds):
        k = k.lower()
        if k == 'f' and is_denorm(v, 'f'):
            return True
    return False


def _f32(v):
    return struct.unpack('<f', struct.pack('<I', v))[0]


def _bits(x):
    try:
        return struct.unpack('<I', struct.pack('<f', x))[0]
    except OverflowError:
        return 0x7f800000 if x > 0 else 0xff800000


def rn_reference(name, row):
    """the round-to-nearest result of a directed-rounding op on one case, or None. Doubles hold a sum or product of two floats exactly (and an FMA's well enough
    for this label), so rounding the double once to float is the nearest float"""
    mode = name.rsplit('_', 1)[-1]
    if mode not in ('rz', 'rm', 'rp'):
        return None
    if name.startswith(('fadd_', 'fmul_', 'ffma_')):
        a, b = _f32(row[0]), _f32(row[1])
        if name.startswith('fadd_'):
            r = a + b
        elif name.startswith('fmul_'):
            r = a * b
        else:
            r = a * b + _f32(row[2])
        return _bits(r) if r == r else None
    if name.startswith('i2f_s32_'):
        return _bits(float(row[0] - (1 << 32) if row[0] >> 31 else row[0]))
    return None


def classify(op, rows, orin, metal):
    """(Counter of causes over differing lanes, number of lanes, the largest ULP distance among 'other' float lanes, up to four differing cases as dicts)"""
    causes, n, worst, examples = Counter(), 0, 0, []
    kinds_in, kinds_out = op['ins'], op['outs']
    for r, (row, o_row, m_row) in enumerate(zip(rows, orin, metal)):
        din = None
        rn = None
        row_causes = []
        for k, (ow, mw) in enumerate(zip(o_row, m_row)):
            kind = kinds_out[k]
            for (a, lk), (b, _) in zip(lanes(ow, kind), lanes(mw, kind)):
                n += 1
                if a == b:
                    continue
                if lk == 'u':
                    if din is None:
                        din = denorm_input(row, kinds_in)
                    cause = 'denorm-in' if din else 'other'
                elif is_nan(a, lk) and is_nan(b, lk):
                    cause = 'nan-bits'
                else:
                    if din is None:
                        din = denorm_input(row, kinds_in)
                    if din:
                        cause = 'denorm-in'
                    elif is_nan(a, lk) or is_nan(b, lk):
                        cause = 'nan-one'
                    elif lk == 'f' and (is_denorm(a, lk) or is_denorm(b, lk) or {a & 0x7fffffff, b & 0x7fffffff} == {0, 0x00800000}):
                        cause = 'denorm-out'
                    elif is_zero(a, lk) and is_zero(b, lk):
                        cause = 'zero-sign'
                    else:
                        if rn is None and len(kinds_out) == 1:
                            rn = rn_reference(op['name'], row) or -1
                        if rn == b:
                            cause = 'rn-result'
                        else:
                            cause = 'other'
                            worst = max(worst, ulps(a, b, lk))
                causes[cause] += 1
                row_causes.append(cause)
        if row_causes and len(examples) < 4 and any(c not in PLATFORM for c in row_causes):
            examples.append({'lane': r % 32, 'in': [f'{v:#010x}' for v in row], 'gpu': [f'{v:#010x}' for v in o_row],
                             'metal': [f'{v:#010x}' for v in m_row], 'why': sorted(set(row_causes))})
    return causes, n, worst, examples
