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
FILE_NAMES = [name for name, _, _ in DP_FILES] + ['simd_ldst', 'fp_seq', 'fp_est']


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


# ---- The estimates and steps of single and double precision (FRECPE, FRSQRTE, FRECPS, FRSQRTS), inline since round 10 ----

# (name, element size, lanes, Q, z): the scalar forms (Q unused) and the vector arrangements
EST_FORMS = [('s', 32, 1, 0, 0), ('d', 64, 1, 0, 1), ('2s', 32, 2, 0, 0), ('4s', 32, 4, 1, 0), ('2d', 64, 2, 1, 1)]
EST_FPCRS = [0, FZ, DN, FZ | DN]
EST_EDGE_FPCRS = [0, FZ, DN, FZ | DN, RP, RM, RZ, FZ | RM]
# Per element size: the bias, the largest biased exponent, the fraction width
EST_FMT = {32: (127, 254, 23), 64: (1023, 2046, 52)}
# Operands the inline estimates leave to the helper or treat apart: signed zeros, infinities, quiet and signalling NaNs with payloads and both signs,
# denormals (the smallest, the largest, one that FRECPE overflows and ones it estimates as denormal), the smallest and largest normals, FRECPE's
# last table exponent (2 * bias - 2) and the two above it (a denormal estimate, or zero under FZ), negative normals (FRSQRTE: the default NaN), 1.0
EST_EDGES = {
    32: [0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00001, 0xFFC12345, 0x7F800001, 0xFFA00000, 0x00000001, 0x007FFFFF, 0x80400000,
         0x00200000, 0x001FFFFF, 0x00400001, 0x00800000, 0x80800000, 0x00FFFFFF, 0x7F7FFFFF, 0xFF7FFFFF, 0x7E7FFFFF, 0x7E000000, 0xFE7FFFFF,
         0x7E800000, 0x7EFFFFFF, 0xFE800000, 0x7F000000, 0x3F800000, 0xBF800000, 0xC0000000, 0x80800001, 0x3F7FFFFF, 0x40000000],
    64: [0x0000000000000000, 0x8000000000000000, 0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000001, 0xFFF8000000ABCDEF,
         0x7FF0000000000001, 0xFFF4000000000000, 0x0000000000000001, 0x000FFFFFFFFFFFFF, 0x8008000000000000, 0x0004000000000000,
         0x0003FFFFFFFFFFFF, 0x0008000000000001, 0x0010000000000000, 0x8010000000000000, 0x001FFFFFFFFFFFFF, 0x7FEFFFFFFFFFFFFF,
         0xFFEFFFFFFFFFFFFF, 0x7FCFFFFFFFFFFFFF, 0x7FC0000000000000, 0xFFCFFFFFFFFFFFFF, 0x7FD0000000000000, 0x7FDFFFFFFFFFFFFF,
         0xFFD0000000000000, 0x7FE0000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0xC000000000000000, 0x8010000000000001,
         0x3FEFFFFFFFFFFFFF, 0x4000000000000000],
}


def est_normal(r, esize, prefix, parity=None, sign=None, emax=None):
    """A normal operand with the given top 8 fraction bits: a random exponent (of the parity asked, up to emax), sign and low fraction bits."""
    bias, top, mant = EST_FMT[esize]
    hi = emax if emax is not None else top
    e = r.range(1, hi)
    if parity is not None and (e & 1) != parity:
        e = e + 1 if e < hi else e - 1
    s = r.below(2) if sign is None else sign
    return (s << (esize - 1)) | (e << mant) | (prefix << (mant - 8)) | (r.next() & ((1 << (mant - 8)) - 1))


def est_word(patterns, op, form, d, n, m=None):
    name, esize, lanes, q, z = form
    scalar = lanes == 1
    fn = {('FRECPE', True): 'FRECPE_2', ('FRECPE', False): 'FRECPE_4', ('FRSQRTE', True): 'FRSQRTE_2', ('FRSQRTE', False): 'FRSQRTE_4',
          ('FRECPS', True): 'FRECPS_2', ('FRECPS', False): 'FRECPS_4', ('FRSQRTS', True): 'FRSQRTS_2', ('FRSQRTS', False): 'FRSQRTS_4'}[(op, scalar)]
    fields = dict(z=z, n=n, d=d)
    if not scalar:
        fields['Q'] = q
    if m is not None:
        fields['m'] = m
    return enc(patterns, fn, **fields)


def est_pack(vals, esize):
    return sum((x & ((1 << esize) - 1)) << (i * esize) for i, x in enumerate(vals))


