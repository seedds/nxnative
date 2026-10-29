"""The floating-point and SIMD vectors of nxaot_vec (aot-recompiler.md section 12), imported by gen_vectors.py.

Encodings. Every floating-point, Advanced SIMD and cryptographic data-processing instruction and every SIMD&FP load and store comes from the
encoding table of dynarmic's A64 decoder (dm/third_party/dynarmic/src/dynarmic/frontend/A64/decoder/a64.inc: one bit pattern per instruction
form, its fields as letters), so that no form the decoder knows is left out; the harness compares two executors that share that decoder, so the
table is the right list of what has to be lowered. Each form's fields are swept with random values (the reserved combinations included: both
executors must agree that they are undefined); register numbers are random, address registers point into the memory image, and immediates of
loads and stores keep the address inside it.

Values. The seed fills the vector registers with lanes of one kind (vk=: f16, f32, f64, i8, i16, i32, i64 or random bits; src/vec/vectors.cpp
FillV), drawn from that kind's special values (signed zeros, infinities, signalling and quiet NaNs with payloads, denormals, the extreme normals,
rounding ties, the integer and conversion boundaries) and random ones. The kind follows the instruction's element type and size fields, with a
quarter of the vectors of another kind. FPCR takes the variants of section 12 (round to nearest, plus, minus, zero; FZ; DN; FZ+DN; AHP; FZ16 and
some mixes; dynarmic has no FPCR.AH). Each encoding runs with a few seeds and FPCR values, so the lifter compiles it once per FPCR.
"""

import os

HERE = os.path.dirname(os.path.abspath(__file__))
A64_INC = os.path.normpath(os.path.join(HERE, '../../../third_party/dynarmic/src/dynarmic/frontend/A64/decoder/a64.inc'))

M64 = (1 << 64) - 1
GUEST_BASE = 0x10000000
IMAGE_SIZE = 0x10000
CODE_BASE = GUEST_BASE + 0x8000

RN, RP, RM, RZ = 0, 0x00400000, 0x00800000, 0x00C00000
FZ, DN, AHP, FZ16 = 0x01000000, 0x02000000, 0x04000000, 0x00080000
FPCRS = [(0, 40), (RP, 6), (RM, 6), (RZ, 6), (FZ, 10), (DN, 10), (FZ | DN, 6), (AHP, 3), (FZ16, 3), (DN | RM, 3), (FZ | RZ, 3), (AHP | DN, 2),
         (FZ | DN | RP, 2)]
FPCR_TOTAL = sum(w for _, w in FPCRS)
KINDS = ['f16', 'f32', 'f64', 'i8', 'i16', 'i32', 'i64', 'rnd']

# Bit patterns of floating-point specials the general registers get for FMOV (general) and the integer-to-float conversions
X_SPECIALS = [0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x7F800000, 0x7FC00000, 0x7F800001, 0x00800000, 0x3F800000, 0x7FF0000000000000,
              0x7FF8000000000000, 0x7FF0000000000001, 0x8000000000000000, 0x0010000000000000, 0x3FF0000000000000, 0xFFFFFFFFFFFFFFFF,
              0x7FFFFFFFFFFFFFFF, 0x0000000000000001, 0x8000000000000001, 0x00000000FFFFFFFF, 0x0000000080000000, 0x4330000000000000,
              0x0020000000000001, 0x0000000001000001, 0x9000000000000001]


def load_patterns():
    """(section, fn, name, bits) of every entry of the decoder table, in table order."""
    entries = []
    section = ''
    with open(A64_INC) as f:
        for line in f:
            line = line.strip()
            if line.startswith('//') and not line.startswith('//INST'):
                section = line[2:].strip()
                continue
            if not line.startswith('INST('):
                continue
            body = line[len('INST('):line.rindex(')')]
            fn, rest = body.split(',', 1)
            parts = rest.split('"')
            entries.append((section, fn.strip(), parts[1], parts[3]))
    return entries


def runs(bits):
    """The maximal runs of one letter: (letter, first index, length), index 0 = bit 31."""
    out = []
    i = 0
    while i < 32:
        c = bits[i]
        j = i
        while j < 32 and bits[j] == c:
            j += 1
        if c not in '01-':
            out.append((c, i, j - i))
        i = j
    return out


def place(word, start, length, value):
    shift = 32 - start - length
    mask = ((1 << length) - 1) << shift
    return (word & ~mask) | ((value << shift) & mask)


