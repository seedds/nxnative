"""The game languages the tools know (dm/tools/nacp.py LANGUAGES): the Switch 2's Polish and Thai are read from control data, offered in the Game language row and taken
by the NSP script's language option, by code or by name. No game files, keys or Xcode needed."""
import importlib.util
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(REPO / 'dm/tools'), str(REPO / 'dm/games')]
import nacp
import settings
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
vars(nsp)['nacp'] = nacp   # main sets it

BOTW = '01007EF00011E000'
SWITCH = ['en-US', 'en-GB', 'ja', 'fr', 'de', 'es-419', 'es', 'it', 'nl', 'fr-CA', 'pt', 'ru', 'ko', 'zh-Hant', 'zh-Hans', 'pt-BR']   # the 16 entries of a Switch NACP, in its order


def control(tmp, flag):   # a control.nacp with this SupportedLanguageFlag
    d = bytearray(0x4000); struct.pack_into('<I', d, 0x302C, flag)
    p = Path(tmp) / 'control.nacp'; p.write_bytes(d)
    return p


class Languages(unittest.TestCase):
    def test_polish_and_thai_follow_the_switch_entries(self):   # entries 16 and 17: the first 16 keep their bits
        self.assertEqual([c for c, _ in nacp.LANGUAGES], SWITCH + ['pl', 'th'])
        self.assertEqual(dict(nacp.LANGUAGES)['pl'], 'Polish'); self.assertEqual(dict(nacp.LANGUAGES)['th'], 'Thai')

    def test_control_data_lists_them_by_bits_16_and_17(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(nacp.languages(control(tmp, 0xffff)), SWITCH)
            self.assertEqual(nacp.languages(control(tmp, 1 << 16 | 1 << 2)), ['ja', 'pl'])
            self.assertEqual(nacp.languages(control(tmp, 1 << 17 | 1)), ['en-US', 'th'])
            self.assertEqual(nacp.languages(control(tmp, 0x3ffff)), SWITCH + ['pl', 'th'])
            self.assertEqual(nacp.languages(control(tmp, 1 << 18)), [])   # a bit no entry has names nothing

    def test_the_nsp_script_takes_them_by_code_or_name(self):
        for typed, code in (('pl', 'pl'), ('PL', 'pl'), ('Polish', 'pl'), ('polish', 'pl'), ('th', 'th'), ('Thai', 'th'), (' thai ', 'th')):
            self.assertEqual(nsp.language_code(typed), code)
        self.assertIsNone(nsp.language_code('pl-PL'))   # not a code the SDK has

    def test_a_game_with_them_packs_them_and_one_without_says_so(self):
        self.assertEqual(nsp.game_language('pl', True, ['en-US', 'pl', 'th']), 'pl')
        self.assertEqual(nsp.game_language('th', True, []), 'th')   # no list (a NUP or a dump with no control data): as asked
        with self.assertRaisesRegex(nsp.BuildError, r'the game has no th \(Thai\)'): nsp.game_language('th', True, ['en-US', 'ja'])
        self.assertEqual(nsp.language_list(['pl', 'th']), 'pl (Polish), th (Thai)')

    def test_the_game_language_row_offers_them(self):
        def row(listed):
            with tempfile.TemporaryDirectory() as tmp, patch.object(settings.nacp, 'languages', return_value=listed), patch.dict(os.environ, {'GFXAPI': 'nvn', 'DM_LANG': 'en-US'}):
                return next(s for s in settings.build(BOTW, tmp, Path(tmp) / 'control.nacp')['settings'] if s['env'] == 'DM_LANG')
        self.assertEqual(row(['en-US', 'pl', 'th'])['options'], [['en-US', 'American English'], ['pl', 'Polish'], ['th', 'Thai']])
        self.assertEqual(row([])['options'][-2:], [['pl', 'Polish'], ['th', 'Thai']])   # no list: every language, with the note


if __name__ == '__main__':
    unittest.main()
