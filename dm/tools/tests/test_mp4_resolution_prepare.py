"""Preparation preflight is read-only and refuses mixed module builds before work."""
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('mp4_resolution_prepare', ROOT / 'scripts/prepare_mp4_resolution.py')
assert spec and spec.loader
prepare = importlib.util.module_from_spec(spec); spec.loader.exec_module(prepare)
BUILD = '8275fbe1ec5191c279f3a943ad8524f4bab087a3000000000000000000000000'
GID = '010019A01E2F2000'


class Preparation(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name); self.app = self.root / 'Game.app'; self.dm = self.root / 'dm'; self.dm.mkdir()
        self.bids = {'main': BUILD[:32], 'sdk': 'ab' * 16, 'subsdk0': 'cd' * 16, 'display': '1.1.0'}
        self.g = types.SimpleNamespace(id=GID, dir=ROOT / 'dm/games' / GID,
            builds=lambda: {'v131072': self.bids}, words=lambda key: ['main', 'sdk', 'subsdk0'])
        ex = self.root / (GID + '_build/extract/v131072'); (ex / 'ExeFS').mkdir(parents=True)
        (ex / 'ExeFS/main').write_bytes(b'mock NSO, no game data'); (ex / 'control.nacp').touch()
        for name in ['main', 'sdk', 'subsdk0']: (self.dm / (name + '.fixups.tsv')).write_text('# BUILD_ID\t' + self.bids[name] + '\n')
        for cm in [mock.patch.object(prepare.repack_macos_app, 'inputs', return_value=(self.app, {}, self.g, self.root, self.dm, self.root, self.root)),
                   mock.patch.object(prepare.nso, 'load', return_value=types.SimpleNamespace(build_id=bytes.fromhex(BUILD)))]:
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)
        self.args = [str(self.app), '--nsp', str(self.root)]

    def test_default_preflight_writes_nothing_and_never_launches(self):
        before = {str(p): p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        with mock.patch.object(prepare.subprocess, 'run') as run, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(prepare.main(self.args), 0)
        run.assert_not_called()
        self.assertEqual(before, {str(p): p.read_bytes() for p in self.root.rglob('*') if p.is_file()})

    def test_mixed_source_app_sdk_stops_before_build_or_copy(self):
        (self.dm / 'sdk.fixups.tsv').write_text('# BUILD_ID\twrong\n')
        with mock.patch.object(prepare.subprocess, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'module build mismatch: sdk'): prepare.main(self.args + ['--repack'])
        run.assert_not_called(); self.assertFalse((self.root / (GID + '-work')).exists())

    def test_unregistered_original_build_stops_before_build_or_copy(self):
        with mock.patch.object(prepare.nso, 'load', return_value=types.SimpleNamespace(build_id=bytes(32))), mock.patch.object(prepare.subprocess, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'exact registered'): prepare.main(self.args + ['--prepare'])
        run.assert_not_called(); self.assertFalse((self.root / (GID + '-work')).exists())


if __name__ == '__main__': unittest.main()
