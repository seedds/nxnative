#!/usr/bin/env python3
"""Maxwell LDC's 16-bit immediate (InstEmitMemory.Ldc). Added to a register it is signed and the sum 32 bits, as nvdisasm prints it and
Xenoblade Chronicles 2's pixel shaders use it (c6 at R - 8 to R - 128); alone (Ra = RZ) it is the address as it is, as Mortal Kombat 11 reads c[14][0x8448].
Sign-extended, that read's word index overflowed into the operand's slot field and became a 512 MB c15 binding the host refused. A fixed word past the 64 KiB bank
reads 0 (ConstantFolding). Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import json, os, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS)); import dmsh_chain   # noqa: E402  (the chain's MSL version)
PT, RZ = 7, 255
SCHED = 0x001f8000fc0007e0     # the scheduling word before each three instructions
NOP = 0x50b0000000070f00
EXIT = 0xe30000000007000f
# Mortal Kombat 11's own encoding (0100F2200C984000): LDC.U16 R49, c[14][0x8448]
LDC_U16 = 0xef9200e84487ff31
# Xenoblade Chronicles 2's own encoding (pixel shader 09030ba6b1d1283e): LDC.64 R4, c[6][R59 - 0x40]
LDC_64_NEG = 0xef95006ffc073b04


def op(top, shift, **fields):
    bit = dict(rd=0, a=8)
    v = top << shift | PT << 16
    for k, value in fields.items(): v |= value << bit[k]
    return v


def sts(rd, offset): return op(0xef58, 48, rd=rd, a=RZ) | 4 << 48 | offset << 20   # STS.B32 [RZ + offset], rd: keeps the result
def mov32i(rd, value): return 0x0100 << 48 | (value & 0xffffffff) << 20 | PT << 16 | 15 << 12 | rd   # MOV32I rd, value
def s2r_tid_x(rd): return op(0xf0c8, 48, rd=rd) | 0x21 << 20   # S2R rd, SR_TID.X: a value only known at run time
def with_ra(inst, ra): return inst & ~(0xff << 8) | ra << 8
def with_imm(inst, imm): return inst & ~(0xffff << 20) | imm << 20


def code(*insts):
    out = b''
    for k in range(0, len(insts), 3):
        out += struct.pack('<4Q', SCHED, *(tuple(insts[k:k + 3]) + (NOP, NOP))[:3])
    return out


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class LdcOffset(unittest.TestCase):
    def translate(self, *insts):
        with tempfile.TemporaryDirectory(prefix='ldc_offset_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(code(*insts))
            (work / 'jobs.json').write_text(json.dumps([dict(key='k', stage='cs', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=True, shared=0x1000)]))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue((work / 'k.spv').exists(), r.stdout + r.stderr)
            self.res = json.loads((work / 'k.res.json').read_text())   # the translator's own report of the shader
            import dmsh_geometry
            dmsh_geometry.flatten_sets(work / 'k.spv')
            m = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', dmsh_chain.MSL_VERSION, '--msl-argument-buffers', str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(m.returncode, 0, m.stderr)
            return m.stdout

    def fixed(self, slot):   # bytes of the slot read at fixed offsets
        return next((c['fixed'] for c in self.res['cbufs'] if c['slot'] == slot), 0)

    def test_the_game_read_alone_is_its_immediate(self):
        msl = self.translate(LDC_U16, sts(49, 0), EXIT)
        self.assertIn('cp_c14)._m0[2116].z', msl)   # 0x8448 bytes = word 8466 = vector 2116, element .z
        self.assertNotIn('cp_c15', msl)              # before: c15 at word 33,552,452
        self.assertEqual(self.fixed(14), 0x8448 + 4)
        self.assertEqual(self.fixed(15), 0)

    def test_immediates_alone_across_the_sign_bit(self):
        for imm in (0x120, 0x7ffc, 0x8000, 0xfffc):
            with self.subTest(imm=hex(imm)):
                msl = self.translate(with_imm(LDC_U16, imm), sts(49, 0), EXIT)
                self.assertIn(f'cp_c14)._m0[{imm // 16}].{"xyzw"[imm // 4 & 3]}', msl)
                self.assertEqual(self.fixed(14), imm + 4)
                self.assertEqual(self.fixed(15), 0)

    def test_added_to_a_register_the_immediate_is_signed(self):
        msl = self.translate(mov32i(59, 0x100), LDC_64_NEG, sts(4, 0), sts(5, 4), EXIT)
        self.assertIn('cp_c6)._m0[12].x', msl)   # 0x100 - 0x40 = 0xc0: word 48, vector 12
        self.assertIn('cp_c6)._m0[12].y', msl)
        self.assertEqual(self.fixed(6), 0xc0 + 8)

    def test_added_to_a_run_time_register_it_is_still_signed(self):
        msl = self.translate(s2r_tid_x(59), LDC_64_NEG, sts(4, 0), sts(5, 4), EXIT)
        self.assertIn('cp_c6)._m0[', msl)
        self.assertNotIn('65472', msl)   # 0xffc0 read as unsigned: 64 KiB away

    def test_dynamic_uniform_load_compares_rows_and_keeps_its_select(self):
        import dmsh_rewrite

        msl = self.translate(s2r_tid_x(59), LDC_64_NEG, sts(4, 0), sts(5, 4), EXIT)
        self.assertRegex(msl, r'uint\(_\d+\) < \(\(\*spvDescriptorSet0\.cp_c25\)\._m0\[0\]\.w >> 4u\)')
        self.assertNotIn('>> 2u);', msl)
        selected = dmsh_rewrite.select_guards(msl)
        self.assertRegex(selected, r'select\(0\.0, \(\*spvDescriptorSet0\.cp_c6\)\._m0\[_\d+ \? _\d+ : 0\]\[_\d+\], _\d+\)')

    def test_a_fixed_word_outside_the_bank_reads_zero(self):
        msl = self.translate(mov32i(59, 0x10), with_imm(LDC_64_NEG, 0xffe0), sts(4, 0), sts(5, 4), EXIT)   # c[6][0x10 - 0x20]
        self.assertNotIn('cp_c6', msl)
        self.assertTrue(all(c['fixed'] <= 0x10000 for c in self.res['cbufs']), self.res['cbufs'])


if __name__ == '__main__':
    unittest.main()
