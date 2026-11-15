#!/usr/bin/env python3
"""Ampere instructions through the offline translator (dmsh_xlate, arch sm86): SUATOM (0x593), the image atomic Donkey Kong Bananza's agl technique compute shaders (agl_technique_pfx) take an
append counter with; shared memory's scaled address, IMAD.HI and ATOMS.CAS; and the instructions tested on the atomic's result below. Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import json, os, re, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
EXIT = (0x000fea0003800000, 0x000000000000794d)   # hi, lo: EXIT
# SUATOM.ADD.U32 R12, [R3], R12 on the 1D image whose handle is word 12 of c0 (AMapC0: image slot 1), as the game encodes it but unpredicated (PT)
SUATOM = (0x002ea200001ee000, 0x00000c0c030c7593)


def inst(hi, lo): return struct.pack('<QQ', lo, hi)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class Translated(unittest.TestCase):
    def translate(self, code, atomics=False, **extra):
        with tempfile.TemporaryDirectory(prefix='ampere_mem_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(code)
            # c0 layout (QueryAmpereComputeC0Layout): no uniform or storage buffers, 3 texture handles from 0x10, image handles from 0x28 (word 12 = image slot 1), 0x40 bytes
            job = [dict(key='k', stage='cs', arch='sm86', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=True, nvn2_cs=extra.pop('nvn2_cs', [0, 0, 3, 0x40, 0]), **extra)]
            (work / 'jobs.json').write_text(json.dumps(job))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue((work / 'k.spv').exists(), r.stdout + r.stderr)
            self.res = json.loads((work / 'k.res.json').read_text())   # the translator's own report of the shader
            sys.path.insert(0, str(TOOLS)); import dmsh_geometry
            dmsh_geometry.flatten_sets(work / 'k.spv')
            xc = ['--msl', '--msl-version', '20300', '--msl-argument-buffers', '--msl-texture-buffer-native']
            if atomics:   # as dmsh_build runs it for image atomics: MSL 3.1 and the native-atomic checks and rewrites (dmsh_atomic.py)
                import dmsh_atomic
                xc = dmsh_atomic.cross_args(xc, str(work / 'k.spv'))
            r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), *xc, str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            return dmsh_atomic.check_msl(r.stdout) if atomics else r.stdout

    def refused(self, code):   # the translator's output for code it refuses
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'k.bin').write_bytes(code)
            (Path(d) / 'jobs.json').write_text(json.dumps([dict(key='k', stage='cs', arch='sm86', code=str(Path(d) / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], nvn2_cs=[0, 0, 3, 0x40, 0])]))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(Path(d) / 'jobs.json'), d], capture_output=True, text=True)
            self.assertFalse((Path(d) / 'k.spv').exists(), r.stdout + r.stderr)
            return r.stdout + r.stderr


class GuardedWrites(Translated):
    P0 = (0x000fda0003f06070, 0x00c0060005007a0c)   # P0 = R5 >= c3[0x18], R5 an instruction's own
    MOV_R5 = (0x000fe20000000f00, 0x0100000000057a02)   # R5 = c4[0]

    def test_a_guarded_load_reads_its_address_before_the_false_guards_zero(self):
        # UR4 = c3[0x10]; LDG.!P0 R5, [R5 + UR4], Unreal Engine 5.4's bounds-checked read of its scene data: zeroing R5 first read at the table's start, every instance with the first one's data
        ur4, ldg = (0x000fe20000000a00, 0x00c0040000047ab9), (0x000ea200081e890f, 0x0000000405057981)
        waits = (0x004ea200001ee000, 0x00000c05050c7593)   # waits for the load's barrier, then atomically adds its result at the address it read: both uses of it
        msl = self.translate(inst(*self.MOV_R5) + inst(*ur4) + inst(*self.P0) + inst(*ldg) + inst(*waits) + inst(*EXIT))
        self.assertNotIn('_m0[0]);', msl.split('kernel')[1].replace('_m0[0].x', ''))   # no read of the table's first word
        self.assertRegex(msl, r'_m0\[(int\()?\w+')   # an index from the register

    def test_a_deferred_write_under_a_false_guard_writes_nothing(self):
        # MUFU.EX2 R17, R32 under P0 (write barrier 3); R17 = R29 * R29 under !P0; R17 = R36 * R17 after waiting on the barrier: Yoshi and the Mysterious Book's local lights' falloff. The wait copied
        # the old R17 over !P0's value, whatever the guard had been
        ex2, other, used = (0x000ee20000000800, 0x0000002000170308), (0x000fe20000410000, 0x0000001d1d178220), (0x008fe20000410000, 0x0000001724177220)
        msl = self.translate(inst(*self.MOV_R5) + inst(*self.P0) + inst(*ex2) + inst(*other) + inst(*used) + inst(SUATOM[0], 0x00000c17030c7593) + inst(*EXIT))
        self.assertRegex(msl, r'\? 1065353216 : 0;')   # the guard as it was at issue, a word that keeps its bits through a select
        self.assertRegex(msl, r'bool \w+ = \w+ != 0;\s+float \w+ = \w+;\s+if \(\w+\)')   # the copy only where it held

    def test_uniform_handle_forms_translate(self):
        for code in ((0x000f4200081e0f04, 0x3000040702067f63), (0x000142000c1e03ff, 0x2000040400167f6c), (0x00014200001e0f12, 0x3800001002101364)):   # TLD4 and TXD through a uniform register, TLD4 through a register
            self.translate(inst(*code) + inst(*EXIT))

    def test_a_computed_handle_reads_the_pools(self):
        # R16 = c4[0] & 0x7fff | 0x10800000, a heap index clamped and ORed with sampler 0x108 (PRAGMATA's RE Engine materials), then TLD4 through R16: a handle no table holds, which
        # BindlessElimination deleted (its result read 0); a sampled texture's ID indexes the texture pool whatever made it
        mov, mask, sampler = (0x000fe20000000f00, 0x0100000000107a02), (0x000fe400078ec0ff, 0x00007fff10107812), (0x000fe400078efcff, 0x1080000010107812)
        tld4 = (0x00014200001e0f12, 0x3800001002101364)   # write barrier 5, which the SUATOM after it waits on (it adds what the TLD4 read)
        msl = self.translate(inst(*mov) + inst(*mask) + inst(*sampler) + inst(*tld4) + inst(0x020ea200001ee000, 0x00000c10030c7593) + inst(*EXIT))
        self.assertIn('cb0_40000000', msl)   # the pool arrays (dmsh_build.py pool_arrays makes them pointers)
        self.assertIn('gather(', msl)

    def test_size_query_through_a_uniform_register_translates(self):   # TXQ R10, UR6, R10 (0xf6f; PRAGMATA, as encoded there): the size of a texture the pools hold
        msl = self.translate(inst(0x000f4200080001ff, 0x000006000a0a7f6f) + inst(SUATOM[0], 0x00000c0a030c7593) + inst(*EXIT))   # the size added by an image atomic, so it stays
        self.assertIn('get_width(', msl)

    def test_lod_query_handle_forms_translate(self):   # TMML through a uniform register and a register (Kirby Air Riders' 4,776 pixel shaders, as encoded there)
        for code in ((0x000f4200080002ff, 0x600004ff20277f69), (0x000f4200000002ff, 0x68000018141d736a)):
            self.translate(inst(*code) + inst(*EXIT))

    def test_gradients_through_a_uniform_register_keep_their_handle(self):
        # UR4 = c3[0x10]; TXD through UR4 (write barrier 5); after waiting on it, the result atomically added to an image, so it stays. The translator takes its source 0 as the handle: without it, it could not find one and deleted the read
        ur4, txd, used = (0x000fe20000000a00, 0x00c0040000047ab9), (0x000142000c1e03ff, 0x2000040400167f6c), (0x020ea200001ee000, 0x00000c16030c7593)
        msl = self.translate(inst(*ur4) + inst(*txd) + inst(*used) + inst(*EXIT))
        self.assertIn('gradient', msl)

    # A handle built under a guard from two words of the game's uniform buffer, read under a guard the translator cannot tie to it, so that a phi reaches the read with the first word and what the
    # register held before: Unreal Engine 5.4's single-layer water and a global pixel shader in Yoshi and the Mysterious Book. The read was deleted. Each ends in the result's atomic add, so it stays
    def test_handle_under_a_uniform_vote_of_the_reads_guard(self):
        # P0 = c4[0x14] test; UR8 = c3[0x60]; UR4 = c3[0x50]; UP0 = vote.all(P0); if (!UP0) UR4 |= UR8; TEX.!P0 through UR4
        code = [(0x000fe20003f05070, 0x01000500ff007a0c), (0x000fe20000000800, 0x00c0180000087ab9), (0x000fe20000000800, 0x00c0140000047ab9), (0x000fe40000000000, 0x00000000003f7886),
                (0x000fe2000f8efc3f, 0x0000000408048292), (0x000f42000c9e0f16, 0xb000042420188f60), (0x020ea200001ee000, 0x00000c16030c7593), EXIT]
        msl = self.translate(b''.join(inst(*c) for c in code))
        self.assertIn('.sample(', msl)
        self.assertIn('cp_t_cb262147_10050018', msl)   # words 0x18 (c3[0x60]) and 0x14 (c3[0x50]), bank 3 twice

    def test_handle_under_a_guard_the_reads_guard_implies(self):
        # P4 and P1 from comparisons; P5 = P4 && !P1; if (P4) R16 = c3[0x28]; if (P4) R16 |= c3[0x70]; TEX.P5 through R16
        code = [(0x040fe20003f85070, 0x000000070800780c), (0x000fc40003f25070, 0x000000040800780c), (0x000fe40000fa8170, 0x000000000000781c), (0x000fc60000000f00, 0x00c00a0000104a02),
                (0x000fc600078efcff, 0x00c01c0010104a12), (0x00014200049e03ff, 0x380000100e125361), (0x020ea200001ee000, 0x00000c12030c7593), EXIT]
        msl = self.translate(b''.join(inst(*c) for c in code))
        self.assertIn('.sample(', msl)
        self.assertIn('cp_t_cb262147_1007000A', msl)

    def test_handle_with_only_its_first_word_under_a_guard(self):
        # R16 = c3[0x28] under P4; R16 |= c3[0x70]; TEX through R16: Yoshi's two vertex shaders of this form
        code = [(0x040fe20003f85070, 0x000000070800780c), (0x000fc60000000f00, 0x00c00a0000104a02), (0x000fc600078efcff, 0x00c01c0010107a12), (0x00014200049e03ff, 0x380000100e127361),
                (0x020ea200001ee000, 0x00000c12030c7593), EXIT]
        msl = self.translate(b''.join(inst(*c) for c in code))
        self.assertIn('.sample(', msl)
        self.assertIn('cp_t_cb262147_1007000A', msl)

    def test_first_word_of_a_handle_is_not_read_as_a_handle_of_its_own(self):
        # As above in the driver's c2, where a word alone can be a handle: one read, through both words
        code = [(0x040fe20003f85070, 0x000000070800780c), (0x000fc40003f25070, 0x000000040800780c), (0x000fe40000fa8170, 0x000000000000781c), (0x000fc60000000f00, 0x00800a0000104a02),
                (0x000fc600078efcff, 0x00801c0010104a12), (0x00014200049e03ff, 0x380000100e125361), (0x020ea200001ee000, 0x00000c12030c7593), EXIT]
        msl = self.translate(b''.join(inst(*c) for c in code))
        self.assertEqual(msl.count('.sample('), 1)
        self.assertRegex(msl, r'_1007000A\b')

    def test_gather_through_a_register_past_a_branch_on_the_guard(self):
        # P6 from a test; if (P6) R49 = c3[0x18]; if (P6) R49 |= c3[0x68]; BRA.!P6 past the read; TLD4 through R49, the first of the second source block (.SCR)
        code = [(0x000fe20003fc5070, 0x01000500ff007a0c), (0x000fe20000000f00, 0x00c0060000316a02), (0x000fe200078efcff, 0x00c01a0031316a12), (0x000fea0003800000, 0x000000200000e947),
                (0x000f4200001e0f0c, 0x38000031020e7364), (0x020ea200001ee000, 0x00000c0c030c7593), EXIT]
        msl = self.translate(b''.join(inst(*c) for c in code))
        self.assertIn('.gather(', msl)
        self.assertIn('cp_t_cb262147_10068006', msl)


class Suatom(Translated):
    def test_handle_in_the_games_uniform_buffer(self):   # Unreal Engine 5.4 on the Switch 2 keeps every image in c[3]: the same word 12, in bank 3
        msl = self.translate(inst(SUATOM[0], SUATOM[1] | 3 << 54) + inst(*EXIT))
        self.assertIn('cp_i_cb3_C_r32ui', msl)
        self.assertIn('atomic_fetch_add', msl)

    # The register forms (0x394, 0x396, 0x3a0; SULD / SUST 0x998..0x99e): the handle in Rc (64..72), read here from c[3][0] by a MOV as Orbitals' (Unreal Engine 5.6) global
    # compute shaders read their ShaderPrint counter's (45389d82b79dbf0c +0x1850, its handle register R20 there). The handle names the binding as the bound form's word would
    MOV_R12_C3 = (0x003fde0000000f00, 0x00c00000000c7a02)   # MOV R12, c[3][0]

    def test_handle_in_a_register(self):   # SUATOM.ADD.U32 R12, [R12], R24 on a 1D buffer image, as the game encodes it with its handle in R12
        msl = self.translate(inst(*self.MOV_R12_C3) + inst(0x00321e00001ee00c, 0x200000180c0c7394) + inst(*EXIT))
        self.assertIn('atomic_fetch_add', msl)
        self.assertIn('cp_i_cb3_', msl)   # bank 3, word 0: the binding the bound form names

    def test_reduction_with_its_handle_in_a_register(self):   # SURED.OR.U32 above with its handle in R12 (0x3a0)
        msl = self.translate(inst(*self.MOV_R12_C3) + inst(0x000fe2000310e00c, 0xa00000010800759f - 0x59f + 0x3a0) + inst(*EXIT))
        self.assertIn('atomic_fetch_or', msl)
        self.assertIn('cp_i_cb3_', msl)

    def test_sured_takes_its_operation(self):   # SURED.OR.U32 as Yoshi and the Mysterious Book's compute shaders encode it, in bank 3
        self.assertIn('atomic_fetch_or', self.translate(inst(0x000fe2000310e000, 0xa0c002010800759f) + inst(*EXIT)))

    def test_add_u32_on_a_1d_image(self):
        msl = self.translate(inst(*SUATOM) + inst(*EXIT))
        self.assertIn('atomic_fetch_add', msl)   # native (the build's options) or through spirv-cross's buffer alias (these defaults)
        self.assertIn('cp_i_tcb_4A_r32ui', msl)   # image slot 1 (c2 0x128), a 32-bit unsigned image

    def test_images_come_after_the_separate_handles(self):   # 1 texture, 1 separate texture, 1 separate sampler from 0x10: word 12 (0x30) is image slot 1 still, past both tables (Star Fox's 1002069a331ceded)
        msl = self.translate(inst(*SUATOM) + inst(*EXIT), nvn2_cs=[0, 0, 1, 0x40, 0, 1, 1])
        self.assertIn('cp_i_tcb_4A_r32ui', msl)

    def test_result_kept(self):   # the old value is the shader's (it shuffles it to the warp): the atomic's result is used, not discarded
        msl = self.translate(inst(*SUATOM) + inst(*EXIT))
        self.assertRegex(msl, r'uint \w+ = [^;]*atomic_fetch_add')


class MufuTurns(Translated):
    """MUFU.SIN / COS take turns and use their fraction: the lowering takes t - rint(t) before the 2pi multiply (Donkey Kong Bananza's SSAO a6f04605183d9915 rotates its kernel by up to
    2.5e6 turns at its full-size effects; t * 2pi lost the fraction and Metal's fast sin / cos returned 0 past 2^20 turns, a screen corner with no ambient occlusion). The values are checked on
    the GPU by dm/gfx/tests/t_mufu_turns.m"""
    COS = (0x000fe60000000000, 0x0000000c000c7308)   # MUFU.COS R12, R12 (op 0, bits 74..80), the F16 form's encoding above without .F16 and the half select
    SIN = (0x000fe60000000400, 0x0000000c000c7308)   # MUFU.SIN R12, R12 (op 1)

    def lowered(self, mufu, fn):
        msl = self.translate(inst(*mufu) + inst(*SUATOM) + inst(*EXIT))
        # R12 is never written here, so the argument is the constant 0.0; the debug build's arithmetic goes through spirv-cross's precise helpers (spvFSub, spvFMul)
        m = re.search(r'float (_\d+) = rint\(([\w.]+)\);\s*float (_\d+) = (?:spvFSub\(\2, \1\)|\2 - \1);\s*float (_\d+) = (?:spvFMul\(\3, 6\.283185482025146484375\)|\3 \* 6\.283185482025146484375);'
                      r'\s*float _\d+ = ' + fn + r'\(\4\);', msl)
        self.assertTrue(m, msl)

    def test_cos(self): self.lowered(self.COS, 'cos')
    def test_sin(self): self.lowered(self.SIN, 'sin')


class Bmsk(Translated):
    """BMSK (Star Fox's 58046f286de779e1 +0x5a0, on registers the atomic reads): a mask of b bits from bit a. The form with c[4] sources keeps it from folding"""
    MOVS = inst(0x000fe20000000f00, 0x0100000000037a02) + inst(0x000fe20000000f00, 0x0100010000047a02)   # R3 = c4[0], R4 = c4[4]
    BMSK = (0x000fe40000000000, 0x000000040303721b)   # BMSK R3, R3, R4: clamping
    WRAP = (0x000fe40000000800, 0x000000040303721b)   # the same, .W

    def msl(self, op):
        return self.translate(self.MOVS + inst(*op) + inst(*SUATOM) + inst(*EXIT))

    def test_a_clamping_mask_tests_its_operands(self):
        self.assertGreaterEqual(self.msl(self.BMSK).count('>= 32u'), 3)   # a + b, a, b

    def test_a_wrapping_mask_tests_only_their_sum(self):
        self.assertEqual(self.msl(self.WRAP).count('>= 32u'), 1)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class PixelCovered(unittest.TestCase):
    """0x925 as Star Fox's quad efficiency views encode it (826168691af2e211 +0x0): 1 for a covered pixel, 0 for a helper invocation"""
    COVERED = (0x040e0200000e4000, 0x0000000000007925)
    OTHER = (0x040e0200000e8000, 0x0000000000007925)   # bits 78..80 = 2, not seen
    SAMPLE = (0x040e0200000ec000, 0x0000000000007925)   # bits 78..80 = 3: the sample index
    SPH = struct.pack('<32I', *([0x000c5482] + [0] * 4 + [0xb0000000] + [0] * 12 + [0xf] + [0] * 13))   # the game's: writes colour 0

    def run_job(self, *ops):
        with tempfile.TemporaryDirectory(prefix='ampere_px_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(self.SPH + b''.join(inst(*op) for op in ops) + inst(*EXIT))
            (work / 'jobs.json').write_text(json.dumps([dict(key='k', stage='ps', arch='sm86', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, debug=True)]))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            if not (work / 'k.spv').exists(): return None, r.stdout + r.stderr
            sys.path.insert(0, str(TOOLS)); import dmsh_geometry
            dmsh_geometry.flatten_sets(work / 'k.spv')
            m = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', '--msl-argument-buffers', str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(m.returncode, 0, m.stderr)
            return m.stdout, r.stdout + r.stderr

    def test_reads_the_helper_invocation(self):
        msl, log = self.run_job(self.COVERED)
        self.assertIsNotNone(msl, log)
        self.assertIn('simd_is_helper_thread()', msl)
        self.assertRegex(msl, r'\? 0 : 1;')   # helper 0, covered 1

    def test_another_form_is_refused(self):
        msl, log = self.run_job(self.OTHER)
        self.assertIsNone(msl)
        self.assertIn('0x925 form 2', log)

    def test_form_3_reads_the_sample_index(self):   # PRAGMATA's a689530d00090ebf +0x0 (into R2 there, R0 here: colour 0's x, so it stays)
        msl, log = self.run_job(self.SAMPLE)
        self.assertIsNotNone(msl, log)
        self.assertIn('[[sample_id]]', msl)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class IpaAtOffset(unittest.TestCase):
    """IPA at an offset from the pixel centre (location 2, the offset word in the second source register) and the barycentric coordinates a[0x3b4], as PRAGMATA's pixel
    shader 4a9d4507eea6c837 reads them at the sample's position (+0x130: IPA R3, a[0x3b4], R5; R0 here, colour 0's x). The offset is the sample index here, which no
    pass folds. What IPA.PASS reads is linear over the screen, so the value at the offset is the centre's plus the offset times its screen derivatives"""
    run_job = PixelCovered.run_job
    SPH = PixelCovered.SPH
    SAMPLE_R5 = (0x040e0200000ec000, 0x0000000000057925)
    BARY_AT = (0x000ee200000e20ed | 0x3f << 52, 0x0000000500007326)   # waits on every scoreboard: it reads the register just written
    BARY = (0x000ee200000e00ed, 0x000000ff00007326)                   # the same read at the centre

    def test_reads_the_centre_value_and_its_derivatives(self):
        msl, log = self.run_job(self.SAMPLE_R5, self.BARY_AT)
        self.assertIsNotNone(msl, log)
        self.assertIn('[[barycentric_coord', msl)
        self.assertRegex(msl, r'float (_\d+) = in\.gl_BaryCoordEXT\.x;\n\s*float (_\d+) = gl_FragCoord\.w;\n\s*float _\d+ = spvFMul\(\1, \2\);')   # over w, as a perspective attribute is read
        self.assertRegex(msl, r'float (_\d+) = dfdx\((_\d+)\);\n\s*float (_\d+) = dfdy\(\2\);')
        self.assertRegex(msl, r'int (_\d+) = as_type<int>\(_\d+\) << 20;\n\s*int _\d+ = \1 >> 28;')   # x: bits 8..11, signed
        self.assertRegex(msl, r'int (_\d+) = as_type<int>\(_\d+\) << 4;\n\s*int _\d+ = \1 >> 28;')    # y: bits 24..27
        self.assertIn('0.0625', msl)

    def test_a_centre_read_takes_no_derivative(self):
        msl, log = self.run_job(self.BARY)
        self.assertIsNotNone(msl, log)
        self.assertIn('[[barycentric_coord', msl)
        self.assertNotIn('dfdx(', msl)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class ShadingRate(unittest.TestCase):
    """S2UR of system register 0x84, the fragment's size in pixels (X in bits 8..15, Y in 16..23), as Nintendo Switch Sports Resort's sea shaders encode it (2271b2e0293b3c55 +0x2600):
    the host shades every pixel, so 1 x 1. It read 0 as an unknown register, and the sea's colour, divided by the fragment's area, was NaN"""
    run_job = PixelCovered.run_job
    SPH = PixelCovered.SPH
    S2UR = (0x000e220000008400, 0x00000000000479c3)   # S2UR UR4, SR 0x84
    I2F_R0 = (0x03ffe20008201400, 0x0000000400007c45)   # I2F R0, UR4 as the game reads it (+0x2d90), waiting on every barrier: colour 0's x, 65792 for 1 x 1

    def test_reads_one_by_one(self):
        msl, log = self.run_job(self.S2UR, self.I2F_R0)
        self.assertIsNotNone(msl, log)
        self.assertIn('float(65792)', msl)   # 0 before (an unknown system register)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class DepthRegister(unittest.TestCase):
    """A pixel shader's depth output follows its colours at four registers per enabled render target, then the sample mask register. Ocarina of Time's agl_common depth pass
    (ps_lib 519, OmapTargets 0x10f: RT0 RGBA in R0..R3, RT2 R in R4) writes its depth to R9; the component count put it in R6, which holds a step before"""
    run_job = PixelCovered.run_job
    SPH = struct.pack('<32I', *([0x000c5482] + [0] * 4 + [0xb0000000] + [0] * 12 + [0x10f, 0x2] + [0] * 12))   # colours 0x10f, depth written (+0x4c bit 1)

    @staticmethod
    def mov(reg, value): return (0x000fe20000000f00, value << 32 | reg << 16 | 0x7802)   # MOV Rd, imm32

    def test_depth_comes_after_the_last_target_block(self):
        msl, log = self.run_job(self.mov(6, 0x3e800000), self.mov(9, 0x3f400000))   # R6 = 0.25, R9 = 0.75
        self.assertIsNotNone(msl, log)
        self.assertRegex(msl, r'gl_FragDepth = 0\.75')


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


class Sured(Translated):
    """SURED.ADD.U32 [R4], R0 (0x59f) on a 2D image, Mario Kart World's ab77eaee44cfef88: SUATOM's layout with no result"""
    FORM = (0x000fe2000010e000, 0x60000c000400759f)

    def test_adds_at_the_coordinates(self):
        msl = self.translate(inst(*self.FORM) + inst(*EXIT))
        self.assertIn('atomic_fetch_add', msl)
        self.assertIn('texture2d', msl)

    def test_another_form_is_refused(self):
        with self.assertRaises(AssertionError) as e: self.translate(inst(0x000fe200001ee000, self.FORM[1]) + inst(*EXIT))
        self.assertIn('SURED form', str(e.exception))


class StageMemory(Translated):
    """A task or mesh shader's stage memory in shared memory, on the game's encodings (job stage_memory: workgroup 0, input 0x10, outputs 0x120, primitives 0x400)"""
    MEMORY = dict(stage_memory=[0, 0x10, 0x120, 0x400], shared=0x800)
    # The game's ISBERDs set a write barrier, which their readers wait on; these set none, so the store after one reads its result
    WORKGROUP = (0x000fe20000007800, 0x00000000ff007923)   # ISBERD R0, mode 0x78 (b6d76b6fc9dc926f +0)
    INPUT = (0x000fe20000003800, 0x00002000ff137923)       # ISBERD R19, [RZ + 0x20], mode 0x38 (ddc034e8e009ebfc, its base register made RZ)
    PAYLOAD = (0x0041e8000000b800, 0x0000100201007927)     # 0x927 [R1 + 0x10], R2, mode 0xb8 (a task's record)
    KEEP_R0 = (0x0041e8000000b800, 0x00001000ff007927)     # the same with [RZ + 0x10], R0: so what ISBERD read is used
    KEEP_R19 = (0x0041e8000000b800, 0x00001013ff007927)    # with R19
    ATTRIBUTE = (0x0001e2000000f800, 0x0000800307007927)   # 0x927 [R7 + 0x80], R3, mode 0xf8 (a vertex's y, b6d76b6fc9dc926f +0x3c0 on another base register)
    COUNT_ZERO = (0x000fe20000008800, 0x000003ffff007927)  # 0x927 [RZ + 3], RZ, mode 0x88: every thread clears the count first
    COUNT = (0x0001e20000008800, 0x00000307ff007927)       # 0x927 [RZ + 3], R7, mode 0x88: the thread with the count
    INDEX = (0x0001e20000008000, 0x0000040605057927)       # 0x927 [R5 + 4], R6, mode 0x80: an index byte

    def msl(self, *code):
        return self.translate(b''.join(inst(*c) for c in code) + inst(*EXIT), **self.MEMORY)

    def test_workgroup_number_reads_word_0(self):
        self.assertRegex(self.msl(self.WORKGROUP, self.KEEP_R0), r'as_type<float>\(_\d+\[0\]\)')

    def test_input_is_at_its_offset(self):   # 0x10 + 0x20
        self.assertRegex(self.msl(self.INPUT, self.KEEP_R19), r'as_type<float>\(_\d+\[12\]\)')

    def test_subword_inputs_are_unsigned_and_keep_the_byte_offset(self):
        # Fast Fusion reads packed U16 entries with mode 0x34. The high half of word 12 must stay unsigned, as must an U8 input at its own byte within the word.
        for mode, offset, bits in ((0x34, 0x22, 16), (0x30, 0x23, 8)):
            read = (self.INPUT[0] & ~0xff00 | mode << 8, self.INPUT[1] & ~(0xffffff << 40) | offset << 40)
            src = self.msl(read, self.KEEP_R19)
            self.assertIn('[12]', src)
            self.assertRegex(src, rf'uint\(as_type<int>\(\w+\)\) >> uint\({(offset & 3) * 8}\)')
            self.assertIn(f'& {(1 << bits) - 1};', src)

    def test_subword_payload_stores_use_their_width(self):
        for mode, bits in ((0xb4, 16), (0xb0, 8)):
            write = (self.PAYLOAD[0] & ~0xff00 | mode << 8, self.PAYLOAD[1])
            self.assertRegex(self.msl(write), rf'void dmsh_shared_store{bits}_\d+\(')

    def test_subword_vertex_records_keep_the_offset_within_the_word(self):
        # A U16 output read at byte 0x202 must read the high half of its remapped SoA word, not its low half.
        read = (self.READBACK[0] & ~0xff00 | 0xb400, self.READBACK[1] & ~(0xffffff << 40) | 0x202 << 40)
        src = self.translate(inst(*read) + inst(*self.KEEP_R8) + inst(*EXIT), stage_memory=[0, 0x10, 0x120, 0x400, 0x200, 6, 128 * 24], shared=0x800)
        self.assertIn('[72]', src)
        self.assertIn('>> uint(16)', src)
        self.assertIn('& 65535;', src)

    def test_word_store_goes_to_the_outputs(self):   # 0x120 + 0x80
        self.assertIn('0 + 416', self.msl(self.ATTRIBUTE))

    READBACK = (0x000fe6000000f800, 0x00018000ff087923)   # ISBERD R8, [RZ + 0x180], mode 0xf8 (PRAGMATA 5aab38bb5ba67ef7 +0x1680, its base register made RZ): a position word read back
    KEEP_R8 = (0x0041e8000000b800, 0x00001008ff007927)    # 0x927 [RZ + 0x10], R8, mode 0xb8

    def test_an_attribute_word_is_read_back_from_the_outputs(self):   # 0x120 + 0x180 = word 168
        self.assertRegex(self.msl(self.READBACK, self.KEEP_R8), r'as_type<float>\(_\d+\[168\]\)')

    def test_the_zero_count_is_left_out(self):   # threads are not in step on Metal: it could land after the real count, and the stage memory starts at zero
        without = self.msl(self.COUNT)
        with_zero = self.msl(self.COUNT_ZERO, self.COUNT)
        self.assertEqual(without.count('atomic_compare_exchange'), with_zero.count('atomic_compare_exchange'))

    def test_a_byte_goes_through_its_word_atomically(self):   # neighbouring threads write neighbouring bytes; the shader build makes the helper's loop one byte store (test_dmsh_shared_store.py)
        self.assertIn('atomic_compare_exchange', self.msl(self.INDEX))

    def test_vertex_records_go_to_their_soa_word(self):   # a mesh shader's mode 0xb8 (PRAGMATA 6adf14e9a4100082): records of 6 words from 0x200, 128 vertices
        mesh = self.translate(inst(*self.PAYLOAD) + inst(*EXIT), stage_memory=[0, 0x10, 0x120, 0x400, 0x200, 6, 128 * 24], shared=0x800)
        self.assertRegex(mesh, r'< 3072u')       # inside the 128 * 24 bytes of records only
        self.assertRegex(mesh, r'\* 43691u?\b')   # v = w * ceil(2^18 / 6) >> 18
        self.assertRegex(mesh, r'\* 768u?\b')     # a block of 32 vertices: 6 components of 0x80 bytes
        self.assertNotRegex(self.msl(self.PAYLOAD), r'43691')   # a task's mode 0xb8 is its payload, as before

    def test_vertex_records_of_many_components_divide_exactly(self):   # 51 components, 160 vertices: the reciprocal gives vertex 109 for word 5609 (110 * 51 = 5610 > 5609 >= 109 * 51 + 50)
        mesh = self.translate(inst(*self.PAYLOAD) + inst(*EXIT), stage_memory=[0, 0x10, 0x120, 0x400, 0x200, 51, 160 * 4 * 51], shared=0x8000)
        self.assertRegex(mesh, r'< 32640u')                    # inside the records only
        self.assertNotRegex(mesh, r'\* %du?\b' % (((1 << 18) + 50) // 51))   # no reciprocal
        self.assertRegex(mesh, r'/ 51\b')                      # an integer division

    def test_outside_a_task_or_mesh_shader_it_is_refused(self):
        with self.assertRaises(AssertionError) as e: self.translate(inst(*self.PAYLOAD) + inst(*EXIT))
        self.assertIn('outside a task or mesh shader', str(e.exception))


class BarrierMoves(Translated):
    """BMOV around warp shuffles (Hyrule Warriors: Age of Imprisonment's pixel shaders 02e49c189b769074 and 49241cf469c76830): convergence barrier B15 to and
    from R26, in the game's encodings. Per thread they do nothing, as the barriers they save do"""
    SAVE = (0x000fe20000000000, 0x000000001a0f7f55)      # +0x25c0
    RESTORE = (0x000fe40000100000, 0x000000001a0f7f56)   # +0x25d0
    RESTORE_PLAIN = (0x000fe40000000000, 0x000000001a0f7f56)

    def test_no_effect_on_the_shader(self):
        plain = self.translate(inst(*SUATOM) + inst(*EXIT))
        moved = self.translate(inst(*self.SAVE) + inst(*self.RESTORE) + inst(*SUATOM) + inst(*self.RESTORE_PLAIN) + inst(*EXIT))
        self.assertEqual(moved, plain)


class Yield(Translated):
    """YIELD (0x946) in Resident Evil 4's encoding, as its 21 compute shaders with it have it (after MEMBAR; ERRBAR, before a branch): per thread it does nothing"""
    YIELD = (0x000fde0003800000, 0x0000000000007946)

    def test_no_effect_on_the_shader(self):
        plain = self.translate(inst(*SUATOM) + inst(*EXIT))
        yielded = self.translate(inst(*SUATOM) + inst(*self.YIELD) + inst(*EXIT))
        self.assertEqual(yielded, plain)


class LdcIndexedBank(Translated):
    """LDC.64 .IS (address mode 2): the bank is the instruction's plus Ra's upper half, the offset Ra's lower half (Hyrule Warriors: Age of Imprisonment's
    vertex shader cc679c7b4d3e703c reads its c1 quad corners at (vertex index % 5) * 8 + 0x10000 this way). Here LDC.64.IS R12, c[0][R3] feeds the atomic"""
    LDC_IS = (0x000e620000008a00, 0x00000000030c7b82)   # the game's control bits, size and mode; Rd R12, Ra R3

    def test_reads_the_bank_chosen_at_run_time(self):
        msl = self.translate(inst(*self.LDC_IS) + inst(*SUATOM) + inst(*EXIT))
        self.assertIn('cp_c1)._m0[', msl)   # the shader's own constant segment is one of the banks it may name
        self.assertRegex(msl, r'>> 16u|>> uint\(16\)|>> 16')   # the bank from Ra's upper half
        self.assertRegex(msl, r'& 65535\b')                     # the offset from its lower half
        self.assertRegex(msl, r'== 1;')                          # selected among the banks by its value
        self.assertNotIn('cp_c0)._m0[', msl)   # never the driver segment at a run-time index, which the host cannot fill (here the instruction's own bank)


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

    def msl(self, *code, shared=0x400):
        msl = self.translate(b''.join(code) + inst(*EXIT), shared=shared)
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

    # 3,312 of Pokemon Pokopia's shared accesses have stride 3, on 4, 8 and 16 bytes alike (cs_rdb 990's STS.U32 [R2 * 16]): the field is a scale, not the access size
    STS_SCALED_16 = 0x0001e4000000c800, 0x0000000302027388

    def test_stride_3_scales_its_register_by_16(self):   # Element 3 of 16 bytes, word 12
        self.assertIn('[12] = 77u', self.msl(mov(2, 3), mov(3, 77), inst(*self.STS_SCALED_16)))

    # The same field on other sizes, with an offset: the stride decides the scale, whatever the access size
    STS64_SCALED_4 = 0x0009ee0000004a00, 0x000900182c007388     # STS.U64 [R44 * 4 + 0x900], R24:R25 (Donkey Kong Bananza's upscaler 7629d857a0a93833)
    STS_SCALED_16_AT_8 = 0x000fe8000000c800, 0x0000080609008388   # STS.U32 [R9 * 16 + 8], R6 (agl_common: Mario Kart World f94552a2a2bff505, Donkey Kong Bananza c5c746915e9a3265)
    STS64_SCALED_16 = 0x000fe8000000ca00, 0x0000000209008388    # STS.U64 [R9 * 16], R2:R3 (the same shaders)
    STS128_SCALED_16 = 0x0001e2000000cc00, 0x000000ff26007388   # STS.128 [R38 * 16], RZ (Pokemon Pokopia 8987796cd5718078)

    def test_a_64_bit_access_with_stride_1_scales_its_register_by_4(self):   # Byte 3 * 4 + 0x900, word 579
        msl = self.msl(mov(44, 3), mov(24, 77), mov(25, 88), inst(*self.STS64_SCALED_4), shared=0x1000)
        self.assertIn('[579] = 77u', msl)
        self.assertIn('[580] = 88u', msl)

    def test_stride_3_adds_its_offset_after_the_scale(self):   # Byte 3 * 16 + 8, word 14
        self.assertIn('[14] = 77u', self.msl(mov(9, 3), mov(6, 77), inst(*self.STS_SCALED_16_AT_8)))

    def test_a_64_bit_access_with_stride_3_scales_its_register_by_16(self):   # Element 3 of 16 bytes, word 12
        msl = self.msl(mov(9, 3), mov(2, 77), mov(3, 88), inst(*self.STS64_SCALED_16))
        self.assertIn('[12] = 77u', msl)
        self.assertIn('[13] = 88u', msl)

    def test_a_128_bit_access_with_stride_3_scales_its_register_by_16(self):   # Element 3 of 16 bytes, words 12 to 15
        msl = self.msl(mov(38, 3), inst(*self.STS128_SCALED_16))
        self.assertIn('[12] = 0u', msl)
        self.assertIn('[15] = 0u', msl)

    # LDSM keeps its mode at 78..79, where LDS has its stride: 1 is a transposed read (MT88), not a scale. Donkey Kong Bananza's upscaler 7629d857a0a93833 +0x1ce0, on R3: scaled, its picture was garbage
    LDSM_MT88 = 0x000fe20000004200, 0x000000000314783b   # LDSM.16.MT88.4 R20, [R3], with no write barrier for the store to wait on

    def test_ldsm_mode_is_not_a_scale(self):
        msl = self.msl(mov(3, 0x40), inst(*self.LDSM_MT88), sts(0x20, 20))
        self.assertIn('spvSubgroupShuffle(8.96831', msl)   # the address 0x40 as a float's bits; scaled, it would be 0x100 (3.5873e-43)

    # LDSM reads rows other lanes stored, often with no BAR before it (Ocarina of Time's movie networks and its scene's compute: dotted rows, other places each run): a SIMD-group
    # barrier before its reads, so the stores have landed, and one after, so no lane's next store reaches the rows before every lane has read them (GitHub issue #1537, PR #1376)
    def test_ldsm_reads_between_subgroup_barriers(self):
        msl = self.msl(mov(3, 0x40), inst(*self.LDSM_MT88), sts(0x20, 20))
        barriers = [m.start() for m in re.finditer(r'simdgroup_barrier\(mem_flags::mem_threadgroup\)', msl)]
        reads = [m.start() for m in re.finditer(r'spvSubgroupShuffle\(', msl)]
        store = msl.index('[8] = ')   # the STS after it: byte 0x20, word 8
        self.assertEqual(len(barriers), 2, msl)
        self.assertTrue(reads and barriers[0] < reads[0] and reads[-1] < barriers[1] < store, msl)


class Pokopia(Translated):
    """Pokemon Pokopia's encodings, on the register the atomic reads: F2FP.MERGE_C (22,886 shaders), F2I from a half (1,067), MEMBAR with ERRBAR (144)"""
    MERGE_FIRST = (0x000fe200000040ff, 0x00c00600ff0c7a3e)   # F2FP.MERGE_C R12, RZ, c[3][word 6], RZ
    MERGE_SECOND = (0x000fe4000000400c, 0x00c00500ff0c7a3e)   # F2FP.MERGE_C R12, RZ, c[3][word 5], R12
    F2I_LOW = (0x000fe2000010f100, 0x0000000c000c7305)   # F2I.S32.F16.TRUNC R12, R12.H0
    F2I_HIGH = (0x000fe2000010f100, 0x1000000c000c7305)   # F2I.S32.F16.TRUNC R12, R12.H1
    # TLD.1D.LZ R12, R12, texture slot 0 (c0 word 4): 80,220 shaders, KTGL's light and decal lists in texture buffers. The game's words set a write barrier its next instructions wait on;
    # this one sets none, so the atomic sees its result
    TLD_1D = (0x000fe200009e01ff, 0x100004ff0c0c7b66)
    ATOMS_ADD = (0x004e680000000000, 0x0007fc0cff0c738c)   # ATOMS.ADD.U32 R12, [RZ + 0x7fc], R12, under PT
    MEMBAR = (0x000fec0000005000, 0x0000000000007992)
    ERRBAR = (0x000fc00000000000, 0x00000000000079ab)

    def msl(self, *code):
        return self.translate(b''.join(inst(*c) for c in code) + inst(*SUATOM) + inst(*EXIT))

    def test_fixed_uniform_reads_are_bare_and_counted(self):   # TranslationFlags.MetalUniformBounds: no comparison and no size table for reads at fixed offsets; the host gets how far they go (GitHub issue #2066)
        sys.path.insert(0, str(TOOLS)); import dmsh_abi
        msl = self.msl(self.MERGE_FIRST, self.MERGE_SECOND)   # c[3] words 6 and 5
        self.assertNotIn('_c%d' % dmsh_abi.CBSIZE_BANK, msl)
        self.assertEqual([c['fixed'] for c in self.res['cbufs'] if c['slot'] == 3], [28])

    def test_merge_shifts_c_into_the_high_half(self):   # word 6, converted first, ends in the high half; word 5 in the low one
        msl = self.msl(self.MERGE_FIRST, self.MERGE_SECOND)
        high = re.search(r'uint (_\d+) = as_type<uint>\(half2\(float2\(\(\*spvDescriptorSet0\.cp_c3\)\._m0\[1\]\.z, 0\.0\)\)\);', msl)
        low = re.search(r'uint (_\d+) = as_type<uint>\(half2\(float2\(\(\*spvDescriptorSet0\.cp_c3\)\._m0\[1\]\.y, 0\.0\)\)\);', msl)
        self.assertTrue(high and low, msl)
        shifted = re.search(rf'int (_\d+) = int\({high.group(1)}\) << 16;', msl)
        self.assertTrue(shifted, msl)
        self.assertRegex(msl, rf'{shifted.group(1)} \| int\({low.group(1)}\)')

    def test_merge_with_rz_in_c_is_the_low_half_alone(self):
        msl = self.msl(self.MERGE_FIRST)
        self.assertRegex(msl, r'as_type<uint>\(half2\(float2\(\(\*spvDescriptorSet0\.cp_c3\)\._m0\[1\]\.z, 0\.0\)\)\)')
        self.assertNotIn('<< 16', msl)

    def test_f2i_reads_the_selected_half(self):   # the merge left its float in the low half and zero in the high one; the translator folds the pack and unpack
        self.assertIn('trunc(0.0)', self.msl(self.MERGE_FIRST, self.F2I_HIGH))
        self.assertRegex(self.msl(self.MERGE_FIRST, self.F2I_LOW), r'trunc\(\(\*spvDescriptorSet0\.cp_c3\)\._m0\[1\]\.z\)')

    def test_1d_fetch_wraps_into_rows(self):   # a texture buffer in the slot is bound as rows of 16384 texels (nvn_mtl.m tbuf_as_2d); a 1D texture, one row, reads the same texel
        msl = self.msl(self.MERGE_FIRST, self.TLD_1D)
        self.assertRegex(msl, r'& 16383')
        self.assertRegex(msl, r'>> uint\(14\)')
        self.assertRegex(msl, r'cp_t_tcb_8\.read\(uint2\(')   # slot 0's handle, c2 word 8, as a 2D texture
        self.assertNotIn('texture1d', msl)

    def test_atoms_add_on_shared_memory(self):   # the offset is in bytes: 0x7fc is word 511 of the workgroup's 0x800 bytes
        msl = self.translate(inst(*self.ATOMS_ADD) + inst(*SUATOM) + inst(*EXIT), shared=0x800)
        self.assertRegex(msl, r'atomic_fetch_add_explicit\(\(threadgroup atomic_uint\*\)&_\d+\[511\]')

    # HADD2 / HMUL2 with an immediate (form 2): the second source is the immediate, not the unused register field at 64 (R0, the blur's texture coordinate, in the shader that hung the GPU)
    HADD2_IMM = (0x000fe20000000900, 0x3e003e00ff0c7430)   # HADD2 R12, RZ, (1.5, 1.5)
    HADD2_NEG_IMM = (0x000fe20000000900, 0xb800b800ff0c7430)   # HADD2 R12, RZ, (-0.5, -0.5): bits 63 and 62 belong to the constant, they are not modifiers
    HMUL2_IMM = (0x000fe2000000080c, 0x400040000c0c7432)   # HMUL2 R12, R12, (2.0, 2.0)

    def test_half_add_takes_the_immediate(self):
        msl = self.msl(self.HADD2_IMM)
        self.assertIn('1.5', msl)
        self.assertNotRegex(msl, r'as_type<half2>\(\s*_?\w*r0')

    def test_half_immediate_keeps_its_sign(self):
        self.assertIn('-0.5', self.msl(self.HADD2_NEG_IMM))

    def test_half_multiply_takes_the_immediate(self):
        self.assertIn('2.0', self.msl(self.MERGE_FIRST, self.HMUL2_IMM))

    # HFMA2 .H0_NH1 (bit 86): the low lane multiplies by B's high half negated, the high lane by B's low half. R12 holds (c3[5], c3[6]) from the two merges
    HFMA2_NH1 = (0x000fe200004000ff, 0x0000000c0c0c7231)   # HFMA2 R12, R12, R12.H0_NH1, RZ
    HFMA2_PLAIN = (0x000fe200000000ff, 0x0000000c0c0c7231)   # HFMA2 R12, R12, R12, RZ

    def hfma2_lanes(self, hfma2):   # the generated code, and the names of A.lo, A.hi, B.lo, B.hi, unpacked in that order
        msl = self.msl(self.MERGE_FIRST, self.MERGE_SECOND, hfma2)
        lanes = re.findall(r'float (_\d+) = float2\(as_type<half2>\(uint\(_\d+\)\)\)\.[xy];', msl)
        self.assertEqual(len(lanes), 4, msl)
        return (msl, *lanes)

    def test_hfma2_nh1_rotates_b(self):
        msl, a_lo, a_hi, b_lo, b_hi = self.hfma2_lanes(self.HFMA2_NH1)
        negated = re.search(rf'float (_\d+) = -{b_hi};', msl)
        self.assertTrue(negated, msl)
        self.assertIn(f'fma({a_lo}, {negated.group(1)}, 0.0)', msl)   # low lane: A.lo * -B.hi
        self.assertIn(f'fma({a_hi}, {b_lo}, 0.0)', msl)               # high lane: A.hi * B.lo

    def test_hfma2_without_nh1_keeps_the_lanes(self):
        msl, a_lo, a_hi, b_lo, b_hi = self.hfma2_lanes(self.HFMA2_PLAIN)
        self.assertIn(f'fma({a_lo}, {b_lo}, 0.0)', msl)
        self.assertIn(f'fma({a_hi}, {b_hi}, 0.0)', msl)

    # FSEL keeps each operand's modifiers (161,748 of the game's register forms negate B): "x or -x" from one register is how its character and prop lighting picks a sign
    FSEL_NEG_B = (0x000fe40000000000, 0x8000000c0c0c7208)   # FSEL R12, P0, R12, -R12

    def test_fsel_negates_b(self):
        msl = self.msl(self.MERGE_FIRST, self.MERGE_SECOND, self.FSEL_NEG_B)
        self.assertRegex(msl, r'float _\d+ = -')

    def test_up2ur_takes_any_predicate_by_its_mask_bit(self):   # Mario Kart World's pixel shaders save UP6 or UP3 and test the register against URZ later
        for mask in (0x40, 0x08): self.msl((0x000fe20008000000, mask << 32 | 0x3f067883))

    def test_up2ur_with_a_merge_source_is_refused(self):
        with self.assertRaises(AssertionError) as e: self.msl((0x000fe20008000000, 0x40 << 32 | 0x05067883))
        self.assertIn('UP2UR with a merge source', str(e.exception))

    def test_up2ur_reads_up0(self):   # cs_main's encoding: UR6 = UP0 ? 1 : 0
        self.msl((0x000fc60008000000, 0x000000013f067883))

    # Shared memory with a stride of 1: the address is Ra * 4 + offset. RZ would fold the scale away, so the index comes from F2I
    F2I_R3 = (0x000e24000021f000, 0x0000000c00037305)   # F2I.S32 R3, R12
    HIST_INC = (0x0011e4000d00403f, 0x0000000003ff7f8c)   # 0xf8c: the luminance histogram's hist[R3]++
    LDS_SCALED = (0x000fe20000004800, 0x00000000030c7984)   # LDS R12, [R3 * 4]
    LDS_PLAIN = (0x000fe20000000800, 0x00000000030c7984)   # LDS R12, [R3]

    def shared_msl(self, *code):
        return self.translate(b''.join(inst(*c) for c in (self.MERGE_FIRST, self.F2I_R3) + code) + inst(*SUATOM) + inst(*EXIT), shared=0x800)

    def test_histogram_increment_adds_one_at_the_scaled_index(self):
        msl = self.shared_msl(self.HIST_INC)
        self.assertRegex(msl, r'atomic_fetch_add_explicit\(\(threadgroup atomic_uint\*\)&_\d+\[[^\]]*\], 1u')
        self.assertRegex(msl, r'<< 2')

    def test_shared_load_scales_only_with_bit_78(self):
        self.assertRegex(self.shared_msl(self.LDS_SCALED), r'<< 2')
        self.assertNotRegex(self.shared_msl(self.LDS_PLAIN), r'<< 2')

    def test_membar_orders_memory(self):
        self.assertRegex(self.msl(self.MEMBAR, self.ERRBAR), r'threadgroup_barrier|atomic_thread_fence')


# SURED.D.2D.MAX.U64.STRONG.GPU.IGN [R0], R2 (PRAGMATA's visibility buffer, 336 shaders: 002292c6b1542bce +0x3510 has 0001e2000110e400600034020000059f, handle 0x34 and
# predicated on P0), here unpredicated on the image whose handle is word 12 of c0 (image slot 1): coordinate R0, R1; value R2 (low word), R3 (high word). After uncraftablexd's PR #1801
SURED_MAX_U64 = (0x0001e2000110e400, 0x60000c020000759f)
SURED_MIN_U64 = (SURED_MAX_U64[0] & ~(0x1f << 23) | 1 << 23, SURED_MAX_U64[1])   # operation 87..91: 1 = MIN
SURED_ADD_U64 = (SURED_MAX_U64[0] & ~(0x1f << 23), SURED_MAX_U64[1])            # 0 = ADD: Metal has no 64-bit texture add
SURED64 = (mov(0, 5), mov(1, 2), mov(2, 0xffffffff), mov(3, 1), inst(*SURED_MAX_U64))


class Sured64(Translated):
    """64-bit image max / min: one texture2d<ulong> atomic on the rg32ui image, low word then high word (dmsh_atomic.wide_atomics, nvn_mtl.m pf_atomic)"""
    def msl(self, *code):
        return self.translate(b''.join(code) + inst(*EXIT), atomics=True)

    def test_max_is_one_64_bit_texture_atomic(self):
        msl = self.msl(*SURED64)
        self.assertIn('texture2d<ulong, access::read_write> cp_i_tcb_4A_rg32ui', msl)
        self.assertEqual(len(re.findall(r'\.atomic_max\(uint2\(\w+\), ulong4\(as_type<ulong>\(uint2\(4294967295u, 1u\)\)\)\);', msl)), 1)
        self.assertNotIn('atomic_fetch', msl)

    def test_bounds_guard_stays(self):   # IGN: outside the image nothing is written
        self.assertRegex(self.msl(*SURED64), r'if \(all\(uint2\(\w+\) < uint2\(spvDescriptorSet0\.cp_i_tcb_4A_rg32ui\.get_width\(\), spvDescriptorSet0\.cp_i_tcb_4A_rg32ui\.get_height\(\)\)\)\)')

    def test_min(self):
        msl = self.msl(mov(0, 5), mov(1, 2), mov(2, 6), mov(3, 7), inst(*SURED_MIN_U64))
        self.assertRegex(msl, r'\.atomic_min\(uint2\(\w+\), ulong4\(as_type<ulong>\(uint2\(6u, 7u\)\)\)\);')

    def test_add_is_refused(self):
        self.assertIn('Metal 64-bit image atomics', self.refused(b''.join((mov(0, 5), mov(1, 2), inst(*SURED_ADD_U64), inst(*EXIT)))))


class WideAtomics(unittest.TestCase):
    """dmsh_atomic.wide_atomics on the shape SPIRV-Cross writes, and its refusals (no translator needed)"""
    DECL = 'struct spvDescriptorSetBuffer0\n{\n    texture2d<uint, access::read_write> cp_i_tcb_4A_rg32ui [[id(0)]];\n};\n'
    GUARD = '    if (all(uint2(_29) < uint2(spvDescriptorSet0.cp_i_tcb_4A_rg32ui.get_width(), spvDescriptorSet0.cp_i_tcb_4A_rg32ui.get_height())))\n    {\n'
    PAIR = ('        uint _40 = spvDescriptorSet0.cp_i_tcb_4A_rg32ui.atomic_fetch_max(uint2(_29), uint(_12)).x;\n'
            '        uint _41 = spvDescriptorSet0.cp_i_tcb_4A_rg32ui.atomic_fetch_max(uint2(_29), as_type<uint>(f(_13, 2))).x;\n')

    def wide(self, body):
        sys.path.insert(0, str(TOOLS)); import dmsh_atomic
        return dmsh_atomic.wide_atomics(self.DECL + self.GUARD + body + '    }\n')

    def test_pair(self):
        out = self.wide(self.PAIR)
        self.assertIn('texture2d<ulong, access::read_write> cp_i_tcb_4A_rg32ui', out)
        self.assertIn('spvDescriptorSet0.cp_i_tcb_4A_rg32ui.atomic_max(uint2(_29), ulong4(as_type<ulong>(uint2(uint(_12), as_type<uint>(f(_13, 2))))));', out)
        self.assertNotIn('atomic_fetch', out)

    def test_a_lone_32_bit_atomic_fails(self):
        with self.assertRaisesRegex(ValueError, 'not the translator'):
            self.wide(self.PAIR.split('\n')[0] + '\n')

    def test_halves_on_different_texels_fail(self):
        with self.assertRaisesRegex(ValueError, 'different texels'):
            self.wide(self.PAIR.replace('(uint2(_29), as_type', '(uint2(_30), as_type'))

    def test_another_access_fails(self):
        with self.assertRaisesRegex(ValueError, 'accesses of a 64-bit atomic image'):
            self.wide(self.PAIR + '        uint4 _50 = spvDescriptorSet0.cp_i_tcb_4A_rg32ui.read(uint2(_29));\n')

    def test_32_bit_images_are_left_alone(self):
        sys.path.insert(0, str(TOOLS)); import dmsh_atomic
        src = self.DECL.replace('rg32ui', 'r32ui') + '        uint _40 = spvDescriptorSet0.cp_i_tcb_4A_r32ui.atomic_fetch_max(uint2(_29), 1u).x;\n'
        self.assertEqual(dmsh_atomic.wide_atomics(src), src)


if __name__ == '__main__':
    unittest.main()