def field(word, start, length):
    return (word >> (32 - start - length)) & ((1 << length) - 1)


def fixed(bits):
    w = 0
    for i, c in enumerate(bits):
        if c == '1':
            w |= 1 << (31 - i)
    return w


def random_word(r, bits):
    w = fixed(bits)
    for i, c in enumerate(bits):
        if c not in '01' and r.below(2):
            w |= 1 << (31 - i)
    return w


def letter_value(bits, word, letter, length=None):
    """The value of the first run of letter (of the given length when set), or None."""
    for c, start, n in runs(bits):
        if c == letter and (length is None or n == length):
            return field(word, start, n)
    return None


def kind_of(name, bits, word):
    """The lane kind of the operands of an encoding: from the element type (yy), the single size bit (z) or the size field (zz)."""
    fp = name.startswith('F') or 'CVTF' in name
    if fp:
        y = letter_value(bits, word, 'y', 2)
        if y is not None:
            return {0: 'f32', 1: 'f64', 3: 'f16'}.get(y, 'rnd')
        z1 = letter_value(bits, word, 'z', 1)
        if 'CVTF' in name and z1 is not None:
            return 'i64' if z1 else 'i32'
        if z1 is not None:
            return 'f64' if z1 else 'f32'
        if name.endswith('_1') or name.endswith('_3'):
            return 'f16'
        return 'f32'
    zz = letter_value(bits, word, 'z', 2)
    if zz is not None:
        return ['i8', 'i16', 'i32', 'i64'][zz]
    return 'i8' if name.startswith(('TBL', 'TBX', 'EXT', 'AES', 'SM4')) else 'rnd'


def pick_fpcr(r):
    k = r.below(FPCR_TOTAL)
    for v, w in FPCRS:
        if k < w:
            return v
        k -= w
    return 0


def pick_fpsr(r):
    k = r.below(20)
    if k < 17:
        return 0
    return r.choice([0x08000000, 0x9F, 0x10, 0x08000001])


def general_regs(r, bits, word, regs, value):
    """General registers an encoding may read (n, m, a fields): integer edges or floating-point bit patterns half the time."""
    for letter in 'nma':
        v = letter_value(bits, word, letter, 5)
        if v is None or v == 31 or f'x{v}' in regs:
            continue
        k = r.below(4)
        if k == 0:
            regs[f'x{v}'] = r.choice(X_SPECIALS)
        elif k == 1:
            regs[f'x{v}'] = value(r)


# ---- Data processing ----

DP_FILES = [
    # file name, title, sections (prefix of "Data Processing - FP and SIMD - ...")
    ('fp', 'scalar floating point: data processing 1, 2 and 3 source, compare, conditional compare and select, immediate, conversions to '
           'and from integers and fixed point',
     ['Floating point', 'Conversion between floating point']),
    ('simd_same', 'Advanced SIMD three same (integer, floating point, extra), scalar three same, three different',
     ['SIMD three same', 'SIMD Three same', 'SIMD Scalar three same', 'Scalar three', 'SIMD three different']),
    ('simd_misc', 'Advanced SIMD two-register misc (vector and scalar), across lanes, scalar pairwise',
     ['SIMD Two-register misc', 'Scalar two register misc', 'Scalar two-register misc', 'SIMD across lanes', 'SIMD Scalar pairwise']),
    ('simd_shift', 'Advanced SIMD shift by immediate (vector and scalar) and modified immediate',
     ['SIMD Shift by immediate', 'SIMD Scalar shift by immediate', 'SIMD modified immediate']),
    ('simd_perm', 'Advanced SIMD by element (vector and scalar), copy, extract, permute, table lookup',
     ['SIMD vector x indexed element', 'SIMD Scalar x indexed element', 'SIMD Copy', 'Scalar copy', 'SIMD Extract', 'SIMD Permute',
      'SIMD Table Lookup']),
    ('crypto', 'cryptographic extension: AES, SHA-1, SHA-256, and the SHA-512 / SHA-3 / SM3 / SM4 forms dynarmic decodes',
     ['AES', 'SHA', 'SHA512', 'Cryptographic']),
]
DP_PREFIX = 'Data Processing - FP and SIMD - '
FILE_NAMES = [name for name, _, _ in DP_FILES] + ['simd_ldst', 'fp_seq']


def section_slug(section):
    return section[len(DP_PREFIX):].lower().replace(' ', '_').replace('-', '_')


