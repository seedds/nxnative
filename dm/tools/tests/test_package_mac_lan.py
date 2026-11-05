"""The portable LAN archive contains only the chosen runtime and launcher."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

REPO = Path(__file__).resolve().parents[3]
with mock.patch.object(sys, 'path', [str(REPO / 'scripts'), *sys.path]):
    spec = importlib.util.spec_from_file_location('package_mac_lan', REPO / 'scripts/package_mac_lan.py')
    assert spec and spec.loader
    pack = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pack)
TITLE = '0400C3F00006E000'


class Package(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name).resolve()
        self.build = self.root / f'{TITLE}_build'
        for name in ('patched_v0/main.patched.bin', 'extract/v0/RomFS.img', 'tools/dmhost',
                     f'profile/{TITLE}/shader_map.tsv', f'profile/{TITLE}/c1_full.tsv',
                     'shader/cache/b-test/inputs.tsv', 'shader/cache/b-test/a_b.metallib',
                     'shader/cache/b-test/a_b.json', 'shader/cache/b-test/a_b.metal',
                     'shader/cache/b-test/_b_build.json', 'run/save/account.sav',
                     'prod.keys', 'font/font.ttf', 'app/pso/applegpu_g13x.mtl4a',
                     'run/lan-user.json', f'profile/{TITLE}/lan-user.json'):
            path = self.build / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'synthetic fixture')
        (self.build / 'tools/dmhost').chmod(0o755)
        self.output = self.root / 'lan.zip'

    def package(self, **kwargs):
        with contextlib.redirect_stdout(io.StringIO()):
            pack.package(self.build, self.output, **kwargs)

    def test_only_runtime_inputs_are_packed(self):
        self.package()
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(set(archive.namelist()), {
                'lan-test-kit/dmhost', 'lan-test-kit/run_mac_lan.py', 'lan-test-kit/Start LAN.command',
                f'lan-test-kit/runtime/tables/{TITLE}/shader_map.tsv',
                f'lan-test-kit/runtime/tables/{TITLE}/c1_full.tsv',
                'lan-test-kit/runtime/shaders/b-test/inputs.tsv',
                'lan-test-kit/runtime/shaders/b-test/a_b.metallib',
                'lan-test-kit/runtime/shaders/b-test/a_b.json'})
            self.assertTrue(archive.getinfo('lan-test-kit/Start LAN.command').external_attr >> 16 & 0o111)
            self.assertIsNone(archive.testzip())

    def test_ambiguous_chain_needs_explicit_selection(self):
        extra = self.build / 'shader/cache/b-other/inputs.tsv'
        extra.parent.mkdir()
        extra.write_text('another chain')
        with self.assertRaisesRegex(ValueError, 'one built shader chain'):
            self.package()
        self.package(shaders=self.build / 'shader/cache/b-test')

    def test_failed_package_keeps_the_previous_archive(self):
        self.output.write_bytes(b'previous output')
        with mock.patch.object(pack.zipfile.ZipFile, 'write', side_effect=OSError('injected write failure')):
            with self.assertRaises(OSError):
                self.package()
        self.assertEqual(self.output.read_bytes(), b'previous output')
        self.assertEqual(list(self.root.glob('*.partial')), [])

    def test_missing_libraries_are_reported_before_output(self):
        (self.build / 'shader/cache/b-test/a_b.metallib').unlink()
        with self.assertRaisesRegex(ValueError, 'no compiled Mac shader libraries'):
            self.package()
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
