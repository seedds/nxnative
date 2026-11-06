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


class FloatRed(Translated):
    """RED.F32.FTZ.RN (0x98e, type 3 at bits 73..76, add): agl's scene analysis sums luminance with it (Ocarina of Time's probe_analyze 6e28359aa3558f61). Dropped with a log line, the exposure the CPU
    reads back from those buffers was several times too bright. A compare-and-swap loop on the word"""
    RED = (0x000fe20000000000 | 3 << 9, 0x0000000608ff098e)   # RED.F32.FTZ.RN [R8], R6: (control and type, opcode and registers)

    def test_adds_in_a_loop(self):
        msl = self.translate(b''.join(inst(*waiting(i)) for i in (s2r_tid(6), s2r_tid(8), s2r_tid(9), self.RED)) + inst(*EXIT))
        self.assertIn('atomic_compare_exchange_weak_explicit', msl)
        self.assertNotIn('atomic_fetch_add', msl)


class DeadChannel(Translated):
    """An image read whose .x is dead and whose .y is used: the optimizer's RemoveNode followed the read as if it were removed, so the chain computing its coordinate lost its last use and went, and the
    shader read an uninitialized variable (Ocarina of Time's wind solver 04942f42cae10d0f, the encodings below are from it: its velocity field oscillated and the grass flickered at 10 Hz)"""
    Y_MINUS_1 = (0x000fe40007ffe0ff, 0xffffffff01077810)   # IADD R7, R1, -1
    SULD = (0x000f6200001e0300, 0x6000080006047b97)        # SULD.XY R4, [R6, R7]: R5 is .y
    STORE_Y = (STORE[0], 0x0000080509008388)               # STS.U32 [R9 * 16 + 8], R5

    def test_coordinate_survives(self):
        msl = self.translate(b''.join(inst(*waiting(i)) for i in (s2r_tid(1), s2r_tid(6), s2r_tid(9), self.Y_MINUS_1, self.SULD, self.STORE_Y)) + inst(*EXIT), shared=0x1000)
        self.assertRegex(msl, r'\+ \(-1\)|- 1\b')
        self.assertNotRegex(msl, r'(?m)^\s+int (_\d+);\n\s+int _\d+ = \1;')


class Brev(Alu):
    BREV = (0x001e240000000000, 0x0000001300067301)   # BREV R6, R19 (Pokémon Pokopia 6e8db04c25a53d8c, whose destination is R3)

    def test_reverses_the_bits(self):
        self.assertIn('reverse_bits', self.kept(self.BREV, srcs=(19,)))


class F2ip(Alu):
    """F2IP.U8 (0x243): Ocarina of Time's FoliageShader 83df46a0b1b379b4 packs colour with it, its foliage mesh shaders one byte at a time"""
    PAIR = (0x000fc400000004ff, 0x000000152c067243)   # F2IP.U8 R6, R44, R21
    ONE = (0x000fe4000000c4ff, 0x00000009ff067243)    # F2IP.U8.TRUNC R6, RZ, R9

    def test_packs_two_bytes(self):
        msl = self.kept(self.PAIR, srcs=(44, 21))
        self.assertIn('rint(', msl)
        self.assertIn('255.0', msl)

    def test_truncates(self):
        self.assertIn('trunc(', self.kept(self.ONE, srcs=(9,)))


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


