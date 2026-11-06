"""Compile and run the Switch 2 BLE protocol's checks (dm/ios/tests/switch2_ios.c) on any machine; no BLE, no simulator."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]

@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('cc'), 'needs a C toolchain')
class Switch2ProtoTests(unittest.TestCase):
    def test_reported_reports_and_pad_rules(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'switch2_ios'
            r = subprocess.run(['cc', '-x', 'c', '-std=gnu11', '-Wall', '-Wextra', '-Wno-unused-parameter',
                                f'-I{REPO / "dm/ios/Sources"}', f'-I{REPO / "dm/host"}',
                                str(Path(__file__).with_name('switch2_ios.c')), '-o', str(exe)],
                               capture_output=True, text=True, timeout=300)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertNotIn('FAIL', r.stdout)
            self.assertIn('checks, 0 failed', r.stdout)

if __name__ == '__main__':
    unittest.main()
