"""NSP script: registered builds packed with derived shader tables are reported without [RED] on multi-build profiles,
and registered builds not covered by shader_versions (or shader_covers) derive their own shaders even when shader_versions is present.
No game files, keys or Xcode needed (temporary files only)."""
import importlib.util
import os
from pathlib import Path
import shutil
import sys
import tempfile
from typing import Any
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games'))
sys.path.insert(0, str(REPO / 'dm/tools'))
import game  # noqa: E402
import nacp  # noqa: E402
import profile_guess  # noqa: E402

_spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
assert _spec is not None and _spec.loader is not None
nsp: Any = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(nsp)
nsp.pg, nsp.nacp = profile_guess, nacp   # main sets them

PROFILE = 0x0100AAAA00000000
BASE_MAIN, BASE_SDK = 'ab' * 16, '11' * 16
UPD_MAIN, UPD_SDK = 'cd' * 16, '22' * 16
NEW_MAIN = 'ef' * 16


def nso(path: Path, bid: str) -> None:   # an NSO header: the build ID at +0x40
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex(bid) + bytes(0x50))


class RegisteredUpdate(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.prof = self.tmp / 'games' / f'{PROFILE:016X}'
        self.prof.mkdir(parents=True)
        self.write_profile()
        (self.prof / 'shader_map.tsv').write_text('# made again from the build\n')
        self.said: list[str] = []
        for cm in (
            mock.patch.dict(os.environ, {'DM_GAMES_DIR': str(self.tmp / 'games')}),
            mock.patch.object(nsp, 'PROG', mock.Mock()),
            mock.patch.object(nsp, 'say', self.said.append),
        ):
            cm.__enter__()
            self.addCleanup(cm.__exit__, None, None, None)

    def write_profile(self, extra: tuple[str, ...] = ()) -> None:
        rows = [
            f'title_id\t{PROFILE:016X}',
            f'program_id\t{PROFILE:016X}',
            'name\tTest Game',
            'modules\tmain sdk',
            f'build\tv0\tv0\tmain={BASE_MAIN}\tsdk={BASE_SDK}',
            f'build\tv65536\t1.1.0\tmain={UPD_MAIN}\tsdk={UPD_SDK}',
            'package\tv0',
            'shader_versions\tv0',
            *extra,
            'pad_layout\tfull',
        ]
        (self.prof / 'game.tsv').write_text('\n'.join(rows) + '\n')

    def derive(self, ver: str, main_bid: str, sdk_bid: str, nacp_ver: str = '') -> tuple[Any, Any]:
        d = self.tmp / 'extract' / f'{ver}_{main_bid[:4]}_{sdk_bid[:4]}'
        nso(d / 'ExeFS/main', main_bid)
        nso(d / 'ExeFS/sdk', sdk_bid)
        if nacp_ver:
            (d / 'control.nacp').write_bytes(bytes(0x3060) + nacp_ver.encode() + bytes(0x1000))
        reg = game.load(f'{PROFILE:016X}')
        out = self.tmp / 'derived' / f'{ver}_{main_bid[:4]}_{sdk_bid[:4]}'
        nsp.derive_profile(reg, out, ver, d, ['main', 'sdk'])
        self.said.clear()   # deriving logs its own line; tests check report_profile_status output only
        return reg, game.Game(out)

    def test_registered_update_derived_is_not_red(self) -> None:
        reg, g = self.derive('v65536', UPD_MAIN, UPD_SDK)
        self.assertEqual(list(g.builds()), ['v65536'])
        nsp.report_profile_status(g, reg, 'v65536', 'shader tables cover v0, not v65536', False)
        self.assertEqual(len(self.said), 1)
        self.assertIn('uses the registered v65536 (1.1.0) builds and every fix for them', self.said[0])
        self.assertNotIn('[RED]', self.said[0])

    def test_registered_build_ignores_nacp_display_spelling_difference(self) -> None:
        reg, g = self.derive('v0', BASE_MAIN, BASE_SDK, nacp_ver='1.0.0')
        self.assertEqual(g.builds()['v0']['display'], '1.0.0')
        self.assertEqual(reg.builds()['v0']['display'], 'v0')
        nsp.report_profile_status(g, reg, 'v0', 'the folder has no v65536', False)
        self.assertEqual(len(self.said), 1)
        self.assertIn('uses the registered v0 (1.0.0) builds and every fix for them', self.said[0])
        self.assertNotIn('[RED]', self.said[0])

    def test_registered_update_without_shader_covers_derives_even_with_base_present(self) -> None:
        reg = game.load(f'{PROFILE:016X}')
        self.assertFalse(nsp.profile_derivation_reason(reg, 'v0', {'v0', 'v65536'}))
        why = nsp.profile_derivation_reason(reg, 'v65536', {'v0', 'v65536'})
        self.assertIsInstance(why, str)
        self.assertIn('shader tables cover v0, not v65536', why)

    def test_registered_update_with_shader_covers_stays_non_derived_when_shader_versions_present(self) -> None:
        self.write_profile(extra=('shader_covers\tv65536',))
        reg = game.load(f'{PROFILE:016X}')
        self.assertFalse(nsp.profile_derivation_reason(reg, 'v65536', {'v0', 'v65536'}))
        why = nsp.profile_derivation_reason(reg, 'v65536', {'v65536'})
        self.assertIsInstance(why, str)
        self.assertIn('the folder has no v0', why)
        _, derived = self.derive('v65536', UPD_MAIN, UPD_SDK)
        self.assertEqual(derived.words('shader_covers', ''), [])   # stripped when deriving a single version

    def test_mixed_modules_from_different_rows_stay_red(self) -> None:
        reg, g = self.derive('v65536', BASE_MAIN, UPD_SDK)
        nsp.report_profile_status(g, reg, 'v65536', 'mismatched modules', False)
        self.assertTrue(self.said[0].startswith('[RED] '), self.said)
        self.assertIn('was not brought up on', self.said[0])
        self.assertIn('every module is a registered build', self.said[0])

    def test_mismatched_module_under_registered_version_is_red(self) -> None:
        reg, g = self.derive('v65536', NEW_MAIN, UPD_SDK)
        nsp.report_profile_status(g, reg, 'v65536', 'not registered', False)
        self.assertTrue(self.said[0].startswith('[RED] '), self.said)
        self.assertIn('main is a build the profile does not register', self.said[0])

    def test_unregistered_version_is_red(self) -> None:
        reg, g = self.derive('v131072', NEW_MAIN, UPD_SDK)
        why = nsp.profile_derivation_reason(reg, 'v131072', {'v0', 'v131072'})
        self.assertIsInstance(why, str)
        self.assertIn('is not a version game profile', why)
        nsp.report_profile_status(g, reg, 'v131072', why, False)
        self.assertTrue(self.said[0].startswith('[RED] '), self.said)
        self.assertIn('was not brought up on', self.said[0])


if __name__ == '__main__':
    unittest.main()
