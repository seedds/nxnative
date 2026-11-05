#!/usr/bin/env python3
"""Ampere ALU instructions through the offline translator (dmsh_xlate, arch sm86), on registers the translator cannot fold, their result stored to shared memory so
that it is kept: BREV, BMSK, F2IP, UIMAD with an immediate in C, and HADD2 / HMUL2 with an immediate (test_dmsh_ampere_mem.py's Pokopia class has those two on constants, a negative immediate's sign, F2FP.MERGE_C,
F2I from a half, UP2UR, MEMBAR and ERRBAR). Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import re, sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_dmsh_ampere_mem import EXIT, Translated, inst

STORE = (0x000fe8000000c800, 0x0000080609008388)   # STS.U32 [R9 * 16 + 8], R6 (Mario Kart World f94552a2a2bff505): the sink, R9 the thread index


def uldc(ur, bank, offset): return (0x000fe20000000800, 0x7ab9 | ur << 16 | offset << 38 | bank << 54)   # ULDC URd, c[bank][offset]
def waiting(i): return (i[0] | 0x3f << 52, i[1])   # the instruction waits on every scoreboard (control bits 116..121): it reads what an S2R before it wrote
def s2r_tid(rd): return (0x000e260000002100, 0x0000000000007919 | rd << 16)   # S2R Rd, SR_TID.X (Mario Kart World's encoding, write barrier 0)


class Alu(Translated):
    def kept(self, *code, srcs=()):
        """code between S2R R9 (and each register of srcs) = the thread index and the store of R6"""
        pre = b''.join(inst(*s2r_tid(r)) for r in (9, *srcs))
        body = b''.join(inst(*waiting(i)) for i in code)
        return self.translate(pre + body + inst(*waiting(STORE)) + inst(*EXIT), shared=0x1000)


class Brev(Alu):
    BREV = (0x001e240000000000, 0x0000001300067301)   # BREV R6, R19 (Pokémon Pokopia 6e8db04c25a53d8c, whose destination is R3)

    def test_reverses_the_bits(self):
        self.assertIn('reverse_bits', self.kept(self.BREV, srcs=(19,)))


class HalfImmediate(Alu):
    """HADD2 / HMUL2 Rd, Ra, imm (form 2): the second source is the immediate at bits 32..63, not the register field at 64 that HFMA2's B uses in that form. Pokopia's
    blur 85446a6753300fc8 steps its loop with HADD2 R6, R6, 2.0h; reading R0 there, the loop never ended and the GPU hung"""
    HADD2 = (0x000fe20000000800, 0x4000400006067430)   # HADD2 R6, R6, 2, 2 (field 64 = R0)
    HMUL2 = (HADD2[0], HADD2[1] & ~0xfff | 0x432)      # HMUL2 R6, R6, 2, 2: the same fields (no game encoding met)

    def test_hadd2_adds_the_immediate(self):
        msl = self.kept(self.HADD2, srcs=(6,))
        self.assertIn('spvFAdd', msl)
        self.assertRegex(msl, r'\b2\.0\b')   # read from R0 (never written) it was 0, and the add folded away

    def test_hmul2_multiplies_by_the_immediate(self):
        msl = self.kept(self.HMUL2, srcs=(6,))
        self.assertIn('spvFMul', msl)
        self.assertRegex(msl, r'\b2\.0\b')


class CompareFtz(Alu):
    """FSETP / FSET .FTZ (bit 80): a denormal operand compares as zero, as on NVIDIA; Apple's GPUs keep denormals. Mario Kart World's deferred lighting 635061c90d75a8d1
    normalises its four depth-weighted samples by 1 / sum only where FSETP.NEU.FTZ says sum != 0: a denormal sum passed, its reciprocal overflowed, and the NaN came out
    of a min(x, 1.0) as a fully lit dot. test_dmsh_maxwell_ftz.py has Maxwell's"""
    FSETP = (0x000fca0003f1d000, 0x800000141300720b)   # FSETP.NEU.FTZ.AND P0, PT, R19, -R20, PT (the game's guard, there on R35 and -R10)
    FSET = (0x000fe2000381d000, 0x800000141306720a)    # FSET.NEU.FTZ.AND R6, R19, -R20, PT
    FSEL = (0x000fe40000000000, 0x0000001413067208)    # FSEL R6, R19, R20, P0: keeps the predicate
    FLUSH = '1.1754943508222875'                       # FLT_MIN, the bound under which an operand is flushed

    def test_fsetp_flushes_both_operands(self):
        self.assertEqual(self.kept(self.FSETP, self.FSEL, srcs=(19, 20)).count(self.FLUSH), 2)

    def test_fset_flushes_both_operands(self):
        self.assertEqual(self.kept(self.FSET, srcs=(19, 20)).count(self.FLUSH), 2)

    def test_without_ftz_compares_as_is(self):
        self.assertNotIn(self.FLUSH, self.kept((self.FSETP[0] & ~(1 << 16), self.FSETP[1]), self.FSEL, srcs=(19, 20)))

    def test_against_zero_flushes_one(self):
        self.assertEqual(self.kept((self.FSETP[0], 0x000000ff1300720b), self.FSEL, srcs=(19, 20)).count(self.FLUSH), 1)


