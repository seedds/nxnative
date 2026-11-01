#!/usr/bin/env python3
"""Ampere memory instructions through the offline translator (dmsh_xlate, arch sm86): SUATOM (0x593), the image atomic Donkey Kong Bananza's agl
technique compute shaders (agl_technique_pfx) take an append counter with. Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import json, os, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
EXIT = (0x000fea0003800000, 0x000000000000794d)   # hi, lo: EXIT
# SUATOM.ADD.U32 R12, [R3], R12 on the 1D image whose handle is word 12 of c0 (AMapC0: image slot 1), as the game encodes it but unpredicated (PT)
SUATOM = (0x002ea200001ee000, 0x00000c0c030c7593)


def inst(hi, lo): return struct.pack('<QQ', lo, hi)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class Suatom(unittest.TestCase):
    def translate(self, code, shared=0):
        with tempfile.TemporaryDirectory(prefix='ampere_mem_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(code)
            # c0 layout (QueryAmpereComputeC0Layout): no uniform or storage buffers, 3 texture handles from 0x10, image handles from 0x28 (word 12 = image slot 1), 0x40 bytes
            job = [dict(key='k', stage='cs', arch='sm86', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=True, nvn2_cs=[0, 0, 3, 0x40, 0], shared=shared)]
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

    def test_add_u32_on_a_1d_image(self):
        msl = self.translate(inst(*SUATOM) + inst(*EXIT))
        self.assertIn('atomic_fetch_add', msl)   # native (the build's options) or through spirv-cross's buffer alias (these defaults)
        self.assertIn('cp_i_tcb_4A_r32ui', msl)   # image slot 1 (c2 0x128), a 32-bit unsigned image

    def test_result_kept(self):   # the old value is the shader's (it shuffles it to the warp): the atomic's result is used, not discarded
        msl = self.translate(inst(*SUATOM) + inst(*EXIT))
        self.assertRegex(msl, r'uint \w+ = [^;]*atomic_fetch_add')


# S2R R40, SR_TID.X; STS.128 [R40.X16 + 0x500], R24 as Mario Kart World's froxel compute shader fd413f04f59c161e encodes it (stride 78..80 = 3, X16); EXIT
S2R_TID = (0x000fe20000002100, 0x0000000000287919)
STS128_X16 = (0x000fe2000000cc00, 0x0005001828007388)
STRIDE = 3 << 14   # the stride field, bits 78..80 of the instruction (half-open: 78 and 79), in the high word


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class SharedStride(unittest.TestCase):
    translate = Suatom.translate

    def store(self, hi):
        return self.translate(inst(*S2R_TID) + inst(hi, STS128_X16[1]) + inst(*EXIT), shared=2304)

    def test_x16_scales_the_register(self):   # thread n stores its vec4 at byte 0x500 + 16 n: the register is shifted left by 4
        self.assertRegex(self.store(STS128_X16[0]), r'<< (uint\()?4')

    def test_x1_keeps_the_register(self):
        self.assertNotRegex(self.store(STS128_X16[0] & ~STRIDE), r'<< (uint\()?4')


# MEMBAR.SC with scope 5 (76..79) as Mario Kart World's DrivePass compute shader 0f31fb64c991a12b encodes it (opcode 0x992), between its STS and BAR.SYNC
MEMBAR = (0x000fec0000005000, 0x0000000000007992)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class Membar(unittest.TestCase):
    translate = Suatom.translate

    def test_membar_orders_device_memory(self):
        self.assertIn('threadgroup_barrier(mem_flags::mem_device', self.translate(inst(*MEMBAR) + inst(*EXIT)))


if __name__ == '__main__':
    unittest.main()
