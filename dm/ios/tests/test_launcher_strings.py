"""The apps' translations (dm/ios/Sources/<language>.lproj/Localizable.strings): each language has every text the iOS launcher, its in-game menu, the Mac settings window and the
game's own messages (front_ios.m, front_mac.m) show, and no entry for text that is gone. The key is the English text: the settings list's sections, names, notes and option labels
(settings.tsv, the game languages of nacp.py), the literals the Swift sources show (SwiftUI's Text, Button, ... and loc(), with \\(...) as %@), and the Objective-C sources' NSLocalizedString."""
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games'))
import settings

LPROJ = REPO / 'dm/ios/Sources'
SWIFT = [REPO / 'dm/ios/Sources' / n for n in ('Launcher.swift', 'LauncherRow.swift', 'LauncherSettings.swift', 'GameMenu.swift')] + [REPO / 'dm/mac/Launcher.swift']
SHOWN = re.compile(r'\b(?:Text|Button|Toggle|DisclosureGroup|LabeledContent|Section|loc)\("')   # a string literal SwiftUI looks up (LocalizedStringKey), or one given to loc()
OBJC = [REPO / 'dm/ios/Sources' / n for n in ('front_ios.m', 'DmGamepad.m')] + [REPO / 'dm/host/front_mac.m']
OBJC_SHOWN = re.compile(r'\bNSLocalizedString\(@"')


def literal(src, i):
    """The key of the Swift string literal whose text starts at i: escapes resolved, each \\(...) as %@ (string literals inside it skipped)"""
    out = []
    while src[i] != '"':
        if src[i] == '\\' and src[i + 1] == '(':
            depth, i = 1, i + 2
            while depth:
                if src[i] == '"': i = skip(src, i + 1)
                else: depth += {'(': 1, ')': -1}.get(src[i], 0); i += 1
            out.append('%@')
        elif src[i:i + 2] == '\\u' and src[i + 2] != '{': out.append(chr(int(src[i + 2:i + 6], 16))); i += 6   # Objective-C's \\u2026
        elif src[i] == '\\': out.append({'n': '\n', 't': '\t'}.get(src[i + 1], src[i + 1])); i += 2
        else: out.append(src[i]); i += 1
    return ''.join(out)


def skip(src, i):   # past the end of the literal whose text starts at i
    while src[i] != '"':
        if src[i] == '\\' and src[i + 1] == '(':
            depth, i = 1, i + 2
            while depth:
                if src[i] == '"': i = skip(src, i + 1)
                else: depth += {'(': 1, ')': -1}.get(src[i], 0); i += 1
        else: i += 2 if src[i] == '\\' else 1
    return i + 1


def source_keys():
    keys = set()
    for f in SWIFT:
        code = '\n'.join(l for l in f.read_text(encoding='utf-8').splitlines() if not l.lstrip().startswith('//'))   # comment lines name some of the texts too
        keys |= {literal(code, m.end()) for m in SHOWN.finditer(code)}
    for f in OBJC:
        code = '\n'.join(l for l in f.read_text(encoding='utf-8').splitlines() if not l.lstrip().startswith('//'))
        keys |= {literal(code, m.end()) for m in OBJC_SHOWN.finditer(code)}
    return keys


def table_keys():
    keys = {settings.UNKNOWN_LANGUAGES} | {n for _, n in settings.nacp.LANGUAGES}
    for _, (var, kind, values, default, cond, section, label, note, extra) in settings.rows():
        keys |= {section, label} | ({note} if note else set())
        if kind == 'choice' and values != 'languages': keys |= {v.split('?', 1)[0].split('=', 1)[1] for v in values.split(',')}
    return keys


def strings(path):   # a .strings file as plutil reads it, which is how Xcode and the Mac app's bundle read it
    r = subprocess.run(['plutil', '-convert', 'json', '-o', '-', str(path)], capture_output=True, text=True)
    if r.returncode: raise AssertionError(f'{path}: {r.stderr or r.stdout}')
    return json.loads(r.stdout)


@unittest.skipUnless(shutil.which('plutil'), 'needs plutil (macOS) to read .strings files')
class LauncherStrings(unittest.TestCase):
    def test_sources_and_table_have_text(self):   # the scan finds what it should, so an empty set cannot pass the checks below
        self.assertTrue({'Play', 'Version %@', 'Reset to tested defaults', 'Advanced settings', 'Quit %@', '%@, set in env.txt', 'Off'} <= source_keys())
        self.assertTrue({'Save failed', 'Starting %@\u2026', 'Menu', 'The GPU stopped running %@', 'Input'} <= source_keys())   # the Objective-C front ends
        self.assertTrue({'Frame rate', 'Docked', 'Handheld', 'Performance', 'Simplified Chinese'} <= table_keys())

    def test_english_falls_back_to_the_keys(self):
        # en.lproj marks English as a language the app has, so a player whose language has no table (German, say) gets English, not the one translation there is
        self.assertEqual(strings(LPROJ / 'en.lproj/Localizable.strings'), {})

    def test_every_language_has_every_text(self):
        want = source_keys() | table_keys()
        langs = sorted(p.parent for p in LPROJ.glob('*.lproj/Localizable.strings') if p.parent.name != 'en.lproj')
        self.assertTrue(langs, 'no translation')
        for lang in langs:
            with self.subTest(lang.name):
                have = strings(lang / 'Localizable.strings')
                self.assertEqual(sorted(want - have.keys()), [], 'text without a translation')
                self.assertEqual(sorted(have.keys() - want), [], 'translation of text the launchers no longer show')
                for k, v in have.items():
                    self.assertTrue(v.strip(), k)
                    self.assertEqual(v.count('%@'), k.count('%@'), k)   # String(format:) and SwiftUI fill the same arguments
                    self.assertEqual(v.count('%') - 2 * v.count('%%'), k.count('%') - 2 * k.count('%%'), k)


if __name__ == '__main__':
    unittest.main()