def s2r(rd, sr): return (0x000e260000000000 | sr << 8, 0x0000000000007919 | rd << 16)   # S2R Rd, SR (0x21 SR_TID.X, 0x25 SR_CTAID.X)


class F2ip(Alu):
    """F2IP.U8.F32.NTZ Rd, a, b, RZ (0x243): each float rounded to the nearest integer, saturated to a byte and NaN to 0, b in byte 0 and a in byte 1. The Legend of Zelda:
    Ocarina of Time's colour converter 99951da6e5cd38f5 has F2IP R22, R13, R12, RZ at +0x530; here it is on R6, R4, R5"""
    GAME = (0x000fe200000004ff, 0x0000000c0d167243)
    F2IP = (GAME[0], GAME[1] & ~0xffffffff0000 | 6 << 16 | 4 << 24 | 5 << 32)

    def msl(self):   # a (R4) from SR_TID.X, b (R5) from SR_CTAID.X
        return self.kept(s2r(4, 0x21), s2r(5, 0x25), self.F2IP)

    def test_rounds_saturates_and_drops_nan(self):
        msl = self.msl()
        self.assertEqual(len(re.findall(r'\brint\(', msl)), 2, msl)   # ties to even
        self.assertEqual(len(re.findall(r'\bisnan\(', msl)), 2, msl)
        self.assertEqual(len(re.findall(r'\bmin\([^;]*255\.0\)', msl)), 2, msl)

    def test_a_is_the_high_byte(self):
        msl = self.msl()
        defs = dict(re.findall(r'^\s*\w+ (_\d+) = (.*);$', msl, re.M))
        def source(v, seen=()):   # the shader inputs a value is computed from
            return set().union(*(source(u, seen + (v,)) for u in re.findall(r'_\d+', defs.get(v, '')) if u not in seen),
                               set(re.findall(r'gl_\w+', defs.get(v, ''))))
        high, low = re.search(r'(_\d+) << 8;', msl).group(1), re.search(r'= _\d+ \| (_\d+);', msl).group(1)
        self.assertEqual(source(high), {'gl_LocalInvocationID'}, msl)
        self.assertEqual(source(low), {'gl_WorkGroupID'}, msl)

    def test_other_encodings_are_refused(self):
        for hi in (self.F2IP[0] & ~0xffffff00, self.F2IP[0] | 1 << 8, self.F2IP[0] & ~0xff | 7, self.F2IP[0] | 1 << 16):   # bit 74 clear, 72 set (signed), C = R7, 80 set
            with self.subTest(hi=hex(hi)), self.assertRaises(AssertionError):
                self.kept(s2r(4, 0x21), (hi, self.F2IP[1]))


class F2ipTruncate(Alu):
    """F2IP.U8.F32.NTZ.TRUNC Rd, RZ, b, RZ (78..79 = 3, F2I's rounding field): Ocarina of Time's foliage mesh shader 740e2b04db624fb1 has F2IP R5, RZ, R5, RZ at +0x370, a
    colour channel times 255 that it then shifts by 16 into an RGBA8 word; here it is on R6 and R5. The other rounding modes are the same field (no game encoding met)"""
    GAME = (0x000fe4000000c4ff, 0x00000005ff057243)
    F2IP = (GAME[0], GAME[1] & ~0xff0000 | 6 << 16)

    def msl(self, hi=GAME[0]):   # b (R5) from SR_CTAID.X
        return self.kept(s2r(5, 0x25), (hi, self.F2IP[1]))

    def test_truncates_b_into_byte_0(self):
        msl = self.msl()
        self.assertEqual(len(re.findall(r'\btrunc\(', msl)), 1, msl)
        self.assertNotIn('rint(', msl)
        self.assertEqual(len(re.findall(r'\bisnan\(', msl)), 1, msl)
        self.assertEqual(len(re.findall(r'\bmin\([^;]*255\.0\)', msl)), 1, msl)
        self.assertNotRegex(msl, r'<< 8;')   # a = RZ: byte 1 stays zero

    def test_floor_and_ceiling(self):
        for mode, fn in ((1, 'floor'), (2, 'ceil')):
            with self.subTest(mode=mode):
                msl = self.msl(self.GAME[0] & ~(3 << 14) | mode << 14)
                self.assertEqual(len(re.findall(rf'\b{fn}\(', msl)), 1, msl)
                self.assertNotIn('trunc(', msl)


class UimadImmediateC(Alu):
    """UIMAD URd, URa, URb, imm (0x4a4, form 2: the immediate in the C slot): Ocarina of Time's e9213c7fba8ccf93 +0x43c0 UIMAD UR4, UR4, UR6, 0xa, here with UR4 and UR6
    from c[0], moved to R6"""
    UIMAD = (0x000fcc000f8e0206, 0x0000000a040474a4)
    MOV = (0x000fe20000000f00, 0x0000000400067c02)   # MOV R6, UR4

    def test_adds_the_immediate(self):
        msl = self.kept(uldc(4, 0, 0x40), uldc(6, 0, 0x44), self.UIMAD, self.MOV)
        self.assertRegex(msl, r'\* as_type<int>\([^;]*\);\s*int _\d+ = _\d+ \+ 10;')


if __name__ == '__main__':
    unittest.main()
