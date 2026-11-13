#!/usr/bin/env python3
"""A storage buffer access at a buffer address plus a uniform word << 4, through the offline translator: the shifted word is the offset, not a bindless constant buffer handle
(GlobalToStorage.cs TryUnwrapHandle). EA SPORTS FC 27's vertex shaders (Maxwell) index their bone matrices so; with the word itself as the offset they read the wrong element and players
lost their heads. Maxwell code has no bindless constant buffers, so there a word << 4 is never a handle; Ampere code takes it for one only as the base, and LEA adds an index << 4 to a
buffer address the same way. Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import json, os, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_dmsh_maxwell_ftz import EXIT, PT, RZ, TOOLS, code, sts   # noqa: E402
from test_dmsh_nvn2_buffers import EXIT as A_EXIT, MOV_R6_C3_10, SUATOM_R5, ULDC64_C0_10   # noqa: E402


def mov_c(rd, slot, offset): return 0x4c98 << 48 | 0xf << 39 | slot << 34 | offset // 4 << 20 | PT << 16 | rd                                 # MOV rd, c[slot][offset]
def shl_i(rd, a, n): return 0x3848 << 48 | n << 20 | PT << 16 | a << 8 | rd                                                                 # SHL rd, a, n
def iadd_c(rd, a, slot, offset, cc=False, x=False): return 0x4c10 << 48 | cc << 47 | x << 43 | slot << 34 | offset // 4 << 20 | PT << 16 | a << 8 | rd   # IADD[.CC][.X] rd, a, c[slot][offset]
def ldg(rd, a): return 0xeed0 << 48 | 4 << 48 | 1 << 45 | PT << 16 | a << 8 | rd                                                            # LDG.E.32 rd, [a]


# Ampere (hi, lo)
LEA_R2_R6_UR4_4 = (0x000fc200078000ff | 4 << 11, 0x0000000406027c11)   # LEA R2, P0, R6, UR4, 0x4: (R6 << 4) + UR4
MOV_R3_UR5 = (0x000fe20008000000, 0x0000000500037c02)                 # MOV R3, UR5: the address's high word
LDG_R5_R2 = (0x000ea200001e8900, 0x0000000002057981)                  # LDG.E R5, [R2.64]


def inst(hi, lo): return struct.pack('<QQ', lo, hi)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class StorageOffset(unittest.TestCase):
    def translate(self, job, code_bytes):   # the translator's report and the shader's Metal source
        with tempfile.TemporaryDirectory(prefix='storage_offset_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(code_bytes)
            (work / 'jobs.json').write_text(json.dumps([dict(key='k', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=True, **job)]))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            res = json.loads((work / 'k.res.json').read_text())
            sys.path.insert(0, str(TOOLS)); import dmsh_geometry
            dmsh_geometry.flatten_sets(work / 'k.spv')
            r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', '--msl-argument-buffers', str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            return res, r.stdout

    def test_a_shifted_uniform_word_stays_the_offset(self):
        body = (mov_c(1, 3, 0x4), shl_i(1, 1, 4), iadd_c(2, 1, 0, 0x110, cc=True), iadd_c(3, RZ, 0, 0x114, x=True), ldg(4, 2), sts(4, 8), EXIT)
        res, msl = self.translate(dict(stage='cs', shared=0x1000), code(*body))
        self.assertIn((0, 0x110 // 4), [(s['cb_slot'], s['cb_offset']) for s in res['sbufs']])   # the storage buffer at c[0] + 0x110
        self.assertIn('<< 4', msl, msl[:3000])   # the element offset c[3][4] << 4, which the read's word index (>> 2) is taken from

    def test_an_ampere_lea_index_stays_the_offset(self):
        # c0 as test_dmsh_nvn2_buffers.py's: push block 1 at 0x10 holds one buffer entry, mapped to c[24] + 0x100. The address is that entry + (c[3][0x10] << 4)
        body = inst(*ULDC64_C0_10) + inst(*MOV_R6_C3_10) + inst(*LEA_R2_R6_UR4_4) + inst(*MOV_R3_UR5) + inst(*LDG_R5_R2) + inst(*SUATOM_R5) + inst(*A_EXIT)
        res, msl = self.translate(dict(stage='cs', arch='sm86', nvn2_cs=[1, 0, 0, 0x40, 0, 0, 0, 0x10]), body)
        sbufs = [(s['cb_slot'], s['cb_offset'], s.get('handle', False)) for s in res['sbufs']]
        self.assertIn((24, 0x100 // 4), [s[:2] for s in sbufs], sbufs)   # the pushed entry is the base
        self.assertNotIn((3, 0x10 // 4), [s[:2] for s in sbufs], sbufs)   # not c[3][0x10] as a handle
        self.assertIn('<< 4', msl, msl[:3000])


if __name__ == '__main__':
    unittest.main()
