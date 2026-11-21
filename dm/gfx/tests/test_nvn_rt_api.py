"""Compile the actual RT HLE regions with test-only CPU backend/record spies."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
NVN = HERE.parent / 'nvn'


class RayTracingCpuApi(unittest.TestCase):
    def test_builder_group_abi_and_record_time_snapshot(self):
        clang = shutil.which('clang')
        if not clang:
            self.skipTest('needs a C compiler')
        source = (NVN / 'hle_nvn.c').read_text()
        # Compile these implementation regions verbatim, avoiding the unrelated
        # in-progress Metal backend and all production host object files.
        dispatch = source[source.index('// NVN2 ray dispatch:'):source.index('NVN(nvnCommandBufferBarrier,')]
        builder = source[source.index('// NVN2 program builders retain'):source.index('// NVN2: sizing writes')]
        fixture = (HERE / 'rt_api/api.c').read_text().replace('/* PRODUCTION_HLE_RT */', dispatch + builder)
        with tempfile.TemporaryDirectory(prefix='dm-rt-api-') as tmp:
            root = Path(tmp)
            test = root / 'api.c'
            test.write_text(fixture)
            binary = root / 'api'
            compile_result = subprocess.run([clang, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                                             '-I', str(NVN), '-I', str(NVN / 'include'), str(test),
                                             str(NVN / 'nvn_capture.c'), '-o', str(binary)],
                                            capture_output=True, text=True)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout + compile_result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('0 failures', result.stdout)
            print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
