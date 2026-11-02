#!/usr/bin/env python3
"""Ampere memory instructions through the offline translator (dmsh_xlate, arch sm86): SUATOM (0x593), the image atomic Donkey Kong Bananza's agl
technique compute shaders (agl_technique_pfx) take an append counter with; SURED (0x59f); ATOMS (0x38c, 0xf8c); shared memory's scaled address ([Ra.X4 / X8 / X16]),
IMAD.HI and ATOMS.CAS. Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import json, os, re, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
EXIT = (0x000fea0003800000, 0x000000000000794d)   # hi, lo: EXIT
# SUATOM.ADD.U32 R12, [R3], R12 on the 1D image whose handle is word 12 of c0 (AMapC0: image slot 1), as the game encodes it but unpredicated (PT)
SUATOM = (0x002ea200001ee000, 0x00000c0c030c7593)


def inst(hi, lo): return struct.pack('<QQ', lo, hi)
def waiting(i): return (i[0] | 0x3f << 52, i[1])   # the instruction waits on every scoreboard (control bits 116..121): it reads what an S2R before it wrote
def s2r_tid(rd): return (0x000e260000002100, 0x0000000000007919 | rd << 16)   # S2R Rd, SR_TID.X (Mario Kart World's encoding, write barrier 0)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class Translated(unittest.TestCase):
    def translate(self, code, **extra):
        with tempfile.TemporaryDirectory(prefix='ampere_mem_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(code)
            # c0 layout (QueryAmpereComputeC0Layout): no uniform or storage buffers, 3 texture handles from 0x10, image handles from 0x28 (word 12 = image slot 1), 0x40 bytes
            job = [dict(key='k', stage='cs', arch='sm86', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=True, nvn2_cs=[0, 0, 3, 0x40, 0], **extra)]
            (work / 'jobs.json').write_text(json.dumps(job))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue((work / 'k.spv').exists(), r.stdout + r.stderr)
            sys.path.insert(0, str(TOOLS)); import dmsh_geometry
            dmsh_geometry.flatten_sets(work / 'k.spv')
            r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', '--msl-argument-buffers', '--msl-texture-buffer-native',
                                str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            return r.stdout


class Suatom(Translated):
    def test_add_u32_on_a_1d_image(self):
        msl = self.translate(inst(*SUATOM) + inst(*EXIT))
        self.assertIn('atomic_fetch_add', msl)   # native (the build's options) or through spirv-cross's buffer alias (these defaults)
        self.assertIn('cp_i_tcb_4A_r32ui', msl)   # image slot 1 (c2 0x128), a 32-bit unsigned image

    def test_result_kept(self):   # the old value is the shader's (it shuffles it to the warp): the atomic's result is used, not discarded
        msl = self.translate(inst(*SUATOM) + inst(*EXIT))
        self.assertRegex(msl, r'uint \w+ = [^;]*atomic_fetch_add')


class MufuF16(Translated):
    """MUFU.F16 (Splatoon Raiders' upscaler network): the source is the half bits 60..61 select (bit 62 its abs, as in every B slot), the result the low half"""
    LOG2_HIGH = (0x000fe60000000e00, 0x5000000c000c7308)   # MUFU.F16.LOG2 R12, |R12.H1|: the game's encoding (c1b8edc09e3313b1 +0x5970) on the register the atomic reads
    LOG2_LOW = (0x0002a20000000e00, 0x4000000c000c7308)    # MUFU.F16.LOG2 R12, |R12.H0| (+0x59a0)

    def msl(self, mufu):
        return self.translate(inst(*mufu) + inst(*SUATOM) + inst(*EXIT))

    def test_high_half(self):
        msl = self.msl(self.LOG2_HIGH)
        self.assertRegex(msl, r'as_type<half2>\(.*\)\)\.y;')
        self.assertIn('log2(abs', re.sub(r'float _\d+ = abs\((_\d+)\);\s*float _\d+ = log2\(_\d+\);', 'log2(abs', msl))

    def test_result_is_the_low_half(self):   # the bits of a half pair, its high half zero: not the float's own bits
        self.assertRegex(self.msl(self.LOG2_HIGH), r'as_type<uint>\(half2\(float2\(\w+, 0\.0\)\)\)')

    def test_low_half_translates_apart(self):   # R12 is a constant here, so the low half folds away; what is left must not read the high one
        self.assertNotRegex(self.msl(self.LOG2_LOW), r'as_type<half2>\(.*\)\)\.y;')


def mov(rd, value):   # MOV Rd, imm32: values the tests can see in the output
    return inst(0x000fc60000000f00, value << 32 | rd << 16 | 0x7802)


def sts(offset, rb):   # STS.U32 [RZ + offset], Rb: keeps its result
    return inst(0x000fe80000000800, offset << 40 | rb << 32 | 0xff << 24 | 0x7388)


class Shared(Translated):
    """Fire Emblem Fortune's Weave's Lighting compute shaders: the exposure histogram and its 64-bit average, in the game's own encodings"""
    STS_SCALED_4 = 0x000fe80000004800, 0x0000000302027388   # STS.U32 [R2 * 4], R3 (6d0f6db3f0d396a3 +0x80 with a value): R2 counts words. Read unscaled, bin b came back at about 4b
    STS_SCALED_8 = 0x000fe80000008a00, 0x0000000302027388   # STS.U64 [R2 * 8], R3:R4 (d5bc99ffd183fa20 +0xa0)
    STS_BYTES = 0x000fe80000000800, 0x0000000302027388      # STS.U32 [R2], R3: a byte address
    IMAD_HI = 0x000fc80007820008, 0x0000000b07087227        # IMAD.HI R8, R7, R11, {R9:R8} (08faccfa2eaa7e01 +0xa80)
    ATOMS_CAS = 0x000e640001800001, 0x00000400ff01738d      # ATOMS.CAS R1, [RZ + 4], R0, R1 (f4aa178bc67c9793 +0x200)

    def msl(self, *code):
        msl = self.translate(b''.join(code) + inst(*EXIT), shared=0x400)
        return msl[msl.index('kernel void'):]

    def test_a_32_bit_access_scales_its_register_by_4(self):   # Element 3, not byte 3 (which is element 0)
        self.assertIn('[3] = 77u', self.msl(mov(2, 3), mov(3, 77), inst(*self.STS_SCALED_4)))

    def test_a_64_bit_access_scales_its_register_by_8(self):   # Element 3 of 8 bytes, word 6
        msl = self.msl(mov(2, 3), mov(3, 77), mov(4, 88), inst(*self.STS_SCALED_8))
        self.assertIn('[6] = 77u', msl)
        self.assertIn('[7] = 88u', msl)

    def test_no_scale_keeps_the_byte_address(self):   # Byte 12 is word 3
        self.assertIn('[3] = 77u', self.msl(mov(2, 12), mov(3, 77), inst(*self.STS_BYTES)))

    def test_imad_hi_adds_the_pair_to_the_whole_product(self):   # Without the upper word and the low word's carry, the shader's 64-bit division gave 893011 for 218
        # 0xffffffff * 1 + {4000:1}: the low word carries, so the result is the product's high word + 4000 + 1 (these constants fold down to that sum)
        msl = self.msl(mov(7, 0xffffffff), mov(11, 1), mov(8, 1), mov(9, 4000), inst(*self.IMAD_HI), sts(0x20, 8))
        self.assertIn('._m1', msl)
        self.assertRegex(msl, r'\+ 4000\b')
        self.assertRegex(msl, r'\+ 1\b')

    # 08faccfa2eaa7e01: ISETP.GE P0, R2, 0x20 sets the carry, then IMAD.X adds it. Dropped, the shader's 64-bit division gave 2 for 340 / 1
    ISETP_P0 = 0x000fe20003f06070, 0x000000200200780c
    IMAD_X = 0x000fe400000e0605, 0x000000ffff057224       # IMAD.X R5, RZ, RZ, R5 + P0 (+0xc10)
    IMAD_X_NEG = 0x000fe400000e0e07, 0x000000ffff067224   # IMAD.X R6, RZ, RZ, -R7 + P0 (+0xb20): with .X a negated addend is its bitwise not

    def test_imad_x_adds_its_carry(self):
        self.assertIn('[8] = 101u', self.msl(mov(2, 0x40), mov(5, 100), inst(*self.ISETP_P0), inst(*self.IMAD_X), sts(0x20, 5)))

    def test_imad_x_without_the_carry_adds_nothing(self):
        self.assertIn('[8] = 100u', self.msl(mov(2, 1), mov(5, 100), inst(*self.ISETP_P0), inst(*self.IMAD_X), sts(0x20, 5)))

    def test_imad_x_negates_by_bitwise_not(self):   # ~100 + 1 is -100; a two's complement negation would give -99
        self.assertIn('[8] = 4294967196u', self.msl(mov(2, 0x40), mov(7, 100), inst(*self.ISETP_P0), inst(*self.IMAD_X_NEG), sts(0x20, 6)))

    def test_atoms_cas_takes_its_expected_value_from_rb_and_its_new_value_from_rc(self):
        msl = self.msl(mov(0, 5), mov(1, 9), inst(*self.ATOMS_CAS), sts(0x20, 1))
        self.assertIn('atomic_compare_exchange', msl)
        self.assertRegex(msl, r'= 5u;')
        self.assertRegex(msl, r', 9u,')


class Sured(Translated):
    """SURED (0x59f): SUATOM without the old value, its fields laid out as SUATOM's. Mario Kart World's ab77eaee44cfef88 counts into a 2D image with it"""
    SURED = (0x000fe2000010e000, 0x60000c000400759f)   # SURED.ADD.U32 [R4], R0 on the 2D image of word 12 of c0, as the game encodes it

    def test_add_translates(self):
        msl = self.translate(inst(*s2r_tid(4)) + inst(*waiting(self.SURED)) + inst(*EXIT))
        self.assertIn('atomic_fetch_add', msl)
        self.assertIn('cp_i_tcb_4A_r32ui', msl)


class SharedScale(Translated):
    """A shared access scales Ra by the factor in bits 78..79, whatever its size: 1 [Ra.X4], 2 [Ra.X8], 3 [Ra.X16]. Mario Kart World's froxel shaders keep a float4 a thread
    at [R.X16 + 0x500]; f94552a2a2bff505 stores 8 bytes at [R9.X16] and 4 at [R9.X16 + 8]; Donkey Kong Bananza's upscaler stores 8 bytes at [i.X4 + 0x900]"""
    X4_64 = (0x0009ee0000004a00, 0x000900182c007388)    # STS.64 [R44.X4 + 0x900], R24 (Donkey Kong Bananza 7629d857a0a93833)
    X8_64 = (0x0211e40000008a00, 0x0000000600007388)    # STS.64 [R0.X8], R6 (Pokémon Pokopia)
    X16_32 = (0x000fe8000000c800, 0x0000080609008388)   # STS [R9.X16 + 0x8], R6 (Mario Kart World f94552a2a2bff505)
    X16_64 = (0x000fe8000000ca00, 0x0000000209008388)   # STS.64 [R9.X16], R2 (the same shader)
    X16_128 = (0x0001e2000000cc00, 0x000000ff26007388)  # STS.128 [R38.X16], RZ (Pokémon Pokopia 396660089f19fc16)

    def msl(self, sts, ra):
        return self.translate(inst(*s2r_tid(ra)) + inst(*waiting(sts)) + inst(*EXIT), shared=0x4000)

    def test_x4_on_a_64_bit_store(self):
        self.assertRegex(self.msl(self.X4_64, 44), r'<< 2\b')

    def test_x8(self):
        self.assertRegex(self.msl(self.X8_64, 0), r'<< 3\b')

    def test_x16_on_32_and_64_bit_stores(self):   # one 12-byte record a thread: not scaled by the access size
        self.assertRegex(self.msl(self.X16_32, 9), r'<< 4\b')
        self.assertRegex(self.msl(self.X16_64, 9), r'<< 4\b')

    def test_x16_on_a_128_bit_store(self):
        self.assertRegex(self.msl(self.X16_128, 38), r'<< 4\b')


class Atoms(Translated):
    """ATOMS (0x38c): a shared memory atomic, the operation and type as ATOMG's, addressed as STS; 0xf8c ATOMS.POPC.INC adds 1 with no source register"""
    MAX = (0x0003e80001000000, 0x00100c00ffff738c)   # ATOMS.MAX RZ, [RZ + 0x100c], R0 (Pokémon Pokopia 2a526184ca0aa932, unpredicated)
    INC = (0x0011e4000d00403f, 0x0000000003ff7f8c)   # ATOMS.POPC.INC RZ, [R3.X4] (Pokopia's 64-bin histogram e4ec13f79f9e0bd4)

    def test_max(self):
        msl = self.translate(inst(*s2r_tid(0)) + inst(*waiting(self.MAX)) + inst(*EXIT), shared=0x2000)
        self.assertIn('atomic_fetch_max', msl)

    def test_popc_inc_adds_one_at_the_scaled_index(self):
        msl = self.translate(inst(*s2r_tid(3)) + inst(*waiting(self.INC)) + inst(*EXIT), shared=0x100)
        self.assertRegex(msl, r'atomic_fetch_add_explicit\([^;]*, 1u,')
        self.assertRegex(msl, r'<< 2\b')


if __name__ == '__main__':
    unittest.main()
