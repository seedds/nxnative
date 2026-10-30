"""Route C shell (docs/launcher-c-plan.md M2): dm/ios/tools/build_shell.sh, off a device.

The checks of the frameworks it is given run anywhere: a manifest of another shader chain, two frameworks of one title, a folder
whose name the manifest does not give and a missing executable are refused before anything is generated, and two games go into one
shell (the host reads each game's runtime profile). With XcodeGen and an iPhoneOS SDK the shell is also generated and built, unsigned, around a synthetic
framework (a one-symbol dylib, no game code): the app embeds the framework without linking it (the shell dlopens it), carries
LC_RPATH @executable_path/Frameworks, the frontend-ios package's resource bundles, its name and the chain in Info.plist, and a prepared
project asks for the Increased Memory Limit (DM_ENTITLEMENTS= leaves it out). No game files, shaders or signing are involved.
"""
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SCRIPT = REPO / 'dm/ios/tools/build_shell.sh'
GAME = '010015100B514000'   # a profile in dm/games: the host is generated for the first framework's title
OTHER = '0100000000010000'
CHAIN = 'b-' + next(line.split('"')[1] for line in (REPO / 'dm/gfx/nvn/dmsh_chain.h').read_text().splitlines()
                    if line.startswith('#define DMSH_CHAIN "'))
HAS_TOOLS = (sys.platform == 'darwin' and shutil.which('xcodegen') and shutil.which('xcodebuild')
             and subprocess.run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], capture_output=True, check=False).returncode == 0)


def font_python():
    """A python that has fontTools, for make_ext_font.py (FONT_PY), or None."""
    for py in (os.environ.get('FONT_PY'), sys.executable, shutil.which('python3')):
        if py and subprocess.run([py, '-c', 'import fontTools'], capture_output=True, check=False).returncode == 0: return py
    return None


def framework(root, title=GAME, chain=CHAIN, name=None, executable=True, compile_it=False):
    """A framework folder as build_ipa.sh DM_FRAMEWORK_ONLY writes it, with a manifest and, optionally, a real arm64 dylib."""
    name = name or f'DmGame{title}'
    fw = Path(root) / f'{name}.framework'
    (fw / 'dm').mkdir(parents=True)
    exe = f'DmGame{title}'
    manifest = {'schema': 1, 'title_id': title, 'name': f'Fixture {title}', 'display_version': '1.0', 'package_version': 'v0',
                'modules': ['main'], 'nro_count': 0, 'chain': chain, 'loader': 'FR', 'framework': f'DmGame{title}.framework',
                'framework_executable': exe, 'image_marker': 'dmgame_image_marker', 'profile': 'profile.tsv',
                'shader_map': 'dm/shader_map.tsv', 'launcher': 'dm/launcher.json', 'romfs_marker': 'fixture.marker',
                'romfs_identity': '', 'min_os': '26.0'}
    (fw / 'game.manifest.json').write_text(json.dumps(manifest, indent=2))
    (fw / 'dm/launcher.json').write_text(json.dumps({'schema': 1, 'game': title, 'settings': []}))
    (fw / 'profile.tsv').write_text(f'id\t{title}\n')
    plist = {'CFBundleExecutable': exe, 'CFBundleIdentifier': f'org.nxnative.ci.{title.lower()}', 'CFBundleName': exe,
             'CFBundlePackageType': 'FMWK', 'CFBundleShortVersionString': '1.0', 'CFBundleVersion': '1.0',
             'CFBundleSupportedPlatforms': ['iPhoneOS'], 'MinimumOSVersion': '26.0'}
    (fw / 'Info.plist').write_bytes(plistlib.dumps(plist))
    if compile_it:
        src = Path(root) / 'marker.c'
        src.write_text('__attribute__((used, visibility("default"))) const char dmgame_image_marker[] = "DmGame";\n')
        r = subprocess.run(['xcrun', '-sdk', 'iphoneos', 'clang', '-arch', 'arm64', '-mios-version-min=26.0', '-dynamiclib',
                            '-install_name', f'@rpath/{exe}.framework/{exe}', str(src), '-o', str(fw / exe)], capture_output=True, text=True, check=False)
        assert r.returncode == 0, r.stderr
    elif executable:
        (fw / exe).write_bytes(b'stand-in')
    return fw


