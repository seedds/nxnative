"""NSP script Mac-only build (scripts/build_ipa_from_nsp.py --platform mac): the shader steps build only the Mac's libraries, each output keeps its own reuse key, a Mac-only build is
reused after an iOS build (which builds the Mac's too) and an iOS build is not reused after a Mac-only one, neither key without the Mac libraries' stamp (inputs.tsv, which the one
chain version's c1_full.tsv / cb_extent.tsv name); the run command's environment file is read as the iOS app reads it.
No game files, keys or Xcode needed (temporary files only in a temporary folder)."""
import importlib.util
from pathlib import Path
import shutil
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
SCRIPT = REPO / 'scripts/build_ipa_from_nsp.py'
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', SCRIPT)
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
CHAIN, INPUTS = '0123456789ab', 'aaaaaaaaaaaa'


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.sh, self.logs, self.ran = self.tmp / 'shader', self.tmp / 'logs', []
        self.logs.mkdir()
        for cm in (mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'say', lambda m: None), mock.patch.object(nsp, 'run', self.fake_run)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def fake_run(self, cmd, log, env=None, cwd=None, on_line=None):   # stands in for the translators: writes the libraries a real run writes
        self.ran.append(([str(c) for c in cmd], dict(env or {})))
        tool = Path(cmd[1]).name
        if tool in ('vk_shaders.py', 'gl_shaders.py'):
            api, a = tool[:2], [str(c) for c in cmd]
            outs = [Path(a[a.index('--ios') - 1]), Path(a[a.index('--ios') + 1])] if '--ios' in a else [Path(a[-1])]
            for o in outs: (o / api).mkdir(parents=True, exist_ok=True); (o / api / f'{api}_shaders.metallib').write_bytes(b'lib')
            log.write_text('macosx: 3 shaders\n' + ('iphoneos: 3 shaders\n' if '--ios' in a else ''))
        elif tool == 'own_compilers.py':   # a full dmsh_build.py run: the libraries, their stamp, and the profile's tables naming it
            outs = [self.sh / 'cache'] + ([Path(env['IOSOUT'])] if 'IOSOUT' in env else [])
            for o in outs:
                (o / f'b-{CHAIN}').mkdir(parents=True, exist_ok=True); (o / f'b-{CHAIN}' / 'k_b.metallib').write_bytes(b'lib')
                (o / f'b-{CHAIN}' / 'inputs.tsv').write_text(f'# stamp\ngame\t0100535012974000\ninputs\t{INPUTS}\n')
            for n in ('c1_full.tsv', 'cb_extent.tsv'): (self.tmp / n).write_text(f'# chain b-{CHAIN} inputs {INPUTS} 2026-10-23T00:00:00+00:00\n')
            log.write_text('shaders 1: ok 1\n')


class Layer(Base):
    def vk(self, mac, key='K'):
        self.ran.clear()
        return nsp.layer_shaders(sys.executable, REPO, 'vk', 'Vulkan', [self.tmp / 'romfs'], self.sh, key, self.logs, {'WORK': str(self.tmp)}, mac)

    def test_mac_only(self):   # no --ios, only the Mac key, the Mac folder for DM_MSL_DIR
        self.assertEqual(self.vk(True), self.sh)
        self.assertNotIn('--ios', self.ran[0][0])
        self.assertTrue((self.sh / 'vk.key').is_file()); self.assertFalse((self.sh / 'ios/vk.key').exists())
        self.assertEqual(self.vk(True), self.sh); self.assertEqual(self.ran, [])   # reused

    def test_mac_after_ios(self):   # an iOS build makes the Mac library too: a Mac-only build is reused
        self.assertEqual(self.vk(False), self.sh / 'ios')
        self.assertIn('--ios', self.ran[0][0])
        (self.sh / 'vk.key').unlink()   # an iOS build from before Mac-only builds wrote only its own key
        self.assertEqual(self.vk(True), self.sh); self.assertEqual(self.ran, [])

    def test_ios_after_mac(self):   # a Mac-only build made no iPhone library: an iOS build runs
        self.vk(True)
        self.assertEqual(self.vk(False), self.sh / 'ios'); self.assertEqual(len(self.ran), 1)
        self.assertTrue((self.sh / 'ios/vk/vk_shaders.metallib').is_file())

    def test_changed_inputs(self):
        self.vk(False)
        self.vk(True, 'K2'); self.assertEqual(len(self.ran), 1)
        self.assertTrue((self.sh / 'ios/vk/vk_shaders.metallib').is_file())   # the iPhone library stays, under its own (older) key
        self.assertEqual(nsp.reused(self.sh / 'ios/vk.key', 'K'), '3 shaders for the Mac and iOS')


