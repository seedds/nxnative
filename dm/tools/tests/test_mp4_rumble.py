"""Build-gated rumble observations and a strictly single-launch manual checkpoint."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT / 'dm/games/010019A01E2F2000'), str(ROOT / 'scripts')]
spec = importlib.util.spec_from_file_location('mp4_rumble_check', ROOT / 'scripts/check_mp4_rumble.py')
assert spec and spec.loader
check = importlib.util.module_from_spec(spec); spec.loader.exec_module(check)
diag = check.rumble_diag


class RumbleProbes(unittest.TestCase):
    def setUp(self):
        end = max(s[1] for s in diag.SITES) + 4
        data = bytearray(end)
        for _, off, word, *_ in diag.SITES: data[off:off + 4] = bytes.fromhex(word)
        self.mod = types.SimpleNamespace(build_id=bytes.fromhex(diag.BUILD), text_end=end, image=data)
        self.fix = {'BUILD_ID': diag.BUILD[:32], 'TEXT_END': hex(end)}

    def test_original_and_rewritten_match(self):
        env = diag.validate(self.mod, self.mod.image, self.fix)
        self.assertEqual(len(env['DM_PROBE'].split(',')), 6)
        self.assertEqual(env['DM_PROBE_CHANGES'], '16')
        self.assertEqual(env['DM_PROBE_LIMIT'], '512')

    def test_incompatible_build_layout_and_code_rejected(self):
        self.mod.build_id = self.mod.build_id[:16] + bytes(16)
        with self.assertRaisesRegex(ValueError, 'verified main'): diag.validate(self.mod, self.mod.image, self.fix)
        self.mod.build_id = bytes.fromhex(diag.BUILD)
        with self.assertRaisesRegex(ValueError, 'layout'): diag.validate(self.mod, self.mod.image, {**self.fix, 'TEXT_END': '0'})
        bad = bytearray(self.mod.image); bad[diag.SITES[0][1]] ^= 1
        with self.assertRaisesRegex(ValueError, 'instruction mismatch'): diag.validate(self.mod, bad, self.fix)
        self.mod.image = bad
        with self.assertRaisesRegex(ValueError, 'instruction mismatch'): diag.validate(self.mod, bad, self.fix)
        with mock.patch.object(diag, 'SITES', (('bad', -4, '00000000', None, 0),)):
            with self.assertRaisesRegex(ValueError, 'invalid rumble'): diag.validate(self.mod, bad, self.fix)

    def test_summary_never_infers_gameplay_success_or_absence(self):
        data = struct.pack('<4f', 0.5, 160, 0.25, 320).hex()
        log = '[dm] rumble: 20 values sent, 10 with no output, peak amplitude 0.50, handle 0x5, nonzero 2, unrouted nonzero 1, nonfinite 0, positive handle 0x205\n'
        log += f'[dm] PROBE main+0xa4e00 #1 F=150 x1=0 [x2+0x0]={data}\n'
        report = diag.summarize(log)
        self.assertIn('nonzero: 2', report); self.assertIn('unrouted nonzero: 1', report)
        self.assertIn('finite positive amplitude observations: 1', report)
        self.assertIn('absence/capped output is unknown', report)
        self.assertIn('gameplay confirmation requires the player', report)


class SingleManualLaunch(unittest.TestCase):
    def test_unsafe_inherited_diagnostics_and_replay_removed(self):
        with mock.patch.dict(os.environ, {'DM_KEYS': 'fire', 'DM_CTL': 'old', 'DM_PROBE_WATCH': 'unsafe', 'DM_AUDIO_DUMP': 'old'}, clear=True):
            env = check.run_environment({'DM_PROBE': 'checked'}, Path('/tmp/session'))
        self.assertEqual(env['DM_INTERNAL_RESOLUTION'], '1800p')
        self.assertEqual(env['DM_PROBE'], 'checked')
        self.assertEqual(env['DM_REC'], '/tmp/session/input.rec')
        self.assertNotIn('DM_KEYS', env); self.assertNotIn('DM_CTL', env)
        self.assertNotIn('DM_AUDIO_DUMP', env); self.assertNotIn('DM_PROBE_WATCH', env)

    def test_default_is_read_only(self):
        with mock.patch.object(check, 'preflight', return_value={}), mock.patch.object(check.subprocess, 'run') as run, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(check.main(['/tmp/game.app', '--nsp', '/tmp/nsp']), 0)
        run.assert_not_called()

    def test_run_repacks_then_launches_exactly_once_and_propagates_exit(self):
        with tempfile.TemporaryDirectory() as t:
            path = Path(t); log = path / 'Library/Logs/nxnative/010019A01E2F2000.log'
            log.parent.mkdir(parents=True); log.write_text('test host log\n')
            with mock.patch.object(check, 'preflight', return_value={'DM_PROBE': 'checked'}), mock.patch.object(check.Path, 'home', return_value=path), mock.patch.object(check.subprocess, 'run', side_effect=[types.SimpleNamespace(returncode=0), types.SimpleNamespace(returncode=9)]) as run, contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(check.main([str(path / 'game.app'), '--nsp', t, '--run']), 9)
            self.assertEqual(run.call_count, 2)
            self.assertIn('repack_macos_app.py', run.call_args_list[0].args[0][1])
            self.assertEqual(run.call_args_list[1].args[0], [str(path.resolve() / 'game.app/Contents/MacOS/nxnative')])
            folders = list(path.glob('010019A01E2F2000-work/analysis/rumble-check-*'))
            self.assertEqual(len(folders), 1); self.assertTrue((folders[0] / 'summary.txt').is_file())


if __name__ == '__main__': unittest.main()
