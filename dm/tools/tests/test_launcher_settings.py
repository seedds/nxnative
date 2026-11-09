"""The launcher's settings (dm/games/settings.tsv through dm/games/settings.py): rows by graphics API and profile, single-screen sections, toggle values and the strict format;
the meta column's optional fields, and the written list against docs/launcher.schema.json, the contract front ends read."""
import importlib.util
import json
import os
import re
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('launcher_settings', ROOT / 'dm/games/settings.py')
settings = importlib.util.module_from_spec(spec)
spec.loader.exec_module(settings)

BOTW, WONDER, OOT = '01007EF00011E000', '010015100B514000', '0400486024DEC000'
PERFORMANCE_NVN = ['DM_PRESENT_SNAP', 'DM_PSO_MTL4', 'DM_PSO_PREBUILD', 'DM_NVN_ENC_THREAD', 'DM_FPS_HOLD', 'DM_METAL_HUD']


def build(gid, api, platform=None):
    env = {'GFXAPI': api, **({'SETTINGS_PLATFORM': platform} if platform else {})}
    with tempfile.TemporaryDirectory() as tmp, patch.object(settings.nacp, 'languages', return_value=['en-US']), patch.dict(os.environ, env):
        if not platform: os.environ.pop('SETTINGS_PLATFORM', None)
        return {s['env']: s for s in settings.build(gid, tmp, Path(tmp) / 'control.nacp')['settings']}


def rows_with_languages(listed):   # the settings rows, by variable, of a game whose control data lists these languages
    with tempfile.TemporaryDirectory() as tmp, patch.object(settings.nacp, 'languages', return_value=listed), patch.dict(os.environ, {'GFXAPI': 'nvn', 'DM_LANG': 'en-US'}):
        return {s['env']: s for s in settings.build(BOTW, tmp, Path(tmp) / 'control.nacp')['settings']}


