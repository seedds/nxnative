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
    def test_ios_rejected_join_requires_a_new_press(self):
        source = (HOST.parent / 'ios/Sources/DmGamepad.m').read_text()
        start = source.index('static void read_pad(')
        end = source.index('// Home and Capture:', start)
        enum_start = source.index('enum { B_A')
        enum_end = source.index(';', enum_start) + 1
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / 'production_ios_read_pad.inc').write_text(source[enum_start:enum_end] + '\n' + source[start:end])
            exe = directory / 'ios_admission'
            result = subprocess.run(['xcrun', '-sdk', 'macosx', 'clang', '-fobjc-arc', '-O1', '-Wall', '-Werror',
                                     '-Wno-unused-function', '-I', str(HOST), '-I', tmp, '-o', str(exe),
                                     str(HOST / 'tests/t_controller_ios_admission_mac.m'),
                                     '-framework', 'Foundation', '-framework', 'GameController'], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('fresh press', result.stdout)

    def test_native_routes_and_device_swap(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name in ('controller_panel', 'controller_assignment', 'controller_join'):
                with self.subTest(component=name):
                    exe = Path(tmp) / name
                    result = subprocess.run(['xcrun', '-sdk', 'macosx', 'clang', '-fobjc-arc', '-O1', '-Wall', '-Werror',
                                             '-Wno-unused-function', '-o', str(exe), str(HOST / f'tests/t_{name}_mac.m'),
                                             '-framework', 'AppKit', '-framework', 'GameController', '-framework', 'QuartzCore'], capture_output=True, text=True, timeout=120)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn(' OK', result.stdout)


if __name__ == '__main__':
    unittest.main()
