"""Provisional multi-program packing (build_ipa_from_nsp.py): sibling discovery via nx_extract --list and same-family NUPs, 32-bit ARM sibling skipping,
shared provisional profile root, child-before-combined-host build order on mac-app / mac / ios, and programs.tsv / DM_PROGRAMS handoff."""
import contextlib
import importlib.util
import os
from pathlib import Path
import shutil
import sys
import tempfile
import types
from typing import Any
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games'))
sys.path.insert(0, str(REPO / 'dm/mac'))
import game as gamemod   # noqa: E402
import pack_app   # noqa: E402

spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
assert spec and spec.loader
nsp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nsp)

APP = 0x01008E3025222000   # ACA NEOGEO SELECTION Vol.9 (unregistered provisional title)


def make_profile(root: Path, tid: int, *, programs: str = '', program_of: str = '', depth: str = ''):
    d = root / f'{tid:016X}'
    d.mkdir(parents=True, exist_ok=True)
    lines = [
        f'title_id\t{tid:016X}',
        f'program_id\t{tid:016X}',
        f'name\tGame {tid & 0xFFF}',
        f'ipa\tGame{tid & 0xFFF}.ipa',
        'app_category\tpublic.app-category.games',
        'build\tv0\t1.0.0\tmain=11111111111111111111111111111111\tsdk=22222222222222222222222222222222',
        'package\tv0',
        'shader_versions\tv0',
        'modules\tmain sdk',
        'romfs_marker\tmarker.bin',
        'zip_store\t.metallib',
        'mem_reserve\t0x100000000',
        'mem_alias\t0x10000000',
        'mem_stack\t0x10000000',
        'app_pool\t0xCD500000',
        'sys_resource\t0x0',
        'shader_groups\t/=romfs',
        f'shader_note\tGame {tid & 0xFFF} shaders',
    ]
    if programs:
        lines.append(f'programs\t{programs}')
    if program_of:
        lines.append(f'program_of\t{program_of}')
    if depth:
        lines.append(f'depth_mode\t{depth}')
    (d / 'game.tsv').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    for name, text in nsp.EMPTY_TABLES.items():
        (d / name).write_text(text, encoding='utf-8')
    return gamemod.Game(d)


class ParseProgramList(unittest.TestCase):
    def test_single_program_and_non_program_ncas_ignored(self):
        out = '\n'.join([
            f'base Program {APP:016x}',
            f'base Control {APP:016x}',
            f'base Manual {APP:016x}',
            f'update Program {APP:016x}',
            'base Program 0100999999999001',   # unrelated application
        ])
        self.assertEqual(nsp.parse_program_list(out, APP), {})

    def test_multi_program_base_update_and_update_only(self):
        out = '\n'.join([
            f'base Program {APP:016x}',
            f'base Program {APP + 1:016x}',
            f'base Control {APP + 1:016x}',
            f'base Program {APP + 6:016x}',
            f'update Program {APP:016x}',
            f'update Program {APP + 1:016x}',
            f'update Program {APP + 2:016x}',   # update-introduced sibling (like XCOM 2 War of the Chosen)
        ])
        self.assertEqual(nsp.parse_program_list(out, APP), {
            1: {'tid': APP + 1, 'in_base': True, 'in_update': True},
            2: {'tid': APP + 2, 'in_base': False, 'in_update': True},
            6: {'tid': APP + 6, 'in_base': True, 'in_update': False},
        })


