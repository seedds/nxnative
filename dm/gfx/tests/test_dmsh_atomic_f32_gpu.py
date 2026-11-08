"""Production-compiled synthetic RED and returning ATOMG numeric regressions."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parent))
import test_dmsh_ampere_mem as t

FIXTURES = Path(__file__).parent / 'fixtures'
FLAGS = ['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O3', '-fpreserve-invariance', '-Xclang', '-disable-llvm-passes']


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE') and sys.platform == 'darwin' and shutil.which('xcrun'), 'needs translator and Metal')
class AtomicF32GPU(unittest.TestCase):
    def run_shader(self, source, runner, expected):
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'ios/tools'))
        import ios_metal
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            shader = out / 'probe.metal'
            shader.write_text(source)
            ios_metal.compile(shader, out / 'probe.air', out / 'probe.metallib', FLAGS)
            subprocess.run(['clang', '-fobjc-arc', '-framework', 'Foundation', '-framework', 'Metal',
                            str(FIXTURES / runner), '-o', str(out / 'probe')], check=True, capture_output=True, text=True)
            result = subprocess.run([str(out / 'probe'), str(out / 'probe.metallib')], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(expected, result.stdout)

    def test_returned_old_word_preserves_all_bits(self):
        hi, lo = t.AtomgCas.CAS
        hi = (hi & ~((31 << 23) | (15 << 9) | 255)) | (3 << 9)
        lo = (lo & ~((0xff << 24) | 0xfff)) | (8 << 24) | 0x3a8
        addrlo = (t.AtomgCas.ADDR_LO[0], (t.AtomgCas.ADDR_LO[1] & ~(255 << 16)) | (8 << 16))
        addrhi = (t.AtomgCas.ADDR_HI[0], (t.AtomgCas.ADDR_HI[1] & ~(255 << 16)) | (9 << 16))
        store = (t.ctrl(wait=0b100, stall=4) | 0x900, 0x0000040208007386)
        code = t.mov(4, 0x3fc00000) + b''.join(t.inst(*x) for x in [t.AtomgCas.ULDC, addrlo, addrhi, (hi, lo), store, t.EXIT])
        source = t.Translated().translate(code, nvn2_cs=[3, 1, 0, 0x50, 0])
        self.assertIn('atomic_compare_exchange_weak_explicit', source)
        self.assertNotIn('device atomic_float', source)
        self.assertEqual(source.count('[[id('), 1)
        self.run_shader(source, 'atomic_f32_return_gpu.m', 'ATOMG_return_checks=24 failures=0')

    def test_red_cancellation_flushes_to_negative_zero(self):
        # NVIDIA PTX specifies sign-preserving FTZ for global red.add.f32.
        # 0x00800000 + 0x80800001 is a negative subnormal, hence -0. The old
        # CAS helper forced +0; baseline equality would assert the wrong result.
        code = t.mov(5, 0x80800001) + t.inst(*t.RedF32.ULDC) + t.inst(*t.RedF32.RED) + t.inst(*t.EXIT)
        source = t.Translated().translate(code, nvn2_cs=[3, 1, 0, 0x50, 0])
        self.assertIn('atomic_fetch_add_explicit((device atomic_float*)', source)
        self.assertEqual(source.count('[[id('), 3)
        self.run_shader(source, 'atomic_f32_signed_zero_gpu.m', 'translated_RED_signed_zero_checks=14680078 failures=0')

    def test_red_bounds_contention_and_write_log(self):
        source = t.RedF32().msl()
        self.assertIn('atomic_fetch_add_explicit((device atomic_float*)', source)
        self.assertEqual(source.count('[[id('), 3)
        self.run_shader(source, 'atomic_f32_red_gpu.m', 'translated_RED_gpu_checks=14680078 failures=0')


if __name__ == '__main__':
    unittest.main()
