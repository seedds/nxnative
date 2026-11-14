"""PKZ directory versions used by PlatinumGames, without game data."""
from pathlib import Path
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
_had = os.environ.get('DM_GAME'); os.environ.setdefault('DM_GAME', '010015100B514000')   # shader_split loads a profile at import; these tests use no game's data, so any
# profile does (tests.yml sets one for the CI); without DM_GAME, the CI's profile, set for the import only (other tests expect no default game)
import shader_split
if _had is None: del os.environ['DM_GAME']


class PkzTests(unittest.TestCase):
    def test_directory_versions(self):
        # ASTRAL CHAIN's v1 has the same entry/name layout as Bayonetta 3's v2.
        names = b'ZStandard\0shader.dat\0None\0plain.dat\0'
        for version, alignment in ((0x10000, 0), (0x20000, 0x1000)):
            with self.subTest(version=hex(version)), tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / 'sample.pkz'
                directory = (struct.pack('<IIQQQ', 10, 0, 200, 0x1000, 50)
                             + struct.pack('<IIQQQ', 26, 21, 100, 0x2000, 100))
                header = struct.pack('<4sIQIIII', b'pkzl', version, 0x2064, 2, 32, len(names), alignment)
                path.write_bytes((header + directory + names).ljust(0x2064, b'\0'))
                self.assertEqual(shader_split.pkz_entries(str(path)), [
                    ('shader.dat', 'ZStandard', 0x1000, 50, 200),
                    ('plain.dat', 'None', 0x2000, 100, 100),
                ])

    def test_unknown_version_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'unknown.pkz'
            path.write_bytes(struct.pack('<4sIQIIII', b'pkzl', 0x30000, 32, 0, 32, 0, 0))
            with self.assertRaisesRegex(SystemExit, 'PKZ version 0x30000'):
                shader_split.pkz_entries(str(path))


if __name__ == '__main__':
    unittest.main()