class ProvisionalMultiProgram(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()).resolve()
        self.addCleanup(shutil.rmtree, self.tmp)
        env = mock.patch.dict(os.environ, {}, clear=False)
        env.start()
        self.addCleanup(env.stop)
        os.environ.pop('DM_GAMES_DIR', None)
        os.environ.pop('DM_GAMES_FIRST', None)

    def test_write_profile_sets_program_of_only_for_child(self):
        facts = {
            'F': {
                'sdk': 19, 'program': APP + 6, 'sysres': 0, 'ounce': False,
                'mods': {
                    'main': {'bid': 'a' * 32, 'sizes': [1, 1, 1], 'bss': 0, 'far': False, 'imports': set(), 'exports': set(), 'comps': set(), 'kind': 'the game'},
                    'sdk': {'bid': 'b' * 32, 'sizes': [1, 1, 1], 'bss': 0, 'far': False, 'imports': set(), 'exports': set(), 'comps': set(), 'kind': 'nnSdk'},
                },
                'romfs': {
                    'n': 1, 'bytes': 10, 'dirs': __import__('collections').Counter(), 'exts': __import__('collections').Counter(),
                    'extb': __import__('collections').Counter(), 'extc': __import__('collections').Counter(),
                    'mags': __import__('collections').Counter(), 'magb': __import__('collections').Counter(),
                    'biggest': (10, 'marker.bin'), 'root': {'marker.bin'},
                },
                'markers': {}, 'main_markers': set(),
            },
            'name': 'Magician Lord', 'survey': 'survey.md', 'version': 'v0', 'display': '1.0.0',
        }
        root_dir = self.tmp / 'p0'
        child_dir = self.tmp / 'p6'
        with mock.patch.object(nsp, 'pg', __import__('profile_guess')), mock.patch.object(nsp, 'say'):
            nsp.write_profile(root_dir, APP, facts)
            nsp.write_profile(child_dir, APP + 6, facts, program_of=APP)
        self.assertNotIn('program_of\t', (root_dir / 'game.tsv').read_text())
        self.assertIn(f'program_of\t{APP:016X}', (child_dir / 'game.tsv').read_text())

    def test_other_programs_skips_arm32_shares_profile_root_and_builds_header_multi(self):
        profile_root = self.tmp / 'profile'
        g0 = make_profile(profile_root, APP)
        discovered = {
            1: {'tid': APP + 1, 'in_base': True, 'in_update': True},
            3: {'tid': APP + 3, 'in_base': True, 'in_update': True},   # 32-bit ARM
            6: {'tid': APP + 6, 'in_base': True, 'in_update': False},  # base-only unpatched sibling
        }
        seen_subs = []

        def fake_main(sub):
            seen_subs.append((sub.program, sub.profile_out, sub.package, sub.in_programs))
            if sub.program == 3:
                raise nsp.Arm32Error('32-bit ARM sibling')
            tid = APP + sub.program
            make_profile(Path(sub.profile_out), tid, program_of=f'{APP:016X}')
            return {'tid': tid, 'profile': f'{tid:016X}', 'profile_dir': Path(sub.profile_out)}

        args = nsp.parse_args([str(self.tmp), '--platform', 'mac-app'])
        args.package = 'v65536'
        with mock.patch.object(nsp, 'main', side_effect=fake_main), mock.patch.object(nsp, 'say'):
            runs = nsp.other_programs(args, g0, provisional=True, discovered=discovered, app=APP, base_version=0)
            g0 = nsp.finalize_provisional_programs(g0, runs, provisional=True)

        self.assertEqual(nsp.PROGRAM_INDEX, 0)
        self.assertEqual(sorted(runs), [1, 6])
        self.assertEqual([(p, po, pkg) for p, po, pkg, _ in seen_subs], [
            (1, str(profile_root), 'v65536'),
            (3, str(profile_root), 'v65536'),
            (6, str(profile_root), 'v0'),
        ])
        self.assertEqual(g0.get('programs'), f'1={APP + 1:016X} 6={APP + 6:016X}')

        env = {'DM_GAMES_DIR': str(profile_root)}
        nsp.merge_games_dir(env, [r['profile_dir'] for r in runs.values()])
        header = gamemod.header(gamemod.load(f'{APP:016X}'))
        self.assertIn('#define DM_GAME_MULTI 1', header)
        self.assertIn(f'.program = 6, .id = "{APP + 6:016X}"', header)

    def test_cached_provisional_profile_with_explicit_programs_is_preserved(self):
        profile_root = self.tmp / 'profile'
        g0 = make_profile(profile_root, APP, programs=f'6={APP + 6:016X}')
        called = []
        args = nsp.parse_args([str(self.tmp), '--platform', 'mac-app'])
        with mock.patch.object(nsp, 'main', side_effect=lambda sub: called.append(sub.program) or {'tid': APP + sub.program, 'profile': f'{APP + sub.program:016X}', 'profile_dir': profile_root}), \
             mock.patch.object(nsp, 'say'):
            runs = nsp.other_programs(args, g0, provisional=True, discovered={1: {'tid': APP + 1, 'in_base': True, 'in_update': False}}, app=APP)
            g0 = nsp.finalize_provisional_programs(g0, runs, provisional=True)
        self.assertEqual(called, [6])
        self.assertEqual(g0.get('programs'), f'6={APP + 6:016X}')


