#!/usr/bin/env python3
"""Ampere ALU and control instructions through the offline translator (dmsh_xlate, arch sm86), each as Pokémon Pokopia or Mario Kart World encodes it, its result
stored to shared memory so that it is kept: BREV, HADD2 / HMUL2 with an immediate, F2FP.MERGE_C, F2I from a half, UP2UR, MEMBAR and ERRBAR.
Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_dmsh_ampere_mem import EXIT, Translated, inst, s2r_tid, waiting

STORE = (0x000fe8000000c800, 0x0000080609008388)   # STS [R9.X4 + 0x8], R6 (Mario Kart World f94552a2a2bff505): the sink, R9 the thread index


def rd(i, r): return (i[0], i[1] & ~(0xff << 16) | r << 16)   # the instruction with its destination register replaced


class Alu(Translated):
    def kept(self, *code, srcs=()):
        """code between S2R R9 (and each register of srcs) = the thread index and the store of R6"""
        pre = b''.join(inst(*s2r_tid(r)) for r in (9, *srcs))
        body = b''.join(inst(*waiting(i)) for i in code)
        return self.translate(pre + body + inst(*waiting(STORE)) + inst(*EXIT), shared=0x1000)


class Brev(Alu):
    BREV = (0x001e240000000000, 0x0000001300037301)   # BREV R3, R19 (Pokémon Pokopia's ocean FFT 9df068f3d7767628, the thread index reversed)

    def test_reverses_the_bits(self):
        self.assertIn('reverse_bits', self.kept(rd(self.BREV, 6), srcs=(19,)))


class HalfImmediate(Alu):
    """HADD2 / HMUL2 Rd, Ra, imm (form 2): the second source is the immediate at bits 32..63, not the register field at 64 that HFMA2's B uses in that form. Pokopia's
    blur 8025c44e35cc19ca steps its loop with HADD2 R6, R6, 2.0h; reading R0 there, the loop never ended and the GPU hung"""
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


class F2fpMergeC(Alu):
    """F2FP.F16.F32.MERGE_C Rd, RZ, b, RZ (Pokopia 2b94ade3b8b173cf): b converted into the low half, c's low 16 bits the high half"""
    F2FP = (0x000fe200000040ff, 0x00c00600ff0d7a3e)   # b = a constant

    def test_low_half_from_b(self):
        msl = self.kept(rd(self.F2FP, 6))
        self.assertRegex(msl, r'as_type<uint>\(half2\(')
        self.assertIn('65535', msl)


class F2iFromHalf(Alu):
    F2I = (0x000ff0000010f100, 0x0000000200047305)   # F2I.S32 R4, R2.H0 (Pokopia 396660089f19fc16)

    def test_reads_the_half(self):
        msl = self.kept(rd(self.F2I, 6), srcs=(2,))
        self.assertRegex(msl, r'as_type<half2>\(.*\)\.x')
        self.assertRegex(msl, r'int\(')


class Up2ur(Alu):
    UP2UR = (0x000fc60008000000, 0x000000013f067883)   # UP2UR UR6, UPR, URZ, 0x1 (Pokopia 44a8391fde1febc0)

    def test_translates(self):
        self.translate(inst(*self.UP2UR) + inst(*EXIT))


class Barriers(Alu):
    MEMBAR = (0x000fec0000002000, 0x0000000000007992)   # MEMBAR.GPU (Pokopia 44a8391fde1febc0)
    ERRBAR = (0x000fc00000000000, 0x00000000000079ab)

    def test_membar_orders_device_memory(self):
        self.assertIn('mem_flags::mem_device', self.kept(self.MEMBAR))

    def test_errbar_is_nothing(self):
        self.translate(inst(*self.ERRBAR) + inst(*EXIT))


if __name__ == '__main__':
    unittest.main()
