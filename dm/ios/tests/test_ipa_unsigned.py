"""The unsigned IPA of build_ipa.sh: the game files stay out with DM_COPY_ROMFS=NO, and the executable carries the entitlements in an ad-hoc signature.
Only the archive step and the signing step run (a stand-in app, no Xcode or game files)."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ipa_pack_fixture import PACKING, REPO, SIGNING, run_step

sys.path.insert(0, str(REPO / 'dm/games'))
import game
sys.path.insert(0, str(REPO / 'dm/tools'))
import romfs_image

GAME = '010015100B514000'
CAN_SIGN = sys.platform == 'darwin' and shutil.which('codesign') and shutil.which('cc')


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
        (root / 'dm/tools').symlink_to(REPO / 'dm/tools', target_is_directory=True)

    def pack(self, image=False, identity=None, **extra):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.stand_in(root)
            (root / 'DmIOS.app/executable').write_text('app')
            if image: romfs_image.pack(root / 'romfs', root / 'RomFS.img')
            result = run_step(PACKING, root, GAME, APP=str(root / 'DmIOS.app'), MSL_IOS=str(root / 'shaders'), MSLDIR='b-fixture', ROMFS=str(root / ('RomFS.img' if image else 'romfs')),
                              AOC=str(root / 'aoc'), OUT_IPA=str(root / 'out.ipa'), O=str(root / 'pkg'), DM_APP_VERSION='1.2.1', PKG='v327680', **extra)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            with zipfile.ZipFile(root / 'out.ipa') as archive:
                if identity is not None: identity.append((archive.read('Payload/DmIOS.app/romfs.id').decode() if 'Payload/DmIOS.app/romfs.id' in archive.namelist() else None, (root / 'RomFS.img').read_bytes() if image else None))
                return set(archive.namelist())

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

    def test_slim_ipa_of_an_image_carries_its_identity_and_the_default_does_not(self):
        got = []
        names = self.pack(image=True, identity=got, DM_COPY_ROMFS='NO')
        self.assertFalse([n for n in names if n.endswith(('/romfs', '/romfs.img')) or '/romfs/' in n])
        self.assertIn('Payload/DmIOS.app/romfs.id', names)
        line, data = got[0]
        self.assertTrue(line.startswith(f'DMROMFSID1 title={GAME} version=1.2.1 build=v327680 size={len(data)} index='), line)   # read back
        with tempfile.NamedTemporaryFile() as f:
            f.write(data); f.flush(); self.assertTrue(romfs_image.same_identity(line, romfs_image.identity(f.name)))
        got = []
        names = self.pack(image=True, identity=got)   # bundle: exactly as before
        self.assertIn('Payload/DmIOS.app/romfs.img', names); self.assertNotIn('Payload/DmIOS.app/romfs.id', names); self.assertIsNone(got[0][0])

    def test_slim_ipa_of_a_folder_has_no_identity(self):
        self.assertNotIn('Payload/DmIOS.app/romfs.id', self.pack(DM_COPY_ROMFS='NO'))

    def test_supplemental_gl_library_is_packed_only_for_nvn_profile_with_gl_capture(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.stand_in(root)
            (root / 'DmIOS.app/executable').write_text('app')
            gl_dir = root / 'shaders/gl'
            gl_dir.mkdir()
            (gl_dir / 'gl_shaders.metallib').write_text('gl-lib')
            (gl_dir / 'gl_shaders.tsv').write_text('0\tvs\n')
            profile, empty_profile = root / 'profile', root / 'empty_profile'
            profile.mkdir(); empty_profile.mkdir()
            (profile / 'gl_capture.tsv').write_text('0.vert\n')
            cases = [
                ('out_gl.ipa', {'GD': str(profile)}, True),
                ('out_no_capture.ipa', {'GD': str(empty_profile)}, False),
                ('out_vk.ipa', {'GFXAPI': 'vulkan', 'GD': str(profile)}, False),
            ]
            for ipa_name, extra, expect_gl in cases:
                with self.subTest(ipa=ipa_name, extra=extra):
                    out = root / ipa_name
                    result = run_step(PACKING, root, GAME, APP=str(root / 'DmIOS.app'), MSL_IOS=str(root / 'shaders'), MSLDIR='b-fixture',
                                      ROMFS=str(root / 'romfs'), AOC=str(root / 'aoc'), OUT_IPA=str(out), O=str(root / 'pkg'),
                                      DM_APP_VERSION='1.2.1', PKG='v327680', **extra)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    with zipfile.ZipFile(out) as archive:
                        names = set(archive.namelist())
                    self.assertEqual('Payload/DmIOS.app/msl/gl/gl_shaders.metallib' in names, expect_gl)
                    self.assertEqual('Payload/DmIOS.app/msl/gl/gl_shaders.tsv' in names, expect_gl)

    @unittest.skipUnless(CAN_SIGN, 'needs macOS codesign and a C compiler')
    def test_executable_carries_the_entitlements(self):
        cases = [({}, True), ({'DM_ENTITLEMENTS': ''}, False)]
        for extra, expected in cases:
            with self.subTest(extra=extra), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / 'DmIOS.app').mkdir(); (root / 'pkg').mkdir()
                (root / 'main.c').write_text('int main(void) { return 0; }\n')
                subprocess.run(['cc', '-o', str(root / 'DmIOS.app/DmIOS'), str(root / 'main.c')], check=True)
                result = run_step(SIGNING, root, GAME, APP=str(root / 'DmIOS.app'), O=str(root / 'pkg'), **extra)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                shown = subprocess.run(['codesign', '-d', '--entitlements', '-', str(root / 'DmIOS.app/DmIOS')], capture_output=True, text=True)
                self.assertEqual('com.apple.developer.kernel.increased-memory-limit' in shown.stdout + shown.stderr, expected)


if __name__ == '__main__':
    unittest.main()