class LauncherSettingsTests(unittest.TestCase):
    def test_game_language_row_offers_the_games_own_or_every_language_when_unknown(self):
        self.assertNotIn('DM_LANG', rows_with_languages(['ja']))   # one language: nothing to pick
        s = rows_with_languages(['en-US', 'ja'])['DM_LANG']
        self.assertEqual((s['options'], s['note']), ([['en-US', 'American English'], ['ja', 'Japanese']], ''))
        s = rows_with_languages([])['DM_LANG']   # no control data: a dump, a NUP without a NACP
        self.assertEqual(s['options'], [list(l) for l in settings.nacp.LANGUAGES])
        self.assertEqual((s['default'], s['note']), ('en-US', settings.UNKNOWN_LANGUAGES))

    def test_upscaling_is_opt_in_for_every_graphics_api(self):
        for api, gid in [('nvn', BOTW), ('vulkan', '0100535012974000'), ('gl', '010014E00DB56000')]:
            with self.subTest(api=api):
                s = build(gid, api)['DM_PRESENT_UPSCALE']
                self.assertEqual((s['kind'], s['section'], s['default']), ('choice', 'Game', 'off'))
                self.assertEqual(s['options'], [['off', 'Standard'], ['metalfx', 'MetalFX']])

    def test_frame_buffering_by_graphics_api(self):
        for api, gid in [('nvn', BOTW), ('vulkan', '0100535012974000'), ('gl', '010014E00DB56000'), ('', BOTW)]:
            with self.subTest(api=api):
                rows = build(gid, api)
                self.assertEqual('DM_PRESENT_SNAP' in rows, api == 'nvn')
                if api == 'nvn':
                    s = rows['DM_PRESENT_SNAP']
                    self.assertEqual((s['kind'], s['section'], s['default']), ('toggle', 'Performance', '1'))   # Breath of the Wild's profile: present_snap on
                    self.assertIn('latency', s['note'])

    def test_sections_keep_the_profile_defaults(self):
        botw, wonder = build(BOTW, 'nvn'), build(WONDER, 'nvn')
        self.assertEqual([e for e, s in botw.items() if s['section'] == 'Performance'], PERFORMANCE_NVN)
        self.assertEqual(botw['DM_PSO_MTL4']['default'], '1')   # pso_mtl4 on
        self.assertEqual((wonder['DM_PRESENT_SNAP']['default'], wonder['DM_PSO_MTL4']['default']), ('0', '0'))
        self.assertEqual((botw['DM_PSO_PREBUILD']['default'], botw['DM_NVN_ENC_THREAD']['default'], botw['DM_METAL_HUD']['default']), ('1', '1', '0'))   # the host's iOS defaults
        self.assertEqual(list(dict.fromkeys(s['section'] for s in botw.values())), ['Game', 'Controls', 'Performance'])
        self.assertNotIn('DM_FPS_HOLD', wonder)   # no frame step, as the Frame rate row
        self.assertIn('first start', botw['DM_PSO_PREBUILD']['note'])   # it runs once in the prepare step, before the first frame

    def test_hold_frames_takes_on_and_off(self):   # the host reads DM_FPS_HOLD as on / off (hle_nvn.c), not 1 / 0
        s = build(BOTW, 'nvn')['DM_FPS_HOLD']
        self.assertEqual((s['on'], s['off'], s['default']), ('on', 'off', 'on'))
        s = build(BOTW, 'nvn')['DM_SHOW_FPS']
        self.assertEqual((s['on'], s['off']), ('1', '0'))

    def test_iphone_switches_only_on_the_iphone(self):   # the frame-rate counter and touch feedback are the iOS front end's (front_ios.m, DmGamepad.m)
        ios, mac = build(BOTW, 'nvn'), build(BOTW, 'nvn', 'mac')
        self.assertEqual(build(BOTW, 'nvn', 'ios').keys(), ios.keys())   # ios when unset
        self.assertEqual(set(ios) - set(mac), {'DM_SHOW_FPS', 'DM_TOUCH_HAPTICS', 'DM_PICTURE_FIT'})
        self.assertIn('DM_RUMBLE', mac); self.assertIn('DM_METAL_HUD', mac)
        self.assertEqual(set(mac) - set(ios), {'DM_FULLSCREEN', 'DM_SW2'})   # the Mac window's Display mode (front_mac.m reads 1 or 0); Switch 2 controllers (pad_switch2_apple.m, wired on the Mac only)
        s = mac['DM_FULLSCREEN']
        self.assertEqual((s['kind'], s['default'], s['options'], s['level']), ('choice', '0', [['0', 'Window'], ['1', 'Fullscreen']], 'basic'))

    def test_defaults_follow_each_platform_host(self):
        # nvn_mtl.m pso4_on: Mac defaults on, iOS follows the profile. nvn_rec.h rec_on: iOS on, Mac off.
        for gid, ios_pso in ((BOTW, '1'), (WONDER, '0')):
            with self.subTest(game=gid):
                ios, mac = build(gid, 'nvn', 'ios'), build(gid, 'nvn', 'mac')
                self.assertEqual((ios['DM_PSO_MTL4']['default'], ios['DM_NVN_ENC_THREAD']['default']), (ios_pso, '1'))
                self.assertEqual((mac['DM_PSO_MTL4']['default'], mac['DM_NVN_ENC_THREAD']['default']), ('1', '0'))

    def test_unknown_platform_is_rejected(self):
        with self.assertRaisesRegex(SystemExit, 'SETTINGS_PLATFORM must be ios or mac'):
            build(WONDER, 'nvn', 'mca')

    def test_malformed_platform_defaults_are_rejected(self):
        for default in ('1|', '|0', '1|0|1'):
            with self.subTest(default=default), tempfile.TemporaryDirectory() as tmp:
                row = '\t'.join(('DM_X', 'toggle', '-', default, 'always', 'Game', 'X')) + '\n'
                (Path(tmp) / 'settings.tsv').write_text(row, encoding='utf-8')
                with patch.object(settings, 'HERE', Path(tmp)), patch.object(settings.nacp, 'languages', return_value=['en-US']), \
                        self.assertRaisesRegex(SystemExit, 'platform default'):
                    settings.build(BOTW, tmp, Path(tmp) / 'control.nacp')

    def test_meta_fields_and_the_schema_version(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(settings.nacp, 'languages', return_value=['en-US', 'ja']), patch.dict(os.environ, {'GFXAPI': 'nvn'}):
            (Path(tmp) / 'main.fixups.tsv').write_text('# RENDER_SCALE\t1\n')   # the render scale row's condition
            listed = settings.build(BOTW, tmp, Path(tmp) / 'control.nacp')
        rows = {s['env']: s for s in listed['settings']}
        self.assertEqual(listed['schema'], 1)
        self.assertEqual([e for e, s in rows.items() if s.get('level') == 'basic'], ['DM_FPS_TARGET', 'DM_RENDER_SCALE', 'DM_OPMODE', 'DM_LANG', 'DM_RUMBLE'])
        self.assertEqual(rows['DM_RUMBLE_GAIN']['needs'], 'DM_RUMBLE=1')
        self.assertFalse({'level', 'needs'} & set(rows['DM_METAL_HUD']), 'a row without meta has no optional fields')

    def test_meta_is_strict(self):
        self.assertEqual(settings.meta('level=basic; needs=DM_RUMBLE=1', 1), {'level': 'basic', 'needs': 'DM_RUMBLE=1'})
        self.assertEqual(settings.meta('', 1), {})
        for bad, why in [('colour=red', 'unknown meta key'), ('level', 'unknown meta key'), ('level=basic;level=basic', 'twice'),
                         ('level=advanced', 'does not match'), ('needs=DM_RUMBLE', 'does not match'), ('needs=rumble=1', 'does not match')]:
            with self.subTest(bad=bad), self.assertRaises(SystemExit) as e:
                settings.meta(bad, 1)
            self.assertIn(why, str(e.exception))

    def test_meta_keys_agree_with_the_docs_and_the_schema(self):
        """A key is added in settings.py's META, docs/frontends.md and docs/launcher.schema.json together"""
        schema = json.loads((ROOT / 'docs/launcher.schema.json').read_text(encoding='utf-8'))
        fields = schema['$defs']['setting']['properties']
        docs = (ROOT / 'docs/frontends.md').read_text(encoding='utf-8')
        for key in settings.META:
            with self.subTest(key=key):
                self.assertIn(key, fields)
                self.assertNotIn(key, schema['$defs']['setting']['required'], 'a meta field is optional')
                self.assertRegex(docs, rf'- `{key}` \(optional\)')
        self.assertEqual(schema['properties']['schema']['const'], settings.SCHEMA)

    def test_written_lists_follow_the_schema(self):
        schema = json.loads((ROOT / 'docs/launcher.schema.json').read_text(encoding='utf-8'))
        for gid, api, platform in [(BOTW, 'nvn', None), (BOTW, 'nvn', 'mac'), (WONDER, 'nvn', None), ('0100535012974000', 'vulkan', 'mac')]:
            with self.subTest(gid=gid, platform=platform), tempfile.TemporaryDirectory() as tmp, \
                    patch.object(settings.nacp, 'languages', return_value=['en-US']), patch.dict(os.environ, {'GFXAPI': api, **({'SETTINGS_PLATFORM': platform} if platform else {})}):
                self.assertEqual(conforms(settings.build(gid, tmp, Path(tmp) / 'control.nacp'), schema, schema), [])

    def test_schema_1_takes_new_sections_but_not_new_kinds(self):
        """A section is a heading a reader draws by its name, so a new one stays schema 1; a new kind would not"""
        schema = json.loads((ROOT / 'docs/launcher.schema.json').read_text(encoding='utf-8'))
        row = {'env': 'DM_X', 'kind': 'toggle', 'section': 'Picture', 'label': 'X', 'note': '', 'default': '1', 'on': '1', 'off': '0'}
        self.assertEqual(conforms({'schema': 1, 'game': BOTW, 'settings': [row]}, schema, schema), [])
        self.assertNotEqual(conforms({'schema': 1, 'game': BOTW, 'settings': [{**row, 'kind': 'slider'}]}, schema, schema), [])

    def test_render_base_goes_into_the_render_scale_row(self):
        schema = json.loads((ROOT / 'docs/launcher.schema.json').read_text(encoding='utf-8'))
        for gid, base in [(BOTW, {'docked': [1600, 900], 'handheld': [1280, 720]}), (WONDER, None)]:   # Breath of the Wild's note; Wonder's profile has none
            with self.subTest(gid=gid), tempfile.TemporaryDirectory() as tmp, \
                    patch.object(settings.nacp, 'languages', return_value=['en-US']), patch.dict(os.environ, {'GFXAPI': 'nvn'}):
                (Path(tmp) / 'main.fixups.tsv').write_text('# RENDER_SCALE\t1\n')   # the render scale row's condition
                listed = settings.build(gid, tmp, Path(tmp) / 'control.nacp')
            self.assertEqual({s['env']: s for s in listed['settings']}['DM_RENDER_SCALE'].get('base'), base)
            self.assertEqual(conforms(listed, schema, schema), [])
        for bad in ('1600x900', '1600x900 1280', 'big small', '1600x900x3 1280x720', 'a1600x900 1280x720', '1600x900 1280x720 640x360'):
            with self.subTest(bad=bad):
                profile = type('Profile', (), {'id': BOTW, 'get': lambda self, key, default='', bad=bad: bad})()
                with self.assertRaises(SystemExit): settings.render_base(profile)

    def test_render_levels_make_the_render_scale_a_choice_of_the_games_levels(self):
        schema = json.loads((ROOT / 'docs/launcher.schema.json').read_text(encoding='utf-8'))
        with tempfile.TemporaryDirectory() as tmp, patch.object(settings.nacp, 'languages', return_value=['en-US']), patch.dict(os.environ, {'GFXAPI': 'nvn', 'SETTINGS_PLATFORM': 'mac'}):
            (Path(tmp) / 'main.fixups.tsv').write_text('# RENDER_SCALE\t1\n')   # Ocarina of Time's render-scale stub
            listed = settings.build(OOT, tmp, Path(tmp) / 'control.nacp')
        s = {s['env']: s for s in listed['settings']}['DM_RENDER_SCALE']
        self.assertEqual((s['kind'], s['default'], s['base']), ('choice', '1.0', {'docked': [2560, 1440], 'handheld': [1920, 1080]}))
        self.assertEqual(s['options'], [['1.0', settings.DYNAMIC], ['0.75', '75 %'], ['0.71112', '71 %'], ['0.66667', '67 %'], ['0.62223', '62 %'], ['0.58334', '58 %'],
                                        ['0.53334', '53 %'], ['0.5', '50 %']])   # the game's levels the loader takes (0.5 to below 1.0), highest first
        self.assertFalse({'min', 'max', 'step'} & set(s))
        self.assertEqual(conforms(listed, schema, schema), [])
        for bad in ('0.5 0.75', '0.75 0.75', 'big', '0.25 0.1'):   # not from the highest down, twice, not numbers, none the setting takes
            with self.subTest(bad=bad):
                profile = type('Profile', (), {'id': OOT, 'get': lambda self, key, default='', bad=bad: bad})()
                with self.assertRaises(SystemExit): settings.render_levels(profile, 0.5, 1.0)

    def test_ocarina_of_time_offers_the_picture_controls(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(settings.nacp, 'languages', return_value=['en-US']), patch.dict(os.environ, {'GFXAPI': 'nvn', 'SETTINGS_PLATFORM': 'mac'}):
            (Path(tmp) / 'main.fixups.tsv').write_text('# RENDER_SCALE\t1\n# DYNAMIC_RESOLUTION\t1\n')   # the rewriter's stubs of its build
            rows = {s['env']: s for s in settings.build(OOT, tmp, Path(tmp) / 'control.nacp')['settings']}
        self.assertEqual([o[0] for o in rows['DM_FPS_TARGET']['options']], ['30', '60'])   # its pacer steps whole vsyncs; 120 is for games checked at it (fps_max)
        self.assertEqual([o[0] for o in rows['DM_OPMODE']['options']], ['auto', '1', '0'])
        self.assertEqual([o[0] for o in rows['DM_GFX_SCALE']['options']], ['auto', '0', '1', '2'])
        self.assertEqual([o[0] for o in rows['DM_FULLSCREEN']['options']], ['0', '1'])
        self.assertEqual(rows['DM_RENDER_SCALE']['options'][0], ['1.0', settings.DYNAMIC])   # 1.0: the game's own, and its dynamic resolution (the switch under it) is its sub-setting
        self.assertEqual(rows['DM_DYNAMIC_RESOLUTION']['needs'], 'DM_RENDER_SCALE=1.0')
        for var in ('DM_FPS_TARGET', 'DM_RENDER_SCALE', 'DM_DYNAMIC_RESOLUTION', 'DM_OPMODE', 'DM_GFX_SCALE', 'DM_FULLSCREEN'): self.assertEqual(rows[var]['level'], 'basic', var)

    def test_auto_stands_for_the_profiles_default(self):
        rows = build(OOT, 'nvn', 'mac')
        self.assertEqual((rows['DM_OPMODE']['default'], rows['DM_OPMODE']['auto'], rows['DM_OPMODE']['options'][0]), ('auto', '1', ['auto', 'Auto (Docked)']))
        self.assertEqual((rows['DM_GFX_SCALE']['default'], rows['DM_GFX_SCALE']['auto'], rows['DM_GFX_SCALE']['options'][0]), ('auto', '0', ['auto', 'Auto (Whole GPU)']))
        self.assertEqual(rows['DM_GFX_SCALE']['options'][1:], [['0', 'Whole GPU'], ['1', '80%'], ['2', '67%']])
        self.assertNotIn('auto', build(BOTW, 'nvn')['DM_FPS_TARGET'], 'a row without an auto option has no auto field')
        aspect = build(BOTW, 'nvn')['DM_SCREEN_ASPECT'] if 'DM_SCREEN_ASPECT' in build(BOTW, 'nvn') else None
        if aspect: self.assertNotIn('auto', aspect, 'Picture shape\'s auto (match the screen) defaults to no profile macro: a value of its own')

    def test_table_format_is_strict(self):
        good = 'DM_X\ttoggle\t-\t0\talways\tGame\tX\n'
        for bad, why in [('DM_X\ttoggle\t-\t0\talways\tGame\tX\t\tlevel=basic\textra\n', 'columns'),
                         ('DM_X\ttoggle\t-\t0\talways\tGame\tX\t\tneeds=DM_NOPE=1\n', 'no row sets'),
                         ('DM_Y\ttoggle\t-\t0\talways\tGame\tY\t\tneeds=DM_X=on\n', 'never takes'),
                         ('DM_X\ttoggle\t-\t0\talways\tX\n', 'columns'), ('DM_X\ttoggle\t-\t0\talways\tsecret\tX\n', 'section'),
                         ('DM_X\ttoggle\ton\t0\talways\tGame\tX\n', 'toggle values'), ('DM_X\ttoggle\t-\t2\talways\tGame\tX\n', 'defaults to 0 or 1')]:
            with self.subTest(why=why), tempfile.TemporaryDirectory() as tmp:
                (Path(tmp) / 'settings.tsv').write_text('# header\n' + good + bad, encoding='utf-8')
                with patch.object(settings, 'HERE', Path(tmp)), \
                        patch.object(settings.nacp, 'languages', return_value=['en-US']), self.assertRaises(SystemExit) as e:
                    settings.build(BOTW, tmp, Path(tmp) / 'control.nacp')
                self.assertIn(why, str(e.exception))


def conforms(value, node, root, at='$'):
    """The JSON Schema keywords launcher.schema.json uses (type, required, properties, items, enum, const, pattern, $ref, allOf with
    if / then), as a list of problems: enough to hold the written lists to the contract without a jsonschema package"""
    if '$ref' in node: return conforms(value, root['$defs'][node['$ref'].rsplit('/', 1)[1]], root, at)
    problems = []
    kinds = {'object': dict, 'array': list, 'string': str, 'integer': int, 'number': (int, float)}
    if 'type' in node and not isinstance(value, kinds[node['type']]): return [f'{at}: not a {node["type"]}']
    if 'const' in node and value != node['const']: problems.append(f'{at}: not {node["const"]!r}')
    if 'enum' in node and value not in node['enum']: problems.append(f'{at}: not one of {node["enum"]}')
    if 'pattern' in node and not re.search(node['pattern'], value): problems.append(f'{at}: does not match {node["pattern"]}')
    if isinstance(value, dict):
        problems += [f'{at}: no {k}' for k in node.get('required', []) if k not in value]
        for k, v in value.items():
            if k in node.get('properties', {}): problems += conforms(v, node['properties'][k], root, f'{at}.{k}')
        for rule in node.get('allOf', []):
            if not conforms(value, rule['if'], root, at): problems += conforms(value, rule['then'], root, at)
    if isinstance(value, list) and 'items' in node:
        for i, v in enumerate(value): problems += conforms(v, node['items'], root, f'{at}[{i}]')
    return problems


if __name__ == '__main__':
    unittest.main()
