"""The unsigned IPA of build_ipa.sh: the game files stay out with DM_COPY_ROMFS=NO, and the executable carries the entitlements in an ad-hoc signature.
Only the archive step and the signing step run (a stand-in app, no Xcode or game files)."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games'))
import game

GAME = '010015100B514000'
SCRIPT = (REPO / 'dm/ios/tools/build_ipa.sh').read_text()
SIGNING = SCRIPT[SCRIPT.index('# The entitlements of an unsigned IPA'):SCRIPT.index('\nP=Payload/DmIOS.app\n')]
PACKING = SCRIPT[SCRIPT.index('\nP=Payload/DmIOS.app\n'):]
PREAMBLE = 'set -u\nt() { :; }; die() { echo "$*" >&2; exit 1; }\ngp() { python3 "$PROFILE_TOOL" "$GAME" "$1"; }\n'
CAN_SIGN = sys.platform == 'darwin' and shutil.which('codesign') and shutil.which('cc')


def run_step(script, root, **extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith('DM_') and k not in ('IOS_PULL', 'AOC', 'PSO')}
    env.update(GAME=GAME, DM=str(root / 'dm'), PROFILE_TOOL=str(REPO / 'dm/games/game.py'), **extra)
    return subprocess.run(['/bin/bash', '-c', PREAMBLE + script], cwd=root, env=env, capture_output=True, text=True)


class UnsignedIpaTests(unittest.TestCase):
    def stand_in(self, root):
        header = root / 'dm/host/gen' / GAME / 'game_profile.h'
        header.parent.mkdir(parents=True)
        header.write_text(game.header(game.Game(REPO / 'dm/games' / GAME)))
        for folder in ('DmIOS.app', 'shaders/b-fixture', 'romfs', 'aoc/0', 'pkg'): (root / folder).mkdir(parents=True)
        (root / 'shaders/b-fixture/fixture_b.metallib').write_text('lib')
        (root / 'romfs/marker').write_text('romfs')
        (root / 'aoc/0/file').write_text('aoc')
        (root / 'tools').symlink_to(REPO / 'dm/ios/tools', target_is_directory=True)

    def pack(self, **extra):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.stand_in(root)
            (root / 'DmIOS.app/executable').write_text('app')
            result = run_step(PACKING, root, APP=str(root / 'DmIOS.app'), MSL_IOS=str(root / 'shaders'), MSLDIR='b-fixture', ROMFS=str(root / 'romfs'),
                              AOC=str(root / 'aoc'), OUT_IPA=str(root / 'out.ipa'), **extra)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            with zipfile.ZipFile(root / 'out.ipa') as archive: return set(archive.namelist())

    def test_game_files_are_packed_by_default(self):
        names = self.pack()
        self.assertIn('Payload/DmIOS.app/romfs/marker', names)
        self.assertIn('Payload/DmIOS.app/aoc/0/file', names)

    def test_copy_romfs_no_leaves_the_game_files_out_and_keeps_the_rest(self):
        names = self.pack(DM_COPY_ROMFS='NO')
        self.assertFalse([n for n in names if '/romfs' in n])
        self.assertIn('Payload/DmIOS.app/executable', names)
        self.assertIn('Payload/DmIOS.app/msl/b-fixture/fixture_b.metallib', names)
        self.assertIn('Payload/DmIOS.app/aoc/0/file', names)   # add-on content has no place outside the app yet

    @unittest.skipUnless(CAN_SIGN, 'needs macOS codesign and a C compiler')
    def test_executable_carries_the_entitlements(self):
        cases = [({}, True), ({'DM_ENTITLEMENTS': ''}, False)]
        for extra, expected in cases:
            with self.subTest(extra=extra), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / 'DmIOS.app').mkdir(); (root / 'pkg').mkdir()
                (root / 'main.c').write_text('int main(void) { return 0; }\n')
                subprocess.run(['cc', '-o', str(root / 'DmIOS.app/DmIOS'), str(root / 'main.c')], check=True)
                result = run_step(SIGNING, root, APP=str(root / 'DmIOS.app'), O=str(root / 'pkg'), **extra)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                shown = subprocess.run(['codesign', '-d', '--entitlements', '-', str(root / 'DmIOS.app/DmIOS')], capture_output=True, text=True)
                self.assertEqual('com.apple.developer.kernel.increased-memory-limit' in shown.stdout + shown.stderr, expected)


if __name__ == '__main__':
    unittest.main()
