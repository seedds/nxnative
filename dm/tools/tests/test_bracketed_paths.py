"""Tools that search a folder with glob find its files when a parent folder's name has brackets (`Game [0400486024DEC000][v131072]`, a usual NSP folder name): glob reads
an unescaped `[...]` as a character class and finds nothing, silently. The rewriter's registry is checked in test_patches.py; these are the matcher's registered cuts, the host
hooks a provisional profile is guessed from, and the NROs captured from a boot. No game files needed (temporary files only in a temporary folder)."""
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools')); sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import nso_match   # noqa: E402
import nso_syms   # noqa: E402
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010029B0118E8000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split   # noqa: E402
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


class BracketedParent(unittest.TestCase):
    def setUp(self):
        tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, tmp)
        self.root = tmp / 'Game [0400486024DEC000][v131072]'

    def write(self, rel, text=''):
        p = self.root / rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text)
        return p

    def test_matcher_finds_a_game_files_registered_cuts(self):
        bid = '6a' * 16
        tool = self.write('dm/tools/nso_match.py'); self.write('dm/tools/sdk_patches.py')
        self.write('dm/games/0100000000000000/patches.py', f"CUTS = {{'{bid}': [(0x10, 'aa', 'bb')]}}\n")
        with mock.patch.object(nso_match, '__file__', str(tool)):
            self.assertEqual(nso_match.registered_cuts(bid), [(0x10, 'aa', 'bb')])

    def test_host_hooks_are_read_from_every_source_folder(self):
        self.write('dm/host/hle_a.c', 'HLE("nnA")\n'); self.write('dm/gfx/b.c', 'HLE("nnB")\n')
        self.write('dm/gfx/nvn/c.c', 'HLE_FORCE("nnC")\n'); self.write('dm/gfx/vk/d.m', 'VK(vkCreateD, x)\n')
        self.assertEqual(nso_syms.host_hooks(str(self.root / 'dm/host')), {'nnA', 'nnB', 'nnC', 'vkCreateD'})

    def test_boot_nros_are_found(self):
        b, a = self.write('build/nro/b.nro'), self.write('build/nro/a.nro'); self.write('build/nro/c.txt')
        with mock.patch.dict(os.environ, DM_NRO_DIR=str(a.parent)):
            self.assertEqual(shader_split.boot_nros(), [str(a), str(b)])

    def test_no_nro_folder_means_no_nros(self):
        with mock.patch.dict(os.environ):
            os.environ.pop('DM_NRO_DIR', None)
            self.assertEqual(shader_split.boot_nros(), [])


if __name__ == '__main__':
    unittest.main()