class EndToEndBuildOrder(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()).resolve()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.nsps = self.tmp / 'nsps'
        self.nsps.mkdir()
        (self.nsps / 'base.nsp').write_bytes(b'PFS0')
        self.keys = self.tmp / 'prod.keys'
        self.keys.write_text('k')
        env = mock.patch.dict(os.environ, {}, clear=False)
        env.start()
        self.addCleanup(env.stop)
        os.environ.pop('DM_GAMES_DIR', None)
        os.environ.pop('DM_GAMES_FIRST', None)

    def run_provisional_collection(self, platform: str):
        events = []
        fam = {'base': self.nsps / 'base.nsp', 'base_version': 0, 'updates': [], 'dlc': []}
        profile_root = self.nsps / f'{APP:016X}_build' / 'profile'

        def fake_survey(nsp_dir, app, fam_, keys, repo, py, env):
            return {'F': {}, 'name': f'ACA {app & 0xFFF}', 'display': '1.0.0', 'version': 'v0'}

        def fake_write_profile(pdir, app, facts, program_of=None):
            make_profile(pdir.parent, app, program_of=f'{program_of:016X}' if program_of is not None else '')

        def fake_extract(nx, keys, ex, ver, b, u, logs, app, repo):
            dst = ex / ver
            (dst / 'ExeFS').mkdir(parents=True, exist_ok=True)
            (dst / 'ExeFS/main').write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex('11' * 16))
            (dst / 'ExeFS/sdk').write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex('22' * 16))
            (dst / 'RomFS').mkdir(parents=True, exist_ok=True)
            (dst / 'RomFS/marker.bin').write_bytes(b'data')
            (dst / 'html').mkdir(parents=True, exist_ok=True)
            return dst

        def fake_discover(fam_, app, b, u, nx, keys, log, env):
            events.append(('discover', f'{app:016X}'))
            return {6: {'tid': APP + 6, 'in_base': True, 'in_update': False}}

        def fake_mac_host(py, repo, work, env, logs, g):
            events.append(('mac_host', g.id, g.get('programs', ''), env.get('DM_GAMES_DIR', '')))
            host = work / 'tools/dmhost'
            host.parent.mkdir(parents=True, exist_ok=True)
            host.write_bytes(b'host')
            font = work / 'font'
            font.mkdir(parents=True, exist_ok=True)
            return host, font

        def fake_mac_app(py, repo, out, app, work, game, patched, host, font, ios, aoc, gfx, lang, g, *rest, **kw):
            programs = rest[9]
            events.append(('mac_app', g.id, sorted(programs)))
            out.mkdir(parents=True, exist_ok=True)

        def fake_build_ipa(cmd, log, env=None, **kw):
            assert env is not None
            events.append(('build_ipa', env.get('GAME'), env.get('PROGRAMS_TSV'), env.get('DM_GAMES_DIR', '')))
            out_ipa = Path(cmd[-1])
            out_ipa.write_bytes(b'ipa')
            return 0

        def fake_rewrite(py, repo, exefs, patched, env, logs, g, prov):
            events.append(('rewrite', g.id))
            patched.mkdir(parents=True, exist_ok=True)
            (patched / 'main.patched.bin').write_bytes(b'code')
            return g

        def fake_vk_shaders(py, repo, roots, sh, env, logs, g, mac=False, provisional=False, dlc_roms=()):
            (sh / 'vk').mkdir(parents=True, exist_ok=True)
            (sh / 'vk/k.metallib').write_bytes(b'lib')
            return sh

        args = nsp.parse_args([str(self.nsps), '-k', str(self.keys), '--platform', platform, '--no-heap-check', '--app-name', 'ACA Vol.9'])
        said = []
        with contextlib.ExitStack() as stack:
            pr = stack.enter_context(mock.patch.object(nsp, 'package_reader'))
            pr.return_value.__enter__.return_value = types.SimpleNamespace(contents=lambda p: {}, check_keys=lambda f: None)
            patches: tuple[tuple[str, dict[str, Any]], ...] = (
                ('find_repo', {'return_value': REPO}), ('WORK_DIR', {'new': ''}), ('PROFILE_OUT', {'new': ''}),
                ('apple_silicon', {'return_value': True}), ('check_native_python', {}), ('check_capstone', {}),
                ('_importable', {'return_value': True}), ('scan_nsps', {'return_value': (APP, fam)}),
                ('decompress_nsz', {'return_value': fam}), ('same_build', {'return_value': None}),
                ('survey', {'side_effect': fake_survey}), ('write_profile', {'side_effect': fake_write_profile}),
                ('check_prereqs', {}), ('find_nx_extract', {'return_value': Path('/bin/true')}),
                ('extract', {'side_effect': fake_extract}), ('extract_html', {}), ('check_formats', {}),
                ('check_versions_aarch64', {}), ('free_gb', {'return_value': 100}),
                ('build_sources', {'return_value': {'graphics': ('vulkan', 'test'), 'vk_capture': (False, '')}}),
                ('rewrite', {'side_effect': fake_rewrite}), ('chain_header', {}), ('uses_jit', {'return_value': False}),
                ('build_vk_shaders', {'side_effect': fake_vk_shaders}),
                ('discover_sibling_programs', {'side_effect': fake_discover}),
                ('mac_host', {'side_effect': fake_mac_host}), ('mac_app', {'side_effect': fake_mac_app}),
                ('run', {'side_effect': fake_build_ipa}), ('say', {'side_effect': said.append}),
            )
            for attr, kw in patches:
                stack.enter_context(mock.patch.object(nsp, attr, **kw))
            stack.enter_context(mock.patch.object(nsp.PROG, 'say', side_effect=said.append))
            nsp.main(args)
        return events, said, profile_root

    def test_mac_app_builds_children_before_combined_host(self):
        events, _, profile_root = self.run_provisional_collection('mac-app')
        names = [e[0] for e in events]
        # Program 0 rewrites, discovers program 6, program 6 rewrites, THEN mac_host runs with programs=6=...
        self.assertEqual(events[:4], [
            ('rewrite', f'{APP:016X}'),
            ('discover', f'{APP:016X}'),
            ('rewrite', f'{APP + 6:016X}'),
            ('mac_host', f'{APP:016X}', f'6={APP + 6:016X}', str(profile_root)),
        ])
        self.assertIn(('mac_app', f'{APP:016X}', [6]), events)
        self.assertEqual(names.count('mac_host'), 1)

    def test_mac_dev_builds_children_before_host_and_writes_programs_tsv(self):
        events, said, profile_root = self.run_provisional_collection('mac')
        self.assertEqual(events, [
            ('rewrite', f'{APP:016X}'),
            ('discover', f'{APP:016X}'),
            ('rewrite', f'{APP + 6:016X}'),
            ('mac_host', f'{APP:016X}', f'6={APP + 6:016X}', str(profile_root)),
        ])
        tsv = self.nsps / f'{APP:016X}_build' / 'programs.tsv'
        self.assertTrue(tsv.is_file())
        text = tsv.read_text()
        self.assertIn(f'\n6\texe={self.nsps / f"{APP:016X}_build" / "tools/dmhost"}', text)
        self.assertIn('DM_PROGRAM=6', text)
        self.assertTrue(any(f'DM_PROGRAMS={tsv}' in s or f"DM_PROGRAMS='{tsv}'" in s for s in said), said)

    def test_ios_writes_ipa_programs_tsv_and_shares_games_dir(self):
        events, _, profile_root = self.run_provisional_collection('ios')
        ipa_event = next(e for e in events if e[0] == 'build_ipa')
        self.assertEqual(ipa_event[1], f'{APP:016X}')
        self.assertTrue(ipa_event[2] and Path(ipa_event[2]).is_file())
        self.assertIn(str(profile_root), ipa_event[3].split(os.pathsep))
        rows = [ln for ln in Path(ipa_event[2]).read_text().splitlines() if not ln.startswith('#')]
        self.assertEqual([r.split('\t')[:2] for r in rows], [['0', f'{APP:016X}'], ['6', f'{APP + 6:016X}']])