def run_script(mode, frameworks, shell_dir, **extra):
    env = dict(os.environ, SHELL_DIR=str(shell_dir), DM_SHELL_FRAMEWORKS='\n'.join(str(f) for f in frameworks), **extra)
    return subprocess.run(['bash', str(SCRIPT), mode], env=env, capture_output=True, text=True, check=False)


class FrameworkChecks(unittest.TestCase):
    """What build_shell.sh refuses before it generates anything."""
    def setUp(self):
        self.root = Path(tempfile.mkdtemp()).resolve()
        self.addCleanup(shutil.rmtree, self.root)
        self.shell = self.root / 'shell'

    def refused(self, frameworks, text):
        r = run_script('--prepare-xcode', frameworks, self.shell)
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn(text, r.stdout + r.stderr)
        self.assertFalse((self.shell / 'Gen').exists(), 'nothing is generated for a refused set')

    def test_another_chain_is_refused(self):
        self.refused([framework(self.root, chain='b-000000000000')], 'built for shader chain b-000000000000')

    def test_two_frameworks_of_one_title_are_refused(self):
        a = framework(self.root / 'a')
        b = framework(self.root / 'b')
        self.refused([a, b], f'title ID {GAME} is already in the shell')

    def test_two_games_go_into_one_shell(self):
        # Checked only up to the frameworks list here (the generation needs XcodeGen): both games, the first one's profile compiled in
        r = run_script('--prepare-xcode', [framework(self.root / 'a'), framework(self.root / 'b', title=OTHER)], self.shell, PATH='/usr/bin:/bin')
        rows = (self.shell / 'frameworks.txt').read_text().splitlines()
        self.assertEqual([row.split('\t')[0] for row in rows], [GAME, OTHER], r.stdout)
        self.assertIn(f'compiled defaults of {GAME}', r.stdout)

    def test_a_folder_the_manifest_does_not_name_is_refused(self):
        self.refused([framework(self.root, name='Renamed')], f'the manifest names DmGame{GAME}.framework')

    def test_a_framework_without_its_executable_is_refused(self):
        self.refused([framework(self.root, executable=False)], f'no executable DmGame{GAME}')

    def test_no_frameworks_and_no_mode_are_refused(self):
        r = run_script('--prepare-xcode', [], self.shell)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('DM_SHELL_FRAMEWORKS is not set', r.stdout)
        r = run_script('--sign', [framework(self.root)], self.shell)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('usage', r.stdout)


class ShellMainProvidesWhatFrontIosTakes(unittest.TestCase):
    """front_ios.m is compiled into the shell unchanged and main.m is not linked, so ShellMain.m has to define every function
    front_ios.m declares as coming from main.m (a missing one is a link error of every shell build, found only on a device build)."""
    def test_every_symbol_front_ios_takes_from_main_m_is_defined(self):
        front = (REPO / 'dm/ios/Sources/front_ios.m').read_text()
        wanted = re.findall(r'^[A-Za-z_][\w \t\*]*?\b(\w+)\([^)]*\);[ \t]*//[^\n]*\bmain\.m\b', front, re.M)
        self.assertIn('front_ios_boot_message', wanted)   # the scan finds the declarations at all
        shell = (REPO / 'dm/ios/Shell/ShellMain.m').read_text()
        missing = [n for n in dict.fromkeys(wanted) if not re.search(r'^[A-Za-z_][\w \t\*]*?\b' + n + r'\([^)]*\)[ \t]*\{', shell, re.M)]
        self.assertEqual(missing, [], 'ShellMain.m does not define what front_ios.m takes from main.m')


