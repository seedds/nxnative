#!/usr/bin/env python3
"""Ampere ALU instructions through the offline translator (dmsh_xlate, arch sm86), on registers the translator cannot fold, their result stored to shared memory so
that it is kept: BREV, and HADD2 / HMUL2 with an immediate (test_dmsh_ampere_mem.py's Pokopia class has those two on constants, a negative immediate's sign, F2FP.MERGE_C,
F2I from a half, UP2UR, MEMBAR and ERRBAR). Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import re, sys, unittest
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


class Csmtest(Alu):
    """0x80d other than Kirby's guard band cull is CSMTEST, the clip state test (ACsmtest): PRAGMATA's vertex shaders hold this form (3,734 of them stopped translating when
    main's guard band handler took the decoder slot). It translates in any stage, and its test never passes"""
    CSMTEST = (0x000fc80003fe0000, 0x800000070000780d)   # PRAGMATA's form: immediate 0x80000007, Pd0 = Pd1 = PT, P = PT

    def test_pragmata_form_translates(self):
        self.assertIn('kernel', self.kept(self.CSMTEST))


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


class CarriesAndModifiers(Alu):
    """Constant inputs the translator folds, the result stored: IADD3's two carry predicates (sum >= 2^32 and sum >= 2 * 2^32, Mesa NAK's OpIAdd3 fold, checked on
    hardware), PLOP3's second table on the sources as they were, POPC / FLO with an immediate whose top bit (63) is no bitwise not, LEA.HI.SX32's sign-extended high
    word and LEA's negated B (~B + 1 with its carry; with .X the bitwise not). Encodings built for the test, each checked with nvdisasm 13.5 (sm_86)"""
    SEL_P0 = (0x000fc200040000ff, 0x00000001ff067807)   # SEL R6, RZ, 0x1, !P0: R6 = P0 ? 1 : 0

    def stored(self, *code):
        m = re.findall(r'\] = (\w+);', self.kept(*code))
        return m[-1] if m else None

    def test_popc_immediate_top_bit_is_no_not(self):
        self.assertEqual(self.stored((0x000fc20000000000, 0x800000ffff067909)), '9u')    # POPC R6, 0x800000ff

    def test_flo_immediate_top_bit_is_no_not(self):
        msl = self.kept((0x000fc200000e0000, 0x800400ffff067900))                         # FLO.U32 R6, 0x800400ff
        self.assertIn('spvFindUMSB(2147746047u)', msl)                                    # 0x800400ff itself, not its bitwise not

    def test_iadd3_carry_of_two_sets_both_predicates(self):
        iadd3 = (0x000fc2000791e9ff, 0xffffffffff077810)                                  # IADD3 R7, P0, P1, -RZ, -0x1, -RZ: 2^32 + 0xffffffff + 2^32
        self.assertEqual(self.stored(iadd3, self.SEL_P0), '1u')

    def test_plop3_second_table_reads_the_old_source(self):
        isetp = (0x000fc20003f42070, 0x000000ffff00720c)                                  # ISETP.EQ.U32.AND P2, PT, RZ, RZ, PT: P2 = true
        plop3 = (0x000fc2000104e070, 0x0000000000f0781c)                                  # PLOP3.LUT P2, P0, P2, PT, PT, 0x0, 0xf0: P2 = false, P0 = the old P2
        self.assertEqual(self.stored(isetp, plop3, self.SEL_P0), '1u')

    def test_lea_hi_sx32_sign_extends(self):
        mov = (0x000fc20000000fff, 0x80000000ff057802)                                    # MOV R5, 0x80000000
        lea = (0x000fc200078f0aff, 0x000000ff05067211)                                    # LEA.HI.SX32 R6, R5, RZ, 0x1: (0xffffffff:R5) >> 31
        self.assertEqual(self.stored(mov, lea), '4294967295u')

    def test_lea_negated_zero_b_carries(self):
        lea = (0x000fc200078000ff, 0x800000ffff067211)                                    # LEA R6, P0, RZ, -RZ, 0x0: ~0 + 1 carries out
        self.assertEqual(self.stored(lea, self.SEL_P0), '1u')

    def test_lea_x_not_b(self):
        lea = (0x000fc200038e04ff, 0x800000ffff067211)                                    # LEA.X R6, RZ, ~RZ, 0x0, PT: 0 + 0xffffffff + 1
        self.assertEqual(self.stored(lea), '0u')


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


class I2fImmediate(Alu):
    """I2F.F16 Rd, imm (form 4): bits 60..62 are the top of the 32-bit immediate, not a byte select. nvdisasm reads 000e620000200c00fffffffe000f7906 as I2F.F16 R15, -0x2
    (PRAGMATA 0cea2c520ed3ab04, here into R6); it was refused as a byte select of a 32-bit source"""
    I2F = (0x000e620000200c00, 0xfffffffe00067906)

    def test_negative_immediate_to_a_half(self):
        msl = self.kept(self.I2F)
        self.assertRegex(msl, r'float\(-2\)|-2\.0')
        self.assertIn('half2', msl)


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



class WrappingMultiply(Alu):
    """IMAD.SHL.U32 (a multiply by a power of two) wraps modulo 2^32, as do IADD3 and every integer multiply. SPIRV-Cross wrote the translator's signed
    OpIMul as int arithmetic, and signed overflow is undefined in MSL: the Metal compiler folded the signed byte extract (x * 256) >> 24 into x >> 16.
    PRAGMATA 50e310abbb05e838 (+0x0220 IMAD.SHL.U32 R3, R4, 0x100, RZ; +0x0270 SHF.R.S32.HI R3, RZ, 0x18, R3) read byte 2 of its snorm8 words as
    0xffffc05d instead of 0x5d (found by an offline differential check against a SASS reference model, and reproduced on the GPU). Integer add, subtract and multiply are
    emitted on uint now, which wraps"""
    IMAD = (0x045fe200078e00ff, 0x0000010013037824)   # IMAD.SHL.U32 R3, R19, 0x100, RZ (the game's +0x0220 with R19, the thread index, for R4)
    SHF = (0x000fc40000011403, 0x00000018ff067819)    # SHF.R.S32.HI R6, RZ, 0x18, R3 (+0x0270, into R6, the stored register)

    def test_multiply_is_unsigned(self):
        msl = self.kept(self.IMAD, self.SHF, srcs=(19,))
        self.assertRegex(msl, r'\* 256u')
        self.assertNotRegex(msl, r'int \w+ = as_type<int>\([^;]*\) \* 256;')

    def test_signed_shift_kept(self):
        # the arithmetic shift stays signed (only the wrapping operations change type)
        self.assertRegex(self.kept(self.IMAD, self.SHF, srcs=(19,)), r'int \w+ = _\d+ >> 24;')


class SingleInstructionSemantics(Alu):
    """Constant inputs the translator folds, the result stored (R6). Found by sw2_tools/xcheck's per-instruction tester (xcheck.py insn: single instructions with
    nvdisasm-checked encodings run on the GPU against an interpreter on Mesa NAK's folds); each encoding below was checked with nvdisasm 13.4 (sm_86).
    - SETP's two results: Pd1 reads the accumulator P as it was, also when Pd0 is P (Pd1 was computed after Pd0 had been written).
    - IMAD has no negation of B: bit 63 (B a register, constant or uniform register) and bit 75 (B the 64-slot register of forms 2, 3, 7) are not printed
      by nvdisasm and are not modifiers; B was negated.
    - I2F's directed roundings (.RM / .RP, and every one to F16) were rounded to nearest; F2FP.PACK_AB.RZ and F2F.F16.F32's .RM / .RP / .RZ too (65520 became
      infinity, not 65504)."""
    SEL_P0 = (0x000fc200040000ff, 0x00000001ff067807)   # SEL R6, RZ, 0x1, !P0: R6 = P0 ? 1 : 0
    P2_TRUE = (0x000fc20003f42070, 0x000000ffff00720c)  # ISETP.EQ.U32.AND P2, PT, RZ, RZ, PT: P2 = true

    def stored(self, *code):
        m = re.findall(r'\] = (\w+);', self.kept(*code))
        return m[-1] if m else None

    def test_isetp_second_result_reads_the_old_accumulator(self):
        isetp = (0x000fc2000104507f, 0x000000ffffff720c)   # ISETP.NE.U32.AND P2, P0, RZ, RZ, P2: P2 = false AND P2, P0 = true AND the old P2 (true)
        self.assertEqual(self.stored(self.P2_TRUE, isetp, self.SEL_P0), '1u')

    def test_fsetp_second_result_reads_the_old_accumulator(self):
        fsetp = (0x000fc200010450ff, 0x000000ffffff720b)   # FSETP.NE.AND P2, P0, RZ, RZ, P2
        self.assertEqual(self.stored(self.P2_TRUE, fsetp, self.SEL_P0), '1u')

    MOV_R5_3 = (0x000fc20000000fff, 0x00000003ff057802)  # MOV R5, 0x3

    def test_imad_bit_63_is_no_negation(self):
        imad = (0x000fc200078e02ff, 0x8000000505067224)    # IMAD R6, R5, R5, RZ with bit 63 set: 9, not -9
        self.assertEqual(self.stored(self.MOV_R5_3, imad), '9u')

    def test_imad_bit_75_of_the_c_slot_forms_is_no_negation(self):
        imad = (0x000fc200078e0a05, 0x0000000105067424)    # IMAD R6, R5, R5, 0x1 (form 2) with bit 75 set: 10, not -8
        self.assertEqual(self.stored(self.MOV_R5_3, imad), '10u')

    def test_i2f_rounds_toward_plus_infinity(self):
        mov = (0x000fc20000000fff, 0x8839e130ff057802)     # MOV R5, 0x8839e130 (-2009472720)
        i2f = (0x000fc200002094ff, 0x00000005ff067306)     # I2F.RP R6, R5: -2009472640.0 (0xceef8c3d, truncated), not the nearest -2009472768.0
        msl = self.kept(mov, i2f)
        self.assertNotRegex(msl, r'float\(-2009472720\)')    # the conversion to nearest
        self.assertIn('spvFindUMSB', msl)                     # the bits under the 24 kept cleared (the truncation, which RP is for a negative value)

    def test_i2f_to_half_rounds_toward_zero(self):
        mov = (0x000fc20000000fff, 0xbae909efff057802)     # MOV R5, 0xbae909ef
        i2f = (0x000fc2000020c8ff, 0x00000005ff067306)     # I2F.F16.U32.RZ R6, R5: 65504 (0x7bff), not infinity
        msl = self.kept(mov, i2f)
        self.assertRegex(msl, r'uint\(_\d+\) < uint\(_\d+\);')   # the magnitude of the half compared with that of the value it came from, as bits
        self.assertIn('4294967295u)', msl)                     # and the half stepped down one where it came out larger

    def test_f2f_to_half_rounds_toward_plus_infinity(self):
        mov = (0x000fc20000000fff, 0x00800000ff057802)     # MOV R5, 0x800000 (FLT_MIN)
        f2f = (0x000fc200002088ff, 0x00000005ff067304)     # F2F.F16.F32.RP R6, R5: the smallest half denormal (0x0001), not 0
        msl = self.kept(mov, f2f)
        self.assertRegex(msl, r'_\d+ < 8388608;')           # the half's value key below FLT_MIN's bits (0x800000): an integer compare (Metal's fast math
        self.assertIn('+ 1u)', msl)                         # folded the float one), and the half stepped up

    def test_f2fp_rz_overflow_is_the_largest_half(self):
        mov = (0x000fc20000000fff, 0x477ff000ff057802)     # MOV R5, 0x477ff000 (65520.0)
        f2fp = (0x000fc200000180ff, 0x00000005ff06723e)    # F2FP.PACK_AB.RZ R6, RZ, R5: 0x00007bff (65504), not 0x7c00
        msl = self.kept(mov, f2fp)
        self.assertIn('1199566848u < uint(', msl)   # 65520.0's bits (0x477ff000) against the half's magnitude
        self.assertIn('4294967295u)', msl)

if __name__ == '__main__':
    unittest.main()
