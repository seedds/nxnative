"""Compaction and the following start use fresh processes and disk-built shaders."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(__name__ == '__main__', 'run test_pso_archive.py with the native t_pso_archive path')
class ArchiveRestartTests(unittest.TestCase):
    def test_cold_start_then_next(self):
        with tempfile.TemporaryDirectory(prefix='pso-cold-') as folder:
            env = dict(os.environ, DM_PSO_MTL4='1', MTL_DEBUG_LAYER='1')
            for name in ('DM_GPU_CAPTURE', 'MTL_CAPTURE_ENABLED', 'DM_TEST_ARCHIVE_CONSTANTS', 'DM_TEST_ARCHIVE_MIXED'):
                env.pop(name, None)
            # nine new bases with a timer flush after each: a file per full period of 4 and one for the rest, which the next start does not compact
            for phase, files in (('seed', 3), ('next', 3)):
                result = subprocess.run([executable, '--cold-' + phase, folder],
                                        env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, phase + '\n' + result.stdout + result.stderr)
                self.assertEqual(len(list(Path(folder).glob('*.mtl4a'))), files, phase)

    def test_old_index_removed(self):
        with tempfile.TemporaryDirectory(prefix='pso-oldix-') as folder:
            env = dict(os.environ, DM_PSO_MTL4='1', MTL_DEBUG_LAYER='1')
            for name in ('DM_GPU_CAPTURE', 'MTL_CAPTURE_ENABLED', 'DM_TEST_ARCHIVE_CONSTANTS', 'DM_TEST_ARCHIVE_MIXED'):
                env.pop(name, None)
            def run(phase):
                result = subprocess.run([executable, '--cold-' + phase, folder], env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, phase + '\n' + result.stdout + result.stderr)
                return result.stdout + result.stderr
            run('seed')
            # the archive as the keys by library file name left it (DMPSOIX1), a prebuild file among it: no key a lookup makes leads to them, so a start removes them, the
            # prebuild's stamp with its file, and builds the bases again once into indexed files; the start after takes them all
            root = Path(folder)
            first = sorted(root.glob('*.mtl4a'))[0]
            (root / 'p00009.mtl4a').write_bytes(first.read_bytes())
            (root / 'p00009.keys').write_bytes(first.with_suffix('.keys').read_bytes())
            (root / 'prebuilt.tsv').write_text('chain\tx\n')
            for keys in root.glob('*.keys'):
                data = keys.read_bytes()
                self.assertEqual(data[:8], b'DMPSOIX2')
                keys.write_bytes(b'DMPSOIX1' + data[8:])
            log = run('seed')   # the cold start's checks: every base compiled, none from the archive
            self.assertIn('4 files indexed by library file names removed (1 of them the prebuild', log)
            self.assertFalse((root / 'p00009.mtl4a').exists() or (root / 'prebuilt.tsv').exists())
            self.assertEqual(len(list(root.glob('*.mtl4a'))), 3)
            self.assertTrue(all(k.read_bytes()[:8] == b'DMPSOIX2' for k in root.glob('*.keys')))
            run('next')   # every base from the archive

    def test_unusable_file_removed(self):
        with tempfile.TemporaryDirectory(prefix='pso-unusable-') as folder:
            env = dict(os.environ, DM_PSO_MTL4='1', MTL_DEBUG_LAYER='1')
            for name in ('DM_GPU_CAPTURE', 'MTL_CAPTURE_ENABLED', 'DM_TEST_ARCHIVE_CONSTANTS', 'DM_TEST_ARCHIVE_MIXED'):
                env.pop(name, None)
            def run(phase):
                result = subprocess.run([executable, '--cold-' + phase, folder], env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, phase + '\n' + result.stdout + result.stderr)
                return result.stdout + result.stderr
            run('seed')
            # a prebuild file Metal will not open, as every file is after a major macOS update: the start removes it with its index and the prebuild's stamp, so the
            # prebuild runs again instead of leaving its bases to the draws; the indexed files still serve every base
            root = Path(folder)
            first = sorted(root.glob('*.mtl4a'))[0]
            (root / 'p00009.mtl4a').write_bytes(b'not a pipeline archive')
            (root / 'p00009.keys').write_bytes(first.with_suffix('.keys').read_bytes())
            (root / 'prebuilt.tsv').write_text('chain\tx\n')
            log = run('next')   # every base from the archive
            self.assertIn('p00009.mtl4a not usable', log)
            self.assertFalse((root / 'p00009.mtl4a').exists() or (root / 'p00009.keys').exists() or (root / 'prebuilt.tsv').exists())
            self.assertEqual(len(list(root.glob('*.mtl4a'))), 3)

    def test_cold_start_cut_short(self):
        with tempfile.TemporaryDirectory(prefix='pso-cold-cut-') as folder:
            env = dict(os.environ, DM_PSO_MTL4='1', MTL_DEBUG_LAYER='1')
            for name in ('DM_GPU_CAPTURE', 'MTL_CAPTURE_ENABLED', 'DM_TEST_ARCHIVE_CONSTANTS', 'DM_TEST_ARCHIVE_MIXED'):
                env.pop(name, None)
            # ended in its prewarm after 6 new bases: the full period's file alone; the next start takes those 4 and builds the other 5
            for phase, files in (('cut-seed', 1), ('after-cut', 3)):
                result = subprocess.run([executable, '--cold-' + phase, folder],
                                        env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, phase + '\n' + result.stdout + result.stderr)
                self.assertEqual(len(list(Path(folder).glob('*.mtl4a'))), files, phase)

    def test_cold_start_quit(self):
        with tempfile.TemporaryDirectory(prefix='pso-cold-quit-') as folder:
            env = dict(os.environ, DM_PSO_MTL4='1', MTL_DEBUG_LAYER='1')
            for name in ('DM_GPU_CAPTURE', 'MTL_CAPTURE_ENABLED', 'DM_TEST_ARCHIVE_CONSTANTS', 'DM_TEST_ARCHIVE_MIXED'):
                env.pop(name, None)
            # quit in its prewarm after 6 new bases: the full period's file and one of the 2 after it; the next start takes all 6 and builds the other 3.
            # An earlier run's compaction marker stays
            for phase, files in (('quit-seed', 2), ('after-quit', 3)):
                result = subprocess.run([executable, '--cold-' + phase, folder],
                                        env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, phase + '\n' + result.stdout + result.stderr)
                self.assertEqual(len(list(Path(folder).glob('*.mtl4a'))), files, phase)
                self.assertTrue((Path(folder) / 'compacting').exists(), phase)

    def test_compaction_with_fresh_bases_and_restart(self):
        for constants in (False, True):
            with self.subTest(constants=constants), tempfile.TemporaryDirectory(prefix='pso-restart-') as folder:
                env = dict(os.environ, DM_PSO_MTL4='1', MTL_DEBUG_LAYER='1', DM_TEST_ARCHIVE_MIXED='1')
                for name in ('DM_GPU_CAPTURE', 'MTL_CAPTURE_ENABLED', 'DM_TEST_ARCHIVE_CONSTANTS'):
                    env.pop(name, None)
                if constants:
                    env['DM_TEST_ARCHIVE_CONSTANTS'] = '1'
                for phase, files in (('seed', 9), ('replay', 1), ('verify', 1)):
                    result = subprocess.run([executable, '--compact-' + phase, folder],
                                            env=env, capture_output=True, text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, phase + '\n' + result.stdout + result.stderr)
                    self.assertEqual(len(list(Path(folder).glob('*.mtl4a'))), files, phase)
                    # every file has its index beside it, and a compacted file's index goes with it
                    self.assertEqual(sorted(path.stem for path in Path(folder).glob('*.keys')), sorted(path.stem for path in Path(folder).glob('*.mtl4a')), phase)
                    self.assertEqual((Path(folder) / 'compacting').exists(), phase == 'seed', phase)


    def test_old_names_compacted_once(self):
        with tempfile.TemporaryDirectory(prefix='pso-oldname-') as folder:
            env = dict(os.environ, DM_PSO_MTL4='1', MTL_DEBUG_LAYER='1')
            for name in ('DM_GPU_CAPTURE', 'MTL_CAPTURE_ENABLED', 'DM_TEST_ARCHIVE_CONSTANTS', 'DM_TEST_ARCHIVE_MIXED'):
                env.pop(name, None)
            # nine files of one base, renamed as an archive from before the count; one compaction, then none
            for phase, files in (('seed', 9), ('replay', 1), ('verify', 1)):
                if phase == 'replay':
                    for path in Path(folder).glob('*-*.mtl4a'):
                        path.rename(path.with_name(path.name.split('-')[0] + '.mtl4a'))
                    self.assertEqual(len(list(Path(folder).glob('*-*.mtl4a'))), 0)
                result = subprocess.run([executable, '--oldname-' + phase, folder],
                                        env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, phase + '\n' + result.stdout + result.stderr)
                self.assertEqual(len(list(Path(folder).glob('*.mtl4a'))), files, phase)

    def test_tier_compaction_restart(self):
        with tempfile.TemporaryDirectory(prefix='pso-tier-') as folder:
            env = dict(os.environ, DM_PSO_MTL4='1', MTL_DEBUG_LAYER='1')
            for name in ('DM_GPU_CAPTURE', 'MTL_CAPTURE_ENABLED', 'DM_TEST_ARCHIVE_CONSTANTS', 'DM_TEST_ARCHIVE_MIXED'):
                env.pop(name, None)
            # a full period's file and nine of one base; the nine become a file per period of 4; then nothing to compact
            for phase, files in (('seed', 10), ('replay', 4), ('verify', 4)):
                result = subprocess.run([executable, '--tier-' + phase, folder],
                                        env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, phase + '\n' + result.stdout + result.stderr)
                self.assertEqual(len(list(Path(folder).glob('*.mtl4a'))), files, phase)



if __name__ == '__main__':
    executable = sys.argv.pop(1)
    unittest.main()