@unittest.skipUnless(HAS_TOOLS and font_python(), 'needs macOS, XcodeGen, an iPhoneOS SDK and a python with fontTools')
class ShellBuild(unittest.TestCase):
    def test_the_shell_embeds_the_framework_unlinked_and_builds(self):
        root = Path(tempfile.mkdtemp()).resolve()
        self.addCleanup(shutil.rmtree, root)
        fw = framework(root / 'frameworks', compile_it=True)
        shell = root / 'shell'
        r = run_script('--prepare-xcode', [fw], shell, FONT_PY=font_python(), DM_SHELL_NAME='Shell Fixture', DM_BUNDLE_ID='org.nxnative.ci.shell')
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])
        spec = (shell / 'project.yml').read_text()
        self.assertIn(f'- framework: "{fw}"', spec)
        self.assertIn('embed: true', spec)
        self.assertIn('link: false', spec)
        self.assertIn('path: ${DM_SHELL_PACKAGE}', spec)   # build_shell.sh exports it: frontend-ios in this checkout
        self.assertIn('CODE_SIGN_ENTITLEMENTS = Gen/DmShell.entitlements', (shell / 'Gen/dm.xcconfig').read_text())
        self.assertIn('increased-memory-limit', (shell / 'Gen/DmShell.entitlements').read_text())
        pbx = (shell / 'DmShell.xcodeproj/project.pbxproj').read_text()
        self.assertIn('Embed Frameworks', pbx)
        self.assertIn('XCLocalSwiftPackageReference', pbx)
        r = run_script('--prepare-xcode', [fw], shell, FONT_PY=font_python(), DM_ENTITLEMENTS='')
        self.assertEqual(r.returncode, 0, r.stdout[-3000:])
        self.assertNotIn('CODE_SIGN_ENTITLEMENTS', (shell / 'Gen/dm.xcconfig').read_text())

        r = run_script('--build', [fw], shell, FONT_PY=font_python(), DM_SHELL_NAME='Shell Fixture', DM_BUNDLE_ID='org.nxnative.ci.shell')
        log = (shell / 'build.log').read_text() if (shell / 'build.log').exists() else ''
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + log[-4000:])
        app = shell / 'dd/Build/Products/Release-iphoneos/DmShell.app'
        info = plistlib.loads((app / 'Info.plist').read_bytes())
        self.assertEqual(info['CFBundleDisplayName'], 'Shell Fixture')
        self.assertEqual(info['CFBundleIdentifier'], 'org.nxnative.ci.shell')
        self.assertEqual('b-' + info['DMChain'], CHAIN)
        self.assertEqual(info['CFBundleIcons']['CFBundlePrimaryIcon']['CFBundleIconName'], 'AppIcon')   # frontend-ios/AppIcon.icon
        self.assertTrue((app / 'Assets.car').is_file())
        self.assertEqual(info['UIApplicationSceneManifest']['UISceneConfigurations']['UIWindowSceneSessionRoleApplication'][0]['UISceneDelegateClassName'], 'SceneDelegate')
        self.assertTrue((app / f'Frameworks/{fw.name}/{fw.stem}').is_file())
        self.assertTrue((app / f'Frameworks/{fw.name}/game.manifest.json').is_file())
        self.assertTrue((app / 'dm/font/standard.ttf').is_file())
        self.assertTrue(any(p.name.endswith('ShellHost.bundle') for p in app.iterdir()), 'the ShellHost package resources')
        load = subprocess.run(['otool', '-l', str(app / 'DmShell')], capture_output=True, text=True, check=False).stdout
        self.assertIn('@executable_path/Frameworks', load)
        self.assertNotIn(fw.stem, subprocess.run(['otool', '-L', str(app / 'DmShell')], capture_output=True, text=True, check=False).stdout,
                         'the shell dlopens the game framework, it must not link it')


if __name__ == '__main__':
    unittest.main()