class PackAppProgramsTsv(unittest.TestCase):
    def test_child_without_shader_map_omits_nvn_shmap(self):
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp)
        for d in ('patched', 'romfs', 'msl', 'tables', 'font', 'p6/patched', 'p6/romfs', 'p6/msl', 'p6/tables', 'p6/html'):
            (tmp / d).mkdir(parents=True)
        (tmp / 'dmhost').write_bytes(b'host')
        for p in ('patched/main.patched.bin', 'patched/main.fixups.tsv', 'romfs/marker', 'msl/k.metallib', 'tables/shader_map.tsv', 'font/a.ttf',
                  'p6/patched/main.patched.bin', 'p6/patched/main.fixups.tsv', 'p6/romfs/data', 'p6/msl/k.metallib'):
            (tmp / p).write_bytes(b'x')
        args = types.SimpleNamespace(
            out=str(tmp / 'Vol9.app'), host=str(tmp / 'dmhost'), patched=str(tmp / 'patched'), romfs=str(tmp / 'romfs'),
            msl=str(tmp / 'msl'), tables=str(tmp / 'tables'), font=str(tmp / 'font'), title_id=f'{APP:016X}',
            name='Vol.9', version='1.0', bundle_id=f'org.nxnative.{APP:016x}', category='public.app-category.games',
            marker='marker', lang='en-US', sign='-', aoc=None, icon=None, pso=None, pso_pairs=None, env_file=None,
            settings=None, html=None, control_nacp=None, msl_name=None, runtime_python=None,
            program=['\t'.join(['6', str(tmp / 'p6/patched'), str(tmp / 'p6/romfs'), str(tmp / 'p6/msl'), str(tmp / 'p6/tables'), str(tmp / 'p6/html')])],
        )
        with mock.patch.object(pack_app, 'min_os', return_value='15.0'), \
             mock.patch.object(pack_app, 'build_launcher', side_effect=lambda dst, *a: dst.write_bytes(b'launcher')), \
             mock.patch.object(pack_app, 'sign'), mock.patch.object(pack_app, 'seals', return_value={}):
            pack_app.pack(args)
        rows = [ln for ln in (tmp / 'Vol9.app/Contents/Resources/dm/programs.tsv').read_text().splitlines() if not ln.startswith('#')]
        self.assertIn('DM_NVN_SHMAP=@R@/dm/shader_map.tsv', rows[0])
        self.assertTrue(rows[1].startswith('6\t'))
        self.assertNotIn('DM_NVN_SHMAP=', rows[1])
        self.assertIn('DM_PROGRAM=6', rows[1])


if __name__ == '__main__':
    unittest.main()
