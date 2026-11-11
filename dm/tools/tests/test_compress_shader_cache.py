"""The user command converts only the existing compile cache; it never starts a game build."""
import importlib.util, subprocess, sys, tempfile, unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
SCRIPT = REPO / 'scripts/compress_shader_cache.py'


class Command(unittest.TestCase):
    def test_delegates_modes_and_reports_failures(self):
        spec = importlib.util.spec_from_file_location('compress_shader_cache', SCRIPT)
        assert spec and spec.loader
        command = importlib.util.module_from_spec(spec); spec.loader.exec_module(command)
        with tempfile.TemporaryDirectory() as root:
            for args, compressed, decompressed, workers in (([], True, False, 7), (['--decompress'], False, True, 7), (['--decompress', '--workers', '1'], False, True, 1), (['--workers', '4'], True, False, 4), (['--decompress', '--workers', 'max'], False, True, 7)):
                with mock.patch.object(command.shader_cache, 'maintain') as maintain, mock.patch.object(command.os, 'cpu_count', return_value=7):
                    self.assertEqual(command.main([root, *args]), 0)
                    maintain.assert_called_once_with(Path(root) / 'shader/cache/cas.db', compress=compressed, decompress=decompressed, workers=workers)
            with mock.patch.object(command.os, 'cpu_count', return_value=None), mock.patch.object(command.shader_cache, 'maintain') as maintain:
                self.assertEqual(command.main([root]), 0)
                self.assertEqual(maintain.call_args.kwargs['workers'], 1)
            with mock.patch.object(command.shader_cache, 'maintain', side_effect=RuntimeError('cache is busy')), mock.patch('sys.stderr') as stderr:
                self.assertEqual(command.main([root]), 1)
                self.assertIn('busy', str(stderr.write.call_args_list))

    def test_missing_cache_does_not_start_a_build_or_create_it(self):
        with tempfile.TemporaryDirectory() as root:
            p = subprocess.run([sys.executable, str(SCRIPT), root], capture_output=True, text=True)
            self.assertEqual(p.returncode, 1); self.assertIn('[RED]', p.stderr)
            self.assertEqual(list(Path(root).iterdir()), [])


if __name__ == '__main__': unittest.main()
