#!/usr/bin/env python3
# scripts/regress.py: the parts that need no game, GPU or git revision: the MSL interface reader (GitHub issue #283), the [RED] normalization, the routes, and the comparisons
import importlib.util, json, tempfile, unittest
from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace as NS
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('regress', REPO / 'scripts/regress.py')
assert spec is not None and spec.loader is not None
regress = importlib.util.module_from_spec(spec); spec.loader.exec_module(regress)

VS = '''struct main0_out
{
    float4 m_51 [[user(locn0)]];
    float4 m_55 [[user(locn2), flat]];
    float4 gl_Position [[position, invariant]];
};

struct main0_in
{
    float4 a0 [[attribute(0)]];
};

vertex main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
}

vertex main0_out main0_z(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
}
'''
PS = '''struct main0_out
{
    float4 c0 [[color(0)]];
};

struct main0_in
{
    float4 m_1 [[user(locn0)]];
    float4 m_2 [[user(locn1)]];
    int4 m_3 [[user(locn2), flat]];
};

fragment main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], float4 gl_FragCoord [[position]])
{
}
'''


class Regress(unittest.TestCase):
    def test_interface(self):
        vo, fi = regress.msl_io(VS, 'main0_z', 'vs'), regress.msl_io(PS, 'main0', 'ps')
        self.assertEqual(vo, {'locn0': 'float4', 'locn2': 'float4'})
        self.assertEqual(fi, {'locn0': 'float4', 'locn1': 'float4', 'locn2': 'int4'})
        self.assertIsNone(regress.msl_io(VS, 'main1', 'vs'))
        self.assertEqual(regress.msl_io('fragment main0_out main0(constant X& x [[buffer(0)]])\n{\n}\n', 'main0', 'ps'), {})   # no stage inputs

    def test_red_normalized(self):
        a = regress.normalize_red('[dm] [RED] shader 0a91af5bef8578e2: 12 bindings at 0x1f00 in /Volumes/NX/games/x [y]/b')
        b = regress.normalize_red('[dm] [RED] shader 1b91af5bef8578e3: 7 bindings at 0x2000 in /tmp/z')
        self.assertEqual(a, b)

    def test_routes(self):   # every profile's smoke.tsv reads
        for f in (REPO / 'dm/games').glob('*/smoke.tsv'):
            r = regress.read_route(f)
            self.assertTrue(r['secs'] > 0 and r['quick'] > 0 and r['keys'], f)
            if r['reach']: self.assertTrue(r['reach'][1] <= r['secs'], f)

    def test_chain_diff(self):
        b = {'shaders': {'a': {'st': 'ok', 'msl': '1'}, 'b': {'st': 'ok', 'msl': '2'}, 'c': {'st': 'metal failed'}, 'd': {'st': 'ok', 'msl': '4'}},
             'interface': {'bad': [{'vs': 'v', 'entry': 'main0', 'ps': 'p', 'missing': ['locn1'], 'type': []}]}}
        c = {'shaders': {'a': {'st': 'ok', 'msl': '1'}, 'b': {'st': 'metal failed', 'err': 'x'}, 'c': {'st': 'ok', 'msl': '3'}, 'd': {'st': 'ok', 'msl': '5'}, 'e': {'st': 'translation failed', 'err': 'y'}},
             'interface': {'bad': [{'vs': 'v', 'entry': 'main0', 'ps': 'p', 'missing': ['locn1'], 'type': []}, {'vs': 'w', 'entry': 'main0', 'ps': 'p', 'missing': ['locn6'], 'type': []}]}}
        d = regress.chain_diff(b, c)
        self.assertEqual([x['key'] for x in d['newly_failing']], ['b']); self.assertEqual(d['newly_passing'], ['c'])
        self.assertEqual([x['key'] for x in d['added_failing']], ['e'])   # shader_regressions.py does not count a new key; the harness lists it apart
        self.assertEqual(d['changed']['msl'], 1); self.assertEqual(d['changed']['keys'], ['d'])
        self.assertEqual([x['vs'] for x in d['interface']['new']], ['w']); self.assertEqual(len(d['interface']['known']), 1)

    def test_smoke_diff(self):
        route = {'reach': (100, 30.0)}
        shot = {'dhash': '0f0f0f0f0f0f0f0f', 'rgb16': '10' * 768}
        base = [{'exit': None, 'max_frame': 500, 'reached': True, 'fps': 60, 'frame_p99': 17, 'peak_footprint_mb': 4000, 'prepare_s': 1.0, 'red': {'a #': 2}, 'shots': {'45': shot}}]
        same = [dict(base[0])]
        lv, known, d = regress.smoke_diff(base, same, route)
        self.assertFalse(lv['NEW-FAIL'] or lv['REGRESSION'] or lv['CHANGED'], lv)
        bad = [dict(base[0], exit=-11, crash={'signal': 'SIGSEGV'}, red={'a #': 1, 'b #': 3}, fps=40, shots={'45': {'dhash': 'f0f0f0f0f0f0f0f0', 'rgb16': '80' * 768}})]
        lv, known, d = regress.smoke_diff(base, bad, route)
        self.assertTrue(any('crash' in x for x in lv['NEW-FAIL']), lv)
        self.assertTrue(any('RED' in x for x in lv['REGRESSION']) and any('fps' in x for x in lv['REGRESSION']) and any('screenshot' in x for x in lv['REGRESSION']), lv)
        self.assertEqual(d['new_red'], [{'text': 'b #', 'count': 3}])
        lv, known, d = regress.smoke_diff(bad, bad, route)   # a failure the baseline has too is known, not new
        self.assertFalse(lv['NEW-FAIL']); self.assertTrue(known)
        quit0 = [dict(base[0], exit=0)]   # the game ended by itself before the route did: a failure even with exit status 0
        self.assertTrue(any('exited before the end' in x for x in regress.smoke_diff(base, quit0, route)[0]['NEW-FAIL']))

    def test_status_order(self):
        self.assertEqual(regress.worst('PASS', 'CHANGED', 'REGRESSION'), 'REGRESSION'); self.assertEqual(regress.worst('NEW-FAIL', 'ERROR'), 'ERROR')

    def test_selection_keeps_games_without_routes(self):
        games = [NS(id='a', name='routed', kind='nvn', route=lambda: {'save': 'new'}), NS(id='b', name='unrouted', kind='nvn', route=lambda: None)]
        with patch.object(regress, 'find_games', return_value=games), patch.object(regress, 'say'):
            self.assertEqual(regress.select(NS(games=None), smoke=True), games)

    def test_no_smoke_checks_every_chain_without_building_hosts(self):
        route = {'save': 'new', 'keys': '1:A', 'secs': 20, 'quick': 10, 'reach': (1, 5)}
        games = [NS(id='a', name='routed', route=lambda: route), NS(id='b', name='unrouted', route=lambda: None)]
        tree = NS(id='tree')
        manifest = {'kind': 'nvn', 'shaders': {}, 'failed': 0}
        a = NS(baseline='base', candidate='cand', no_smoke=True, rewrite=False, quick=True, runs=1)
        with tempfile.TemporaryDirectory() as td, ExitStack() as stack:
            stack.enter_context(patch.object(regress, 'ROOT', Path(td)))
            stack.enter_context(patch.object(regress, 'Tree', return_value=tree))
            stack.enter_context(patch.object(regress, 'select', return_value=games))
            stack.enter_context(patch.object(regress, 'rewriter_key', return_value='same'))
            chain = stack.enter_context(patch.object(regress, 'chain_build', return_value=(manifest, Path(td))))
            hosts = stack.enter_context(patch.object(regress, 'artifacts', side_effect=AssertionError('a static check built a host')))
            smoke = stack.enter_context(patch.object(regress, 'smoke_series', side_effect=AssertionError('a static check ran a game')))
            stack.enter_context(patch.object(regress, 'new_report', return_value={'games': {}}))
            finish = stack.enter_context(patch.object(regress, 'finish', return_value=0))
            stack.enter_context(patch.object(regress, 'say'))
            self.assertEqual(regress.cmd_check(a), 0)
            self.assertEqual(chain.call_count, 4)
            hosts.assert_not_called(); smoke.assert_not_called()
            self.assertEqual([g['status'] for g in finish.call_args.args[0]['games'].values()], ['PASS', 'PASS'])

    def test_reused_chain_rechecks_current_interface(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); g = NS(id='game', kind='nvn', folder=root)
            out = regress.game_root(g) / 'chain/key'; out.mkdir(parents=True)
            (out / 'manifest.json').write_text(json.dumps({'kind': 'nvn', 'interface': {'bad': []}}))
            current = {'bad': [{'vs': 'new-pair'}]}
            with patch.object(regress, 'ROOT', root), patch.object(regress, 'chain_key', return_value=('v', 'i', 'key')), patch.object(regress, 'interface_check', return_value=current) as check:
                manifest, _ = regress.chain_build(NS(), g, NS(tools_fp='', jobs=1))
            check.assert_called_once()
            self.assertEqual(manifest['interface'], current)

    def test_smoke_requires_prebuild_and_screenshots(self):
        route = {'reach': (100, 10.0), 'shots': [8]}
        good = {'exit': None, 'max_frame': 500, 'reached': True, 'prebuild': {'exit': 0, 'done': True, 'failed': 0},
                'shots': {'8': {'dhash': '0' * 16, 'rgb16': '00' * 768}}}
        for prebuild in ({'exit': -11, 'hang': False}, {'exit': None, 'hang': False}, {'exit': 0, 'done': False}):
            with self.subTest(prebuild=prebuild):
                bad = dict(good, prebuild=prebuild)
                self.assertTrue(regress.smoke_diff([good], [bad], route)[0]['NEW-FAIL'])
        self.assertTrue(regress.smoke_diff([good], [dict(good, shots={})], route)[0]['NEW-FAIL'])
        self.assertTrue(regress.run_failures({'exit': None, 'max_frame': -1, 'reached': False}, {'reach': None}))

    def test_each_candidate_screenshot_is_checked(self):
        route = {'reach': None}
        good = {'exit': None, 'max_frame': 500, 'shots': {'8': {'dhash': '0' * 16, 'rgb16': '00' * 768}}}
        bad = dict(good, shots={'8': {'dhash': 'f' * 16, 'rgb16': 'ff' * 768}})
        self.assertTrue(regress.smoke_diff([good], [good, bad], route)[0]['REGRESSION'])

    def test_shader_build_pauses_for_unknown_lock_owner(self):
        with tempfile.TemporaryDirectory() as td:
            with patch.object(regress.subprocess, 'Popen') as spawn, patch.object(regress.bench, 'lock_holder', return_value=0), patch.object(regress.os, 'killpg') as kill, patch.object(regress.time, 'sleep'), patch.object(regress, 'say'):
                spawn.return_value.poll.side_effect = [None, 0, 0]
                regress.run_heavy(['build'], Path(td) / 'build.log', td, {}, NS(pause_lock=Path(td) / 'lock'), 'build')
            self.assertTrue(any(c.args[1] == regress.signal.SIGSTOP for c in kill.call_args_list))

    def test_heavy_jobs_recognizes_harness_descendants_outside_shared_root(self):
        processes = '100 1 S python3 /source/scripts/regress.py check\n101 100 S python3 /trees/dmsh_build.py\n102 101 R metal /games/title-work/regress/tmp/shader.metal\n103 1 R metal /another/build.metal\n104 100 R make dmhost\n'
        with patch.object(regress, 'sh', return_value=NS(stdout=processes)), patch.object(regress.os, 'getpid', return_value=200):
            self.assertEqual(regress.heavy_jobs(), ['103 metal /another/build.metal', '104 make dmhost'])

    def test_chain_key_tracks_shader_package_and_split_pairs(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); pkg = root / 'shader/pkg'; pkg.mkdir(parents=True)
            shader = pkg / 'a.bin'; shader.write_bytes(b'original')
            prof = root / 'profile'; prof.mkdir()
            g = regress.Game(root, root / ('0' * 16 + '_build'))
            g.build, g.id, g.kind, g.derived = root, 'game', 'nvn', False
            tree = NS(path=root, py=lambda *args: '["chain", "inputs"]')
            with patch.object(g, 'profile_dir', return_value=prof):
                original = regress.chain_key(tree, g, 'tools')[2]
                shader.write_bytes(b'changed')
                changed = regress.chain_key(tree, g, 'tools')[2]
                (root / 'shader/pso_pairs.tsv').write_text('vertex\tfragment\n')
                paired = regress.chain_key(tree, g, 'tools')[2]
            self.assertNotEqual(original, changed)
            self.assertNotEqual(changed, paired)

    def test_baseline_context_tracks_route_save_artifacts_and_settings(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); save = root / 'save'; save.mkdir()
            (save / 'data').write_bytes(b'original')
            prof = root / 'profile'; prof.mkdir(); (prof / 'game.tsv').write_text('depth_mode\tgl\n')
            art = {'host_key': 'host', 'modules_key': 'modules', 'manifest': {'key': 'chain'}, 'profile': prof}
            a = NS(env=[], tools_fp='tools', no_hud=False, runs=1, prebuild_limit=100, prebuild_hang=10)
            route = {'save': 'run', 'keys': '', 'secs': 90, 'quick': 60, 'shots': [40]}
            with patch.object(regress, 'save_source', return_value=save):
                original = regress.smoke_context(NS(), art, route, a, {})
                self.assertNotEqual(original, regress.smoke_context(NS(), art, dict(route, shots=[45]), a, {}))
                self.assertNotEqual(original, regress.smoke_context(NS(), dict(art, host_key='other'), route, a, {}))
                a.env = ['DM_RENDER_SCALE=0.5']
                self.assertNotEqual(original, regress.smoke_context(NS(), art, route, a, {}))
                a.env = []; (save / 'data').write_bytes(b'changed')
                self.assertNotEqual(original, regress.smoke_context(NS(), art, route, a, {}))

    def test_stored_baseline_is_reused_only_with_matching_context(self):
        with tempfile.TemporaryDirectory() as td, ExitStack() as stack:
            root = Path(td); tree = NS(id='tree')
            route = {'reach': None, 'shots': []}
            g = NS(id='game', name='game', folder=root, route=lambda: route)
            a = NS(baseline='base', candidate='cand', no_smoke=False, rewrite=False, quick=True, runs=1)
            art = {'manifest': {'kind': 'nvn', 'shaders': {}, 'failed': 0}}
            good = {'exit': None, 'max_frame': 500, 'shots': {}}
            stack.enter_context(patch.object(regress, 'ROOT', root))
            stack.enter_context(patch.object(regress, 'Tree', return_value=tree))
            stack.enter_context(patch.object(regress, 'select', return_value=[g]))
            stack.enter_context(patch.object(regress, 'rewriter_key', return_value='same'))
            stack.enter_context(patch.object(regress, 'artifacts', return_value=art))
            stack.enter_context(patch.object(regress, 'new_report', side_effect=lambda *args, **kwargs: {'games': {}, 'machine': {}}))
            stack.enter_context(patch.object(regress, 'finish', return_value=0))
            stack.enter_context(patch.object(regress, 'say'))
            context = stack.enter_context(patch.object(regress, 'smoke_context', return_value={'settings': 'a'}))
            smoke = stack.enter_context(patch.object(regress, 'smoke_series', return_value=[good]))
            regress.cmd_check(a)
            self.assertEqual(smoke.call_count, 2)   # baseline and candidate
            smoke.reset_mock(); regress.cmd_check(a)
            self.assertEqual(smoke.call_count, 1)   # candidate only
            smoke.reset_mock(); context.return_value = {'settings': 'b'}; regress.cmd_check(a)
            self.assertEqual(smoke.call_count, 2)   # changed settings record a fresh baseline

    def test_profiles_and_baselines_stay_in_the_game_work_folder(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); folder = root / 'nsp'; folder.mkdir()
            build = folder / ('0' * 16 + '_build'); build.mkdir()
            tree = NS(path=root / 'tree', id='tree')
            source = tree.path / 'dm/games' / ('0' * 16); source.mkdir(parents=True)
            (source / 'game.tsv').write_text('depth_mode\tgl\n')
            g = regress.Game(folder, build)
            prof = g.profile_dir(tree)
            self.assertTrue(prof.is_relative_to(folder / f'{g.id}-work'))
            self.assertTrue(regress.baseline_dir(tree, g).is_relative_to(folder / f'{g.id}-work'))
            (prof / 'c1_full.tsv').write_text('generated\n')
            self.assertFalse((source / 'c1_full.tsv').exists())
            (source / 'game.tsv').write_text('depth_mode\tzero\n')
            self.assertNotEqual(g.profile_dir(tree), prof)
            env = g.env(tree)
            self.assertEqual(env['DM_GAMES_DIR'], str(g.profile_dir(tree).parent))
            self.assertEqual(env['DM_GAMES_FIRST'], '1')

    def test_vk_roots_include_runtime_capture(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); build = root / ('0' * 16 + '_build')
            romfs = build / 'extract/v0/RomFS'; romfs.mkdir(parents=True)
            (build / 'patched_v0').mkdir()
            capture = romfs.parent / 'vk_capture'; capture.mkdir()
            g = regress.Game(root, build)
            self.assertEqual(g.shader_roots(), [romfs, capture])

    def test_vk_build_records_failure_even_without_generated_shaders(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); tool = root / 'dm/gfx/tools/vk_shaders.py'; tool.parent.mkdir(parents=True)
            tool.write_text('raise SystemExit(1)\n')
            g = NS(id='game', folder=root, build=root, kind='vk', env=lambda tree: dict(regress.os.environ), shader_roots=list)
            with patch.object(regress, 'chain_key', return_value=('version', 'inputs', 'key')):
                manifest, _ = regress.chain_build(NS(id='tree', path=root), g, NS(tools_fp='', jobs=1, pause_lock=None))
            self.assertTrue(manifest['build_failed'])
            self.assertEqual(manifest['shaders'], {})

    def test_vk_failure_without_msl_fails_the_check(self):
        with tempfile.TemporaryDirectory() as td, ExitStack() as stack:
            root = Path(td)
            tree = NS(id='tree'); g = NS(id='game', name='game')
            a = NS(baseline='base', candidate='cand', no_smoke=True, rewrite=False, quick=False)
            stack.enter_context(patch.object(regress, 'Tree', return_value=tree))
            stack.enter_context(patch.object(regress, 'select', return_value=[g]))
            stack.enter_context(patch.object(regress, 'rewriter_key', return_value='same'))
            baseline = {'kind': 'vk', 'shaders': {'key': {'st': 'ok'}}, 'failed': 0}
            candidate = {'kind': 'vk', 'shaders': {}, 'failed': 0, 'build_failed': True}
            stack.enter_context(patch.object(regress, 'chain_build', side_effect=[(baseline, root), (candidate, root)]))
            stack.enter_context(patch.object(regress, 'new_report', return_value={'games': {}}))
            finish = stack.enter_context(patch.object(regress, 'finish', return_value=1))
            stack.enter_context(patch.object(regress, 'say'))
            regress.cmd_check(a)
            self.assertEqual(finish.call_args.args[0]['games']['game']['status'], 'NEW-FAIL')


if __name__ == '__main__':
    unittest.main()
