"""NSP script --bc7-astc (BC7_ASTC): the build bakes DM_VK_BC7_ASTC=1 into the app's environment file beside the other switches, and packs a copy of the RomFS image whose
Supergiant packages dm/tools/pkg_astc.py re-encoded; a RomFS folder is refused. No game files, keys or Xcode needed (temporary files only in a temporary folder)."""
import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)


class Bc7Astc(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        (self.tmp / 'logs').mkdir()

    def test_env_file(self):
        self.assertEqual(nsp.build_env_file(self.tmp, '', True), '')
        f = Path(nsp.build_env_file(self.tmp, '', True, bc7_astc=True))
        self.assertEqual(nsp.env_file_pairs(f), {'DM_VK_BC7_ASTC': '1'})
        own = self.tmp / 'mine.txt'; own.write_text('DM_LANG=fr-FR\n')
        f = Path(nsp.build_env_file(self.tmp, str(own), False, bc7_astc=True))
        self.assertEqual(nsp.env_file_pairs(f), {'DM_LANG': 'fr-FR', 'DM_MOTION': '0', 'DM_VK_BC7_ASTC': '1'})
        self.assertEqual(own.read_text(), 'DM_LANG=fr-FR\n')

    def test_romfs_copy(self):
        img = self.tmp / 'extract/v0/RomFS.img'; img.parent.mkdir(parents=True); img.write_bytes(b'DMROMFS1')
        (img.parent / 'control.nacp').write_bytes(b'nacp'); (img.parent / 'icon.jpg').write_bytes(b'jpg')
        ran = []
        def run(cmd, log, env=None):
            ran.append([str(c) for c in cmd]); log.write_text(f'{cmd[-1]}: 610 textures (610 levels) in 46 of 46 packages re-encoded in 64 s\n')
        with mock.patch.object(nsp, 'run', run), mock.patch.object(nsp, 'PROG'):
            out = nsp.bc7_astc_romfs('python3', REPO, img, self.tmp, {}, self.tmp / 'logs')
        self.assertEqual(out, self.tmp / 'astc/RomFS.img')
        self.assertEqual(ran, [['python3', str(REPO / 'dm/tools/pkg_astc.py'), '--cache', str(self.tmp / 'astc/cache'), str(img), str(out)]])
        # build_ipa.sh reads the game's NACP (launcher languages, app version) and icon beside the RomFS image it packs
        self.assertEqual((out.parent / 'control.nacp').read_bytes(), b'nacp'); self.assertEqual((out.parent / 'icon.jpg').read_bytes(), b'jpg')
        with self.assertRaises(nsp.BuildError):
            nsp.bc7_astc_romfs('python3', REPO, self.tmp / 'extract/v0/RomFS', self.tmp, {}, self.tmp / 'logs')


if __name__ == '__main__':
    unittest.main()