def dp_entries(entries, sections):
    out = []
    for section, fn, name, bits in entries:
        if not section.startswith(DP_PREFIX) or fn == 'UnallocatedEncoding':
            continue
        rest = section[len(DP_PREFIX):]
        if any(rest == s or rest.startswith(s + ' ') or rest == s for s in sections):
            out.append((section, fn, name, bits))
    return out


def gen_dp(out, r, entries, value, words_per_entry, seeds_per_word, fpcr_per_word=False):
    for section, fn, name, bits in entries:
        cls = section_slug(section)
        for k in range(words_per_entry):
            word = random_word(r, bits)
            kind = kind_of(fn, bits, word)
            for s in range(seeds_per_word):
                if s == 0 or (not fpcr_per_word and r.below(3) == 0):
                    fpcr = pick_fpcr(r)
                vk = kind if r.below(4) else r.choice(KINDS)
                regs = {'vk': vk}
                general_regs(r, bits, word, regs, value)
                fpsr = pick_fpsr(r)
                if fpsr:
                    regs['fpsr'] = fpsr
                out.add(r, cls, f'{fn} {word:#010x}', [word], regs, fpcr=fpcr)


# ---- Loads and stores of SIMD&FP registers ----

LDST_SECTIONS = ['Loads and stores - Advanced SIMD Load/Store multiple structures', 'Loads and stores - Advanced SIMD Load/Store single structures']


def ldst_entries(entries):
    out = []
    for section, fn, name, bits in entries:
        if section in LDST_SECTIONS or 'fpsimd' in fn:
            out.append((section, fn, name, bits))
    return out


def gen_ldst(out, r, entries, value, words_per_entry, seeds_per_word):
    for section, fn, name, bits in entries:
        cls = 'simd_ldst' if section in LDST_SECTIONS else 'fp_ldst'
        for k in range(words_per_entry):
            word = random_word(r, bits)
            # Immediates that keep the address inside the image: imm12 below 256, imm19 (literal) within +-16 KiB of the code
            for c, start, n in runs(bits):
                if c == 'i' and n == 12:
                    word = place(word, start, n, r.below(256))
                elif c == 'i' and n == 19:
                    word = place(word, start, n, r.range(-0x1000, 0x1000) & ((1 << 19) - 1))
            rn = letter_value(bits, word, 'n', 5)
            rm = letter_value(bits, word, 'm', 5)
            if rn is not None and rm is not None and rm == rn and rn != 31:
                rm = (rn + 1 + r.below(30)) % 31
                for c, start, n in runs(bits):
                    if c == 'm' and n == 5:
                        word = place(word, start, n, rm)
            for s in range(seeds_per_word):
                regs = {'vk': r.choice(KINDS)}
                if rn is not None:
                    # the middle of the image: offsets of -1 KiB .. +4 KiB and structures of 64 bytes stay inside (sp too, 16-byte aligned:
                    # the random sp of the seed may lie within 256 bytes of the image's ends)
                    base = GUEST_BASE + 0x2000 + r.below(0x8000)
                    if rn == 31:
                        regs['sp'] = base & ~15
                    else:
                        regs[f'x{rn}'] = base
                if rm is not None and rm != 31:
                    # small offsets: the register-offset forms extend and shift them (by up to 4), which keeps 64 << 4 inside the image
                    regs[f'x{rm}'] = r.choice([0, 1, 8, 16, 24, 32, 48, 64, r.below(64)])
                fpcr = 0 if r.below(2) else pick_fpcr(r)
                out.add(r, cls, f'{fn} {word:#010x}', [word], regs, fpcr=fpcr)


# ---- Sequences: NaN chains, FPSR and FPCR accesses, compares and branches ----

def enc(patterns, fn, **fields):
    """The word of a decoder entry with its fields set by letter (several runs of one letter take the value MSB first)."""
    bits = patterns[fn]
    w = fixed(bits)
    for letter, v in fields.items():
        positions = [i for i, c in enumerate(bits) if c == letter]
        for k, i in enumerate(reversed(positions)):
            if (v >> k) & 1:
                w |= 1 << (31 - i)
    return w


