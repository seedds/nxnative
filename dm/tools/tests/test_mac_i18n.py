"""Mac launcher catalogs and native localization, without a game or a visible window."""
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
import uuid

REPO = Path(__file__).resolve().parents[3]
RESOURCES = REPO / 'dm/mac/Resources'
TEST_ROOT = Path(os.environ.get('NXNATIVE_TEST_ROOT', tempfile.gettempdir()))
sys.path.insert(0, str(REPO / 'dm/games'))
import settings


def strings(path):
    """Read the quoted keys/values and comments used in these UTF-8 .strings files."""
    quoted = r'"(?:[^"\\]|\\.)*"'
    token = re.compile(r'\s*(?://[^\n]*(?:\n|$)|/\*.*?\*/|(' + quoted + r')\s*=\s*(' + quoted + r')\s*;)', re.S)
    data, result, pos = path.read_text(encoding='utf-8'), {}, 0
    while data[pos:].strip():
        match = token.match(data, pos)
        if not match:
            raise ValueError(f'{path}: invalid string table near {data[pos:pos + 60]!r}')
        if match[1] is not None:
            key, value = json.loads(match[1]), json.loads(match[2])
            if key in result:
                raise ValueError(f'{path}: repeated key {key!r}')
            result[key] = value
        pos = match.end()
    return result


class CatalogTests(unittest.TestCase):
    def test_catalogs_have_the_same_keys_and_format_arguments(self):
        # Include InfoPlist.strings: the Bluetooth permission prompt is shown by macOS.
        english = {p.name: p for p in (RESOURCES / 'en.lproj').glob('*.strings')}
        chinese = {p.name: p for p in (RESOURCES / 'zh-Hans.lproj').glob('*.strings')}
        self.assertEqual(set(english), {'Launcher.strings', 'InfoPlist.strings'})
        self.assertEqual(set(english), set(chinese))
        argument = re.compile(r'%(?:\d+\$)?[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|ll|[hlLzjtq])?[@diuoxXfFeEgGaAcCsSp]')
        for table in english:
            en, zh = strings(english[table]), strings(chinese[table])
            self.assertEqual(set(en), set(zh), table)
            for key in en:
                with self.subTest(table=table, key=key):
                    self.assertTrue(en[key].strip())
                    self.assertTrue(zh[key].strip())
                    self.assertEqual(argument.findall(en[key]), argument.findall(zh[key]))
                    if table == 'Launcher.strings':
                        self.assertEqual(key, en[key])

    def test_every_mac_setting_and_dynamic_language_note_is_covered(self):
        required = {settings.UNKNOWN_LANGUAGES, *settings.SECTIONS}
        for _, row in settings.rows():
            _, kind, values, _, condition, section, label, note, _ = row
            if condition == 'platform:ios':
                continue
            required.update(filter(None, (section, label, note)))
            if kind == 'choice':
                if values == 'languages':
                    required.update(label for _, label in settings.nacp.LANGUAGES)
                else:
                    for value in values.split(','):
                        option, _, conditions = value.partition('?')
                        if 'platform:ios' not in conditions.split('&'):
                            required.add(option.split('=', 1)[1])
        self.assertFalse(required - strings(RESOURCES / 'en.lproj/Launcher.strings').keys())
        self.assertFalse(required - strings(RESOURCES / 'zh-Hans.lproj/Launcher.strings').keys())

    def test_static_mac_ui_and_save_messages_are_covered(self):
        required = {'Set at launch', 'Set by an env file', 'Cancel', 'Quit', 'another device',
                    'General', 'Language', 'Applies only to this settings window.',
                    'Choose a folder…', 'Choose a different folder…',
                    'Which save do you want to play?', "The cloud save didn't come"}
        quoted = r'"(?:[^"\\]|\\.)*"'
        for name in ('Launcher.swift', 'I18n.swift'):
            source = (REPO / 'dm/mac' / name).read_text()
            for match in re.finditer(r'\b(?:MacI18n\.)?(?:text|format)\(\s*(' + quoted + ')', source):
                required.add(json.loads(match[1]))
        source = (REPO / 'dm/mac/launcher.c').read_text()
        for match in re.finditer(r'\balert\([^,]+,\s*(' + quoted + ')', source):
            required.add(json.loads(match[1]))
        for locale in ('en', 'zh-Hans'):
            self.assertFalse(required - strings(RESOURCES / f'{locale}.lproj/Launcher.strings').keys())


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs the macOS Swift toolchain')
class NativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        TEST_ROOT.mkdir(parents=True, exist_ok=True)
        cls.tmp = Path(tempfile.mkdtemp(prefix='mac-i18n-', dir=TEST_ROOT))
        cls.addClassCleanup(shutil.rmtree, cls.tmp)
        bundle_id = 'org.nxnative.localization-test.' + uuid.uuid4().hex
        cls.addClassCleanup(subprocess.run, ['defaults', 'delete', bundle_id], capture_output=True)
        contents = cls.tmp / 'Localization.app/Contents'
        binary = contents / 'MacOS'; binary.mkdir(parents=True)
        shutil.copytree(RESOURCES, contents / 'Resources')
        (contents / 'Info.plist').write_bytes(plistlib.dumps({
            'CFBundleIdentifier': bundle_id, 'CFBundleName': 'Localization Test',
            'CFBundleExecutable': 'i18n_check', 'CFBundlePackageType': 'APPL', 'CFBundleDevelopmentRegion': 'en',
            'NSBluetoothAlwaysUsageDescription': 'Connect Switch 2 Joy-Con and Pro Controllers over Bluetooth.',
        }))
        cls.exe = binary / 'i18n_check'
        sources = ['dm/mac/I18n.swift', 'dm/ios/Sources/LauncherSettings.swift', 'dm/ios/Sources/LauncherRow.swift',
                   'dm/ios/Sources/SaveSyncReport.swift', 'dm/mac/tests/i18n_check.swift']
        result = subprocess.run(['xcrun', '-sdk', 'macosx', 'swiftc', '-swift-version', '5', '-parse-as-library',
                                 '-module-cache-path', str(cls.tmp / 'module-cache'), '-o', str(cls.exe),
                                 *(str(REPO / source) for source in sources)], capture_output=True, text=True, timeout=300)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_check(self, language, *arguments):
        # Separate launches avoid Bundle and UserDefaults language caches between assertions.
        env = {k: v for k, v in os.environ.items() if not k.startswith('DM_')}
        env['DM_LANG'] = 'ja'   # The game's language never selects the launcher's language.
        result = subprocess.run([str(self.exe), '-AppleLanguages', f'({language})', *arguments],
                                env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('mac i18n: 0 failed', result.stdout)

    def test_language_selection_and_raw_settings(self):
        for language, expected in [('zh-Hans', 'zh'), ('zh-Hans-CN', 'zh'), ('zh-CN', 'zh'), ('en', 'en'),
                                   ('de', 'en'), ('ja', 'en'), ('zh-Hant', 'en'), ('de,zh-Hans', 'zh')]:
            with self.subTest(language=language):
                self.run_check(language, expected)

    def test_manual_language_only_affects_the_current_launch(self):
        for selected, system, expected in [('zh-Hans', 'en', 'en'), ('en', 'zh-Hans', 'zh'), ('zh-Hans', 'de', 'en')]:
            with self.subTest(selected=selected, system=system):
                self.run_check(system, '--select-language', selected)
                self.run_check(system, '--initial-language', expected)


if __name__ == '__main__':
    unittest.main()
