#!/usr/bin/env python3
"""Maxwell LDC's immediate is an unsigned byte offset from the address register. Read as signed (Imm16ToSInt), a shader's read at 0x8448 above RZ became
0xFFFF8448, its word index 0x3FFFE112 overflowed the operand's 5-bit slot field, and the read landed in c15 at word 33,552,452: the host refused the 512 MB binding
Mortal Kombat 11's shaders then declared (27 shaders, 11 distinct). A fixed read past the 64 KiB constant buffer reads 0 and does not stretch its slot, as Maxwell
bounds every read by the size the game bound. Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import json, os, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
PT, RZ = 7, 255
SCHED = 0x001f8000fc0007e0     # the scheduling word before each three instructions
NOP = 0x50b0000000070f00
EXIT = 0xe30000000007000f
# Mortal Kombat 11's own encoding (0100F2200C984000, ps_asset.bin shader ed801038e0691ea2 +0x2a20): LDC.U16 R49, c[14][0x8448]
LDC_U16 = 0xef9200e84487ff31
# the same read as LDC.64 R40, c[14][0xfffc]: its second word (16384) is the first past the end of the block
LDC_64_END = 0xef9500efffc7ff28


def op(top, shift, **fields):
    bit = dict(rd=0, a=8, b=20, guard=16)
    v = top << shift | PT << 16
    for k, value in fields.items(): v |= value << bit[k]
    return v


def sts(rd, offset): return op(0xef58, 48, rd=rd, a=RZ) | 4 << 48 | offset << 20   # STS.B32 [RZ + offset], rd: keeps the result


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
            sys.path.insert(0, str(TOOLS)); import dmsh_geometry
            dmsh_geometry.flatten_sets(work / 'k.spv')
            r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', '--msl-argument-buffers', str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            return r.stdout

    def fixed(self, slot):
        return next((c['fixed'] for c in self.res['cbufs'] if c['slot'] == slot), 0)

    def test_the_immediate_is_an_unsigned_byte_offset(self):
        msl = self.translate(LDC_U16, sts(49, 0), EXIT)
        self.assertIn('._m0[2116].z', msl)          # 0x8448 bytes = word 8466 = vec 2116, element .z of c14
        self.assertNotIn('_m0[33552452]', msl)      # the signed read: c15 at word 33,552,452
        self.assertNotIn('cp_c15', msl)
        self.assertEqual(self.fixed(14), 33868)     # 8467 words, the read and the three vectors before it
        self.assertEqual(self.fixed(15), 0)

    def test_unsigned_offset_boundaries(self):
        for offset in (0x120, 0x7ffc, 0x8000, 0xfffc):
            with self.subTest(offset=hex(offset)):
                instruction = (LDC_U16 & ~(0xffff << 20)) | (offset << 20)
                msl = self.translate(instruction, sts(49, 0), EXIT)
                component = 'xyzw'[(offset // 4) & 3]
                self.assertIn(f'cp_c14)._m0[{offset // 16}].{component}', msl)
                self.assertEqual(self.fixed(14), offset + 4)
                self.assertEqual(self.fixed(15), 0)

    def test_address_register_is_added_to_the_unsigned_immediate(self):
        for address, expected in ((4, 0x844c), (-4, 0x8444)):
            with self.subTest(address=address):
                mov_r0 = (0x0100 << 48) | ((address & 0xffffffff) << 20) | (PT << 16) | (15 << 12)
                instruction = LDC_U16 & ~(RZ << 8)  # source R0 instead of RZ
                msl = self.translate(mov_r0, instruction, sts(49, 0), EXIT)
                component = 'xyzw'[(expected // 4) & 3]
                self.assertIn(f'cp_c14)._m0[{expected // 16}].{component}', msl)
                self.assertEqual(self.fixed(14), expected + 4)
                self.assertEqual(self.fixed(15), 0)

    def test_a_fixed_read_past_the_block_reads_zero(self):
        msl = self.translate(LDC_64_END, sts(40, 0), sts(41, 4), EXIT)
        self.assertIn('._m0[4095].w', msl)          # the last word of the block's last vector
        self.assertNotIn('._m0[4096]', msl)
        self.assertEqual(self.fixed(14), 0x10000)   # the whole block, not a word past it


if __name__ == '__main__':
    unittest.main()
