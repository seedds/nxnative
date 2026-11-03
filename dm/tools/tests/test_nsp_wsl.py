"""NSP script in WSL (scripts/build_ipa_from_nsp.py, WSL = True): the tools it puts first, the prerequisites it checks instead of Xcode's, the Mac steps it refuses, the hint that
replaces push_romfs.sh, and the progress of build_app.py; and that a Mac (WSL = False) gets none of it. Every input is mocked: no WSL, Xcode or Metal needed, on any system."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
assert spec and spec.loader
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)


def fake_capstone():
    return types.SimpleNamespace(__version__='5.0.0', cs_version=lambda: (5, 0, 5 << 8))


class Environment(unittest.TestCase):
    def test_wsl_env_finds_the_setup_and_keeps_given_paths(self):
        with tempfile.TemporaryDirectory() as d:
            env = {'PATH': '/usr/bin', 'DM_WSL_CACHE': d}; nsp.wsl_env(REPO, env)
            self.assertEqual(env['PATH'].split(os.pathsep), [str(REPO / 'dm/ios/tools/wsl'), '/usr/bin'])
            self.assertEqual(env['XLATE'], str(Path(d) / 'xlate/dmsh_xlate')); self.assertEqual(env['SPIRV_CROSS'], str(Path(d) / 'spirv-cross/spirv-cross'))
            env = {'PATH': '/usr/bin', 'DM_WSL_CACHE': d, 'XLATE': '/x/dmsh_xlate'}; nsp.wsl_env(REPO, env)
            self.assertEqual(env['XLATE'], '/x/dmsh_xlate')

    def test_windows_path(self):
        with mock.patch.dict(os.environ, WSL_DISTRO_NAME='archlinux'):
            self.assertEqual(nsp.windows_path('/mnt/e/MIG Games/Z A.xci/RomFS.img'), 'E:\\MIG Games\\Z A.xci\\RomFS.img')
            self.assertEqual(nsp.windows_path('/mnt/c'), 'C:\\')
            self.assertEqual(nsp.windows_path('/home/u/nx_build/x/RomFS.img'), '\\\\wsl.localhost\\archlinux\\home\\u\\nx_build\\x\\RomFS.img')

    def test_apple_silicon_asks_only_macos(self):
        with mock.patch.object(nsp.sys, 'platform', 'linux'), mock.patch.object(nsp.subprocess, 'run') as run:
            self.assertFalse(nsp.apple_silicon())
        run.assert_not_called()

    def test_made_without_birth_time(self):   # Linux's stat has no st_birthtime: the folder's ctime stands in
        st = types.SimpleNamespace(st_ctime=5.0)
        self.assertEqual(nsp.made(types.SimpleNamespace(stat=lambda: st)), 5.0)
        st = types.SimpleNamespace(st_ctime=5.0, st_birthtime=3.0)
        self.assertEqual(nsp.made(types.SimpleNamespace(stat=lambda: st)), 3.0)


class Prereqs(unittest.TestCase):
    def check(self, wsl, which=lambda *a, **k: '/x', importable=lambda m: True):
        def run(cmd, **kw): return subprocess.CompletedProcess(cmd, 0, 'f21af1efa737a270\n' if str(cmd[-1]) == '--source-sha' else '27.2\n', '')
        with tempfile.TemporaryDirectory() as d, mock.patch.object(nsp, 'WSL', wsl), mock.patch.object(nsp, '_importable', importable), \
                mock.patch.object(nsp.shutil, 'which', which), mock.patch.object(nsp.subprocess, 'run', run), mock.patch.dict(sys.modules, capstone=fake_capstone()):
            xlate = Path(d) / 'dmsh_xlate'; xlate.write_text('')
            nsp.check_prereqs({'PATH': '/usr/bin'}, xlate)

    def test_wsl_asks_for_its_own_tools(self):
        with self.assertRaisesRegex(nsp.BuildError, r'tools not found: ldid \(AUR ldid\)$'): self.check(True, which=lambda t, **k: None if t == 'ldid' else '/x')
        with self.assertRaisesRegex(nsp.BuildError, 'python-yaml'): self.check(True, importable=lambda m: m != 'yaml')
        self.check(True, which=lambda t, **k: None if t in ('xcodebuild', 'xcodegen') else '/x')   # no Xcode

    def test_mac_asks_for_xcode_not_wsl_tools(self):
        with self.assertRaisesRegex(nsp.BuildError, r'xcodegen \(brew install xcodegen\)'): self.check(False, which=lambda t, **k: None if t == 'xcodegen' else '/x')
        self.check(False, which=lambda t, **k: None if t in ('ldid', 'swiftc') else '/x')


class MacSteps(unittest.TestCase):
    def test_mac_builds_and_the_xcode_project_need_a_mac(self):
        for options in (['--platform', 'mac'], ['--platform', 'mac-app'], ['--prepare-xcode']):
            with self.subTest(options=options), tempfile.TemporaryDirectory() as t, mock.patch.object(nsp, 'WSL', True):
                with self.assertRaisesRegex(nsp.BuildError, r'it needs a Mac \(in WSL the script packs the iOS app\)'): nsp.main(nsp.parse_args([*options, '-k', t, t]))

    def test_the_mac_host_is_refused(self):
        with tempfile.TemporaryDirectory() as t, mock.patch.object(nsp, 'WSL', True), mock.patch.object(nsp, 'run') as run:
            with self.assertRaisesRegex(nsp.BuildError, 'boots it in the Mac host'): nsp.build_mac_host(REPO, Path(t), {}, Path(t), None)
        run.assert_not_called()

    def test_oodle_decoder_is_dm_oodle_host(self):
        with tempfile.TemporaryDirectory() as t, mock.patch.object(nsp, 'WSL', True), mock.patch.object(nsp, 'build_mac_host') as host, mock.patch.object(nsp.PROG, 'say'):
            with self.assertRaisesRegex(nsp.BuildError, 'set DM_OODLE_HOST'): nsp.oodle_decoder(REPO, Path(t), {'PATH': '/usr/bin'}, Path(t), None, Path(t) / 'patched')
            dec = Path(t) / 'dec'; dec.write_text(''); dec.chmod(0o755)
            got = nsp.oodle_decoder(REPO, Path(t), {'PATH': '/usr/bin', 'DM_OODLE_HOST': str(dec)}, Path(t), None, Path(t) / 'patched')
            self.assertEqual(got, {'DM_OODLE_HOST': str(dec), 'DM_OODLE_MODULES': str(Path(t) / 'patched')})
        host.assert_not_called()


class PushHint(unittest.TestCase):
    def said(self, wsl):
        said = []
        with tempfile.TemporaryDirectory() as t, mock.patch.object(nsp, 'WSL', wsl), mock.patch.object(nsp.PROG, 'say', side_effect=said.append):
            image = Path(t) / 'RomFS.img'; image.write_bytes(b'')
            nsp.say_push_romfs(str(image), 'org.nxnative.0100f43008c44000')
        return '\n'.join(said), image

    def test_wsl_names_the_windows_path_and_file_sharing(self):
        text, image = self.said(True)
        self.assertIn(nsp.windows_path(image), text); self.assertIn('File Sharing', text); self.assertNotIn('push_romfs.sh <device UDID>', text)

    def test_mac_gets_the_push_command(self):
        text, image = self.said(False)
        self.assertIn(f'scripts/push_romfs.sh <device UDID> org.nxnative.0100f43008c44000 "{image}"', text); self.assertNotIn('File Sharing', text)


class AppBuildProgress(unittest.TestCase):
    def test_build_app_lines_count_as_xcodebuild_lines(self):   # build_ipa.sh's step line, then build_app.py's CompileC lines in build.log
        with tempfile.TemporaryDirectory() as t, mock.patch.object(nsp, 'PROG') as prog:
            repo, w = Path(t) / 'repo', Path(t) / 'w'
            for d in ('dm/ios/Gen/src', 'dm/ios/Sources'): (repo / d).mkdir(parents=True)
            for n in ('a.c', 'b.c'): (repo / 'dm/ios/Gen/src' / n).write_text('')
            (repo / 'dm/ios/Sources/main.m').write_text('')
            (w / 'build/ios/pkg').mkdir(parents=True)
            watch = nsp.IpaWatch(repo, w, Path(t) / 'out.ipa')
            watch.line("[12:00:00] build_app.py (Release, unsigned, iPhone + iPad; xtool's iOS SDK)")
            self.assertEqual(prog.set.call_args.kwargs['step'], 'build_app.py'); self.assertEqual(prog.set.call_args.kwargs['total'], 5)
            (w / 'build/ios/pkg/build.log').write_text('compile 5 C / Objective-C / assembly files and 2 Swift files\nCompileC /r/a.c\nCompileC /r/b.c\nCompileC /r/main.m\nLd /r/DmIOS (4 objects)\n')
            self.assertEqual(watch.poll(), {'done': 3, 'note': 'linking'})
            watch.line('[12:00:30] pack IPA')
            prog.end.assert_called_with('3 files compiled')


if __name__ == '__main__':
    unittest.main()