def gen_estimates(out, r, patterns):
    """FRECPE and FRSQRTE of single and double precision, every form, eight instructions a vector (v0..v7 into v16..v23): every table entry
    (FRECPE's 256 fraction prefixes of either sign, FRSQRTE's 256 for each parity of the biased exponent) in every lane position of every form under
    FZ clear and set, DN clear and set; then the edges (EST_EDGES) between normal lanes, under those and the other rounding modes. FRECPS and
    FRSQRTS (a in v0..v7, b in v8..v15): infinity times zero of both orders and signs, a denormal factor (zero under FZ), quiet and signalling NaNs
    in either operand, infinite operands, overflowing products (FRSQRTS's intermediate overflows where its halved result does not), exact zeros,
    Newton steps of normal values, each special lane between normal ones and next to a denormal one (IDC under FZ)."""
    for op in ('FRECPE', 'FRSQRTE'):
        rsqrt = op == 'FRSQRTE'
        for form in EST_FORMS:
            name, esize, lanes, q, z = form
            bias, top, mant = EST_FMT[esize]
            entries = list(range(512 if rsqrt else 256))
            per_vector = 8 * lanes
            for fpcr in EST_FPCRS:
                # rotate the entries so each FPCR puts them in other lane positions
                shift = EST_FPCRS.index(fpcr) * 3
                order = entries[shift:] + entries[:shift]
                for start in range(0, len(order), per_vector):
                    chunk = order[start:start + per_vector]
                    regs = {'vk': f'f{esize}'}
                    words = []
                    for k in range(8):
                        vals = []
                        for i in range(128 // esize):
                            j = k * lanes + i
                            if i < lanes and j < len(chunk):
                                e = chunk[j]
                                if rsqrt:
                                    vals.append(est_normal(r, esize, e & 0xFF, parity=e >> 8, sign=0))
                                else:
                                    vals.append(est_normal(r, esize, e, emax=2 * bias - 2))
                            else:
                                vals.append(r.next())   # lanes the form does not read
                        regs[f'v{k}'] = est_pack(vals, esize)
                        words.append(est_word(patterns, op, form, 16 + k, k))
                    out.add(r, 'fp_est_table', f'{op} {name} entries {chunk[0]}..{chunk[-1]} (8 instructions)', words, regs, fpcr=fpcr)
            # the edges, one or two a register between normal lanes, each edge in every lane position across the vectors
            edges = EST_EDGES[esize]
            for fpcr in EST_EDGE_FPCRS:
                for rot in range(max(1, lanes)):
                    regs = {'vk': f'f{esize}'}
                    words = []
                    pos = 0
                    for k in range(8):
                        vals = []
                        for i in range(128 // esize):
                            if i < lanes and (i + k + rot) % 2 == 0:
                                vals.append(edges[(pos + rot * 5) % len(edges)])
                                pos += 1
                            elif i < lanes:
                                vals.append(est_normal(r, esize, r.below(256), sign=0 if rsqrt else None, emax=2 * bias - 2))
                            else:
                                vals.append(r.next())
                        regs[f'v{k}'] = est_pack(vals, esize)
                        words.append(est_word(patterns, op, form, 16 + k, k))
                    out.add(r, 'fp_est_edge', f'{op} {name} edges (8 instructions)', words, regs, fpcr=fpcr)
                # one instruction a vector, under three FPCRs: the final FPSR holds that instruction's flags alone
                if fpcr in (0, FZ, DN):
                    for j, e in enumerate(edges):
                        vals = [est_normal(r, esize, r.below(256), sign=0 if rsqrt else None, emax=2 * bias - 2) for _ in range(128 // esize)]
                        vals[j % lanes] = e
                        out.add(r, 'fp_est_edge1', f'{op} {name} edge {e:#x}', [est_word(patterns, op, form, 16, 0)],
                                {'vk': f'f{esize}', 'v0': est_pack(vals, esize)}, fpcr=fpcr)
                # the scalar forms: every edge in lane 0 once more
                if lanes == 1:
                    for start in range(0, len(edges), 8):
                        regs = {'vk': f'f{esize}'}
                        words = []
                        for k, e in enumerate(edges[start:start + 8]):
                            regs[f'v{k}'] = est_pack([e, r.next()] if esize == 64 else [e, r.next(), r.next(), r.next()], esize)
                            words.append(est_word(patterns, op, form, 16 + k, k))
                        out.add(r, 'fp_est_edge', f'{op} {name} edges {start}.. (scalar)', words, regs, fpcr=fpcr)
    for op in ('FRECPS', 'FRSQRTS'):
        for form in EST_FORMS:
            name, esize, lanes, q, z = form
            bias, top, mant = EST_FMT[esize]
            sign = 1 << (esize - 1)
            inf = ((1 << (esize - 1)) - 1) & ~((1 << mant) - 1)
            qnan = inf | (1 << (mant - 1))
            one, two, three = (bias << mant), ((bias + 1) << mant), ((bias + 1) << mant) | (1 << (mant - 1))
            big = (top << mant) | ((1 << mant) - 1)   # the largest normal
            p127 = ((bias + bias) << mant)            # 2^bias
            p126 = ((bias + bias - 1) << mant)        # 2^(bias - 1)
            den, den2 = 1, (1 << (mant - 1)) | 5

            def normal():
                return est_normal(r, esize, r.below(256), emax=bias + 20 if r.below(2) else None)

            def newton():
                # a and b near a reciprocal (FRECPS) or a reciprocal square root pair (FRSQRTS): small results, exact zeros sometimes
                x = est_normal(r, esize, r.below(256), sign=0, emax=bias + 8)
                return (x, (bias + bias - ((x >> mant) & ((1 << (esize - 1 - mant)) - 1))) << mant | (r.next() & ((1 << mant) - 1)))
            specials = [(0, inf), (inf, 0), (sign, inf), (0, inf | sign), (sign, inf | sign), (inf, sign), (den, inf), (inf, den | sign),
                        (den2 | sign, inf), (qnan | 1, one), (one, qnan | 2), (inf | 1, one), (one, inf | 3), (qnan | 7, inf | 1), (inf | 9, qnan | 4),
                        (qnan | sign | 5, qnan | 6), (qnan, 0), (0, qnan | sign), (inf, one), (two, inf | sign), (inf, inf), (inf | sign, inf),
                        (big, big), (big, big | sign), (big, three), (p127, two), (p126 | sign, three | sign), (big, one | 1), (one, two),
                        (one, three), (two, one), (den, two), (two, den2), (den | sign, den2)]
            for fpcr in EST_EDGE_FPCRS:
                for rot in range(2):
                    regs = {'vk': f'f{esize}'}
                    words = []
                    pos = rot * 7
                    for k in range(8):
                        av, bv = [], []
                        for i in range(128 // esize):
                            if i < lanes and (i + k + rot) % 2 == 0:
                                a_, b_ = specials[pos % len(specials)]
                                pos += 1
                            elif i < lanes and (k + rot) % 3 == 0:
                                a_, b_ = newton()
                            elif i < lanes:
                                a_, b_ = normal(), normal()
                            else:
                                a_, b_ = r.next(), r.next()
                            av.append(a_)
                            bv.append(b_)
                        regs[f'v{k}'] = est_pack(av, esize)
                        regs[f'v{k + 8}'] = est_pack(bv, esize)
                        words.append(est_word(patterns, op, form, 16 + k, k, m=k + 8))
                    out.add(r, 'fp_step_edge', f'{op} {name} special lanes (8 instructions)', words, regs, fpcr=fpcr)
                # no special lane: the fast path alone, and a lone denormal (no IDC there: dynarmic's native step raises none)
                regs = {'vk': f'f{esize}'}
                words = []
                for k in range(8):
                    pairs = [newton() if (k + i) % 2 else (normal(), normal()) for i in range(128 // esize)]
                    if k == 7:
                        pairs[0] = (den, two)
                    regs[f'v{k}'] = est_pack([p[0] for p in pairs], esize)
                    regs[f'v{k + 8}'] = est_pack([p[1] for p in pairs], esize)
                    words.append(est_word(patterns, op, form, 16 + k, k, m=k + 8))
                out.add(r, 'fp_step_edge', f'{op} {name} normal lanes (8 instructions)', words, regs, fpcr=fpcr)
                # one instruction a vector, each special pair in one lane between normal ones, under three FPCRs
                if fpcr in (0, FZ, DN):
                    for j, (a_, b_) in enumerate(specials):
                        av = [normal() for _ in range(128 // esize)]
                        bv = [normal() for _ in range(128 // esize)]
                        av[j % lanes], bv[j % lanes] = a_, b_
                        out.add(r, 'fp_step_edge1', f'{op} {name} {a_:#x} {b_:#x}', [est_word(patterns, op, form, 16, 0, m=1)],
                                {'vk': f'f{esize}', 'v0': est_pack(av, esize), 'v1': est_pack(bv, esize)}, fpcr=fpcr)


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
    if not only or 'fp_est' in only:
        out = Out('fp_est', 'FRECPE, FRSQRTE, FRECPS and FRSQRTS of single and double precision: every estimate table entry in every form, the edges '
                            'of the inline estimates and steps, under FZ and DN clear and set and the other rounding modes')
        gen_estimates(out, Rng(seed + 140), patterns)
        files['fp_est'] = out
    return files
