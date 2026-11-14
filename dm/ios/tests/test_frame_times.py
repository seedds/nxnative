"""Compile and run the launcher's frame-time recordings code (LauncherFrameTimes.swift) on macOS: parsing, the numbers, compaction."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs the macOS Swift toolchain')
class FrameTimesTests(unittest.TestCase):
    def test_parse_summary_and_compaction(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            exe = tmp / 'frame_times_check'
            c = subprocess.run(['xcrun', '-sdk', 'macosx', 'swiftc', '-swift-version', '5', '-parse-as-library', '-module-cache-path', str(tmp / 'module-cache'),
                                '-o', str(exe), str(REPO / 'dm/ios/Sources/LauncherFrameTimes.swift'), str(Path(__file__).with_name('frame_times_check.swift'))],
                               capture_output=True, text=True, timeout=300)
            self.assertEqual(c.returncode, 0, c.stdout + c.stderr)
            r = subprocess.run([str(exe), str(tmp)], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('frame times: 0 failed', r.stdout)


if __name__ == '__main__':
    unittest.main()
