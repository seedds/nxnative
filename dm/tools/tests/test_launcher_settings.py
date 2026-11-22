"""The launcher's settings (dm/games/settings.tsv through dm/games/settings.py): rows by graphics API and profile, single-screen sections, toggle values and the strict format;
the meta column's optional fields, and the written list against docs/launcher.schema.json, the contract front ends read."""
import importlib.util
import json
import os
import re
import struct
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('launcher_settings', ROOT / 'dm/games/settings.py')
settings = importlib.util.module_from_spec(spec)
spec.loader.exec_module(settings)

BOTW, WONDER = '01007EF00011E000', '010015100B514000'
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

    def test_hdr_is_opt_in_on_both_platforms(self):   # the host reads DM_HDR 1 and 2 (nvn_mtl.m hdr_mode, hle_vi.c); every graphics API presents through the same path
        for platform in ('ios', 'mac'):
            for api, gid in [('nvn', BOTW), ('vulkan', '0100535012974000'), ('gl', '010014E00DB56000')]:
                with self.subTest(platform=platform, api=api):
                    s = build(gid, api, platform)['DM_HDR']   # no modules: no HDR query, so no Game's HDR
                    self.assertEqual((s['kind'], s['section'], s['default']), ('choice', 'Game', '0'))
                    self.assertEqual(s['options'], [['0', 'Off'], ['1', 'BT.2446']])

    def test_games_hdr_is_listed_only_when_main_or_an_nro_imports_the_hdr_query(self):
        """vi-hdr: main or an NRO nro.tsv lists imports nn::vi::QueryDisplayHdrStatus, as the host's dm_game_imports reads them (nvn_mtl.m mtl_hdr_game_native)"""
        schema = json.loads((ROOT / 'docs/launcher.schema.json').read_text(encoding='utf-8'))
        sdr, hdr = [['0', 'Off'], ['1', 'BT.2446']], [['0', 'Off'], ['1', 'BT.2446'], ['2', "Game's HDR"]]
        nro_rows = '# name\tsha256\tsize\tnrr\twhere\nrefused\t' + '0' * 64 + '\t0\nloop\t' + '1' * 64 + '\t4096\n'
        for case, files, options in [
                ('no modules', {}, sdr),
                ('main imports it', {'main.patched.bin': module(HDR_QUERY)}, hdr),
                ('main imports another vi call', {'main.patched.bin': module('_ZN2nn2vi11OpenDisplayEPPNS0_7DisplayEPKc')}, sdr),
                ('main defines it', {'main.patched.bin': module(HDR_QUERY, defined=True)}, sdr),
                ('an NRO imports it', {'main.patched.bin': module('x'), 'nro.tsv': nro_rows, 'loop.patched.bin': module(HDR_QUERY)}, hdr),
                ('an unlisted module imports it', {'main.patched.bin': module('x'), 'subsdk0.patched.bin': module(HDR_QUERY)}, sdr)]:
            for platform in ('ios', 'mac'):
                with self.subTest(case=case, platform=platform), tempfile.TemporaryDirectory() as tmp, \
                        patch.object(settings.nacp, 'languages', return_value=['en-US']), patch.dict(os.environ, {'GFXAPI': 'nvn', 'SETTINGS_PLATFORM': platform}):
                    for name, data in files.items(): (Path(tmp) / name).write_bytes(data.encode() if isinstance(data, str) else data)
                    listed = settings.build(BOTW, tmp, Path(tmp) / 'control.nacp')
                    s = {r['env']: r for r in listed['settings']}['DM_HDR']
                    self.assertEqual(s['options'], options)
                    self.assertEqual(conforms(listed, schema, schema), [])

    def test_games_hdr_counts_the_other_programs_modules(self):   # build_ipa.sh and the NSP script's Mac app pass every program's rewritten modules
        hdr = [['0', 'Off'], ['1', 'BT.2446'], ['2', "Game's HDR"]]
        for case, other, options in [('its main imports it', 'main.patched.bin', hdr), ('an unlisted module of it imports it', 'subsdk0.patched.bin', hdr[:2])]:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp, \
                    patch.object(settings.nacp, 'languages', return_value=['en-US']), patch.dict(os.environ, {'GFXAPI': 'nvn'}):
                first, second = Path(tmp) / 'p0', Path(tmp) / 'p1'
                first.mkdir(); second.mkdir()
                (first / 'main.patched.bin').write_bytes(module('x')); (second / other).write_bytes(module(HDR_QUERY))
                listed = settings.build(BOTW, first, first / 'control.nacp', other_programs=[second])
                self.assertEqual({r['env']: r for r in listed['settings']}['DM_HDR']['options'], options)

    def test_launcher_and_host_ask_for_the_same_hdr_query(self):   # settings.py's vi-hdr and nvn_mtl.m mtl_hdr_game_native
        prefix = '_ZN2nn2vi21QueryDisplayHdrStatus'
        self.assertTrue(HDR_QUERY.startswith(prefix))
        for path in ('dm/games/settings.py', 'dm/gfx/nvn/nvn_mtl.m'):
            with self.subTest(path=path):
                names = set(re.findall(r"""['"](_ZN2nn2vi\d+QueryDisplayHdr\w*)['"]""", (ROOT / path).read_text(encoding='utf-8')))
                self.assertEqual(names, {prefix})

    def test_a_hidden_default_falls_back_to_the_first_value_offered(self):
        rows = 'DM_X\tchoice\ta=A?vi-hdr,b=B,c=C?vi-hdr\ta\talways\tGame\tX\n'
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'settings.tsv').write_text(rows, encoding='utf-8')
            with patch.object(settings, 'HERE', Path(tmp)), patch.object(settings.nacp, 'languages', return_value=['en-US']):
                s = settings.build(BOTW, tmp, Path(tmp) / 'control.nacp')['settings'][0]
        self.assertEqual((s['default'], s['options']), ('b', [['b', 'B']]))

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
        self.assertEqual(set(ios) - set(mac), {'DM_SHOW_FPS', 'DM_TOUCH_HAPTICS', 'DM_PICTURE_FIT', 'DM_ROTATION'})
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

    def test_picture_shape_requires_the_rewriter_capability(self):
        games = ('0400486024DEC000', BOTW, '01006F8002326000')
        for gid in games:
            self.assertNotIn('DM_SCREEN_ASPECT', build(gid, 'nvn'))
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {'GFXAPI': 'nvn', 'SETTINGS_PLATFORM': 'ios'}):
            (Path(tmp) / 'main.fixups.tsv').write_text('# SCREEN_ASPECT\t1\n', encoding='utf-8')
            for gid in games:
                with self.subTest(gid=gid):
                    rows = settings.build(gid, tmp, Path(tmp) / 'control.nacp')['settings']
                    shape = next(row for row in rows if row['env'] == 'DM_SCREEN_ASPECT')
                    self.assertEqual(shape['default'], 'auto')

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

    def test_render_scale_reaches_the_stubs_largest_scale(self):
        """The rewriter writes a render-scale stub's largest scale as RENDER_SCALE: 1 keeps the table's range (a stub that only lowers the size), 3 raises its top (Hades II)"""
        for value, top in (('1', 1.0), ('3', 3.0)):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as tmp, \
                    patch.object(settings.nacp, 'languages', return_value=['en-US']), patch.dict(os.environ, {'GFXAPI': 'nvn'}):
                (Path(tmp) / 'main.fixups.tsv').write_text(f'# RENDER_SCALE\t{value}\n')
                row = {s['env']: s for s in settings.build(BOTW, tmp, Path(tmp) / 'control.nacp')['settings']}['DM_RENDER_SCALE']
            self.assertEqual((row['min'], row['max']), (0.5, top))

    def test_table_format_is_strict(self):
        good = 'DM_X\ttoggle\t-\t0\talways\tGame\tX\n'
        for bad, why in [('DM_X\ttoggle\t-\t0\talways\tGame\tX\t\tlevel=basic\textra\n', 'columns'),
                         ('DM_X\ttoggle\t-\t0\talways\tGame\tX\t\tneeds=DM_NOPE=1\n', 'no row sets'),
                         ('DM_Y\ttoggle\t-\t0\talways\tGame\tY\t\tneeds=DM_X=on\n', 'never takes'),
                         ('DM_X\ttoggle\t-\t0\talways\tX\n', 'columns'), ('DM_X\ttoggle\t-\t0\talways\tsecret\tX\n', 'section'),
                         ('DM_Y\tchoice\ta=A?nope,b=B\tb\talways\tGame\tY\n', 'unknown condition'),
                         ('DM_X\ttoggle\ton\t0\talways\tGame\tX\n', 'toggle values'), ('DM_X\ttoggle\t-\t2\talways\tGame\tX\n', 'defaults to 0 or 1')]:
            with self.subTest(why=why), tempfile.TemporaryDirectory() as tmp:
                (Path(tmp) / 'settings.tsv').write_text('# header\n' + good + bad, encoding='utf-8')
                with patch.object(settings, 'HERE', Path(tmp)), \
                        patch.object(settings.nacp, 'languages', return_value=['en-US']), self.assertRaises(SystemExit) as e:
                    settings.build(BOTW, tmp, Path(tmp) / 'control.nacp')
                self.assertIn(why, str(e.exception))


HDR_QUERY = '_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE'


def module(symbol, defined=False):
    """A rewritten module's image as nso_syms.dynsyms reads it: the MOD0 offset at +4, MOD0 with the dynamic table's offset, DT_STRTAB and DT_SYMTAB,
    the null symbol and one symbol, undefined (an import) unless defined"""
    img = bytearray(0x200)
    struct.pack_into('<I', img, 4, 0x10)
    img[0x10:0x14] = b'MOD0'; struct.pack_into('<i', img, 0x14, 0x40 - 0x10)
    struct.pack_into('<qQqQqQ', img, 0x40, 5, 0x130, 6, 0x100, 0, 0)
    struct.pack_into('<IBBHQQ', img, 0x118, 1, 0x12, 0, 1 if defined else 0, 0, 0)
    name = symbol.encode() + b'\0'
    return bytes(img[:0x131]) + name


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
