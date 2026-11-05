#!/usr/bin/env python3
# tree_tar.py: a folder becomes one tar and comes back the same, and every state a stopped build can leave is taken up by the next one.
import fcntl, os, shutil, subprocess, sys, tempfile, time, unittest
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import tree_tar


class TreeTar(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='tree_tar_'))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.addCleanup(tree_tar._unlock, self.root / 'x')
        self.d = self.root / 'x'; self.tar = tree_tar.tar_of(self.d); self.lines = []

    def fill(self):
        (self.d / 'a/b').mkdir(parents=True); (self.d / 'a/b/f').write_bytes(b'data'); (self.d / 'top').write_text('t')
        os.utime(self.d / 'a/b/f', ns=(1_700_000_000_123_456_000, 1_700_000_000_123_456_000))

    def names(self): return sorted(p.name for p in self.root.iterdir())

    def test_round_trip_keeps_bytes_and_times(self):
        self.fill()
        tree_tar.pack(self.d, self.lines.append)
        self.assertEqual(self.names(), ['x.tar'])
        self.assertTrue(tree_tar.exists(self.d))
        self.assertTrue(tree_tar.unpack(self.d, self.lines.append))
        self.assertEqual(self.names(), ['x'])
        self.assertEqual((self.d / 'a/b/f').read_bytes(), b'data')
        self.assertEqual((self.d / 'a/b/f').stat().st_mtime_ns, 1_700_000_000_123_456_000)   # Xcode's incremental build and prune_shader_cache compare times
        self.assertEqual(self.lines, [])

    def test_nothing_yet(self):
        self.assertFalse(tree_tar.exists(self.d))
        self.assertFalse(tree_tar.unpack(self.d, self.lines.append))
        tree_tar.pack(self.d, self.lines.append)
        self.assertEqual(self.names(), [])

    def test_a_folder_from_before_is_used_then_packed(self):
        self.fill()
        self.assertTrue(tree_tar.unpack(self.d, self.lines.append))
        self.assertEqual(self.names(), ['x'])

    def test_stop_with_folder_and_tar_keeps_the_folder(self):
        self.fill(); self.tar.write_bytes(b'older')
        self.assertTrue(tree_tar.unpack(self.d, self.lines.append))
        self.assertEqual(self.names(), ['x'])
        self.assertEqual((self.d / 'top').read_text(), 't')

    def test_stop_while_removing_after_a_pack(self):
        self.fill(); tree_tar.pack(self.d, self.lines.append)
        (self.root / 'x.gone/half').mkdir(parents=True)
        self.assertTrue(tree_tar.unpack(self.d, self.lines.append))
        self.assertEqual(self.names(), ['x'])

    def test_stop_during_unpack_or_pack_leaves_nothing_used(self):
        self.fill(); tree_tar.pack(self.d, self.lines.append)
        (self.root / 'x.unpack/half').mkdir(parents=True); Path(f'{self.tar}.part').write_bytes(b'half')
        self.assertTrue(tree_tar.unpack(self.d, self.lines.append))
        self.assertEqual(self.names(), ['x'])
        self.assertEqual((self.d / 'a/b/f').read_bytes(), b'data')

    def test_a_damaged_tar_only_costs_the_cache(self):
        self.tar.write_bytes(b'this is not a tar file')
        self.assertFalse(tree_tar.unpack(self.d, self.lines.append))
        self.assertEqual(self.names(), [])
        self.assertEqual(len(self.lines), 1); self.assertTrue(self.lines[0].startswith('[RED] '))

    def test_a_failed_pack_keeps_the_folder(self):
        self.fill(); self.tar.mkdir()   # os.replace onto a folder fails, as a full drive fails the tar
        tree_tar.pack(self.d, self.lines.append)
        self.assertEqual((self.d / 'top').read_text(), 't')
        self.assertFalse(Path(f'{self.tar}.part').exists())
        self.assertEqual(len(self.lines), 1); self.assertTrue(self.lines[0].startswith('[RED] '))

    def test_second_unpack_in_one_process_does_not_wait_for_itself(self):   # Spyder: the next F5 after a build that raised
        self.fill()
        tree_tar.unpack(self.d, self.lines.append); tree_tar.unpack(self.d, self.lines.append)
        self.assertEqual(self.lines, [])

    def test_a_folder_in_use_by_another_build_is_left_to_it(self):
        self.fill()
        fd = os.open(self.root, os.O_RDONLY); self.addCleanup(os.close, fd)
        fcntl.flock(fd, fcntl.LOCK_EX)   # another open file description: as another process's lock
        tree_tar.pack(self.d, self.lines.append)
        self.assertEqual(self.names(), ['x'])
        fcntl.flock(fd, fcntl.LOCK_UN)
        tree_tar.pack(self.d, self.lines.append)
        self.assertEqual(self.names(), ['x.tar'])


class PackQuiet(unittest.TestCase):
    OLD = time.time() - 30 * 86400

    def setUp(self):
        self.w = Path(tempfile.mkdtemp(prefix='tree_tar_work_')); self.addCleanup(shutil.rmtree, self.w, True); self.lines = []

    def folder(self, name, files=100):
        d = self.w / name / 'sub'; d.mkdir(parents=True)
        for i in range(files): (d / f'f{i}').write_text('x')
        return self.w / name

    def run_old(self, **k):   # now is what the folders' ctimes are, which a test cannot set: ask for a quiet time of less than a moment
        time.sleep(0.05)
        return tree_tar.pack_quiet(self.w, self.lines.append, days=0.0000001, **k)

    def test_packs_a_quiet_folder_and_keeps_its_attributes(self):
        d = self.folder('issue1')
        subprocess.run(['xattr', '-w', 'org.d2rmobile.guest-name', 'Foo.txt', d / 'sub/f0'], check=True)
        self.assertEqual(self.run_old(), ['issue1'])
        self.assertEqual(sorted(p.name for p in self.w.iterdir()), ['issue1.tar'])
        (self.w / 'back').mkdir()
        subprocess.run(['tar', '--xattrs', '-xf', self.w / 'issue1.tar', '-C', self.w / 'back'], check=True)
        self.assertEqual(subprocess.run(['xattr', '-p', 'org.d2rmobile.guest-name', self.w / 'back/sub/f0'], capture_output=True, text=True).stdout.strip(), 'Foo.txt')

    def test_leaves_regress_small_recent_and_an_existing_tar(self):
        self.folder('regress'); self.folder('small', 99); self.folder('kept'); (self.w / 'kept.tar').write_bytes(b'mine')
        self.assertEqual(self.run_old(), [])
        self.assertEqual((self.w / 'kept.tar').read_bytes(), b'mine')
        self.assertTrue((self.w / 'kept').is_dir()); self.assertTrue((self.w / 'regress').is_dir()); self.assertTrue((self.w / 'small').is_dir())

    def test_a_folder_with_a_recent_file_stays(self):
        self.folder('busy')
        self.assertEqual(tree_tar.pack_quiet(self.w, self.lines.append), [])   # 14 days: everything here is newer
        self.assertTrue((self.w / 'busy').is_dir())

    def test_no_room_stays_a_folder(self):
        self.folder('big')
        with mock.patch.object(tree_tar, 'QUIET_FREE', 2**62): self.assertEqual(self.run_old(), [])
        self.assertTrue((self.w / 'big').is_dir()); self.assertIn('no room', self.lines[0])

    def test_no_work_folder(self):
        self.assertEqual(tree_tar.pack_quiet(self.w / 'absent', self.lines.append), [])


if __name__ == '__main__': unittest.main()