def gen_sequences(out, r, patterns, value, count):
    scalar_ops = ['FADD_float', 'FSUB_float', 'FMUL_float', 'FDIV_float', 'FNMUL_float']
    unary_ops = ['FSQRT_float', 'FNEG_float', 'FABS_float']
    fma_ops = ['FMADD_float', 'FMSUB_float', 'FNMADD_float', 'FNMSUB_float']
    vec_ops = ['FADD_2', 'FSUB_2', 'FMUL_vec_2', 'FDIV_2', 'FMLA_vec_2', 'FMLS_vec_2']
    for i in range(count):
        kind = r.below(6)
        y = r.below(2)  # 0 single, 1 double
        fpcr = pick_fpcr(r)
        regs = {'vk': 'f64' if y else 'f32'}
        words = []
        text = []
        if kind <= 2:
            # A chain of 2 to 6 scalar P operations on v0..v5, then what makes the chain escape (a store, a move to a general register, a
            # compare and a conditional select, an FMAX, an FMOV, nothing: the register write)
            for k in range(r.range(2, 6)):
                d, n, m, a = r.below(6), r.below(6), r.below(6), r.below(6)
                c = r.below(10)
                if c < 6:
                    op = r.choice(scalar_ops)
                    words.append(enc(patterns, op, y=y, m=m, n=n, d=d))
                elif c < 8:
                    op = r.choice(fma_ops)
                    words.append(enc(patterns, op, y=y, m=m, a=a, n=n, d=d))
                else:
                    op = r.choice(unary_ops)
                    words.append(enc(patterns, op, y=y, n=n, d=d))
                text.append(op)
            e = r.below(6)
            if e == 0:
                words.append(enc(patterns, 'STR_imm_fpsimd_2', z=2 + y, o=0, i=r.below(64), n=9, t=r.below(6)))
                regs['x9'] = GUEST_BASE + 0x2000 + r.below(0x8000)
                text.append('STR')
            elif e == 1:
                words.append(enc(patterns, 'FMOV_float_gen', z=y, y=y, r=0, o=0, n=r.below(6), d=r.below(28)))
                text.append('FMOV to general')
            elif e == 2:
                words.append(enc(patterns, 'FCMP_float', y=y, m=r.below(6), n=r.below(6), o=r.below(2)))
                words.append(enc(patterns, 'FCSEL_float', y=y, m=r.below(6), c=r.below(16), n=r.below(6), d=r.below(6)))
                text.append('FCMP; FCSEL')
            elif e == 3:
                words.append(enc(patterns, r.choice(['FMAX_float', 'FMINNM_float']), y=y, m=r.below(6), n=r.below(6), d=r.below(6)))
                text.append('FMAX/FMINNM')
            elif e == 4:
                words.append(enc(patterns, 'FCVT_float', y=y, o=1 - y, n=r.below(6), d=r.below(6)))
                text.append('FCVT')
        elif kind == 3:
            # Vector chains (4S or 2D)
            for k in range(r.range(2, 5)):
                op = r.choice(vec_ops)
                words.append(enc(patterns, op, Q=1, z=y, m=r.below(6), n=r.below(6), d=r.below(6)))
                text.append(op)
        elif kind == 4:
            # FPSR: a write, operations, a read (the modelled flags and QC, section 7.4); not under FZ, where dynarmic's native operations
            # raise no Input Denormal (README.md, "Known issues", 12) and the value read would differ
            fpcr &= ~FZ
            words.append(0xD51B4420 | 9)  # msr fpsr, x9
            regs['x9'] = r.choice([0, 0x08000000, 0x9F, 0x10, 0x01]) | (value(r) & 0x0800009F if r.below(4) == 0 else 0)
            # operations whose flags dynarmic raises as Arm does (README.md, "Known issues": the read value would differ otherwise)
            for k in range(r.range(1, 3)):
                c = r.below(3)
                if c == 0:
                    words.append(enc(patterns, r.choice(['FADD_float', 'FSUB_float', 'FMUL_float', 'FDIV_float']), y=y, m=r.below(6), n=r.below(6),
                                     d=r.below(6)))
                elif c == 1:
                    words.append(enc(patterns, r.choice(['SQADD_2', 'UQSUB_2']), Q=1, z=r.below(4), m=r.below(6), n=r.below(6), d=r.below(6)))
                else:
                    words.append(enc(patterns, r.choice(['FSQRT_float', 'FRINTX_float']), y=y, n=r.below(6), d=r.below(6)))
            words.append(0xD53B4420 | 10)  # mrs x10, fpsr
            text.append('MSR FPSR; ops; MRS FPSR')
        else:
            # FPCR: msr fpcr then a floating-point operation, which runs in code specialised for the new value (the harness continues in a
            # function lifted for it, as a pack's dispatcher would)
            words.append(0xD51B4400 | 9)  # msr fpcr, x9
            regs['x9'] = pick_fpcr(r)
            words.append(enc(patterns, r.choice(scalar_ops), y=y, m=r.below(6), n=r.below(6), d=r.below(6)))
            words.append(enc(patterns, 'FRINTX_float', y=y, n=r.below(6), d=r.below(6)))
            words.append(0xD53B4400 | 10)  # mrs x10, fpcr
            text.append('MSR FPCR; op; FRINTX; MRS FPCR')
        out.add(r, 'fp_seq', '; '.join(text), words, regs, fpcr=fpcr)


