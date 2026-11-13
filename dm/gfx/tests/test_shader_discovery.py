"""Discovery and mixed scan scheduling overlap while preserving main/archive/loose source precedence."""
import concurrent.futures as cf
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


class Discovery(unittest.TestCase):
    def setUp(self):
        self.assertTrue(hasattr(split, 'scan_inputs'), 'discovery still runs before the worker pool')

    def test_main_discovery_and_both_scan_groups_overlap_without_reordering(self):
        loose_done = threading.Event(); main_started = threading.Event(); archive_started = threading.Event()
        def mains(*_):
            main_started.set()
            if not loose_done.wait(5): raise TimeoutError('main scan blocked later work')
            return [('main', 'first'), ('main-update', 'second')]
        def archives(_, main_paths):
            self.assertEqual(main_paths, ['base.nso', 'update.nso'])
            if not main_started.wait(5): raise TimeoutError('main and archive discovery did not overlap')
            return ['archive'], ['loose']
        def scan(job):
            if job == 'archive':
                archive_started.set()
                if not loose_done.wait(5): raise TimeoutError('archive queue blocked loose files')
            else:
                if not archive_started.wait(5): raise TimeoutError('archive job never started')
                loose_done.set()
            return job
        out = io.StringIO()
        with mock.patch.object(split, 'scan_mains', mains), mock.patch.object(split, 'archive_jobs', archives), \
                mock.patch.object(split, 'scan_file', scan), cf.ThreadPoolExecutor(3) as pool, contextlib.redirect_stdout(out):
            try: result = split.scan_inputs(pool, [('base', 'base.nso', ''), ('update', 'update.nso', 'update')], [], b'')
            finally: loose_done.set()
        self.assertEqual(result, [('main', 'first'), ('main-update', 'second'), 'archive', 'loose'])
        self.assertIn('progress: 2/2', out.getvalue())

    def test_discovery_errors_reach_the_caller(self):
        with mock.patch.object(split, 'scan_mains', return_value=[]), mock.patch.object(split, 'archive_jobs', side_effect=ValueError('broken index')), \
                cf.ThreadPoolExecutor(2) as pool:
            with self.assertRaisesRegex(ValueError, 'broken index'): split.scan_inputs(pool, [], [], b'')

    def test_real_archive_discovery_keeps_loose_file_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = [Path(tmp) / x for x in ('first.bin', 'second.bin', 'third.bin')]
            for p in paths: p.write_bytes(b'not a shader')
            files = [(str(p), p.name) for p in paths]
            big, loose = split.archive_jobs(files)
            self.assertEqual(big, []); self.assertEqual(loose, files)

    def test_new_pdat_ranges_keep_archive_precedence_and_are_not_scanned_again_as_loose_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive, loose = (Path(tmp) / n for n in ('data.dat', 'loose.bin'))
            archive.write_bytes(b'PDAT' + bytes(32)); loose.write_bytes(b'not a shader')
            files = [(str(archive), archive.name), (str(loose), loose.name)]
            jobs, remaining = split.archive_jobs(files)
            self.assertEqual(jobs, [(str(archive), archive.name, (split.PDAT, 0, archive.stat().st_size))])
            self.assertEqual(remaining, [files[1]])

    def test_new_main_archive_readers_keep_their_order_and_are_not_scanned_loose(self):
        with tempfile.TemporaryDirectory() as tmp:
            files = [(str(Path(tmp) / n), n) for n in ('d3.bin', 'casc.bin', 'rpf.bin', 'loose.bin')]
            for path, _ in files: Path(path).write_bytes(b'not a shader')
            with mock.patch.object(split, 'd3cpk_jobs', return_value=(['d3'], {files[0][0]})), \
                    mock.patch.object(split, 'casc_jobs', return_value=(['casc'], {files[1][0]})), \
                    mock.patch.object(split, 'rpf_jobs', return_value=(['rpf'], {files[2][0]})) as rpf:
                jobs, loose = split.archive_jobs(files, ['main.nso'])
            self.assertEqual(jobs, ['d3', 'casc', 'rpf']); self.assertEqual(loose, files[3:])
            rpf.assert_called_once_with(files, ['main.nso'])

    def test_runtime_nro_is_scanned_only_with_the_first_executable(self):
        with tempfile.TemporaryDirectory() as tmp:
            nro = Path(tmp) / 'Game [title][v0]' / 'captured.nro'
            nro.parent.mkdir(); nro.write_bytes(b'captured shader data')
            scans = []
            with mock.patch.dict('os.environ', {'DM_NRO_DIR': str(nro.parent)}), mock.patch.object(split, 'nso_image', return_value=bytes(128)), \
                    mock.patch.object(split, 'scan_blob', side_effect=lambda data, label, found: scans.append((data, label))):
                split.scan_mains([(tmp, 'main', ''), (tmp, 'update', 'update')], bytes(128))
            self.assertEqual([label for data, label in scans if data == nro.read_bytes()], ['main/captured.nro'])
            self.assertEqual([label for _, label in scans], ['main', 'main/captured.nro', 'main@update'])

    def test_cra_ranges_keep_their_archive_precedence_and_leave_only_loose_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = [Path(tmp) / name for name in ('archive.bin', 'loose.bin')]
            for path in paths: path.write_bytes(b'not a shader')
            files = [(str(path), path.name) for path in paths]
            with mock.patch.object(split, 'cra_jobs', return_value=(['cra-range'], {str(paths[0])})):
                jobs, loose = split.archive_jobs(files)
            self.assertEqual(jobs, ['cra-range'])
            self.assertEqual(loose, [files[1]])

    def test_main_scan_restores_worker_context_on_success_and_failure(self):
        common, trees, dictionaries = b'unity strings', {'tree': 'layout'}, {'dict': b'dictionary'}
        with mock.patch.object(split, 'COMMON', common), mock.patch.object(split, 'TREES', trees), mock.patch.object(split, 'ZDICTS', dictionaries), \
                tempfile.TemporaryDirectory() as tmp:
            mains = [(tmp, str(Path(tmp) / 'main'), '')]
            self.assertEqual(split.scan_mains(mains, bytes(128)), [('main', {}, set(), {})])
            self.assertIs(split.COMMON, common); self.assertIs(split.TREES, trees); self.assertIs(split.ZDICTS, dictionaries)
            def broken(*_):
                self.assertIsNone(split.COMMON); self.assertEqual(split.TREES, {}); self.assertEqual(split.ZDICTS, {})
                raise ValueError('damaged main')
            with mock.patch.object(split, 'scan_blob', broken):
                with self.assertRaisesRegex(ValueError, 'damaged main'): split.scan_mains(mains, bytes(128))
            self.assertIs(split.COMMON, common); self.assertIs(split.TREES, trees); self.assertIs(split.ZDICTS, dictionaries)


if __name__ == '__main__': unittest.main()
