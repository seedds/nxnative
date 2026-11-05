"""The real Mac runtime sheet and swap adapter, exercised without a game or paired hardware."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HOST = Path(__file__).resolve().parents[3] / 'dm/host'


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs the macOS toolchain and AppKit')
class ControllerRuntimeTests(unittest.TestCase):
    def test_native_routes_and_device_swap(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name in ('controller_panel', 'controller_assignment'):
                with self.subTest(component=name):
                    exe = Path(tmp) / name
                    result = subprocess.run(['xcrun', '-sdk', 'macosx', 'clang', '-fobjc-arc', '-O1', '-Wall', '-Werror',
                                             '-Wno-unused-function', '-o', str(exe), str(HOST / f'tests/t_{name}_mac.m'),
                                             '-framework', 'AppKit', '-framework', 'GameController'], capture_output=True, text=True, timeout=120)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn(' OK', result.stdout)


if __name__ == '__main__':
    unittest.main()