# ---- The fused multiply-add's invalid case: a quiet NaN addend with infinity times zero ----

# Lane values by element size: quiet NaNs (with payloads and signs), infinities, zeros, a denormal, and 1.0, 2.0, 3.0 (exact sums)
FMA_VALUES = {
    16: dict(qnan=[0x7E01, 0xFE23], inf=[0x7C00, 0xFC00], zero=[0x0000, 0x8000], den=0x0001, one=0x3C00, two=0x4000, three=0x4200),
    32: dict(qnan=[0x7FC00001, 0xFFC12345], inf=[0x7F800000, 0xFF800000], zero=[0, 0x80000000], den=0x00000001, one=0x3F800000,
             two=0x40000000, three=0x40400000),
    64: dict(qnan=[0x7FF8000000000001, 0xFFF8000000ABCDEF], inf=[0x7FF0000000000000, 0xFFF0000000000000], zero=[0, 0x8000000000000000],
             den=0x0000000000000001, one=0x3FF0000000000000, two=0x4000000000000000, three=0x4008000000000000),
}

# (label, words, element size, lanes, addend register, by element): op1 is v4, op2 v5 (element 0 for the by-element forms)
FMA_FORMS = [
    ('fmadd d1, d4, d5, d2', [0x1F450881], 64, 1, 2, False),
    ('fmsub d1, d4, d5, d2', [0x1F458881], 64, 1, 2, False),
    ('fnmadd d1, d4, d5, d2', [0x1F650881], 64, 1, 2, False),
    ('fnmsub d1, d4, d5, d2', [0x1F658881], 64, 1, 2, False),
    ('fmadd s1, s4, s5, s2', [0x1F050881], 32, 1, 2, False),
    ('fnmsub s1, s4, s5, s2', [0x1F258881], 32, 1, 2, False),
    ('fmadd h1, h4, h5, h2', [0x1FC50881], 16, 1, 2, False),
    ('fmla v1.4s, v4.4s, v5.4s', [0x4E25CC81], 32, 4, 1, False),
    ('fmla v1.2s, v4.2s, v5.2s', [0x0E25CC81], 32, 2, 1, False),
    ('fmla v1.2d, v4.2d, v5.2d', [0x4E65CC81], 64, 2, 1, False),
    ('fmls v1.4s, v4.4s, v5.4s', [0x4EA5CC81], 32, 4, 1, False),
    ('fmls v1.2d, v4.2d, v5.2d', [0x4EE5CC81], 64, 2, 1, False),
    ('fmla v1.8h, v4.8h, v5.8h', [0x4E450C81], 16, 8, 1, False),
    ('fmla v1.4s, v4.4s, v5.s[0]', [0x4F851081], 32, 4, 1, True),
    ('fmla v1.2d, v4.2d, v5.d[0]', [0x4FC51081], 64, 2, 1, True),
    ('fmla s1, s4, v5.s[0]', [0x5F851081], 32, 1, 1, True),
    ('fmla d1, d4, v5.d[0]', [0x5FC51081], 64, 1, 1, True),
    # chains: the NaN escapes through an add
    ('fmadd d1, d4, d5, d2; fadd d3, d1, d6', [0x1F450881, 0x1E662823], 64, 1, 2, False),
    ('fmla v1.4s, v4.4s, v5.4s; fadd v3.4s, v1.4s, v6.4s', [0x4E25CC81, 0x4E26D423], 32, 4, 1, False),
]
FMA_FPCRS = [0, DN, FZ, FZ | DN, RZ, DN | RM]


