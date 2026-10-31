"""Use real Apple string tables and the production settings model, without game files or a UI."""
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games'))
import settings


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs the macOS Swift toolchain')
class LauncherLocalizationTests(unittest.TestCase):
    def test_english_catalog_covers_the_settings_table(self):
        path = REPO / 'dm/ios/Sources/Resources/en.lproj/Launcher.strings'
        catalog = json.loads(subprocess.check_output(['plutil', '-convert', 'json', '-o', '-', str(path)], text=True))
        for section in settings.SECTIONS:
            self.assertEqual(catalog['section.' + section], section)
        for _, (env, kind, values, _, _, _, label, note, _) in settings.rows():
            with self.subTest(env=env):
                self.assertEqual(catalog[f'setting.{env}.label'], label)
                if note:
                    self.assertEqual(catalog[f'setting.{env}.note'], note)
                if kind == 'choice':
                    options = settings.nacp.LANGUAGES if values == 'languages' else [v.split('=', 1) for v in values.split(',')]
                    for value, text in options:
                        self.assertEqual(catalog[f'setting.{env}.option.{value}'], text)

    def test_literal_ui_keys_are_in_the_catalog(self):
        path = REPO / 'dm/ios/Sources/Resources/en.lproj/Launcher.strings'
        catalog = json.loads(subprocess.check_output(['plutil', '-convert', 'json', '-o', '-', str(path)], text=True))
        for source in ('dm/ios/Sources/Launcher.swift', 'dm/ios/Sources/LauncherRow.swift', 'dm/mac/Launcher.swift'):
            for literal in re.findall(r'launcherString\(("(?:[^"\\]|\\.)*")', (REPO / source).read_text()):
                key = json.loads(literal)
                with self.subTest(source=source, key=key):
                    self.assertEqual(catalog[key], key)

    def test_native_translation_fallback_and_raw_values(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            app = root / 'Fixture.app/Contents'
            exe = app / 'MacOS/check'
            exe.parent.mkdir(parents=True)
            resources = app / 'Resources'
            resources.mkdir()
            shutil.copytree(REPO / 'dm/ios/Sources/Resources/en.lproj', resources / 'en.lproj')
            (app / 'Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': 'org.nxnative.localization-test',
                'CFBundleExecutable': 'check', 'CFBundleDevelopmentRegion': 'en', 'CFBundlePackageType': 'APPL'}))
            translated = resources / 'fr.lproj'
            translated.mkdir()
            strings = {'Play': 'Translated play', 'Version %@': '%1$@ translated version',
                'setting.DM_OPMODE.label': 'Translated console mode', 'setting.DM_OPMODE.option.0': 'Translated handheld',
                'setting.DM_PRESENT_UPSCALE.note': 'Translated GPU note', 'setting.DM_LANG.option.ja': 'Translated Japanese',
                'section.Game': 'Translated section', 'section.Controls': 'Translated section'}
            (translated / 'Launcher.strings').write_text('\n'.join(f'{json.dumps(k)} = {json.dumps(v)};' for k, v in strings.items()))
            with patch.dict(os.environ, {'GFXAPI': 'nvn', 'SETTINGS_PLATFORM': 'mac', 'DM_LANG': 'en-US'}), \
                    patch.object(settings.nacp, 'languages', return_value=['en-US', 'ja']):
                data = settings.build('01007EF00011E000', root, root / 'control.nacp')
            (root / 'launcher.json').write_text(json.dumps(data))
            command = ['xcrun', '-sdk', 'macosx', 'swiftc', '-swift-version', '5', '-parse-as-library',
                       '-module-cache-path', str(root / 'module-cache'), '-o', str(exe),
                       str(REPO / 'dm/ios/Sources/LauncherSettings.swift'), str(Path(__file__).with_name('launcher_localization_check.swift'))]
            compile_result = subprocess.run(command, capture_output=True, text=True, timeout=300)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout + compile_result.stderr)
            env = {k: v for k, v in os.environ.items() if not k.startswith('DM_')}
            for language, extra in [('fr', []), ('de', ['--english'])]:  # no German table in this fixture
                with self.subTest(language=language):
                    result = subprocess.run([str(exe), str(root / 'launcher.json'), '-AppleLanguages', f'({language})', *extra], env=env,
                                            capture_output=True, text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn('launcher localization: passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
