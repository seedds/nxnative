#!/usr/bin/env python3
"""Storage bounds through the offline translator (TranslationFlags.MetalStorageBounds, GitHub issue #2618): a storage buffer whose address the shader takes from a game uniform slot
(SBUFU) or the push constants (SBUFQ) has no size, so each access compares its word index with the memory the host bound, from the length table in bank 27 (dmsh_abi.h DMSH_SBLEN_BANK):
a load gives 0 outside it, a store or an atomic is skipped. NVN's own storage buffer table (c[0], with a size) is left as it was. The reflection gives the table its kind and each
bounded binding its slot in it. PRAGMATA's GI voxeliser ef4ad01151918469 read its surfel colours past the one run mapped at the base of a sparse pool, and the other memory there washed
the R53 hangar in colour. Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import json, os, re, subprocess, sys, tempfile, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_dmsh_maxwell_ftz import EXIT, PT, RZ, TOOLS, code, sts   # noqa: E402
from test_dmsh_nvn2_buffers import EXIT as A_EXIT, SUATOM_R5, ULDC64_C0_10, inst   # noqa: E402
from test_dmsh_storage_offset import iadd_c, ldg, mov_c, shl_i   # noqa: E402
from test_dmsh_hull_cbe import stg   # noqa: E402
sys.path.insert(0, str(TOOLS))
import dmsh_abi as abi   # noqa: E402

# Ampere (hi, lo)
S2R_R5 = (0x000e260000002100, 0x0000000000057919)       # S2R R5, SR_TID.X
LDG_R8_R5_UR4 = (0x000ea200081e8900, 0x0000000405087981)   # LDG.E R8, [R5 + UR4]
MOV_R20_UR4 = (0x000fe20008000000, 0x0000000400147c02)  # MOV R20, UR4
MOV_R21_UR5 = (0x000fe20008000000, 0x0000000500157c02)  # MOV R21, UR5
ATOMG_ADD = (0x000fe200001ee100, 0x00000013140673a8)    # ATOMG.E.ADD.STRONG.GPU PT, R6, [R20], R19 (test_dmsh_ampere_alu.py's INC with operation 0)
def waiting(i): return (i[0] | 0x3f << 52, i[1])         # waits on every scoreboard
def suatom(r): return (SUATOM_R5[0], (SUATOM_R5[1] & ~(0xff << 32)) | r << 32)   # adds r to an image texel: keeps r


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class StorageBounds(unittest.TestCase):
    def translate(self, job, code_bytes):   # the translator's report, the shader's Metal source and its reflection
        with tempfile.TemporaryDirectory(prefix='storage_bounds_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(code_bytes)
            (work / 'jobs.json').write_text(json.dumps([dict(key='k', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=True, **job)]))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            res = json.loads((work / 'k.res.json').read_text())
            import dmsh_geometry
            dmsh_geometry.flatten_sets(work / 'k.spv')
            r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '30100', '--msl-argument-buffers', '--msl-texture-buffer-native', str(work / 'k.spv')],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            import dmsh_reflect
            return res, r.stdout, dmsh_reflect.reflect('k', job['stage'], r.stdout, res)

    def table(self, msl):   # the length table's two words for a storage slot: before, total
        return r'\(\*spvDescriptorSet0\.cp_c%d\)\._m0\[\d+\]\.[xyzw]' % abi.SBLEN_BANK

    def test_a_uniform_address_is_bounded_and_the_driver_table_is_not(self):
        # R2:R3 = c[3][0x10] + (c[3][4] << 2), a storage buffer in the game's uniform slot 0: read, then written back. R6:R7 = c[0][0x110] + the same, NVN's storage slot 0
        body = (mov_c(1, 3, 0x4), shl_i(1, 1, 2), iadd_c(2, 1, 3, 0x10, cc=True), iadd_c(3, RZ, 3, 0x14, x=True), ldg(4, 2), stg(2, 4),
                iadd_c(6, 1, 0, 0x110, cc=True), iadd_c(7, RZ, 0, 0x114, x=True), ldg(8, 6), sts(8, 8), EXIT)
        res, msl, binds = self.translate(dict(stage='cs', shared=0x1000), code(*body))
        flags = {(s['cb_slot'], s['cb_offset']): s['flags'] for s in res['sbufs']}
        self.assertEqual(flags.get((3, 0x10 // 4)), 3, flags)   # written, bounded
        self.assertEqual(flags.get((0, 0x110 // 4)), 0, flags)   # NVN's table: a size of its own, unbounded
        t = self.table(msl)
        # the load: inside, its word, else 0; the store: only inside
        self.assertRegex(msl, r'bool (_\d+) = \(uint\((_\d+)\) \+ %s\) < %s;' % (t, t))
        self.assertRegex(msl, r'(_\d+) \? \(\*spvDescriptorSet0\.cp_s\d+\)\._m0\[\1 \? _\d+ : 0\] : 0u')
        self.assertRegex(msl, r'if \(_\d+\)\s*\{\s*\(\*spvDescriptorSet0\.cp_s\d+\)\._m0\[_\d+ \? _\d+ : 0\] = ')
        self.assertEqual(len(re.findall(r'< %s' % t, msl)), 2, msl)   # the load and the store; not the driver table's buffer
        kinds = {b['kind'] for b in binds}
        self.assertIn(abi.KIND_SBLEN, kinds)
        ubo = [b for b in binds if b['kind'] == abi.KIND_SBUF_UBO]
        self.assertEqual(len(ubo), 1); self.assertIn('sbx', ubo[0])
        self.assertTrue(all('sbx' not in b for b in binds if b['kind'] == abi.KIND_SBUF))

    def test_a_pushed_address_load_and_atomic_are_bounded(self):
        # c0 as test_dmsh_nvn2_buffers.py's: push block 1 at 0x10 holds one buffer entry (c[24] + 0x100). A load at the thread's offset from it, and an atomic add at its start
        body = (inst(*ULDC64_C0_10) + inst(*S2R_R5) + inst(*waiting(LDG_R8_R5_UR4)) + inst(*MOV_R20_UR4) + inst(*MOV_R21_UR5) + inst(*ATOMG_ADD) +
                inst(*waiting(suatom(8))) + inst(*waiting(suatom(6))) + inst(*A_EXIT))
        res, msl, binds = self.translate(dict(stage='cs', arch='sm86', nvn2_cs=[1, 0, 0, 0x40, 0, 0, 0, 0x10]), body)
        self.assertEqual([(s['cb_slot'], s['cb_offset'], s['flags']) for s in res['sbufs']], [(24, 0x100 // 4, 3)])
        self.assertRegex(msl, r'(_\d+) \? \(\*spvDescriptorSet0\.cp_s0\)\._m0\[\1 \? _\d+ : 0\] : 0u')
        # the atomic runs inside only, and gives 0 outside
        self.assertRegex(msl, r'if \(_\d+\)\s*\{\s*uint _\d+ = atomic_fetch_add_explicit\(\(device atomic_uint\*\)&\(\*spvDescriptorSet0\.cp_s0\)\._m0\[_\d+ \? \w+ : 0\]')
        self.assertRegex(msl, r'else\s*\{\s*_\d+ = 0u;\s*\}')
        push = [b for b in binds if b['kind'] == abi.KIND_SBUF_PUSH]
        self.assertEqual([b.get('sbx') for b in push], [0])

    @unittest.skipUnless(os.environ.get('DMSH_TEST_METAL'), 'set DMSH_TEST_METAL=1 to compile with the Metal compiler')
    def test_it_compiles(self):
        body = (inst(*ULDC64_C0_10) + inst(*S2R_R5) + inst(*waiting(LDG_R8_R5_UR4)) + inst(*MOV_R20_UR4) + inst(*MOV_R21_UR5) + inst(*ATOMG_ADD) +
                inst(*waiting(suatom(8))) + inst(*waiting(suatom(6))) + inst(*A_EXIT))
        _, msl, _ = self.translate(dict(stage='cs', arch='sm86', nvn2_cs=[1, 0, 0, 0x40, 0, 0, 0, 0x10]), body)
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'k.metal'; p.write_text(msl)
            r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-std=metal3.1', '-c', str(p), '-o', str(Path(d) / 'k.air')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == '__main__':
    unittest.main()
