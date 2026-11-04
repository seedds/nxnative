"""Vulkan/GL command-line target selection writes only requested platform artifacts."""
import contextlib
import importlib
import io
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'gfx/tools'))


class LayerTargets(unittest.TestCase):
    def exercise(self, api, targets, ios=True):
        module = importlib.import_module(api + '_shaders')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); out = root / 'out'; phone = root / 'phone'
            for base in (out, phone):
                (base / api).mkdir(parents=True); (base / api / 'untouched').write_text('keep')
            args = ['tool', str(root / 'input'), str(out)] + (['--ios', str(phone)] if ios else [])
            if targets is not None: args += ['--targets', targets]
            def compile_msls(msls, dest, sdk, extra):
                p = Path(dest) / f'{api}_shaders.metallib'; p.write_text(sdk); return str(p)
            if api == 'vk':
                sources = mock.patch.object(module, 'spirv_files', return_value=['synthetic.spv'])
                translated = ('key', 'vert', [], [], 'fixture')
            else:
                sources = mock.patch.object(module, 'sources', return_value=[(b'shader', 'vert', 'fixture')])
                translated = [('key', 'vert', [], 'synthetic.metal', 'fixture')]
            with sources, mock.patch.object(module, 'one', return_value=translated), mock.patch.object(module, 'metallib', compile_msls), \
                    mock.patch.object(module.subprocess, 'run', return_value=types.SimpleNamespace(stdout='version', stderr='')), \
                    mock.patch.object(sys, 'argv', args), contextlib.redirect_stdout(io.StringIO()):
                module.main()
            actual = set()
            for target, base, sdk in (('mac', out, 'macosx'), ('ios', phone, 'iphoneos')):
                lib = base / api / f'{api}_shaders.metallib'
                if lib.exists():
                    self.assertEqual(lib.read_text(), sdk)
                    self.assertTrue((base / api / f'{api}_shaders.tsv').is_file()); actual.add(target)
                self.assertEqual((base / api / 'untouched').read_text(), 'keep')
            return actual

    def test_selected_outputs(self):
        for api in ('vk', 'gl'):
            for targets in ('mac', 'ios', 'mac,ios'):
                with self.subTest(api=api, targets=targets): self.assertEqual(self.exercise(api, targets), set(targets.split(',')))

    def test_legacy_defaults(self):
        for api in ('vk', 'gl'):
            self.assertEqual(self.exercise(api, None), {'mac', 'ios'})
            self.assertEqual(self.exercise(api, None, ios=False), {'mac'})

    def test_invalid_targets(self):
        for api in ('vk', 'gl'):
            for targets, ios in (('ios', False), ('other', True), ('', True)):
                with self.subTest(api=api, targets=targets), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    self.exercise(api, targets, ios)


if __name__ == '__main__': unittest.main()
