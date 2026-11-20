"""vol_space.free_bytes: the space a build may write is macOS's capacity for important usage (APFS purgeable space included), never less than statvfs's free
space, asked of the nearest existing folder; without that capacity (another system, a failed query) statvfs alone. No game files needed (temporary files only)."""
from pathlib import Path
import shutil
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import vol_space


class FreeBytes(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)

    def raw(self, free): return mock.patch.object(vol_space.shutil, 'disk_usage', lambda p: types.SimpleNamespace(free=free))

    def test_purgeable_space_counts(self):   # 18.8 GiB unallocated, 68 GiB for important usage: the build may write 68
        with self.raw(18 << 30), mock.patch.object(vol_space, 'important_bytes', lambda p: 68 << 30): self.assertEqual(vol_space.free_bytes(self.tmp), 68 << 30)

    def test_never_less_than_statvfs(self):
        with self.raw(20 << 30), mock.patch.object(vol_space, 'important_bytes', lambda p: 5 << 30): self.assertEqual(vol_space.free_bytes(self.tmp), 20 << 30)

    def test_statvfs_without_the_capacity(self):
        with self.raw(7 << 30), mock.patch.object(vol_space, 'important_bytes', lambda p: None): self.assertEqual(vol_space.free_bytes(self.tmp), 7 << 30)

    def test_missing_path_asks_its_existing_parent(self):
        asked = []
        with self.raw(1), mock.patch.object(vol_space, 'important_bytes', lambda p: asked.append(p) or 2): self.assertEqual(vol_space.free_bytes(self.tmp / 'a/b/c'), 2)
        self.assertEqual(asked, [self.tmp])

    def test_no_capacity_off_macos(self):
        with mock.patch.object(vol_space.sys, 'platform', 'linux'):
            vol_space._cf.cache_clear(); self.addCleanup(vol_space._cf.cache_clear)
            self.assertIsNone(vol_space.important_bytes(self.tmp))

    @unittest.skipUnless(sys.platform == 'darwin', 'the capacity for important usage is a macOS figure')
    def test_macos_reports_the_capacity(self):   # the real query: a figure for a folder and for a file, none for a path that does not exist
        (self.tmp / 'f').write_bytes(b'x')
        for p in (self.tmp, self.tmp / 'f'): self.assertGreater(vol_space.important_bytes(p) or 0, 0)
        self.assertIsNone(vol_space.important_bytes(self.tmp / 'missing'))
        self.assertGreaterEqual(vol_space.free_bytes(self.tmp), shutil.disk_usage(self.tmp).free - (1 << 30))   # the volume may change between the two reads


if __name__ == '__main__':
    unittest.main()
