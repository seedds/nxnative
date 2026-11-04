#!/usr/bin/env python3
"""Ampere ALU instructions through the offline translator (dmsh_xlate, arch sm86), on registers the translator cannot fold, their result stored to shared memory so
that it is kept: BREV, and HADD2 / HMUL2 with an immediate (test_dmsh_ampere_mem.py's Pokopia class has those two on constants, a negative immediate's sign, F2FP.MERGE_C,
F2I from a half, UP2UR, MEMBAR and ERRBAR). Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_dmsh_ampere_mem import EXIT, Translated, inst

STORE = (0x000fe8000000c800, 0x0000080609008388)   # STS.U32 [R9 * 16 + 8], R6 (Mario Kart World f94552a2a2bff505): the sink, R9 the thread index


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


if __name__ == '__main__':
    unittest.main()
