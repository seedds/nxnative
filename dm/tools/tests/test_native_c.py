"""native_c.load: a C source built at first use into a per-user cache and loaded through ctypes, built again only when its source changes, and None (with the
caller's Python code running instead) where it cannot be built: a failed build, a cache folder others can write into, another platform, a Mac without Xcode."""
import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import native_c

ADD = 'int dm_add(int a, int b) { return a + b; }\n'


def compiler():   # what load needs to build: Xcode's clang on a Mac, cc on Linux
    if sys.platform == 'darwin': return subprocess.run(['xcode-select', '-p'], capture_output=True).returncode == 0
    return sys.platform.startswith('linux') and subprocess.run(['sh', '-c', 'command -v cc'], capture_output=True).returncode == 0


class NativeC(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name); self.cache = self.dir / 'cache'; self.cache.mkdir(mode=0o700)
        self.src = self.dir / 'add.c'; self.src.write_text(ADD)

    def built(self):   # the library, built or from the cache
        lib = native_c.load(self.src, self.cache)
        if lib is None: self.fail(f'{self.src.name} did not build')
        return lib

    @unittest.skipUnless(compiler(), 'no C compiler')
    def test_built_once_per_source(self):
        self.assertEqual(self.built().dm_add(2, 3), 5)
        with mock.patch.object(native_c.subprocess, 'run', side_effect=AssertionError('built again')):
            self.assertEqual(self.built().dm_add(4, 5), 9)   # from the cache
        self.src.write_text(ADD.replace('a + b', 'a - b'))
        self.assertEqual(self.built().dm_add(4, 5), -1)
        self.assertEqual(len(list(self.cache.iterdir())), 2, 'one library for each source, and no temporary file left')

    @unittest.skipUnless(compiler(), 'no C compiler')
    def test_failed_build(self):
        self.src.write_text('int dm_add(int a, int b) { return a + ; }\n')
        with contextlib.redirect_stderr(io.StringIO()) as err: self.assertIsNone(native_c.load(self.src, self.cache))
        self.assertIn('the C build failed, the Python code runs', err.getvalue())
        self.assertEqual(list(self.cache.iterdir()), [], 'no temporary file left')

    @unittest.skipUnless(hasattr(os, 'getuid'), 'no user IDs on this platform')
    def test_cache_others_can_write(self):
        shared = self.dir / f'dm-native-{os.getuid()}'; shared.mkdir(); shared.chmod(0o777)
        with mock.patch.object(native_c.tempfile, 'gettempdir', return_value=str(self.dir)), mock.patch.object(native_c.subprocess, 'run', side_effect=AssertionError('built')):
            self.assertIsNone(native_c.cache_dir())
            self.assertIsNone(native_c.load(self.src))
        shared.chmod(0o700)
        with mock.patch.object(native_c.tempfile, 'gettempdir', return_value=str(self.dir)): self.assertEqual(native_c.cache_dir(), shared)

    def test_other_platform(self):
        with mock.patch.object(native_c.sys, 'platform', 'win32'), mock.patch.object(native_c.subprocess, 'run', side_effect=AssertionError('built')):
            self.assertIsNone(native_c.load(self.src, self.cache))

    def test_mac_without_xcode(self):
        """xcode-select -p fails: nothing is built (xcrun would offer to install the command line tools) and nothing is printed"""
        no = subprocess.CompletedProcess(['xcode-select', '-p'], 2)
        with mock.patch.object(native_c.sys, 'platform', 'darwin'), mock.patch.object(native_c.subprocess, 'run', return_value=no) as run, contextlib.redirect_stderr(io.StringIO()) as err:
            self.assertIsNone(native_c.load(self.src, self.cache))
        self.assertEqual([c.args[0] for c in run.call_args_list], [['xcode-select', '-p']]); self.assertEqual(err.getvalue(), '')


if __name__ == '__main__':
    unittest.main()
