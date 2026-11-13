"""Repacking preserves the source assets/settings and refuses incompatible shaders."""
import importlib.util
from pathlib import Path
import plistlib
import tempfile
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('repack_macos', REPO / 'scripts/repack_macos_app.py')
assert spec and spec.loader
repack = importlib.util.module_from_spec(spec); spec.loader.exec_module(repack)


class Repack(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.app = Path(self.tmp.name).resolve() / 'Game.app'; self.res = self.app / 'Contents/Resources'
        self.dm = self.res / 'dm'; self.dm.mkdir(parents=True)
        self.g = types.SimpleNamespace(id='010019A01E2F2000')
        import re
        match = re.search(r'#define DMSH_CHAIN "([0-9a-f]+)"', (REPO / 'dm/gfx/nvn/dmsh_chain.h').read_text())
        assert match
        self.chain = match[1]
        self.info = {'DMTitleID': self.g.id, 'DMChain': self.chain, 'CFBundleDisplayName': 'Original Name',
                     'CFBundleShortVersionString': '1.1.0', 'DMLanguage': 'en-US', 'DMRomfsMarker': 'Original/Marker'}
        (self.app / 'Contents/Info.plist').write_bytes(plistlib.dumps(self.info))
        (self.res / 'romfs.img').write_bytes(b'game')
        for module, name, value in ((repack.game, 'load', self.g), (repack.dmsh_chain, 'inputs', 'I'),
                                    (repack.shader_lib, 'read_stamp', {'game': self.g.id, 'inputs': 'I'})):
            p = mock.patch.object(module, name, return_value=value); p.start(); self.addCleanup(p.stop)

    def test_reuses_assets_and_identity(self):
        source = repack.inputs(self.app)
        (self.res / 'html').mkdir(); (self.dm / 'launcher.json').write_text('{}')
        cmd = repack.command(source, self.app, Path('/tmp/env.txt'))
        for flag, value in (('--patched', self.dm), ('--tables', self.dm), ('--romfs', self.res / 'romfs.img'),
                            ('--settings', self.dm / 'launcher.json'), ('--marker', 'Original/Marker'), ('--version', '1.1.0')):
            self.assertEqual(cmd[cmd.index(flag)+1], str(value))
        self.assertNotIn('--pso', cmd)

    def test_preserves_quality_environment_and_overrides_only_requested_flags(self):
        original = 'DM_INTERNAL_RESOLUTION=1800p\nDM_PRESENT_UPSCALE=off\nDM_PSO_PREBUILD=1\nDM_PSO_BUNDLE=old\n'
        (self.dm / 'env.txt').write_text(original)
        self.assertEqual(repack.environment(self.dm, False, False), original)
        new = repack.environment(self.dm, True, True)
        self.assertIn('DM_INTERNAL_RESOLUTION=1800p\nDM_PRESENT_UPSCALE=off\n', new)
        self.assertEqual(new.count('DM_PSO_PREBUILD='), 1); self.assertIn('DM_PSO_PREBUILD=0', new)
        self.assertIn('DM_PSO_BUNDLE=off', new)
        self.assertEqual((self.dm / 'env.txt').read_text(), original)

    def test_refuses_incompatible_chain_or_profile(self):
        for info in ({**self.info, 'DMChain': 'old'}, {**self.info, 'DMTitleID': '../invalid'}):
            (self.app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
            with self.assertRaises(ValueError): repack.inputs(self.app)
        (self.app / 'Contents/Info.plist').write_bytes(plistlib.dumps(self.info))
        with mock.patch.object(repack.shader_lib, 'read_stamp', return_value={'game': self.g.id, 'inputs': 'OLD'}):
            with self.assertRaises(ValueError): repack.inputs(self.app)

    def test_rejects_nested_output_and_multi_program_before_building(self):
        with mock.patch.object(repack.subprocess, 'run') as run:
            with self.assertRaises(ValueError):
                repack.main([str(self.app), '--out', str(self.app / 'nested.app')])
            (self.dm / 'programs.tsv').write_text('0\tprogram\n')
            with self.assertRaises(ValueError): repack.main([str(self.app)])
            run.assert_not_called()

    def test_never_runs_shader_or_pipeline_builders(self):
        (self.res / 'pso').mkdir()
        with mock.patch.object(repack.subprocess, 'run') as run:
            repack.main([str(self.app), '--without-bundled-pso', '--skip-pipeline-prebuild'])
        self.assertEqual(run.call_count, 2)
        self.assertEqual(run.call_args_list[0].args[0][0], 'make')
        pack = run.call_args_list[1].args[0]
        self.assertEqual(Path(pack[1]).name, 'pack_app.py'); self.assertNotIn('--pso', pack)
        self.assertEqual(pack[2], str(self.app))


if __name__ == '__main__': unittest.main()