def gen_fma_invalid(out, r):
    """Every FMA form with quiet NaN addends and infinity times zero (Arm: the default NaN and IOC; x86 raises nothing, known issue 16),
    next to lanes without the case: all lanes invalid, every other lane, a denormal factor (invalid under FZ only), none."""
    for label, words, esize, lanes, addend_reg, by_elem in FMA_FORMS:
        v = FMA_VALUES[esize]
        for variant in ('all', 'alternate', 'denormal', 'none'):
            add, op1, op2 = [], [], []
            for i in range(lanes):
                invalid = variant == 'all' or (variant == 'alternate' and i % 2 == 0)
                swap = (i // 2) % 2 == 1 and not by_elem
                if invalid or variant == 'denormal':
                    zero = v['den'] if variant == 'denormal' else v['zero'][i % 2]
                    a, b = (v['inf'][i % 2], zero) if swap else (zero, v['inf'][i % 2])
                    add.append(v['qnan'][i % 2])
                elif variant == 'none':
                    a, b = v['one'], v['inf'][0]
                    add.append(v['qnan'][i % 2])
                else:
                    a, b = v['one'], v['two']
                    add.append(v['three'])
                op1.append(a)
                op2.append(b)
            if by_elem:
                # the element is op2's lane 0 for every lane: infinity, and the factors of v4 zeros (or 1.0)
                op2 = [v['inf'][0]] + [v['two']] * (lanes - 1)
                op1 = [v['den'] if variant == 'denormal' else v['one'] if (variant == 'none' or (variant == 'alternate' and i % 2)) else
                       v['zero'][i % 2] for i in range(lanes)]
                add = [v['three'] if variant == 'alternate' and i % 2 else v['qnan'][i % 2] for i in range(lanes)]

            def pack(vals):
                return sum(x << (i * esize) for i, x in enumerate(vals))

            regs = {f'v{addend_reg}': pack(add), 'v4': pack(op1), 'v5': pack(op2), 'v6': pack([v['one']] * lanes)}
            for fpcr in FMA_FPCRS:
                out.add(r, 'fma_invalid', f'{label} ({variant})', words, regs, fpcr=fpcr)


def build_all(Out, Rng, seed, value, only=None, seeds=5):
    """The FP / SIMD vector files: {file name: Out}, all of them or the ones named in only. seeds: vectors per encoding (5 committed; the
    sweep of gen_vectors.py --count uses more, so that the lifter compiles each encoding once for many seeds)."""
    entries = load_patterns()
    patterns = {fn: bits for _, fn, _, bits in entries}
    files = {}
    scale = 1
    for k, (name, title, sections) in enumerate(DP_FILES):
        if only and name not in only:
            continue
        out = Out(name, title)
        ents = dp_entries(entries, sections)
        gen_dp(out, Rng(seed + 100 + k), ents, value, words_per_entry=60 * scale, seeds_per_word=seeds, fpcr_per_word=seeds > 5)
        if name == 'fp':
            gen_fma_invalid(out, Rng(seed + 109))
        if name == 'crypto':
            # PMULL / PMULL2 of 64-bit lanes (1Q, the cryptographic extension's form) with the crypto vectors, which CTest also runs for
            # x86-64-v3 (no PCLMULQDQ there: the software form)
            r = Rng(seed + 110)
            for i in range(300 * scale):
                word = enc(patterns, 'PMULL', Q=r.below(2), z=3, m=r.below(32), n=r.below(32), d=r.below(32))
                out.add(r, 'pmull_1q', f'PMULL 1Q {word:#010x}', [word], {'vk': r.choice(['i64', 'rnd'])})
        files[name] = out
    if not only or 'simd_ldst' in only:
        out = Out('simd_ldst', 'loads and stores of SIMD&FP registers: LD1-LD4 / ST1-ST4 multiple and single structures, LD1R-LD4R, LDR / STR / '
                               'LDUR / STUR / LDP / STP / LDNP / STNP of B, H, S, D, Q, LDR literal')
        gen_ldst(out, Rng(seed + 120), ldst_entries(entries), value, words_per_entry=120 * scale, seeds_per_word=max(3, seeds * 3 // 5))
        files['simd_ldst'] = out
    if not only or 'fp_seq' in only:
        out = Out('fp_seq', 'floating-point sequences: NaN chains with their escapes, FPSR written and read, FPCR changed mid-block, vector chains')
        gen_sequences(out, Rng(seed + 130), patterns, value, count=6000 * scale)
        files['fp_seq'] = out
    return files