class Nvn(Base):
    def setUp(self):
        super().setUp()
        self.g = types.SimpleNamespace(table=lambda n: self.tmp / n, dir=self.tmp)
        for cm in (mock.patch.object(nsp, 'fingerprint', lambda *a, **k: 'K'), mock.patch.object(nsp, 'chain_of', lambda repo: CHAIN)):   # one chain version for every game
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def build(self, mac):
        self.ran.clear()
        return nsp.build_shaders(sys.executable, REPO, self.sh, REPO / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate', {'WORK': str(self.tmp), 'IOSOUT': '/elsewhere'}, self.logs, self.g, pkg_key='P', mac=mac)

    def test_mac_only(self):   # no IOSOUT (not even the caller's), the Mac key beside the cache folder, not in it
        self.assertEqual(self.build(True), self.sh / 'cache')
        self.assertNotIn('IOSOUT', self.ran[0][1])
        self.assertTrue((self.sh / f'mac-b-{CHAIN}.key').is_file()); self.assertEqual([p.name for p in (self.sh / 'cache').iterdir()], [f'b-{CHAIN}'])
        self.assertEqual(self.build(True), self.sh / 'cache'); self.assertEqual(self.ran, [])

    def test_mac_after_ios(self):
        self.assertEqual(self.build(False), self.sh / 'ios')
        self.assertEqual(self.ran[0][1]['IOSOUT'], str(self.sh / 'ios'))
        (self.sh / f'mac-b-{CHAIN}.key').unlink()   # an iOS build from before Mac-only builds
        self.assertEqual(self.build(True), self.sh / 'cache'); self.assertEqual(self.ran, [])

    def test_ios_after_mac(self):
        self.build(True)
        self.assertEqual(self.build(False), self.sh / 'ios'); self.assertEqual(len(self.ran), 1)

    def test_no_stamp(self):   # Mac libraries no full build stamped (an interrupted one): no table can name them, so neither key reuses them
        self.build(False)
        (self.sh / 'cache' / f'b-{CHAIN}' / 'inputs.tsv').unlink()
        self.assertEqual(self.build(True), self.sh / 'cache'); self.assertEqual(len(self.ran), 1)
        self.assertTrue((self.sh / 'cache' / f'b-{CHAIN}' / 'inputs.tsv').is_file())

    def test_stale_tables(self):   # a reused build makes a table again when it does not name the Mac libraries' stamp
        self.build(True)
        (self.tmp / 'cb_extent.tsv').write_text(f'# chain b-{CHAIN} inputs bbbbbbbbbbbb 2026-10-23T00:00:00+00:00\n')
        self.assertEqual(self.build(True), self.sh / 'cache'); self.assertEqual([Path(c[1]).name for c, _ in self.ran], ['cb_extent.py'])


class EnvFile(Base):
    def test_pairs(self):
        p = self.tmp / 'env.txt'; p.write_text('# a comment\nDM_A=1\n  DM_B = x=y\n=nokey\nnoeq\n\nDM_C=\n')
        self.assertEqual(nsp.env_file_pairs(p), {'DM_A': '1', 'DM_B': ' x=y', 'DM_C': ''})

    def test_missing(self):
        with self.assertRaises(nsp.BuildError): nsp.env_file_pairs(self.tmp / 'none.txt')


class AppBundle(Base):
    def test_thin_bundle_structure_and_symlinks(self):
        from dm.tools.pack_mac_app import build_app_bundle
        work = self.tmp / 'work'; work.mkdir()
        host = work / 'dmhost'; host.write_bytes(b'host')
        patched = work / 'patched'; patched.mkdir(); (patched / 'main.fixups.tsv').write_text('table')
        romfs = work / 'RomFS'; romfs.mkdir(); (romfs / 'game.data').write_bytes(b'data')
        msl = work / 'msl'; msl.mkdir(); (msl / 'shader.metallib').write_bytes(b'shader')
        font = work / 'font'; font.mkdir(); (font / 'font.ttf').write_bytes(b'font')
        shmap = work / 'shader_map.tsv'; shmap.write_text('shmap')
        out_app = self.tmp / 'TestGame.app'

        with mock.patch('subprocess.run'):
            app = build_app_bundle(
                game_id='0100000000000001',
                work_dir=work,
                host_bin=host,
                patched_dir=patched,
                romfs_dir=romfs,
                msl_dir=msl,
                font_dir=font,
                shmap_path=shmap,
                control_nacp=work / 'control.nacp',
                icon_jpg=None,
                out_app=out_app,
                mode='thin'
            )
        self.assertEqual(app, out_app.resolve())
        self.assertTrue((out_app / 'Contents/MacOS/Game').is_file())
        self.assertTrue((out_app / 'Contents/Info.plist').is_file())
        self.assertTrue((out_app / 'Contents/PkgInfo').is_file())

        # Thin mode: proves large assets are symlinked, not duplicated
        res = out_app / 'Contents/Resources'
        self.assertTrue((res / 'romfs').is_symlink())
        self.assertEqual((res / 'romfs').resolve(), romfs.resolve())
        self.assertTrue((res / 'msl').is_symlink())
        self.assertEqual((res / 'msl').resolve(), msl.resolve())
        self.assertTrue((res / 'dm/font').is_symlink())
        self.assertEqual((res / 'dm/font').resolve(), font.resolve())
        self.assertTrue((res / 'dm/main.fixups.tsv').is_symlink())

    def test_standalone_bundle_copies(self):
        from dm.tools.pack_mac_app import build_app_bundle
        work = self.tmp / 'work'; work.mkdir()
        host = work / 'dmhost'; host.write_bytes(b'host')
        patched = work / 'patched'; patched.mkdir(); (patched / 'main.fixups.tsv').write_text('table')
        romfs = work / 'RomFS'; romfs.mkdir(); (romfs / 'game.data').write_bytes(b'data')
        msl = work / 'msl'; msl.mkdir(); (msl / 'shader.metallib').write_bytes(b'shader')
        out_app = self.tmp / 'StandaloneGame.app'

        with mock.patch('subprocess.run'):
            app = build_app_bundle(
                game_id='0100000000000001',
                work_dir=work,
                host_bin=host,
                patched_dir=patched,
                romfs_dir=romfs,
                msl_dir=msl,
                font_dir=None,
                shmap_path=None,
                control_nacp=work / 'control.nacp',
                icon_jpg=None,
                out_app=out_app,
                mode='standalone'
            )
        res = out_app / 'Contents/Resources'
        self.assertFalse((res / 'romfs').is_symlink())
        self.assertTrue((res / 'romfs/game.data').is_file())
        self.assertFalse((res / 'msl').is_symlink())
        self.assertTrue((res / 'msl/shader.metallib').is_file())


if __name__ == '__main__':
    unittest.main()
