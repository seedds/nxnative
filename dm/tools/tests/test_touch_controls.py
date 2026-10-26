"""Run the actual iOS touch math without UIKit, a GPU, or game files."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


@unittest.skipUnless(shutil.which('clang'), 'needs clang')
class TouchControlTests(unittest.TestCase):
    def test_input_math_and_runtime_settings(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'touch-controls'
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'dm/ios/tests/t_touch_controls.c'), '-lm', '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('touch controls: PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
