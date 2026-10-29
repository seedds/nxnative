#!/usr/bin/env python3
"""Writes the instruction vectors of nxaot_vec (tests/vectors/*.tsv) and the census test's synthetic code blob.

Every encoding here is written by hand from the Arm Architecture Reference Manual (A64 base instructions: the encoding index of chapter C4), one
function per instruction class, each sweeping its fields over their ranges with operands drawn from edge values (0, 1, -1, the 32- and 64-bit
extremes, single bits, byte patterns) and from a fixed-seed pseudo-random stream, so the files are the same on every run and every Python.
nxaot_vec runs each vector on dynarmic's JIT and as lifted code and compares the results; the expected values are not in the files.

  gen_vectors.py                 write tests/vectors/*.tsv
  gen_vectors.py --check         exit 1 when the committed files differ from what this script writes (a CTest)
  gen_vectors.py --census-blob <file> --census-expect <file>
                                 write the synthetic code blob of the census test and the counts nxaot census must find in it

A vector line (src/vec/vectors.h): id, class, text, code (hex words), seed, regs (name=hex), mem (offset:hexbytes), tab-separated. The seed fills
the registers and the 64 KiB memory image the vector does not set; memory operands always point into that image.
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VECTOR_DIR = os.path.join(HERE, 'vectors')

# The conventions of src/vec/vectors.h
GUEST_BASE = 0x10000000
IMAGE_SIZE = 0x10000
CODE_OFFSET = 0x8000
CODE_BASE = GUEST_BASE + CODE_OFFSET
M64 = (1 << 64) - 1
M32 = (1 << 32) - 1

SEED = 0x6E78616F74766563  # "nxaotvec"


class Rng:
    """splitmix64: the same numbers on every Python (random.Random's helpers changed between versions)."""

    def __init__(self, seed):
        self.s = seed & M64

    def next(self):
        self.s = (self.s + 0x9E3779B97F4A7C15) & M64
        z = self.s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
        return z ^ (z >> 31)

    def below(self, n):
        return self.next() % n

    def range(self, lo, hi):
        return lo + self.below(hi - lo + 1)

    def choice(self, seq):
        return seq[self.below(len(seq))]

    def chance(self, num, den):
        return self.below(den) < num


EDGE = [0, 1, 2, 3, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x100000000,
        0x7FFFFFFFFFFFFFFF, 0x8000000000000000, M64, M64 - 1, 0xFFFFFFFF80000000, 0xFFFFFFFF7FFFFFFF, 0x0000000080000001,
        0x0101010101010101, 0x8080808080808080, 0xFF00FF00FF00FF00, 0x00FF00FF00FF00FF, 0x5555555555555555, 0xAAAAAAAAAAAAAAAA,
        0x00000000FFFFFFFF, 0xFFFFFFFF00000000, 0x123456789ABCDEF0, 0xFEDCBA9876543210, 0x8000000000000001, 0x7FFFFFFF80000000]


def value(r):
    """An operand: an edge value, a random 64-bit value, a small one, a small negative one, a single bit, or a sign-extended 32-bit edge."""
    k = r.below(10)
    if k < 3:
        return r.choice(EDGE)
    if k < 6:
        return r.next()
    if k == 6:
        return r.below(256)
    if k == 7:
        return (-r.range(1, 256)) & M64
    if k == 8:
        return 1 << r.below(64)
    return (r.choice([0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0]) | (r.below(2) * 0xFFFFFFFF00000000)) & M64


def sext(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


COND_NAMES = ['eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', 'al', 'nv']


class Out:
    """The vectors of one file."""

    def __init__(self, name, title):
        self.name = name
        self.title = title
        self.lines = []
        self.per_class = {}

    def add(self, r, cls, text, words, regs=None, mem=None):
        n = len(self.lines) + 1
        seed = r.next()
        regs_s = ','.join(f'{k}={v & M64:x}' for k, v in regs.items()) if regs else '-'
        mem_s = ','.join(f'{off:x}:{data.hex()}' for off, data in mem) if mem else '-'
        code = ','.join(f'{w & M32:08x}' for w in words)
        assert '\t' not in text
        self.lines.append('\t'.join([f'{self.name}.{n:05d}', cls, text, code, f'{seed:016x}', regs_s, mem_s]))
        self.per_class[cls] = self.per_class.get(cls, 0) + 1

    def text(self):
        head = [f'# nxaot vectors: {self.title}',
                '# Written by dm/tools/aot/tests/gen_vectors.py; do not edit (run it again). Format: src/vec/vectors.h',
                '# id\tclass\ttext\tcode\tseed\tregs\tmem']
        return '\n'.join(head + self.lines) + '\n'


def setreg(regs, n, v, sp=False):
    """Register n of an instruction: x0..x30, 31 as SP where the encoding means SP, else XZR (nothing to set)."""
    if n == 31:
        if sp:
            regs['sp'] = v & M64
    else:
        regs[f'x{n}'] = v & M64


def rname(n, sf, sp=False):
    if n == 31:
        return ('sp' if sf else 'wsp') if sp else ('xzr' if sf else 'wzr')
    return f'{"x" if sf else "w"}{n}'


# ---- Encoders (Arm ARM, A64 base instructions) ----

def enc_adr(op, imm, rd):
    imm &= (1 << 21) - 1
    return (op << 31) | ((imm & 3) << 29) | (0b10000 << 24) | ((imm >> 2) << 5) | rd


def enc_addsub_imm(sf, op, s, sh, imm12, rn, rd):
    return (sf << 31) | (op << 30) | (s << 29) | (0b100010 << 23) | (sh << 22) | (imm12 << 10) | (rn << 5) | rd


def enc_logic_imm(sf, opc, n, immr, imms, rn, rd):
    return (sf << 31) | (opc << 29) | (0b100100 << 23) | (n << 22) | (immr << 16) | (imms << 10) | (rn << 5) | rd


def enc_movewide(sf, opc, hw, imm16, rd):
    return (sf << 31) | (opc << 29) | (0b100101 << 23) | (hw << 21) | (imm16 << 5) | rd


def enc_bitfield(sf, opc, n, immr, imms, rn, rd):
    return (sf << 31) | (opc << 29) | (0b100110 << 23) | (n << 22) | (immr << 16) | (imms << 10) | (rn << 5) | rd


def enc_extr(sf, n, rm, imms, rn, rd):
    return (sf << 31) | (0b100111 << 23) | (n << 22) | (rm << 16) | (imms << 10) | (rn << 5) | rd


def enc_bcond(imm19, cond):
    return 0x54000000 | ((imm19 & 0x7FFFF) << 5) | cond


def enc_b(link, imm26):
    return (0x94000000 if link else 0x14000000) | (imm26 & 0x3FFFFFF)


def enc_cb(sf, op, imm19, rt):
    return (sf << 31) | (0b011010 << 25) | (op << 24) | ((imm19 & 0x7FFFF) << 5) | rt


def enc_tb(op, bit, imm14, rt):
    return ((bit >> 5) << 31) | (0b011011 << 25) | (op << 24) | ((bit & 31) << 19) | ((imm14 & 0x3FFF) << 5) | rt


def enc_brreg(kind, rn):
    return {'br': 0xD61F0000, 'blr': 0xD63F0000, 'ret': 0xD65F0000}[kind] | (rn << 5)


def enc_logic_sh(sf, opc, shift, n, rm, imm6, rn, rd):
    return (sf << 31) | (opc << 29) | (0b01010 << 24) | (shift << 22) | (n << 21) | (rm << 16) | (imm6 << 10) | (rn << 5) | rd


def enc_addsub_sh(sf, op, s, shift, rm, imm6, rn, rd):
    return (sf << 31) | (op << 30) | (s << 29) | (0b01011 << 24) | (shift << 22) | (rm << 16) | (imm6 << 10) | (rn << 5) | rd


def enc_addsub_ext(sf, op, s, rm, option, imm3, rn, rd):
    return (sf << 31) | (op << 30) | (s << 29) | (0b01011 << 24) | (1 << 21) | (rm << 16) | (option << 13) | (imm3 << 10) | (rn << 5) | rd


def enc_adc(sf, op, s, rm, rn, rd):
    return (sf << 31) | (op << 30) | (s << 29) | (0b11010000 << 21) | (rm << 16) | (rn << 5) | rd


def enc_ccmp(sf, op, imm, rm_or_imm5, cond, rn, nzcv):
    return (sf << 31) | (op << 30) | (1 << 29) | (0b11010010 << 21) | (rm_or_imm5 << 16) | (cond << 12) | (imm << 11) | (rn << 5) | nzcv


def enc_csel(sf, op, op2, rm, cond, rn, rd):
    return (sf << 31) | (op << 30) | (0b11010100 << 21) | (rm << 16) | (cond << 12) | (op2 << 10) | (rn << 5) | rd


def enc_dp1(sf, opcode, rn, rd):
    return (sf << 31) | (1 << 30) | (0b11010110 << 21) | (opcode << 10) | (rn << 5) | rd


def enc_dp2(sf, opcode, rm, rn, rd):
    return (sf << 31) | (0b11010110 << 21) | (rm << 16) | (opcode << 10) | (rn << 5) | rd


def enc_dp3(sf, op31, o0, rm, ra, rn, rd):
    return (sf << 31) | (0b11011 << 24) | (op31 << 21) | (rm << 16) | (o0 << 15) | (ra << 10) | (rn << 5) | rd


def enc_ldst_uimm(size, opc, imm12, rn, rt):
    return (size << 30) | (0b111 << 27) | (0b01 << 24) | (opc << 22) | (imm12 << 10) | (rn << 5) | rt


def enc_ldst_imm9(size, opc, imm9, kind, rn, rt):
    """kind: 0 unscaled (ldur), 1 post-index, 2 unprivileged (ldtr), 3 pre-index."""
    return (size << 30) | (0b111 << 27) | (opc << 22) | ((imm9 & 0x1FF) << 12) | (kind << 10) | (rn << 5) | rt


def enc_ldst_reg(size, opc, rm, option, s, rn, rt):
    return (size << 30) | (0b111 << 27) | (opc << 22) | (1 << 21) | (rm << 16) | (option << 13) | (s << 12) | (0b10 << 10) | (rn << 5) | rt


def enc_ldr_lit(opc, imm19, rt):
    return (opc << 30) | (0b011 << 27) | ((imm19 & 0x7FFFF) << 5) | rt


def enc_pair(opc, mode, l, imm7, rt2, rn, rt):
    return (opc << 30) | (0b101 << 27) | (mode << 23) | (l << 22) | ((imm7 & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt


def enc_excl(size, o2, l, o1, rs, o0, rt2, rn, rt):
    return (size << 30) | (0b001000 << 24) | (o2 << 23) | (l << 22) | (o1 << 21) | (rs << 16) | (o0 << 15) | (rt2 << 10) | (rn << 5) | rt


def enc_sys(read, op0, op1, crn, crm, op2, rt):
    return (0xD5300000 if read else 0xD5100000) | ((op0 - 2) << 19) | (op1 << 16) | (crn << 12) | (crm << 8) | (op2 << 5) | rt


SYSREGS = {  # name: (op0, op1, CRn, CRm, op2, writable from EL0 through dynarmic)
    'nzcv': (3, 3, 4, 2, 0, True), 'fpcr': (3, 3, 4, 4, 0, True), 'fpsr': (3, 3, 4, 4, 1, True), 'tpidr_el0': (3, 3, 13, 0, 2, True),
    'tpidrro_el0': (3, 3, 13, 0, 3, False), 'cntfrq_el0': (3, 3, 14, 0, 0, False), 'cntpct_el0': (3, 3, 14, 0, 1, False),
    'cntvct_el0': (3, 3, 14, 0, 2, False), 'ctr_el0': (3, 3, 0, 0, 1, False), 'dczid_el0': (3, 3, 0, 0, 7, False),
}


def mrs(name, rt):
    op0, op1, crn, crm, op2, _ = SYSREGS[name]
    return enc_sys(True, op0, op1, crn, crm, op2, rt)


def msr(name, rt):
    op0, op1, crn, crm, op2, _ = SYSREGS[name]
    return enc_sys(False, op0, op1, crn, crm, op2, rt)


NOP, YIELD, WFE, WFI, SEV, SEVL = 0xD503201F, 0xD503203F, 0xD503205F, 0xD503207F, 0xD503209F, 0xD50320BF


def add_imm(rd, rn, imm):  # add xd, xn, #imm (64-bit)
    return enc_addsub_imm(1, 0, 0, 0, imm, rn, rd)


def bitmask_imm(r, sf):
    """A valid (N, immr, imms) for the logical immediates: an element size, a run of ones in it, a rotation."""
    sizes = [2, 4, 8, 16, 32, 64] if sf else [2, 4, 8, 16, 32]
    esize = r.choice(sizes)
    ones = r.range(1, esize - 1)
    rot = r.below(esize)
    n = 1 if esize == 64 else 0
    imms = ((~(esize * 2 - 1)) & 0x3F) | (ones - 1) if esize != 64 else ones - 1
    return n, rot, imms & 0x3F


def data_address(r, size, align=1, sp_base=False):
    """An effective address whose size bytes are inside the image, aligned to align."""
    lo = GUEST_BASE
    hi = GUEST_BASE + IMAGE_SIZE - size
    ea = r.range(lo, hi)
    ea -= ea % align
    return ea


# ---- Data processing, immediate ----

def gen_dp_imm(out, r):
    for i in range(300):
        op = r.below(2)
        imm = r.below(1 << 21) if i % 3 else r.choice([0, 1, (1 << 20) - 1, 1 << 20, (1 << 21) - 1])
        rd = r.below(32)
        name = 'ADRP' if op else 'ADR'
        out.add(r, 'adr', f'{name} {rname(rd, 1)}, imm={sext(imm, 21)}', [enc_adr(op, imm, rd)])

    for i in range(1000):
        sf, op, s, sh = r.below(2), r.below(2), r.below(2), r.below(2)
        imm12 = r.choice([0, 1, 0xFFF, 0x800, 0x7FF]) if i % 4 == 0 else r.below(4096)
        rn, rd = r.below(32), r.below(32)
        regs = {}
        setreg(regs, rn, value(r), sp=True)
        if r.chance(1, 4) and rn != 31:
            # make the flags interesting: an operand right at the immediate
            regs[f'x{rn}'] = ((imm12 << (12 * sh)) + r.choice([0, 1, -1])) & M64
        name = ['ADD', 'SUB'][op] + ('S' if s else '')
        out.add(r, 'addsub_imm', f'{name} (immediate) {rname(rd, sf, not s)}, {rname(rn, sf, True)}, #{imm12}, lsl #{12 * sh}',
                [enc_addsub_imm(sf, op, s, sh, imm12, rn, rd)], regs)

    for i in range(1000):
        sf, opc = r.below(2), r.below(4)
        rn, rd = r.below(32), r.below(32)
        if i % 10 == 9:
            n, immr, imms = r.below(2), r.below(64), r.below(64)  # often reserved: dynarmic and the lifter must agree on the exit
        else:
            n, immr, imms = bitmask_imm(r, sf)
        regs = {}
        setreg(regs, rn, value(r))
        name = ['AND', 'ORR', 'EOR', 'ANDS'][opc]
        out.add(r, 'logic_imm', f'{name} (immediate) sf={sf} N={n} immr={immr} imms={imms} Rn={rn} Rd={rd}',
                [enc_logic_imm(sf, opc, n, immr, imms, rn, rd)], regs)

    for i in range(500):
        sf, opc, hw = r.below(2), r.choice([0, 2, 3, 3, 1]), r.below(4)
        imm16 = r.choice([0, 1, 0xFFFF, 0x8000, 0x7FFF]) if i % 3 == 0 else r.below(65536)
        rd = r.below(32)
        regs = {}
        setreg(regs, rd, value(r))
        name = ['MOVN', '(unallocated)', 'MOVZ', 'MOVK'][opc]
        out.add(r, 'movewide', f'{name} sf={sf} hw={hw} imm16={imm16:#x} Rd={rd}', [enc_movewide(sf, opc, hw, imm16, rd)], regs)

    for i in range(1000):
        sf, opc = r.below(2), r.choice([0, 1, 2, 3])
        n = sf if i % 16 else r.below(2)
        immr = r.below(64 if sf else 32) if i % 8 else r.below(64)
        imms = r.below(64 if sf else 32) if i % 8 else r.below(64)
        rn, rd = r.below(32), r.below(32)
        regs = {}
        setreg(regs, rn, value(r))
        setreg(regs, rd, value(r))
        name = ['SBFM', 'BFM', 'UBFM', '(unallocated)'][opc]
        out.add(r, 'bitfield', f'{name} sf={sf} N={n} immr={immr} imms={imms} Rn={rn} Rd={rd}', [enc_bitfield(sf, opc, n, immr, imms, rn, rd)], regs)

    for i in range(400):
        sf = r.below(2)
        lsb = r.below(64 if sf else 32) if i % 10 else r.below(64)
        rm, rn, rd = r.below(32), r.below(32), r.below(32)
        regs = {}
        setreg(regs, rm, value(r))
        setreg(regs, rn, value(r))
        out.add(r, 'extract', f'EXTR sf={sf} Rm={rm} lsb={lsb} Rn={rn} Rd={rd}', [enc_extr(sf, sf, rm, lsb, rn, rd)], regs)


# ---- Data processing, register ----

def gen_dp_reg(out, r):
    for i in range(1000):
        sf, opc, shift, n = r.below(2), r.below(4), r.below(4), r.below(2)
        imm6 = r.below(64 if sf else 32) if i % 10 else r.choice([0, 31, 32, 63])
        rm, rn, rd = r.below(32), r.below(32), r.below(32)
        regs = {}
        setreg(regs, rm, value(r))
        setreg(regs, rn, value(r))
        name = [['AND', 'BIC'], ['ORR', 'ORN'], ['EOR', 'EON'], ['ANDS', 'BICS']][opc][n]
        out.add(r, 'logic_shift', f'{name} (shifted register) sf={sf} {["lsl", "lsr", "asr", "ror"][shift]} #{imm6} Rm={rm} Rn={rn} Rd={rd}',
                [enc_logic_sh(sf, opc, shift, n, rm, imm6, rn, rd)], regs)

    for i in range(800):
        sf, op, s = r.below(2), r.below(2), r.below(2)
        shift = r.below(3) if i % 20 else 3
        imm6 = r.below(64 if sf else 32) if i % 10 else r.choice([0, 31, 32, 63])
        rm, rn, rd = r.below(32), r.below(32), r.below(32)
        regs = {}
        a, b = value(r), value(r)
        if i % 5 == 0:
            b = a if r.below(2) else (a + r.choice([1, -1])) & M64
            imm6 = 0
        setreg(regs, rn, a)
        setreg(regs, rm, b)
        name = ['ADD', 'SUB'][op] + ('S' if s else '')
        out.add(r, 'addsub_shift', f'{name} (shifted register) sf={sf} shift={shift} #{imm6} Rm={rm} Rn={rn} Rd={rd}',
                [enc_addsub_sh(sf, op, s, shift, rm, imm6, rn, rd)], regs)

    for i in range(700):
        sf, op, s = r.below(2), r.below(2), r.below(2)
        option = r.below(8)
        imm3 = r.below(5) if i % 20 else r.range(5, 7)
        rm, rn, rd = r.below(32), r.below(32), r.below(32)
        regs = {}
        setreg(regs, rn, value(r), sp=True)
        setreg(regs, rm, value(r))
        name = ['ADD', 'SUB'][op] + ('S' if s else '')
        ext = ['uxtb', 'uxth', 'uxtw', 'uxtx', 'sxtb', 'sxth', 'sxtw', 'sxtx'][option]
        out.add(r, 'addsub_ext', f'{name} (extended register) sf={sf} {ext} #{imm3} Rm={rm} Rn={rn} Rd={rd}',
                [enc_addsub_ext(sf, op, s, rm, option, imm3, rn, rd)], regs)

    for i in range(500):
        sf, op, s = r.below(2), r.below(2), r.below(2)
        rm, rn, rd = r.below(32), r.below(32), r.below(32)
        regs = {'nzcv': r.below(16) << 28}
        setreg(regs, rn, value(r))
        setreg(regs, rm, value(r))
        name = ['ADC', 'SBC'][op] + ('S' if s else '')
        out.add(r, 'adc', f'{name} sf={sf} Rm={rm} Rn={rn} Rd={rd} nzcv={regs["nzcv"] >> 28:04b}', [enc_adc(sf, op, s, rm, rn, rd)], regs)

    for i in range(800):
        sf, op, imm = r.below(2), r.below(2), r.below(2)
        cond, nzcv = r.below(16), r.below(16)
        rn = r.below(32)
        rm = r.below(32)
        regs = {'nzcv': r.below(16) << 28}
        a = value(r)
        setreg(regs, rn, a)
        if imm:
            if r.below(2):
                setreg(regs, rn, rm)  # rm is the 5-bit immediate here: compare equal sometimes
        else:
            setreg(regs, rm, a if r.below(3) == 0 else value(r))
        name = ['CCMN', 'CCMP'][op] + (' (immediate)' if imm else ' (register)')
        out.add(r, 'ccmp', f'{name} sf={sf} {"#" if imm else "Rm="}{rm} {COND_NAMES[cond]} Rn={rn} nzcv={nzcv:04b} flags={regs["nzcv"] >> 28:04b}',
                [enc_ccmp(sf, op, imm, rm, cond, rn, nzcv)], regs)

    for i in range(800):
        sf, op, op2 = r.below(2), r.below(2), r.below(2) if i % 25 else r.range(2, 3)
        cond = r.below(16)
        rm, rn, rd = r.below(32), r.below(32), r.below(32)
        regs = {'nzcv': r.below(16) << 28}
        setreg(regs, rn, value(r))
        setreg(regs, rm, value(r))
        name = [['CSEL', 'CSINC'], ['CSINV', 'CSNEG']][op][op2 & 1]
        out.add(r, 'csel', f'{name} sf={sf} op2={op2} Rm={rm} {COND_NAMES[cond]} Rn={rn} Rd={rd} flags={regs["nzcv"] >> 28:04b}',
                [enc_csel(sf, op, op2, rm, cond, rn, rd)], regs)

    ones = [(0, 'RBIT'), (1, 'REV16'), (2, 'REV32/REV'), (3, 'REV'), (4, 'CLZ'), (5, 'CLS'), (6, '(unallocated)')]
    for i in range(500):
        sf = r.below(2)
        opcode, name = r.choice(ones)
        rn, rd = r.below(32), r.below(32)
        regs = {}
        setreg(regs, rn, value(r))
        out.add(r, 'dp1', f'{name} sf={sf} opcode={opcode} Rn={rn} Rd={rd}', [enc_dp1(sf, opcode, rn, rd)], regs)

    for i in range(1500):
        kind = r.below(4)
        rm, rn, rd = r.below(32), r.below(32), r.below(32)
        regs = {}
        sf = r.below(2)
        if kind == 0:
            opcode = r.choice([2, 3])
            name = 'UDIV' if opcode == 2 else 'SDIV'
            a, b = value(r), value(r)
            pick = r.below(6)
            if pick == 0:
                b = 0
            elif pick == 1:
                a, b = (0x8000000000000000 if sf else 0x80000000), M64
            elif pick == 2:
                b = r.choice([1, M64, 2, 0xFFFFFFFF])
        elif kind == 1:
            opcode = r.range(8, 11)
            name = ['LSLV', 'LSRV', 'ASRV', 'RORV'][opcode - 8]
            a = value(r)
            b = r.choice([r.below(64), r.below(64), r.range(64, 127), r.range(128, 255), value(r), 32, 31, 63, 64, 0])
        elif kind == 2:
            sz = r.below(4)
            opcode = 0b010000 | sz
            sf = 1 if sz == 3 else 0
            if r.chance(1, 20):
                sf ^= 1  # the other sf is unallocated
            name = ['CRC32B', 'CRC32H', 'CRC32W', 'CRC32X'][sz]
            a, b = value(r), value(r)
        else:
            sz = r.below(4)
            opcode = 0b010100 | sz
            sf = 1 if sz == 3 else 0
            name = ['CRC32CB', 'CRC32CH', 'CRC32CW', 'CRC32CX'][sz]
            a, b = value(r), value(r)
        setreg(regs, rn, a)
        setreg(regs, rm, b)
        out.add(r, 'dp2', f'{name} sf={sf} Rm={rm} Rn={rn} Rd={rd}', [enc_dp2(sf, opcode, rm, rn, rd)], regs)

    threes = [(1, 0, 0, 'MADD'), (1, 0, 1, 'MSUB'), (0, 0, 0, 'MADD'), (0, 0, 1, 'MSUB'), (1, 1, 0, 'SMADDL'), (1, 1, 1, 'SMSUBL'),
              (1, 2, 0, 'SMULH'), (1, 5, 0, 'UMADDL'), (1, 5, 1, 'UMSUBL'), (1, 6, 0, 'UMULH')]
    for i in range(800):
        sf, op31, o0, name = r.choice(threes)
        rm, ra, rn, rd = r.below(32), r.below(32), r.below(32), r.below(32)
        if name in ('SMULH', 'UMULH'):
            ra = 31
        regs = {}
        setreg(regs, rn, value(r))
        setreg(regs, rm, value(r))
        setreg(regs, ra, value(r))
        out.add(r, 'dp3', f'{name} sf={sf} Rm={rm} Ra={ra} Rn={rn} Rd={rd}', [enc_dp3(sf, op31, o0, rm, ra, rn, rd)], regs)


# ---- Branches ----

def gen_branch(out, r):
    for i in range(300):
        link = r.below(2)
        off = r.choice([1, -1, 2, (1 << 25) - 1, -(1 << 25)]) if i % 4 == 0 else (sext(r.next(), 26) or 1)
        out.add(r, 'b_bl', f'{"BL" if link else "B"} {off * 4:+#x}', [enc_b(link, off)], {'x30': value(r)})

    for i in range(600):
        cond = r.below(16)
        off = r.choice([1, 2, -1, (1 << 18) - 1, -(1 << 18)]) if i % 5 == 0 else (sext(r.next(), 19) or 1)
        nzcv = r.below(16) << 28
        out.add(r, 'bcond', f'B.{COND_NAMES[cond]} {off * 4:+#x} flags={nzcv >> 28:04b}', [enc_bcond(off, cond)], {'nzcv': nzcv})

    for i in range(600):
        regs = {}
        rt = r.below(32)
        off = sext(r.next(), 14 if i % 2 else 19) or 1
        v = value(r)
        if i % 2:
            op, bit = r.below(2), r.below(64)
            if r.chance(1, 3):
                v ^= 1 << bit
            setreg(regs, rt, v)
            out.add(r, 'cbz_tbz', f'{"TBNZ" if op else "TBZ"} {rname(rt, 1)}, #{bit}, {off * 4:+#x}', [enc_tb(op, bit, off, rt)], regs)
        else:
            sf, op = r.below(2), r.below(2)
            if r.chance(1, 3):
                v = r.choice([0, 0x100000000, 0xFFFFFFFF00000000, 1])
            setreg(regs, rt, v)
            out.add(r, 'cbz_tbz', f'{"CBNZ" if op else "CBZ"} {rname(rt, sf)}, {off * 4:+#x}', [enc_cb(sf, op, off, rt)], regs)

    for i in range(300):
        kind = r.choice(['br', 'blr', 'ret'])
        rn = 30 if kind == 'ret' and r.below(2) else r.below(31)
        # A canonical target (bits 63:55 equal), as a real branch target is: dynarmic keeps 56-bit PCs (README.md, "Known issues")
        target = r.next() & 0x007FFFFFFFFFFFFF
        if r.below(4) == 0:
            target |= 0xFF80000000000000
        if r.below(3):
            target &= ~3
        regs = {f'x{rn}': target, 'x30': value(r)} if rn != 30 else {'x30': target}
        out.add(r, 'br_blr_ret', f'{kind.upper()} x{rn}', [enc_brreg(kind, rn)], regs)

    # Branches inside the vector's own code: the lifter links its blocks, conditions read flags set earlier in the sequence
    for i in range(400):
        a, b = value(r), value(r)
        if r.below(3) == 0:
            b = a
        cond = r.below(14)
        rd = r.range(5, 9)
        words = [enc_addsub_sh(1, 1, 1, 0, 2, 0, 1, 31),  # cmp x1, x2
                 enc_bcond(2, cond),                       # b.cond +8
                 add_imm(rd, rd, 1),                       # add xd, xd, #1 (skipped when taken)
                 add_imm(rd, rd, 2)]                       # add xd, xd, #2
        if r.below(2):
            words[0] = enc_addsub_sh(0, 1, 1, 0, 2, 0, 1, 31)  # cmp w1, w2
        if r.below(4) == 0:
            words[1] = enc_cb(1, r.below(2), 2, 1)  # cbz / cbnz x1, +8
        out.add(r, 'branch_seq', f'cmp x1, x2; b.{COND_NAMES[cond]} +8; add x{rd}, x{rd}, #1; add x{rd}, x{rd}, #2', words,
                {'x1': a, 'x2': b, f'x{rd}': value(r)})


# ---- Loads and stores ----

# (size, opc) of the integer loads and stores, with their name and access size in bytes
INT_LDST = [(0, 0, 'STRB', 1), (0, 1, 'LDRB', 1), (0, 2, 'LDRSB (64)', 1), (0, 3, 'LDRSB (32)', 1),
            (1, 0, 'STRH', 2), (1, 1, 'LDRH', 2), (1, 2, 'LDRSH (64)', 2), (1, 3, 'LDRSH (32)', 2),
            (2, 0, 'STR (32)', 4), (2, 1, 'LDR (32)', 4), (2, 2, 'LDRSW', 4), (3, 0, 'STR (64)', 8), (3, 1, 'LDR (64)', 8)]


def base_regs(r, rn, ea, offset, avoid=()):
    regs = {}
    base = (ea - offset) & M64
    if rn == 31:
        regs['sp'] = base
    else:
        regs[f'x{rn}'] = base
    return regs


def pick_base(r, avoid=()):
    while True:
        rn = r.below(32)
        if rn not in avoid:
            return rn


def gen_loadstore(out, r):
    for i in range(1000):
        size, opc, name, nbytes = r.choice(INT_LDST)
        imm12 = r.below(4096) if i % 4 else r.choice([0, 1, 4095])
        offset = imm12 << size
        rt = r.below(32)
        rn = pick_base(r)
        if offset > IMAGE_SIZE - nbytes:
            imm12 = r.below((IMAGE_SIZE - nbytes) >> size)
            offset = imm12 << size
        ea = r.range(GUEST_BASE + offset, GUEST_BASE + IMAGE_SIZE - nbytes)
        if rn == 31:
            ea = ((ea - offset) & ~15) + offset
            if ea < GUEST_BASE + offset:
                ea += 16
        regs = base_regs(r, rn, ea, offset)
        if rt != rn:
            setreg(regs, rt, value(r))
        out.add(r, 'ldst_uimm', f'{name} (unsigned offset) Rt={rt} [{rname(rn, 1, True)}, #{offset}] ea={ea:#x}',
                [enc_ldst_uimm(size, opc, imm12, rn, rt)], regs)

    for kind, cls, count in ((0, 'ldst_unscaled', 500), (2, 'ldst_unpriv', 300), (1, 'ldst_post', 450), (3, 'ldst_pre', 450)):
        for i in range(count):
            size, opc, name, nbytes = r.choice(INT_LDST)
            imm9 = sext(r.next(), 9) if i % 4 else r.choice([0, 1, -1, 255, -256])
            rt = r.below(32)
            rn = pick_base(r)
            # unscaled and pre-index access base + imm9, post-index accesses base
            off = 0 if kind == 1 else imm9
            lo = max(GUEST_BASE, GUEST_BASE - imm9)
            hi = min(GUEST_BASE + IMAGE_SIZE - nbytes, GUEST_BASE + IMAGE_SIZE - nbytes - imm9)
            ea = r.range(lo + 16, hi - 16)
            if rn == 31:
                ea = ((ea - off) & ~15) + off
            regs = base_regs(r, rn, ea, off)
            if rt != rn:
                setreg(regs, rt, value(r))
            label = ['LDUR/STUR', 'post-index', 'unprivileged', 'pre-index'][kind]
            out.add(r, cls, f'{name} ({label}) Rt={rt} Rn={rn} imm9={imm9} ea={ea:#x}', [enc_ldst_imm9(size, opc, imm9, kind, rn, rt)], regs)

    for i in range(800):
        size, opc, name, nbytes = r.choice(INT_LDST)
        option = r.choice([2, 3, 6, 7]) if i % 20 else r.choice([0, 1, 4, 5])
        s = r.below(2)
        rt = r.below(32)
        rn = pick_base(r)
        rm = pick_base(r, avoid=(rn, 31)) if r.below(8) else 31
        shift = size if s else 0
        if option == 2:      # uxtw
            idx = r.below(0x2000 >> shift)
            mval = idx | (r.next() & 0xFFFFFFFF00000000)
        elif option == 6:    # sxtw
            idx = sext(r.next(), 32) % (0x2000 >> shift) - (0x1000 >> shift)
            mval = (idx & M32) | (r.next() & 0xFFFFFFFF00000000)
        else:                # lsl (uxtx), sxtx
            idx = r.range(-(0x1000 >> shift), 0x1000 >> shift)
            mval = idx & M64
        if rm == 31:
            idx, mval = 0, 0
        offset = idx << shift
        ea = r.range(GUEST_BASE + 0x2000, GUEST_BASE + IMAGE_SIZE - 0x2000 - nbytes)
        if rn == 31:
            ea = ((ea - offset) & ~15) + offset
        regs = base_regs(r, rn, ea, offset)
        if rm != 31:
            regs[f'x{rm}'] = mval
        if rt not in (rn, rm):
            setreg(regs, rt, value(r))
        ext = ['uxtb?', 'uxth?', 'uxtw', 'lsl', 'sxtb?', 'sxth?', 'sxtw', 'sxtx'][option]
        out.add(r, 'ldst_reg', f'{name} (register) Rt={rt} [{rname(rn, 1, True)}, {rname(rm, 1)}, {ext} #{shift}] ea={ea:#x}',
                [enc_ldst_reg(size, opc, rm, option, s, rn, rt)], regs)

    pairs = [(0, 'W', 4), (1, 'SW', 4), (2, 'X', 8)]
    for i in range(900):
        opc, kind, nbytes = r.choice(pairs)
        mode = r.choice([0, 1, 2, 3])
        l = 1 if kind == 'SW' else r.below(2)
        if mode == 0 and kind == 'SW':
            mode = 2
        imm7 = sext(r.next(), 7) if i % 4 else r.choice([0, 1, -1, 63, -64])
        rt, rt2 = r.below(32), r.below(32)
        rn = pick_base(r)
        off = imm7 * nbytes
        acc = 0 if mode == 1 else off
        lo = GUEST_BASE + 0x400
        ea = r.range(lo, GUEST_BASE + IMAGE_SIZE - 0x400)
        if rn == 31:
            ea = ((ea - acc) & ~15) + acc
        regs = base_regs(r, rn, ea, acc)
        for t in (rt, rt2):
            if t != rn:
                setreg(regs, t, value(r))
        name = ('LD' if l else 'ST') + ('NP' if mode == 0 else 'P') + (kind if kind == 'SW' else '')
        mode_name = ['no-allocate', 'post-index', 'signed offset', 'pre-index'][mode]
        out.add(r, 'ldst_pair', f'{name} {kind} ({mode_name}) Rt={rt} Rt2={rt2} Rn={rn} imm7={imm7} ea={ea:#x}',
                [enc_pair(opc, mode, l, imm7, rt2, rn, rt)], regs)

    for i in range(300):
        opc = r.choice([0, 1, 2, 3])
        rt = r.below(32)
        # pc-relative: the literal lies in the image (the code is in its middle)
        imm19 = r.range(-(CODE_OFFSET // 4) + 2, (IMAGE_SIZE - CODE_OFFSET) // 4 - 4)
        name = ['LDR (literal, 32)', 'LDR (literal, 64)', 'LDRSW (literal)', 'PRFM (literal)'][opc]
        regs = {}
        setreg(regs, rt, value(r))
        out.add(r, 'ldr_lit', f'{name} Rt={rt} {imm19 * 4:+#x}', [enc_ldr_lit(opc, imm19, rt)], regs)

    for i in range(200):
        rt = r.below(32)
        rn = pick_base(r)
        regs = {}
        setreg(regs, rn, r.next(), sp=True)  # a prefetch never faults: any address
        kind = r.below(3)
        if kind == 0:
            word = enc_ldst_uimm(3, 2, r.below(4096), rn, rt)
        elif kind == 1:
            word = enc_ldst_imm9(3, 2, sext(r.next(), 9), 0, rn, rt)
        else:
            rm = pick_base(r, avoid=(rn,))
            setreg(regs, rm, r.next())
            word = enc_ldst_reg(3, 2, rm, r.choice([2, 3, 6, 7]), r.below(2), rn, rt)
        out.add(r, 'prfm', f'PRFM kind={kind} op={rt} Rn={rn}', [word], regs)


# ---- Exclusives and ordered accesses ----

def gen_exclusive(out, r):
    sizes = [(0, 'B', 1), (1, 'H', 2), (2, 'W', 4), (3, 'X', 8)]
    for i in range(500):
        size, sname, nbytes = r.choice(sizes)
        acq, rel = r.below(2), r.below(2)
        rn = r.below(31)
        rt = r.choice([x for x in range(31) if x != rn])
        rv = r.choice([x for x in range(31) if x not in (rn,)])
        rs = r.choice([x for x in range(31) if x not in (rn, rt, rv)])
        ea = data_address(r, nbytes, align=nbytes)
        ldxr = enc_excl(size, 0, 1, 0, 31, acq, 31, rn, rt)
        op = r.below(4)
        if op == 0:
            mid = add_imm(rv, rt, r.below(4096))
            mtext = f'add x{rv}, x{rt}, #imm'
        elif op == 1:
            mid = enc_logic_sh(1, 2, 0, 0, rt, 0, rt, rv) if rv != rt else NOP  # eor xv, xt, xt
            mtext = f'eor x{rv}, x{rt}, x{rt}'
        elif op == 2:
            mid = enc_addsub_sh(1, 1, 0, 0, rt, 0, rt, rv)  # sub
            mtext = f'sub x{rv}, x{rt}, x{rt}'
        else:
            mid = enc_movewide(1, 2, 0, r.below(65536), rv)
            mtext = f'movz x{rv}, #imm'
        stxr = enc_excl(size, 0, 0, 0, rs, rel, 31, rn, rv)
        regs = {f'x{rn}': ea}
        out.add(r, 'excl_seq', f'LD{"A" if acq else ""}XR{sname} x{rt}, [x{rn}]; {mtext}; ST{"L" if rel else ""}XR{sname} w{rs}, x{rv}, [x{rn}]',
                [ldxr, mid, stxr], regs)

    for i in range(300):
        size, sname, nbytes = r.choice(sizes)
        rn = r.below(31)
        rt = r.choice([x for x in range(31) if x != rn])
        rs = r.choice([x for x in range(31) if x not in (rn, rt)])
        ea = data_address(r, nbytes, align=nbytes)
        case = r.below(4)
        regs = {f'x{rn}': ea}
        ldxr = enc_excl(size, 0, 1, 0, 31, r.below(2), 31, rn, rt)
        stxr = enc_excl(size, 0, 0, 0, rs, r.below(2), 31, rn, rt)
        if case == 0:
            words, text = [stxr], 'stxr with no exclusive load: fails'
        elif case == 1:
            words, text = [ldxr, 0xD503305F, stxr], 'ldxr; clrex; stxr: fails'
        elif case == 2:
            ro = r.choice([x for x in range(31) if x not in (rn, rt, rs)])
            other = data_address(r, nbytes, align=nbytes)
            regs[f'x{ro}'] = other if other != ea else ea + nbytes * 2 if ea + nbytes * 2 < GUEST_BASE + IMAGE_SIZE else ea - nbytes * 2
            words, text = [ldxr, enc_excl(size, 0, 0, 0, rs, 0, 31, ro, rt)], 'ldxr at one address, stxr at another: fails'
        else:
            words, text = [ldxr, stxr, stxr], 'ldxr; stxr; stxr: the second fails'
        out.add(r, 'excl_fail', f'{sname}: {text}', words, regs)

    for i in range(300):
        sz = r.below(2)  # 0: two W, 1: two X
        nbytes = 8 << sz
        acq, rel = r.below(2), r.below(2)
        rn = r.below(31)
        rt, rt2 = [x for x in r_sample(r, 31, 2, avoid=(rn,))]
        rv, rv2 = [x for x in r_sample(r, 31, 2, avoid=(rn,))]
        rs = r.choice([x for x in range(31) if x not in (rn, rv, rv2)])
        ea = data_address(r, nbytes, align=nbytes)
        ldxp = enc_excl(2 | sz, 0, 1, 1, 31, acq, rt2, rn, rt)
        stxp = enc_excl(2 | sz, 0, 0, 1, rs, rel, rv2, rn, rv)
        regs = {f'x{rn}': ea}
        for v in (rv, rv2):
            regs.setdefault(f'x{v}', value(r))
        words = [ldxp, stxp] if r.below(4) else [stxp]
        out.add(r, 'excl_pair', f'LD{"A" if acq else ""}XP {"X" if sz else "W"} x{rt}, x{rt2}, [x{rn}]; ST{"L" if rel else ""}XP w{rs}, x{rv}, x{rv2}',
                words, regs)

    for i in range(500):
        size, sname, nbytes = r.choice(sizes)
        l = r.below(2)
        o0 = 1 if r.below(5) else 0  # o0 = 0: the ARMv8.1 LORegion forms (ldlar / stllr)
        rn = r.below(32)
        rt = r.below(32)
        ea = data_address(r, nbytes, align=nbytes)
        if rn == 31:
            ea &= ~15
        regs = base_regs(r, rn, ea, 0)
        if rt != rn:
            setreg(regs, rt, value(r))
        name = ('LDAR' if o0 else 'LDLAR') if l else ('STLR' if o0 else 'STLLR')
        out.add(r, 'ordered', f'{name}{sname} Rt={rt} [{rname(rn, 1, True)}] ea={ea:#x}', [enc_excl(size, 1, l, 0, 31, o0, 31, rn, rt)], regs)

    for i in range(200):
        size, sname, nbytes = r.choice(sizes)
        rn = r.below(31)
        rt = r.choice([x for x in range(31) if x != rn])
        ea = data_address(r, nbytes, align=nbytes)
        out.add(r, 'excl_single', f'LDXR{sname} x{rt}, [x{rn}] alone', [enc_excl(size, 0, 1, 0, 31, r.below(2), 31, rn, rt)], {f'x{rn}': ea})


def r_sample(r, n, k, avoid=()):
    pool = [x for x in range(n) if x not in avoid]
    got = []
    while len(got) < k:
        x = r.choice(pool)
        if x not in got:
            got.append(x)
    return got


# ---- System ----

def gen_system(out, r):
    names = list(SYSREGS)
    for i in range(500):
        name = r.choice(names)
        rt = r.below(32)
        regs = {}
        if r.below(2) and SYSREGS[name][5]:
            v = value(r)
            if name == 'fpsr':
                v &= 0x0800009F | r.next()  # nxaot_vec starts from FPSR bits both executors keep; see README.md
            setreg(regs, rt, v)
            regs['nzcv'] = r.below(16) << 28
            out.add(r, 'sysreg', f'MSR {name}, {rname(rt, 1)}', [msr(name, rt)], regs)
        else:
            if name == 'nzcv':
                regs['nzcv'] = r.below(16) << 28
            if name in ('fpcr',):
                regs['fpcr'] = r.choice([0, 0x03C00000, 0x00400000, 0x01000000, 0x02000000, 0x04000000, 0x00C00000])
            if name == 'fpsr':
                regs['fpsr'] = r.choice([0, 1, 0x9F, 0x08000000, 0x0800009F, 0x10])
            if name in ('cntpct_el0', 'cntvct_el0'):
                regs['cnt'] = r.next() >> 8
            out.add(r, 'sysreg', f'MRS {rname(rt, 1)}, {name}', [mrs(name, rt)], regs)

    for i in range(300):
        name = r.choice(['nzcv', 'fpcr', 'fpsr', 'tpidr_el0'])
        rt, rt2 = r.below(31), r.below(31)
        v = value(r)
        if name == 'fpsr':
            v &= 0x0800009F | r.next()
        words = [msr(name, rt), mrs(name, rt2)]
        regs = {f'x{rt}': v}
        if name == 'nzcv':
            words.append(enc_csel(1, 0, 1, 31, r.below(16), 31, r.range(10, 20)))  # cset-like: reads the flags just written
        out.add(r, 'msr_mrs', f'MSR {name}, x{rt}; MRS x{rt2}, {name}', words, regs)

    hints = [(NOP, 'NOP'), (YIELD, 'YIELD'), (WFE, 'WFE'), (WFI, 'WFI'), (SEV, 'SEV'), (SEVL, 'SEVL')]
    for i in range(300):
        if i % 4 == 3:
            imm7 = r.below(128)
            word, name = 0xD503201F | (imm7 << 5), f'HINT #{imm7}'
        else:
            word, name = r.choice(hints)
        rd = r.below(31)
        out.add(r, 'hint', f'{name}; add x{rd}, x{rd}, #1', [word, add_imm(rd, rd, 1)], {f'x{rd}': value(r)})

    for i in range(300):
        kind = r.below(4)
        crm = r.below(16)
        word = [0xD503305F, 0xD503309F, 0xD50330BF, 0xD50330DF][kind] | (crm << 8)
        name = ['CLREX', 'DSB', 'DMB', 'ISB'][kind]
        rd = r.below(31)
        out.add(r, 'barrier', f'{name} #{crm}; add x{rd}, x{rd}, #1', [word, add_imm(rd, rd, 1)], {f'x{rd}': value(r)})

    dcs = [(0xD50B7420, 'DC ZVA'), (0xD50B7E20, 'DC CIVAC'), (0xD50B7A20, 'DC CVAC'), (0xD50B7B20, 'DC CVAU'), (0xD5087620, 'DC IVAC'),
           (0xD50B7520, 'IC IVAU'), (0xD508751F, 'IC IALLU'), (0xD508711F, 'IC IALLUIS')]
    for i in range(400):
        base, name = dcs[0] if i % 2 == 0 else r.choice(dcs)
        rt = r.below(31)
        word = base | rt if base & 31 == 0 else base
        ea = r.range(GUEST_BASE, GUEST_BASE + IMAGE_SIZE - 1)  # dc zva zeroes the aligned 64 bytes around it: always inside the image
        out.add(r, 'dc_ic', f'{name} x{rt} ea={ea:#x}', [word], {f'x{rt}': ea})

    for i in range(300):
        imm16 = r.below(65536) if i % 4 else r.choice([0, 1, 0xFFFF, 0x7F])
        kind = r.below(2)
        word = (0xD4000001 if kind == 0 else 0xD4200000) | (imm16 << 5)
        rd = r.below(31)
        out.add(r, 'exception', f'add x{rd}, x{rd}, #1; {"SVC" if kind == 0 else "BRK"} #{imm16:#x}; add x{rd}, x{rd}, #2',
                [add_imm(rd, rd, 1), word, add_imm(rd, rd, 2)], {f'x{rd}': value(r)})


# ---- Words dynarmic does not translate, and FP / SIMD (refused by the lifter today) ----

def gen_undefined(out, r):
    for i in range(400):
        kind = r.below(6)
        if kind == 0:
            word, name = r.below(65536), 'UDF'
        elif kind == 1:
            word, name = (r.next() & 0x01FFFFFF) | (r.choice([0b0001, 0b0011]) << 25) | (r.below(8) << 29), 'unallocated op0'
        elif kind == 2:
            word, name = 0x38200000 | (r.below(4) << 30) | (r.below(4) << 22) | (r.below(31) << 16) | (r.below(16) << 12) | (r.below(31) << 5) | r.below(31), 'LSE atomic'
        elif kind == 3:
            word, name = 0x08A07C00 | (r.below(4) << 30) | (r.below(2) << 22) | (r.below(31) << 16) | (r.below(2) << 15) | (r.below(31) << 5) | r.below(31), 'CAS'
        elif kind == 4:
            word, name = (0xD4000002 | (r.below(65536) << 5)) if r.below(2) else (0xD4000003 | (r.below(65536) << 5)), 'HVC / SMC'
        else:
            word, name = 0xD500401F | (r.below(8) << 16) | (r.below(16) << 8) | (r.below(8) << 5), 'MSR (immediate)'
        out.add(r, 'undefined', f'{name} {word:#010x}', [word])


def gen_fp_simd(out, r):
    def reg3(base):
        return base | (r.below(32) << 16) | (r.below(32) << 5) | r.below(32)

    kinds = [
        (lambda: reg3(0x1E202800 | (r.below(2) << 22)), 'FADD (scalar)'),
        (lambda: reg3(0x1E200800 | (r.below(2) << 22)), 'FMUL (scalar)'),
        (lambda: 0x1E270000 | (r.below(32) << 5) | r.below(32), 'FMOV (general to S)'),
        (lambda: 0x1E380000 | (r.below(32) << 5) | r.below(32), 'FCVTZS (scalar, integer)'),
        (lambda: 0x1E220000 | (r.below(32) << 5) | r.below(32), 'SCVTF (scalar, integer)'),
        (lambda: 0x3DC00000 | (r.below(31) << 5) | r.below(32), 'LDR Q'),
        (lambda: 0x3D800000 | (r.below(31) << 5) | r.below(32), 'STR Q'),
        (lambda: reg3(0x4EA08400), 'ADD (vector, 4S)'),
        (lambda: 0x4E284800 | (r.below(32) << 5) | r.below(32), 'AESE'),
        (lambda: reg3(0x5E000000), 'SHA1C'),
        (lambda: reg3(0x0EE0E000), 'PMULL (1Q)'),
        (lambda: 0x6F00E400 | r.below(32), 'MOVI (2D, #0)'),
        (lambda: reg3(0x1E202000) & ~31, 'FCMP'),
    ]
    for i in range(150):
        fn, name = kinds[i % len(kinds)]
        rn = r.below(31)
        word = fn()
        regs = {}
        if name in ('LDR Q', 'STR Q'):
            rn = (word >> 5) & 31
            regs[f'x{rn}'] = data_address(r, 16, align=16)
        out.add(r, 'fp_simd', f'{name} {word:#010x}', [word], regs)


# ---- Sequences: flags set by one instruction and read by later ones, and straight-line mixes ----

def gen_sequences(out, r):
    for i in range(600):
        a, b = value(r), value(r)
        if r.below(3) == 0:
            b = (a + r.choice([0, 1, -1])) & M64
        sf = r.below(2)
        setter = r.below(4)
        if setter == 0:
            first, ftext = enc_addsub_sh(sf, 1, 1, 0, 2, 0, 1, 31), 'cmp x1, x2'
        elif setter == 1:
            first, ftext = enc_addsub_sh(sf, 0, 1, 0, 2, 0, 1, 3), 'adds x3, x1, x2'
        elif setter == 2:
            first, ftext = enc_logic_sh(sf, 3, 0, 0, 2, 0, 1, 31), 'tst x1, x2'
        else:
            first, ftext = enc_addsub_imm(sf, 1, 1, 0, r.below(4096), 1, 31), 'cmp x1, #imm'
        consumers = []
        for k in range(r.range(1, 3)):
            c = r.below(5)
            cond = r.below(16)
            rd = r.range(4, 12)
            if c == 0:
                consumers.append((enc_csel(1, 0, 1, 31, cond ^ 1, 31, rd), f'cset x{rd}, {COND_NAMES[cond]}'))
            elif c == 1:
                consumers.append((enc_csel(sf, r.below(2), r.below(2), 2, cond, 1, rd), f'csel-family x{rd}, {COND_NAMES[cond]}'))
            elif c == 2:
                consumers.append((enc_adc(sf, r.below(2), r.below(2), 2, 1, rd), f'adc/sbc x{rd}'))
            elif c == 3:
                consumers.append((enc_ccmp(sf, r.below(2), r.below(2), r.below(32) if r.below(2) else 2, cond, 1, r.below(16)), f'ccmp {COND_NAMES[cond]}'))
            else:
                consumers.append((mrs('nzcv', rd), f'mrs x{rd}, nzcv'))
        words = [first] + [w for w, _ in consumers]
        out.add(r, 'flag_seq', '; '.join([ftext] + [t for _, t in consumers]), words, {'x1': a, 'x2': b, 'x3': value(r), 'nzcv': r.below(16) << 28})

    simple = [
        lambda rd, rn, rm: enc_addsub_sh(r.below(2), r.below(2), r.below(2), r.below(3), rm, r.below(32), rn, rd),
        lambda rd, rn, rm: enc_logic_sh(r.below(2), r.below(4), r.below(4), r.below(2), rm, r.below(32), rn, rd),
        lambda rd, rn, rm: enc_dp2(1, r.choice([2, 3, 8, 9, 10, 11]), rm, rn, rd),
        lambda rd, rn, rm: enc_dp3(1, 0, r.below(2), rm, r.below(32), rn, rd),
        lambda rd, rn, rm: enc_bitfield(1, r.choice([0, 1, 2]), 1, r.below(64), r.below(64), rn, rd),
        lambda rd, rn, rm: enc_csel(1, r.below(2), r.below(2), rm, r.below(16), rn, rd),
        lambda rd, rn, rm: enc_movewide(1, r.choice([0, 2, 3]), r.below(4), r.below(65536), rd),
        lambda rd, rn, rm: enc_dp1(1, r.choice([0, 1, 2, 3, 4, 5]), rn, rd),
        lambda rd, rn, rm: enc_adc(1, r.below(2), r.below(2), rm, rn, rd),
        lambda rd, rn, rm: enc_extr(1, 1, rm, r.below(64), rn, rd),
    ]
    for i in range(800):
        n = r.range(2, 6)
        words = []
        regs = {'nzcv': r.below(16) << 28}
        for k in range(n):
            rd, rn, rm = r.below(12), r.below(12), r.below(12)
            words.append(r.choice(simple)(rd, rn, rm))
        for k in range(12):
            regs[f'x{k}'] = value(r)
        out.add(r, 'mixed_seq', f'{n} data-processing instructions on x0..x11', words, regs)


FILES = [
    ('dp_imm', 'data processing, immediate (ADR, add/sub, logical, move wide, bitfield, extract)', gen_dp_imm),
    ('dp_reg', 'data processing, register (logical, add/sub, carry, conditional compare and select, 1-, 2- and 3-source, CRC32)', gen_dp_reg),
    ('branch', 'branches (immediate, conditional, compare and test, register) and branches inside a sequence', gen_branch),
    ('loadstore', 'integer loads and stores (all addressing forms, pairs, literal, prefetch, unprivileged)', gen_loadstore),
    ('exclusive', 'exclusive windows, failing store-exclusives, pairs, load-acquire and store-release', gen_exclusive),
    ('system', 'system registers, hints, barriers, data and instruction cache maintenance, svc and brk', gen_system),
    ('sequence', 'flags set and read across instructions, and straight-line mixes', gen_sequences),
    ('undefined', 'words dynarmic does not translate (udf, unallocated groups, LSE, hvc / smc, msr immediate)', gen_undefined),
    ('fp_simd', 'floating point and SIMD: refused by the lifter today (counted as unsupported)', gen_fp_simd),
]


def build():
    files = {}
    for k, (name, title, fn) in enumerate(FILES):
        out = Out(name, title)
        fn(out, Rng(SEED + k))
        files[name + '.tsv'] = out
    return files


# ---- The census test's synthetic blob ----

def census_blob():
    """Words of a code blob and the counts `nxaot census` must report for them (keys of src/analysis/census.cpp)."""
    words = []
    expect = {}

    def emit(word, *keys):
        words.append(word & M32)
        for k in keys:
            expect[k] = expect.get(k, 0) + 1

    DPI, DPR, LS, BS, FP = 'class.dp_imm', 'class.dp_reg', 'class.loadstore', 'class.branch_sys', 'class.fp_simd'
    D, U = 'decoded', 'undecoded'
    r = Rng(SEED ^ 0xCE)
    for i in range(40):
        emit(add_imm(r.below(31), r.below(31), r.below(4096)), DPI, D)
    for i in range(30):
        emit(enc_logic_sh(1, 1, 0, 0, r.below(31), r.below(64), r.below(31), r.below(31)), DPR, D)
    for i in range(20):
        emit(enc_ldst_uimm(3, 1, r.below(4096), r.below(31), r.below(31)), LS, D)
    for i in range(10):
        emit(enc_ldst_uimm(2, 0, r.below(4096), r.below(31), r.below(31)), LS, D)
    for i in range(12):
        emit(0x1E222820 | (r.below(32) << 5) | r.below(32), FP, D, 'fp_simd.dp')
    for i in range(6):
        emit(0x3DC00000 | (r.below(31) << 5) | r.below(32), LS, D, 'fp_simd.loadstore')
    for i in range(3):
        emit(0x4E284800 | (r.below(32) << 5) | r.below(32), FP, D, 'fp_simd.dp', 'crypto.aes')
    for i in range(2):
        emit(0x4E286800 | (r.below(32) << 5) | r.below(32), FP, D, 'fp_simd.dp', 'crypto.aes')
    for i in range(2):
        emit(0x5E000000 | (r.below(32) << 16) | (r.below(32) << 5) | r.below(32), FP, D, 'fp_simd.dp', 'crypto.sha1')
    for i in range(2):
        emit(0x5E004000 | (r.below(32) << 16) | (r.below(32) << 5) | r.below(32), FP, D, 'fp_simd.dp', 'crypto.sha256')
    emit(0x0EE0E000 | (1 << 16) | (2 << 5) | 3, FP, D, 'fp_simd.dp', 'crypto.pmull64')
    emit(0x0E20E000 | (1 << 16) | (2 << 5) | 3, FP, D, 'fp_simd.dp')  # PMULL 8B: base SIMD, not the crypto form
    for sz in (0, 2, 0, 2):
        emit(enc_dp2(0, 0b010000 | sz, 2, 1, 0), DPR, D, 'crc32')
    for i in range(3):
        emit(enc_dp2(1, 0b010111, 2, 1, 0), DPR, D, 'crc32c')
    # LSE, which dynarmic does not decode
    for i in range(2):
        emit(0x08A07C00 | (3 << 30) | (1 << 22) | (2 << 16) | (1 << 5) | 3, LS, U, 'lse', 'lse.cas')
    emit(0x08207C00 | (1 << 30) | (2 << 16) | (1 << 5) | 4, LS, U, 'lse', 'lse.casp')
    for i in range(3):
        emit(0x38200000 | (3 << 30) | (1 << 23) | (2 << 16) | (0b0000 << 12) | (1 << 5) | 3, LS, U, 'lse', 'lse.ldop')
    for i in range(2):
        emit(0x38200000 | (2 << 30) | (2 << 16) | (0b1000 << 12) | (1 << 5) | 3, LS, U, 'lse', 'lse.swp')
    emit(0x38BFC000 | (3 << 30) | (1 << 5) | 3, LS, U, 'rcpc.ldapr')
    # System registers
    for name, n in (('cntfrq_el0', 3), ('fpsr', 2), ('cntvct_el0', 1), ('tpidr_el0', 4), ('tpidrro_el0', 1), ('nzcv', 1), ('cntpct_el0', 2)):
        for i in range(n):
            emit(mrs(name, r.below(31)), BS, D, 'mrs.' + name)
    for name in ('fpcr', 'tpidr_el0', 'nzcv'):
        emit(msr(name, r.below(31)), BS, D, 'msr.' + name)
    emit(0xD50340DF | (2 << 8), BS, U, 'msr.pstate_imm')  # msr daifset, #2
    for i in range(4):
        emit(0xD50B7420 | r.below(31), BS, D, 'dc.zva')
    emit(0xD50B7E20 | 5, BS, D, 'dc.other')
    emit(0xD50B7520 | 6, BS, D, 'ic')
    for word, key, n in ((WFE, 'hint.wfe', 2), (YIELD, 'hint.yield', 5), (WFI, 'hint.wfi', 1), (SEV, 'hint.sev', 1), (SEVL, 'hint.sev', 1),
                         (NOP, 'hint.nop', 3), (0xD503233F, 'hint.other', 2)):
        for i in range(n):
            emit(word, BS, D, key)
    for word, key, n in ((0xD5033BBF, 'barrier.dmb', 3), (0xD5033B9F, 'barrier.dsb', 1), (0xD5033FDF, 'barrier.isb', 1), (0xD503305F, 'clrex', 1)):
        for i in range(n):
            emit(word, BS, D, key)
    # Branches
    for i in range(2):
        emit(enc_b(0, 4), BS, D, 'branch.b')
    for i in range(4):
        emit(enc_b(1, -40), BS, D, 'branch.bl')
    for i in range(3):
        emit(enc_bcond(8, 1), BS, D, 'branch.cond')
    for i in range(2):
        emit(enc_cb(1, 0, 8, 1), BS, D, 'branch.cbz')
    emit(enc_tb(1, 40, 8, 2), BS, D, 'branch.tbz')
    for i in range(3):
        emit(enc_brreg('br', 16 + i), BS, D, 'branch.br')
    for i in range(2):
        emit(enc_brreg('blr', 8), BS, D, 'branch.blr')
    for i in range(5):
        emit(enc_brreg('ret', 30), BS, D, 'branch.ret')
    for i in range(2):
        emit(0xD4000001 | (r.below(65536) << 5), BS, D, 'svc')
    emit(0xD4200000 | (0x3E8 << 5), BS, D, 'brk')
    emit(0x0000BEEF, 'class.reserved', U, 'udf')
    emit(0xD4000002 | (1 << 5), BS, U, 'hvc_smc')

    # Exclusive windows. Equivalent ones (ldaxr w8, [x0]; add w9, w8, #1; stlxr w10, w9, [x0]; cbnz w10, retry)
    for i in range(3):
        emit(enc_excl(2, 0, 1, 0, 31, 1, 31, 0, 8), LS, D, 'excl.load')
        emit(enc_addsub_imm(0, 0, 0, 0, 1, 8, 9), DPI, D)
        emit(enc_excl(2, 0, 0, 0, 10, 1, 31, 0, 9), LS, D, 'excl.store')
        emit(enc_cb(0, 1, -3, 10), BS, D, 'branch.cbz')
        expect['excl.windows'] = expect.get('excl.windows', 0) + 1
    # A compare-and-swap loop with a conditional branch inside: ldaxr x8, [x0]; cmp x8, x1; b.ne out; stlxr w9, x2, [x0]
    emit(enc_excl(3, 0, 1, 0, 31, 1, 31, 0, 8), LS, D, 'excl.load')
    emit(enc_addsub_sh(1, 1, 1, 0, 1, 0, 8, 31), DPR, D)
    emit(enc_bcond(3, 1), BS, D, 'branch.cond')
    emit(enc_excl(3, 0, 0, 0, 9, 1, 31, 0, 2), LS, D, 'excl.store')
    emit(0xD503305F, BS, D, 'clrex')
    expect['excl.windows'] = expect.get('excl.windows', 0) + 1
    # ABA-sensitive pops: ldaxr x1, [x0]; ldr x2, [x1, #8]; stlxr w3, x2, [x0]
    for i in range(2):
        emit(enc_excl(3, 0, 1, 0, 31, 1, 31, 0, 1), LS, D, 'excl.load')
        emit(enc_ldst_uimm(3, 1, 1, 1, 2), LS, D)
        emit(enc_excl(3, 0, 0, 0, 3, 1, 31, 0, 2), LS, D, 'excl.store')
        expect['excl.windows'] = expect.get('excl.windows', 0) + 1
        expect['excl.windows.aba_sensitive'] = expect.get('excl.windows.aba_sensitive', 0) + 1
    # Another load inside, not through the loaded value: ldaxr x1, [x0]; ldr x5, [x6]; add x1, x1, x5; stlxr w3, x1, [x0]
    emit(enc_excl(3, 0, 1, 0, 31, 1, 31, 0, 1), LS, D, 'excl.load')
    emit(enc_ldst_uimm(3, 1, 0, 6, 5), LS, D)
    emit(enc_addsub_sh(1, 0, 0, 0, 5, 0, 1, 1), DPR, D)
    emit(enc_excl(3, 0, 0, 0, 3, 1, 31, 0, 1), LS, D, 'excl.store')
    expect['excl.windows'] = expect.get('excl.windows', 0) + 1
    expect['excl.windows.other_load'] = expect.get('excl.windows.other_load', 0) + 1
    # Unpaired: ldxr x1, [x0]; ret
    emit(enc_excl(3, 0, 1, 0, 31, 0, 31, 0, 1), LS, D, 'excl.load')
    emit(enc_brreg('ret', 30), BS, D, 'branch.ret')
    expect['excl.unpaired'] = expect.get('excl.unpaired', 0) + 1

    # SP written from memory: ldr x9, [x0]; mov sp, x9 (twice) and ldr x9, [sp, #8]; add x9, x9, #16; mov sp, x9; each block ends with a b
    for i in range(2):
        emit(enc_ldst_uimm(3, 1, 0, 0, 9), LS, D)
        emit(add_imm(31, 9, 0), DPI, D)
        emit(enc_b(0, 1), BS, D, 'branch.b')
        expect['sp.from_memory'] = expect.get('sp.from_memory', 0) + 1
    emit(enc_ldst_uimm(3, 1, 1, 31, 9), LS, D)
    emit(add_imm(9, 9, 16), DPI, D)
    emit(add_imm(31, 9, 0), DPI, D)
    emit(enc_b(0, 1), BS, D, 'branch.b')
    expect['sp.from_memory'] = expect.get('sp.from_memory', 0) + 1
    # Not from memory: add sp, sp, #16 twice; movz x9, #5; mov sp, x9
    emit(add_imm(31, 31, 16), DPI, D)
    emit(add_imm(31, 31, 16), DPI, D)
    emit(enc_movewide(1, 2, 0, 5, 9), DPI, D)
    emit(add_imm(31, 9, 0), DPI, D)

    expect['words'] = len(words)
    for k in ('lse', 'mrs.fpsr', 'sp.from_memory', 'excl.windows.aba_sensitive', 'mrs.cntfrq_el0'):
        expect.setdefault(k, 0)
    expect['answer.lse_present'] = int(expect['lse'] > 0)
    expect['answer.fpsr_read'] = int(expect['mrs.fpsr'] > 0)
    expect['answer.sp_from_memory'] = int(expect['sp.from_memory'] > 0)
    expect['answer.aba_sensitive_windows'] = int(expect['excl.windows.aba_sensitive'] > 0)
    expect['answer.cntfrq_read'] = int(expect['mrs.cntfrq_el0'] > 0)
    return words, expect


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--check', action='store_true', help='compare the committed vector files with what this script writes; exit 1 when they differ')
    ap.add_argument('--out', default=VECTOR_DIR, help='folder for the .tsv files (default: tests/vectors beside this script)')
    ap.add_argument('--census-blob', help='write the census test blob (raw little-endian words) to this file')
    ap.add_argument('--census-expect', help='with --census-blob: write the expected counts (key<TAB>count) to this file')
    args = ap.parse_args()

    if args.census_blob:
        words, expect = census_blob()
        with open(args.census_blob, 'wb') as f:
            f.write(b''.join(w.to_bytes(4, 'little') for w in words))
        if args.census_expect:
            with open(args.census_expect, 'w', newline='\n') as f:
                for k in sorted(expect):
                    f.write(f'{k}\t{expect[k]}\n')
        print(f'gen_vectors.py: census blob of {len(words)} words, {len(expect)} expected counts')
        return 0

    files = build()
    total = sum(len(o.lines) for o in files.values())
    if args.check:
        stale = []
        for name, o in files.items():
            path = os.path.join(args.out, name)
            try:
                with open(path) as f:  # universal newlines: a checkout with CRLF line ends compares equal
                    current = f.read()
            except OSError:
                current = None
            if current != o.text():
                stale.append(name)
        on_disk = sorted(n for n in os.listdir(args.out) if n.endswith('.tsv')) if os.path.isdir(args.out) else []
        extra = [n for n in on_disk if n not in files]
        if stale or extra:
            print('gen_vectors.py --check: out of date: ' + ', '.join(stale + [f'{n} (not written by this script)' for n in extra]) +
                  '; run python3 dm/tools/aot/tests/gen_vectors.py')
            return 1
        print(f'gen_vectors.py --check: {len(files)} files, {total} vectors, current')
        return 0

    os.makedirs(args.out, exist_ok=True)
    for name, o in files.items():
        with open(os.path.join(args.out, name), 'w', newline='\n') as f:
            f.write(o.text())
    print(f'gen_vectors.py: {total} vectors in {len(files)} files under {args.out}')
    for name, o in files.items():
        print(f'  {name:16s} {len(o.lines):6d}  ' + ', '.join(f'{c} {n}' for c, n in sorted(o.per_class.items())))
    return 0


if __name__ == '__main__':
    sys.exit(main())
