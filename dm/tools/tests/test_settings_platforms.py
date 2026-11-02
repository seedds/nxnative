"""The launcher settings of the port's platforms (dm/games/settings.py with SETTINGS_PLATFORM=linux / windows, which the NSP script writes as <id>_build/launcher-<platform>.json
for the launcher nxlauncher, dm/launcher): the rows each platform lists (Upscaling's values on the Vulkan backend, Metal's switches on Apple only), the condition syntax that says so
(platform lists, conditions joined by &, one row per platform for a variable), and Apple's lists byte for byte as they were before the port's rows (the base commit's settings.py and
settings.tsv, from git, when the checkout has that commit)."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


settings = load(ROOT / 'dm/games/settings.py', 'launcher_settings_platforms')
conforms = load(ROOT / 'dm/tools/tests/test_launcher_settings.py', 'launcher_settings_schema').conforms
BOTW, WONDER, VK, GL = '01007EF00011E000', '010015100B514000', '0100535012974000', '010014E00DB56000'
BASE = '08af422'   # the commit before the port's rows: Apple's lists must come out of it and of this tree alike (a deliberate change to Apple's rows moves BASE to that change)
APPLE_ONLY = {'DM_PRESENT_SNAP', 'DM_PSO_MTL4', 'DM_PSO_PREBUILD', 'DM_NVN_ENC_THREAD', 'DM_METAL_HUD', 'DM_SHOW_FPS', 'DM_TOUCH_HAPTICS'}


def build(gid, api, platform, module=settings, langs=('en-US',)):
    env = {'GFXAPI': api, 'DM_LANG': 'en-US', **({'SETTINGS_PLATFORM': platform} if platform else {})}
    with tempfile.TemporaryDirectory() as tmp, patch.object(module.nacp, 'languages', return_value=list(langs)), patch.dict(os.environ, env):
        if not platform: os.environ.pop('SETTINGS_PLATFORM', None)
        (Path(tmp) / 'main.fixups.tsv').write_text('# RENDER_SCALE\t1\n')   # the render scale row's condition
        return module.build(gid, tmp, Path(tmp) / 'control.nacp')


def rows(gid, api, platform): return {s['env']: s for s in build(gid, api, platform)['settings']}


def table(text):   # settings.build over a table of its own
    tmp = tempfile.TemporaryDirectory()
    (Path(tmp.name) / 'settings.tsv').write_text(text, encoding='utf-8')
    return tmp


class PortPlatforms(unittest.TestCase):
    def test_upscaling_offers_the_vulkan_backends_values(self):   # dm/gfx/hostvk/vk_present.c: off, fsr, integer (decision D4)
        for platform in ('linux', 'windows'):
            for api, gid in (('nvn', BOTW), ('vulkan', VK), ('gl', GL)):
                with self.subTest(platform=platform, api=api):
                    s = rows(gid, api, platform)['DM_PRESENT_UPSCALE']
                    self.assertEqual((s['kind'], s['section'], s['label'], s['default']), ('choice', 'Game', 'Upscaling', 'off'))
                    self.assertEqual(s['options'], [['off', 'Standard'], ['fsr', 'FSR 1'], ['integer', 'Integer']])
        for platform in (None, 'ios', 'mac'):
            with self.subTest(platform=platform):
                self.assertEqual(rows(BOTW, 'nvn', platform)['DM_PRESENT_UPSCALE']['options'], [['off', 'Standard'], ['metalfx', 'MetalFX']])

    def test_metal_switches_stay_on_apple(self):
        for platform in ('linux', 'windows'):
            with self.subTest(platform=platform):
                port = rows(BOTW, 'nvn', platform)
                self.assertFalse(APPLE_ONLY & set(port), sorted(APPLE_ONLY & set(port)))
                self.assertEqual([e for e, s in port.items() if s['section'] == 'Performance'], ['DM_FPS_HOLD', 'DM_VK_PSO_CACHE_LIMIT', 'DM_VK_PSO_CACHE_CLEANUP'])   # the frame step is the NVN frontend's (hle_nvn.c), on every backend; the pipeline cache's upkeep the Vulkan backend's (vk_cache.c, decision D8)
                for env in ('DM_FPS_TARGET', 'DM_RENDER_SCALE', 'DM_OPMODE', 'DM_FULLSCREEN', 'DM_RUMBLE', 'DM_RUMBLE_GAIN', 'DM_RUMBLE_MIN'):   # front_sdl.c, hle_oe.c, loader.c read them
                    self.assertIn(env, port)
                self.assertEqual(port['DM_FULLSCREEN']['options'], [['0', 'Window'], ['1', 'Fullscreen']])
        self.assertEqual(APPLE_ONLY - {'DM_SHOW_FPS', 'DM_TOUCH_HAPTICS'}, APPLE_ONLY & set(rows(BOTW, 'nvn', 'mac')))

    def test_linux_and_windows_list_the_same_rows(self):
        for api, gid in (('nvn', BOTW), ('nvn', WONDER), ('vulkan', VK), ('gl', GL)):
            with self.subTest(gid=gid):
                self.assertEqual(build(gid, api, 'linux'), build(gid, api, 'windows'))

    def test_port_lists_follow_the_schema(self):
        schema = json.loads((ROOT / 'docs/launcher.schema.json').read_text(encoding='utf-8'))
        for platform in ('linux', 'windows'):
            for api, gid in (('nvn', BOTW), ('nvn', WONDER), ('vulkan', VK), ('gl', GL)):
                with self.subTest(platform=platform, gid=gid):
                    self.assertEqual(conforms(build(gid, api, platform), schema, schema), [])

    def test_condition_syntax(self):
        text = ('DM_A\ttoggle\t-\t1\tplatform:mac,linux\tGame\tA\n'
                'DM_B\ttoggle\t-\t1\tgfxapi:nvn&platform:linux\tGame\tB\n'
                'DM_C\tchoice\tx=X,y=Y\tx\tplatform:ios,mac\tGame\tC\n'
                'DM_C\tchoice\tx=X,z=Z\tz\tplatform:linux,windows\tGame\tC\n'
                'DM_D\ttoggle\t-\t1\talways\tGame\tD\t\tneeds=DM_C=z\n')
        listed = {}
        with table(text) as tmp, patch.object(settings, 'HERE', Path(tmp)):
            for platform, api in (('linux', 'nvn'), ('linux', 'gl'), ('windows', 'nvn'), ('mac', 'nvn'), ('ios', 'nvn')):
                listed[platform, api] = {s['env']: s for s in build(BOTW, api, platform)['settings']}
        self.assertEqual(list(listed['linux', 'nvn']), ['DM_A', 'DM_B', 'DM_C', 'DM_D'])
        self.assertEqual(list(listed['linux', 'gl']), ['DM_A', 'DM_C', 'DM_D'])   # both parts of gfxapi:nvn&platform:linux must hold
        self.assertEqual(list(listed['windows', 'nvn']), ['DM_C', 'DM_D'])
        self.assertEqual(list(listed['mac', 'nvn']), ['DM_A', 'DM_C', 'DM_D'])
        self.assertEqual(list(listed['ios', 'nvn']), ['DM_C', 'DM_D'])
        self.assertEqual((listed['linux', 'nvn']['DM_C']['options'], listed['linux', 'nvn']['DM_C']['default']), ([['x', 'X'], ['z', 'Z']], 'z'))   # each platform its own row
        self.assertEqual((listed['mac', 'nvn']['DM_C']['options'], listed['mac', 'nvn']['DM_C']['default']), ([['x', 'X'], ['y', 'Y']], 'x'))

    def test_bad_conditions_and_rows_are_refused(self):
        for text, platform, why in [
                ('DM_X\ttoggle\t-\t1\tplatform:linx\tGame\tX\n', 'linux', 'platform: takes names'),
                ('DM_X\ttoggle\t-\t1\tplatform:mac,\tGame\tX\n', 'mac', 'platform: takes names'),
                ('DM_X\ttoggle\t-\t1\tplatform:mac&\tGame\tX\n', 'mac', 'joined by &'),
                ('DM_X\ttoggle\t-\t1\talways&platform:nope\tGame\tX\n', 'mac', 'platform: takes names'),   # every part is checked
                ('DM_X\ttoggle\t-\t1\tplatform:mac,linux\tGame\tX\nDM_X\ttoggle\t-\t0\tplatform:linux\tGame\tX\n', 'linux', 'listed twice for linux'),
                ('DM_X\ttoggle\t-\t1|0\talways\tGame\tX\n', 'windows', 'names no windows value'),
                ('DM_X\ttoggle\t-\t1\talways\tGame\tX\n', 'beos', 'SETTINGS_PLATFORM must be ios or mac')]:
            with self.subTest(why=why, platform=platform), table(text) as tmp, patch.object(settings, 'HERE', Path(tmp)), self.assertRaises(SystemExit) as e:
                build(BOTW, 'nvn', platform)
            self.assertIn(why, str(e.exception))
        with table('DM_X\ttoggle\t-\t1|0\talways\tGame\tX\n') as tmp, patch.object(settings, 'HERE', Path(tmp)):   # Apple still takes <ios>|<mac>
            self.assertEqual([s['default'] for s in build(BOTW, 'nvn', 'mac')['settings']], ['0'])

    def test_needs_takes_the_values_of_every_row_of_a_variable(self):   # a sub-setting may name a value only the port's row offers
        text = ('DM_C\tchoice\tx=X\tx\tplatform:ios,mac\tGame\tC\nDM_C\tchoice\tz=Z\tz\tplatform:linux,windows\tGame\tC\nDM_D\ttoggle\t-\t1\talways\tGame\tD\t\tneeds=DM_C=z\n')
        with table(text) as tmp, patch.object(settings, 'HERE', Path(tmp)):
            self.assertEqual([s['env'] for s in build(BOTW, 'nvn', 'mac')['settings']], ['DM_C', 'DM_D'])
        with table(text.replace('needs=DM_C=z', 'needs=DM_C=w')) as tmp, patch.object(settings, 'HERE', Path(tmp)), self.assertRaises(SystemExit) as e:
            build(BOTW, 'nvn', 'linux')
        self.assertIn('never takes', str(e.exception))


def base_files():   # BASE's settings.py and settings.tsv in a folder of their own, or None when git or the commit is not there (a shallow CI checkout)
    out = Path(tempfile.mkdtemp())
    try:
        for name in ('settings.py', 'settings.tsv'):
            r = subprocess.run(['git', '-C', str(ROOT), 'show', f'{BASE}:dm/games/{name}'], capture_output=True, timeout=60)
            if r.returncode: return None
            (out / name).write_bytes(r.stdout)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return out


class AppleUnchanged(unittest.TestCase):
    def test_apple_lists_byte_for_byte_as_before_the_port(self):
        """Every profile's iPhone and Mac lists, for each graphics API, with one and with two languages: the JSON the packers write (settings.py's main), from BASE's settings.py
        and settings.tsv and from this tree's"""
        base = base_files()
        if base is None: self.skipTest(f'commit {BASE} is not in this checkout (a shallow clone): the comparison with the lists before the port\'s rows needs it')
        old = load(base / 'settings.py', 'launcher_settings_base')
        profiles = sorted(d.name for d in (ROOT / 'dm/games').glob('0*') if (d / 'game.tsv').is_file())
        self.assertGreater(len(profiles), 2)
        n = 0
        for gid in profiles:
            for api in ('nvn', 'vulkan', 'gl'):
                for platform in (None, 'ios', 'mac'):
                    for langs in (('en-US',), ('en-US', 'ja')):
                        before, after = (json.dumps(build(gid, api, platform, m, langs), indent=1, ensure_ascii=False) + '\n' for m in (old, settings))
                        if before != after: self.fail(f'{gid} {api} {platform or "ios (unset)"} {len(langs)} languages: the list differs from {BASE}\n--- {BASE}\n{before}\n--- now\n{after}')
                        n += 1
        self.assertEqual(n, len(profiles) * 18)


if __name__ == '__main__':
    unittest.main()
