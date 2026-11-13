"""A clean, strictly single-launch manual audio checkpoint; no actual game runs here."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts'))
spec = importlib.util.spec_from_file_location('mp4_audio_check', ROOT / 'scripts/check_mp4_audio.py')
assert spec and spec.loader
check = importlib.util.module_from_spec(spec); spec.loader.exec_module(check)


class AudioCheckpoint(unittest.TestCase):
    def test_default_is_read_only(self):
        with mock.patch.object(check, 'preflight'), mock.patch.object(check.subprocess, 'run') as run, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(check.main(['/tmp/game.app', '--nsp', '/tmp/nsp']), 0)
        run.assert_not_called()

    def test_clean_capture_keeps_rumble_without_guest_probes(self):
        with mock.patch.dict(os.environ, {'DM_KEYS': 'old', 'DM_CTL': 'old', 'DM_PROBE': 'old', 'DM_PROBE_WATCH': 'old', 'DM_RUMBLE_LOG': '1', 'DM_AUDREN_TRACE': '1', 'DM_NVN_PASSDUMP': 'every:1'}, clear=True):
            env = check.run_environment(Path('/tmp/session'))
        for key in ['DM_KEYS', 'DM_CTL', 'DM_PROBE', 'DM_PROBE_WATCH', 'DM_RUMBLE_LOG', 'DM_AUDREN_TRACE', 'DM_NVN_PASSDUMP']: self.assertNotIn(key, env)
        self.assertEqual(env['DM_RUMBLE'], '1'); self.assertEqual(env['DM_AUDREN_AUX_TRACE'], '0')
        self.assertEqual(env['DM_INTERNAL_RESOLUTION'], '1800p')
        self.assertEqual(env['DM_AUDREN_PCM_TRACE'], '/tmp/session/frames.tsv')

    def test_run_builds_then_launches_once_and_preserves_failure_log(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t).resolve(); log = root / 'Library/Logs/nxnative/010019A01E2F2000.log'
            log.parent.mkdir(parents=True); log.write_text('test log\n')
            with mock.patch.object(check, 'preflight'), mock.patch.object(check.Path, 'home', return_value=root), mock.patch.object(check.subprocess, 'run', side_effect=[types.SimpleNamespace(returncode=0), types.SimpleNamespace(returncode=8)]) as run, contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(check.main([str(root / 'game.app'), '--nsp', str(root), '--run']), 8)
            self.assertEqual(run.call_count, 2)
            self.assertIn('repack_macos_app.py', run.call_args_list[0].args[0][1])
            self.assertEqual(run.call_args_list[1].args[0], [str(root / 'game.app/Contents/MacOS/nxnative')])
            sessions = list(root.glob('010019A01E2F2000-work/analysis/audio-fifo-*'))
            self.assertEqual(len(sessions), 1); self.assertEqual((sessions[0] / 'host.log').read_text(), 'test log\n')


if __name__ == '__main__': unittest.main()
