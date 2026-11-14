"""NSP script: an update whose build the profile registers, packed from a profile derived for its own shader tables (the profile's are the base game's), is reported as the registered
build, with no red line, although the derived profile keeps only that version's build row (Mario Kart World 1.8.0); an update whose modules the profile does not register still is red.
No game files, keys or Xcode needed (temporary files only)."""
import importlib.util
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games')); sys.path.insert(0, str(REPO / 'dm/tools'))
import game, nacp, profile_guess
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
nsp.pg, nsp.nacp = profile_guess, nacp   # main sets them

PROFILE = 0x0100AAAA00000000
BASE, UPD, NEW = 'ab' * 16, 'cd' * 16, 'ef' * 16


def nso(path, bid):   # an NSO header: the build ID at +0x40
    path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex(bid) + bytes(0x50))


class RegisteredUpdate(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        prof = self.tmp / 'games' / f'{PROFILE:016X}'; prof.mkdir(parents=True)
        (prof / 'game.tsv').write_text('\n'.join([f'title_id\t{PROFILE:016X}', f'program_id\t{PROFILE:016X}', 'name\tTest Game', 'modules\tmain',
                                                  f'build\tv0\t1.0.0\tmain={BASE}', f'build\tv65536\t1.1.0\tmain={UPD}', 'package\tv0', 'shader_versions\tv0', 'pad_layout\tfull']) + '\n')
        (prof / 'shader_map.tsv').write_text('# made again from the build\n')
        self.said = []
        for cm in (mock.patch.dict(os.environ, {'DM_GAMES_DIR': str(self.tmp / 'games')}), mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'say', self.said.append)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def derive(self, ver, bid):
        d = self.tmp / 'extract' / ver; nso(d / 'ExeFS/main', bid)
        reg = game.load(f'{PROFILE:016X}'); out = self.tmp / 'derived' / ver
        nsp.derive_profile(reg, out, ver, d, ['main'])
        return reg, game.Game(out)

    def test_a_registered_update_is_not_red(self):
        reg, g = self.derive('v65536', UPD)
        self.assertEqual(list(g.builds()), ['v65536'])   # a derived profile keeps only the packed version's row
        nsp.report_profile_status(g, reg, 'v65536', 'its shader tables are the base game\'s', False)
        self.assertEqual(len(self.said), 1)
        self.assertIn('uses the registered v65536 (1.1.0) builds and every fix for them', self.said[0])
        self.assertNotIn('[RED]', self.said[0])

    def test_an_update_the_profile_does_not_register_is_red(self):
        reg, g = self.derive('v131072', NEW)
        nsp.report_profile_status(g, reg, 'v131072', 'not a version the profile registers', False)
        self.assertTrue(self.said[0].startswith('[RED] '), self.said)
        self.assertIn('was not brought up on', self.said[0])


if __name__ == '__main__':
    unittest.main()