class GlobalMemory(Alu):
    """Global memory. desc[URb][Ra.64] (bit 76 of LDG / STG, bit 71 of ATOMG / RED; nvdisasm): URb is a memory descriptor, not part of the address, which is Ra.64 (plus the offset)
    alone; [Ra.64+URb] adds it. The descriptor's value was added to the address. ATOMG.CAS (0x3a9) was refused; ATOMG / RED INC and DEC had no case in EmitAtomicOp (the operation
    vanished). A GPU cross-check of 300 LDG / STG / ATOMG / RED through the host's gmem_map (one-thread compute shaders, production options): 135 identical to 300. Encodings
    built from the sm_86 oracle corpus and checked with nvdisasm"""
    R2UR = (0x000fe200000e0000, 0x00000000130473c2)       # R2UR UR4, R19
    LDG_DESC = (0x000fe2200c1e1900, 0x0000000414067981)   # LDG.E R6, desc[UR4][R20.64]
    LDG_URZ = (0x000fe2200c1e1900, 0x0000003f14067981)    # LDG.E R6, desc[URZ][R20.64]
    CAS = (0x000fe200001ee116, 0x00000013140673a9)        # ATOMG.E.CAS.STRONG.GPU PT, R6, [R20], R19, R22
    INC = (0x000fe200019ee100, 0x00000013140673a8)        # ATOMG.E.INC.STRONG.GPU PT, R6, [R20], R19

    def test_a_descriptor_is_not_an_offset(self):   # the same code whatever the descriptor register holds
        self.assertEqual(self.kept(self.R2UR, self.LDG_DESC, srcs=(19, 20, 21)), self.kept(self.R2UR, self.LDG_URZ, srcs=(19, 20, 21)))

    def test_atomg_cas_is_translated(self):
        self.assertIn('atomic_compare_exchange', self.kept(self.CAS, srcs=(19, 20, 21, 22)))

    def test_atomg_inc_is_a_compare_and_swap_loop(self):
        self.assertIn('atomic_compare_exchange', self.kept(self.INC, srcs=(19, 20, 21)))


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


class Vote(Alu):
    """VOTEU (0x886) writes the uniform predicate UPd and leaves the lane predicate of that number alone; VOTE (0x806) writes the lane predicate. nvdisasm reads 0x886 as
    VOTEU.ANY UP0, P0 (CuAssembler's sm_86 records) and NAK encodes it with a uniform destination at 81..84. Yoshi and the Mysterious Book's cbd80cb96ad38a61 votes P0 into
    UP0, guards a handle's bounds mask on !UP0 and its TEX on !P0: written to P0, UP0 stayed undefined and every lane's guard became the subgroup's"""
    VOTEU = (0x000fe20000000100, 0x00000000003f7886)   # VOTEU.ANY UP0, P0 (the game's, destination register URZ)
    VOTE = (0x000fe20000000100, 0x0000000000ff7806)    # VOTE.ANY P0, P0 (destination register RZ)
    FSETP = CompareFtz.FSETP                           # P0 from R19 and R20: differs across a subgroup
    MOV_P0 = (0x000fe20000000f00, 0x0000001300060202)  # @P0 MOV R6, R19: a lane guard
    UR4_0 = (0x000fe20000000000, 0x0000003f00047c82)   # UMOV UR4, URZ
    UR4_UP0 = (0x000fe20000000000, 0x0000123400040882)   # @UP0 UMOV UR4, 0x1234: a uniform guard
    MOV_UR4 = (0x000fe20000000f00, 0x0000000400067c02)   # MOV R6, UR4

    def test_voteu_writes_the_uniform_predicate(self):
        msl = self.kept(self.FSETP, self.VOTEU, self.UR4_0, self.UR4_UP0, self.MOV_UR4, srcs=(19, 20))
        self.assertIn('simd_any', msl)
        self.assertNotIn('if (false)', msl)   # an unwritten predicate reads false, so the uniform guard never held

    def test_voteu_leaves_the_lane_predicate(self):   # nothing reads UP0: the guards (the store's !P0 too) read the comparison, not the vote
        self.assertNotIn('simd_any', self.kept(self.FSETP, self.VOTEU, self.MOV_P0, srcs=(6, 19, 20)))

    def test_vote_writes_the_lane_predicate(self):
        self.assertIn('simd_any', self.kept(self.FSETP, self.VOTE, self.MOV_P0, srcs=(6, 19, 20)))


if __name__ == '__main__':
    unittest.main()
