"""The pipeline archive made offline (dm/gfx/tools/pso_offline.py): the pairs as the host reads them, the stamp the host compares (pso_prebuild's pre_stamp), the function list of a
.metallib header, the bases the host's prebuild would make (pre_desc) and those it leaves out, the pipelines script, and the per-architecture reuse of metal-tt's archives.
No game files, Xcode or GPU needed (metal-tt is replaced; temporary files only in a temporary folder)."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import re
import types
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import tracemalloc
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('pso_offline', REPO / 'dm/gfx/tools/pso_offline.py')
po = importlib.util.module_from_spec(spec); spec.loader.exec_module(po)
CHAIN = '0123456789ab'


def tag(name, data): return name + struct.pack('<H', len(data)) + data


def metallib(funcs, layer=False):   # layer: the layer's name in the library, as a library with a function that writes it holds it
    """A .metallib header as the Metal compiler writes it: the function list (count, then per function its size counting the size field, NAME, ENDT) and the public metadata
    (per function its size not counting the size field, a CNST tag for a function with constants, ENDT); funcs: [(name, has constants)], or (name, (constant name, ...))"""
    fl = struct.pack('<I', len(funcs))
    for name, _ in funcs:
        body = tag(b'NAME', name.encode() + b'\0') + tag(b'TYPE', b'\0') + b'ENDT'
        fl += struct.pack('<I', 4 + len(body)) + body
    pm = b''
    for _, const in funcs:
        names = const if isinstance(const, tuple) else ('spvLinearTextureAlignmentOverride',) if const else ()
        cnst = struct.pack('<H', len(names)) + b''.join(n.encode() + b'\0\x35' + struct.pack('<H', 216 if n == 'dm_border_off' else 0xffff) + b'\0' for n in names)
        body = (tag(b'CNST', cnst) if names else b'') + b'ENDT'
        pm += struct.pack('<I', len(body)) + body
    fo = 0x58; po_ = fo + len(fl)
    head = b'MTLB' + b'\0' * 0x14 + struct.pack('<QQQQ', fo, len(fl), po_, len(pm))
    head += b'\0' * (fo - len(head))
    return head + fl + pm + (b'render_target_array_index' if layer else b'') + b'BC\xc0\xde'


class Pairs(unittest.TestCase):
    def test_read_pairs_as_the_host(self):
        text = b'# header\nv1\tf1\n\nv2\tf2\nv1\tf1\nv3\n v4\tf4\nv1\tf1\textra\n'
        self.assertEqual(po.read_pairs(text), [('v1', 'f1'), ('v2', 'f2'), (' v4', 'f4'), ('v1', 'f1')])   # the last: another line, the same pair (plan drops the base twice)

    def test_stamp_matches_the_hosts_format(self):
        text = b'a\tb\n' + b'c\td\n'
        self.assertEqual(po.stamp(CHAIN, False, text), f'chain\t{CHAIN}\ndepth\tnear=-w\npairs\t8\t{hashlib.sha256(text).hexdigest()[:16]}\n')
        self.assertIn('depth\tnear=0\n', po.stamp(CHAIN, True, text))

    def test_pair_text_joins_the_lists_and_skips_a_missing_one(self):
        with tempfile.TemporaryDirectory() as t:
            a, b = Path(t, 'shader_pairs.tsv'), Path(t, 'pso_pairs.tsv')
            a.write_bytes(b'v\tf\n'); self.assertEqual(po.pair_text([a, b]), b'v\tf\n')
            b.write_bytes(b'w\tg\n'); self.assertEqual(po.pair_text([a, b]), b'v\tf\nw\tg\n')

    def test_arch_names(self):
        self.assertEqual(po.arch_name('g18p'), 'applegpu_g18p')
        self.assertEqual(po.arch_name(' applegpu_g17p'), 'applegpu_g17p')


class Functions(unittest.TestCase):
    def test_function_list_and_constants(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t, 'x.metallib'); p.write_bytes(metallib([('main0', False), ('main0_z', False), ('fmain', True)]))
            self.assertEqual(po.functions(p), {'main0': False, 'main0_z': False, 'fmain': True})
            p.write_bytes(b'not a library'); self.assertRaises(ValueError, po.functions, p)
            p.write_bytes(metallib([('main0', False)])[:0x60]); self.assertRaises(ValueError, po.functions, p)

    def test_constant_names(self):   # the CNST tag as the Metal compiler writes it (Ocarina of Time's border-off libraries: count 1, "dm_border_off", bool 0x35, index 216, 0)
        with tempfile.TemporaryDirectory() as t:
            p = Path(t, 'x.metallib'); p.write_bytes(metallib([('main0', ('dm_border_off',)), ('main0_u', ('dm_border_off', 'spvLinearTextureAlignmentOverride')), ('vs', False)]))
            self.assertEqual(po.constants(p), {'main0': ('dm_border_off',), 'main0_u': ('dm_border_off', 'spvLinearTextureAlignmentOverride'), 'vs': ()})
            self.assertEqual(po.functions(p), {'main0': True, 'main0_u': True, 'vs': False})
            self.assertEqual(po.HOST_CONSTANTS['dm_border_off'], (216, True))   # dmsh_abi.h DMSH_BORDER_FC, set as nvn_mtl.m pre_fn sets it
            self.assertEqual(po.HOST_CONSTANTS['dm_blend_off'], (217, False))   # dmsh_abi.h DMSH_BLEND_OFF_FC: false in the pair archive so hybrid stand-ins serve every blend state

    @unittest.skipUnless(shutil.which('xcrun'), 'needs the Metal compiler')
    def test_a_real_library(self):
        src = ('#include <metal_stdlib>\nusing namespace metal;\nconstant bool k [[function_constant(0)]];\nstruct V { float4 p [[position]]; };\n'
               'vertex V vs(uint i [[vertex_id]]) { V o; o.p = float4(i); return o; }\nfragment float4 fs() { return is_function_constant_defined(k) ? 1 : 0; }\n')
        with tempfile.TemporaryDirectory() as t:
            m, air, lib = Path(t, 'a.metal'), Path(t, 'a.air'), Path(t, 'a.metallib'); m.write_text(src)
            r = subprocess.run(['xcrun', '-sdk', 'iphoneos', 'metal', '-c', m, '-o', air], capture_output=True)
            if r.returncode: self.skipTest('no iOS Metal compiler')
            subprocess.run(['xcrun', '-sdk', 'iphoneos', 'metallib', air, '-o', lib], check=True, capture_output=True)
            self.assertEqual(po.functions(lib), {'vs': False, 'fs': True})


class Plan(unittest.TestCase):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp()) / f'b-{CHAIN}'; self.d.mkdir(); self.addCleanup(shutil.rmtree, self.d.parent)

    def shader(self, key, j, libs):   # reflection <key>_b.json and libraries {suffix: funcs or (funcs, layer)}
        (self.d / f'{key}_b.json').write_text(json.dumps(dict({'chain': CHAIN, 'entry': 'main0'}, **j)))
        for suf, f in libs.items():
            funcs, layer = f if isinstance(f, tuple) else (f, False)
            (self.d / f'{key}{suf}.metallib').write_bytes(metallib(funcs, layer))

    def test_bases_rest_and_left_out(self):
        vz = [('main0', False), ('main0_z', False)]
        self.shader('pull', {'entry_z': 'main0_z', 'pull': {'entry': 'main0'}, 'vertex_inputs': [{'location': 0}]}, {'_b': vz, '_bp': vz})
        self.shader('plain', {'entry_z': 'main0_z', 'vertex_inputs': []}, {'_b': vz})
        self.shader('inputs', {'vertex_inputs': [{'location': 0}]}, {'_b': vz})
        self.shader('layer', {'pull': {}, 'layer_out': True}, {'_b': (vz, True), '_bp': (vz, True)})   # main0 writes the layer: no base
        self.shader('other', {'chain': 'fedcba987654'}, {'_b': vz})
        self.shader('nolib', {'pull': {}}, {'_b': vz})   # no _bp
        self.shader('fs', {}, {'_b': [('main0', False)]})
        self.shader('fsc', {}, {'_b': [('main0', True)]})
        self.shader('fsx', {'entry': 'main1'}, {'_b': [('main0', False)]})
        self.shader('fsp', {'raster': {'primitive_id': 1}}, {'_b': [('main0', False)]})   # reads the primitive ID: no base
        self.shader('mesh', {'entry_z': 'main0_z', 'mesh_stage': {'kind': 'ms'}}, {'_b': vz})   # a mesh shader's pair (for its padding): built at the draw, no base
        # Layer entry points of its own (dmsh_entry.py vs_layer_entry): the library names the layer, but main0 writes none (no layer_out). Ocarina of Time's effect vertex shaders, with
        # no stage inputs and so no stand-in, were left out when the layer's name anywhere in the library counted, and compiled at their first draw
        self.shader('layered', {'entry_z': 'main0_z', 'entry_layer': 'main0_l', 'vertex_inputs': []}, {'_b': (vz + [('main0_l', False)], True)})
        pairs = [('pull', 'fs'), ('plain', 'fs'), ('inputs', 'fs'), ('layer', 'fs'), ('other', 'fs'), ('nolib', 'fs'), ('pull', 'fsc'), ('pull', 'fsx'), ('pull', 'gone'), ('pull', 'fs'),
                 ('plain', 'fsp'), ('mesh', 'fs'), ('layered', 'fs')]
        bases, rest, left = po.plan(self.d, CHAIN, pairs, False)
        self.assertEqual(bases, [('pull_bp', 'main0', 'fs_b', 'main0'), ('plain_b', 'main0', 'fs_b', 'main0'), ('layered_b', 'main0', 'fs_b', 'main0')])
        self.assertEqual(rest, [('pull', 'fsc')])
        self.assertEqual(left, {'reflection': 2, 'mesh': 1, 'stage inputs': 1, 'library': 1, 'layer': 1, 'primitive ID': 1, 'entry': 1})
        bases, _, _ = po.plan(self.d, CHAIN, pairs[:2], True)   # near = 0: the entry_z entries
        self.assertEqual(bases, [('pull_bp', 'main0_z', 'fs_b', 'main0'), ('plain_b', 'main0_z', 'fs_b', 'main0')])

    def test_vertex_entry_is_the_name_the_reflection_gives_the_library(self):   # nvn_mtl.m side_entry, as shader_load and pre_desc take it
        own = {'entry': 'main0', 'entry_z': 'main0_z'}
        self.assertEqual([po.vertex_entry(own, z) for z in (False, True)], ['main0', 'main0_z'])   # no pulling library: the shader's own, in <key>_b
        named = dict(own, pull={'entry': 'main0_p', 'entry_z': 'main0_zp', 'slot': 16, 'inputs': [0]})
        self.assertEqual([po.vertex_entry(named, z) for z in (False, True)], ['main0_p', 'main0_zp'])
        for pull in ({}, {'entry': 'main0'}, {'entry': '', 'entry_z': None}, {'entry_z': 7}):   # names none for the depth mode: the shader's own (today's libraries repeat it)
            self.assertEqual(po.vertex_entry(dict(own, pull=pull), True), 'main0_z', pull)
        self.assertEqual(po.vertex_entry(dict(own, pull={'entry_z': 'main0_zp'}), False), 'main0')
        self.assertEqual(po.vertex_entry(dict(own, pull=True), False), 'main0')   # not a pulling library's record
        self.assertEqual(po.vertex_entry({'entry': 'main0', 'pull': {'entry': 'main0_p', 'entry_z': 'main0_zp'}}, True), 'main0_p')   # no near = 0 entry of its own: `entry`, as the host
        self.assertIsNone(po.vertex_entry({}, False))

    def test_a_pulling_library_with_names_of_its_own(self):   # one compile unit holding a shader's entry points and its pulling ones cannot name both main0
        vz = [('main0', False), ('main0_z', False)]
        refl = {'entry_z': 'main0_z', 'vertex_inputs': [{'location': 0}]}
        self.shader('unit', dict(refl, pull={'entry': 'main0_p', 'entry_z': 'main0_zp'}), {'_b': vz, '_bp': [('main0_p', False), ('main0_zp', ('dm_border_off',))]})
        self.shader('own', dict(refl, pull={'entry': 'main0', 'entry_z': 'main0_z'}), {'_b': vz, '_bp': vz})   # as dmsh_pull.variant names them today
        self.shader('stale', dict(refl, pull={'entry': 'main0_p', 'entry_z': 'main0_zp'}), {'_b': vz, '_bp': vz})   # names an entry point its library does not hold: no base
        self.shader('fs', {}, {'_b': [('main0', False)]})
        pairs = [('unit', 'fs'), ('own', 'fs'), ('stale', 'fs')]
        bases, rest, left = po.plan(self.d, CHAIN, pairs, False)
        self.assertEqual(bases, [('unit_bp', 'main0_p', 'fs_b', 'main0'), ('own_bp', 'main0', 'fs_b', 'main0')])
        self.assertEqual((rest, left['entry']), ([], 1))
        bases, rest, left = po.plan(self.d, CHAIN, pairs, True)
        self.assertEqual(bases, [('unit_bp', 'main0_zp', 'fs_b', 'main0'), ('own_bp', 'main0_z', 'fs_b', 'main0')])
        self.assertEqual((rest, left['entry']), ([], 1))
        self.assertEqual(po.cut_entries(self.d, bases), {('unit_bp', 'main0_zp')})   # specialized, and not its library's first function: from a library of its own, under that name

    def test_border_constant_is_specialized_as_the_host_does(self):   # pre_fn: dm_border_off true, every other constant at its default; a pair with another constant stays on the device
        vz = [('main0', False), ('main0_z', False)]
        self.shader('pull', {'entry_z': 'main0_z', 'pull': {'entry': 'main0'}, 'vertex_inputs': [{'location': 0}]}, {'_b': vz, '_bp': [('main0', ('dm_border_off',))]})
        self.shader('fb', {}, {'_b': [('main0', ('dm_border_off',))]})
        self.shader('fbs', {}, {'_b': [('main0', ('dm_border_off', 'spvLinearTextureAlignmentOverride'))]})
        self.shader('fs', {}, {'_b': [('main0', False)]})
        bases, rest, _ = po.plan(self.d, CHAIN, [('pull', 'fb'), ('pull', 'fbs'), ('pull', 'fs')], False)
        self.assertEqual(bases, [('pull_bp', 'main0', 'fb_b', 'main0'), ('pull_bp', 'main0', 'fs_b', 'main0')])
        self.assertEqual(rest, [('pull', 'fbs')])
        j = po.script(self.d, bases)
        spec = j['function_descriptors']['specialized_function_descriptors']
        self.assertEqual([(s['label'], s['function_descriptor']) for s in spec], [('pull_bp.main0.host', 'fnd:pull_bp.main0'), ('fb_b.main0.host', 'fnd:fb_b.main0')])
        self.assertEqual(spec[0]['constant_values'], [{'id_type': 'FunctionConstantIndex', 'id': {'data': 216}, 'value_type': 'ConstantBool', 'value': {'data': True}}])
        p = j['pipeline_descriptors']['render_pipeline_descriptors']
        self.assertEqual([(x['vertex_function_descriptor'], x['fragment_function_descriptor']) for x in p],
                         [('fnd:pull_bp.main0.host', 'fnd:fb_b.main0.host'), ('fnd:pull_bp.main0.host', 'fnd:fs_b.main0')])
        self.shader('plain', {}, {'_b': vz})
        self.assertNotIn('specialized_function_descriptors', po.script(self.d, [('plain_b', 'main0', 'fs_b', 'main0')])['function_descriptors'])   # none: the script as before

    def test_a_specialized_function_after_the_first_has_a_library_of_its_own(self):   # metal-tt fails on it ("cannot find private metadata at offset '8'"): needs_cut
        both = [('main0', ('dm_border_off',)), ('main0_z', ('dm_border_off',))]
        self.shader('pull', {'entry_z': 'main0_z', 'pull': {'entry': 'main0'}, 'vertex_inputs': [{'location': 0}]}, {'_b': both, '_bp': both})
        self.shader('fb', {}, {'_b': [('main0', ('dm_border_off',)), ('main0_u', ('dm_border_off',))]})
        self.shader('plainz', {'entry_z': 'main0_z', 'pull': {'entry': 'main0'}, 'vertex_inputs': [{'location': 0}]}, {'_b': both, '_bp': [('main0', False), ('main0_z', False)]})
        bases = [('pull_bp', 'main0_z', 'fb_b', 'main0'), ('pull_bp', 'main0', 'fb_b', 'main0'), ('plainz_bp', 'main0_z', 'fb_b', 'main0')]
        j = po.script(self.d, bases)
        d = self.d.resolve()
        self.assertEqual({l['label']: l['path'] for l in j['libraries']}, {'pull_bp@main0_z': str(d / 'pull_bp@main0_z.metallib'), 'fb_b': str(d / 'fb_b.metallib'),
                                                                          'pull_bp': str(d / 'pull_bp.metallib'), 'plainz_bp': str(d / 'plainz_bp.metallib')})
        fns = {f['label']: f['library'] for f in j['function_descriptors']['library_function_descriptors']}
        self.assertEqual(fns, {'pull_bp.main0_z': 'lib:pull_bp@main0_z', 'fb_b.main0': 'lib:fb_b', 'pull_bp.main0': 'lib:pull_bp', 'plainz_bp.main0_z': 'lib:plainz_bp'})   # the first function, and one unspecialized, stay
        self.assertEqual([s['label'] for s in j['function_descriptors']['specialized_function_descriptors']], ['pull_bp.main0_z.host', 'fb_b.main0.host', 'pull_bp.main0.host'])
        self.assertEqual(po.cut_entries(self.d, bases), {('pull_bp', 'main0_z')})
        self.assertFalse(po.needs_cut({}, 'main0')); self.assertFalse(po.needs_cut({'main0': ('dm_border_off',)}, 'main0'))

    @unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode and Metal')
    def test_the_cut_library_holds_the_function_alone(self):   # real_libraries writes it, for a library in a folder as for one in an image
        src = '#include <metal_stdlib>\nusing namespace metal;\nconstant bool dm_border_off [[function_constant(216)]];\nstruct V { float4 p [[position]]; };\n' + ''.join(
            'vertex V %s(uint id [[vertex_id]]) { V o; o.p = float4(float(id), 0, is_function_constant_defined(dm_border_off) && dm_border_off ? %d.5 : 1, 1); return o; }\n' % (e, n)
            for n, e in enumerate(('main0', 'main0_z')))
        (self.d / 'v.metal').write_text(src)
        r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', str(self.d / 'v.metal'), '-o', str(self.d / 'v.air')], capture_output=True, text=True)
        if r.returncode: self.skipTest('no Metal compiler: ' + r.stderr[-200:])
        subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(self.d / 'v.air'), '-o', str(self.d / 'v_bp.metallib')], check=True, capture_output=True)
        self.assertEqual(po.constants(self.d / 'v_bp.metallib'), {'main0': ('dm_border_off',), 'main0_z': ('dm_border_off',)})
        work = self.d.parent / 'work'
        real = po.real_libraries(self.d, [('v_bp', 'main0_z', 'v_bp', 'main0')], work)
        self.assertEqual(real, work / 'libraries')
        self.assertEqual(po.constants(real / 'v_bp@main0_z.metallib'), {'main0_z': ('dm_border_off',)})
        self.assertEqual((real / 'v_bp.metallib').read_bytes(), (self.d / 'v_bp.metallib').read_bytes())
        self.assertTrue((real / 'v_bp.metallib').is_symlink(), 'a library of a folder is linked, not copied')
        po.clear_real_libraries(work)
        self.assertEqual(po.real_libraries(self.d, [('v_bp', 'main0', 'v_bp', 'main0')], work), self.d, 'nothing to cut: the folder as it is')

    def test_script(self):
        j = po.script(self.d, [('v_bp', 'main0', 'f_b', 'main0'), ('w_bp', 'main0_z', 'f_b', 'main0')])
        self.assertEqual([l['label'] for l in j['libraries']], ['v_bp', 'f_b', 'w_bp'])
        self.assertEqual(j['libraries'][0]['path'], str(self.d.resolve() / 'v_bp.metallib'))
        fns = j['function_descriptors']['library_function_descriptors']
        self.assertEqual([(f['label'], f['library'], f['name']) for f in fns],
                         [('v_bp.main0', 'lib:v_bp', 'main0'), ('f_b.main0', 'lib:f_b', 'main0'), ('w_bp.main0_z', 'lib:w_bp', 'main0_z')])
        p = j['pipeline_descriptors']['render_pipeline_descriptors']
        self.assertEqual([(x['vertex_function_descriptor'], x['fragment_function_descriptor']) for x in p],
                         [('fnd:v_bp.main0', 'fnd:f_b.main0'), ('fnd:w_bp.main0_z', 'fnd:f_b.main0')])
        self.assertEqual(set(p[0]), {'vertex_function_descriptor', 'fragment_function_descriptor', 'color_attachments'})   # no vertex descriptor, one sample: the defaults
        self.assertEqual(len(p[0]['color_attachments']), 8)
        self.assertTrue(all(v == 'Unspecialized' for a in p[0]['color_attachments'] for v in a.values()))
        self.assertEqual(len(p[0]['color_attachments'][0]), 9)


class Compute(unittest.TestCase):
    """The compute pipelines: every compute shader of the shader map after the prewarm list's C rows, each from its function alone"""
    d: Path   # Plan.setUp's folder
    setUp, shader = Plan.setUp, Plan.shader
    def test_shaders_from_the_list_then_the_map(self):
        m, l = self.d / 'shader_map.tsv', self.d / 'pso_manifest.tsv'
        m.write_text('package\tindex\tstage\tdm_ctl_sha16\tsource\ncs\t0\tcompute\tc2\tx\nvs\t0\tvertex\tv\tx\ncs\t1\tcompute\tc1\tx\ncs\t2\tcompute\tc3\tx\n')
        l.write_text('# header\nS\tv_b\t0\nC\tc1_b\nR\t00ff\nC\told_b\nC\tc1_b\n')
        self.assertEqual(po.read_computes([m], [l]), ['c1_b', 'old_b', 'c2_b', 'c3_b'])   # recorded order first, each once; the map's own order after
        self.assertEqual(po.read_computes([m], [self.d / 'none.tsv']), ['c2_b', 'c1_b', 'c3_b'])   # a provisional profile: no list, the map's alone
        self.assertEqual(po.read_computes([], []), [])

    def test_pipelines_and_left_out(self):
        self.shader('c1', {'stage': 'cs'}, {'_b': [('main0', False)]})
        self.shader('c2', {'stage': 'cs'}, {'_b': [('main0', False)]})
        self.shader('c3', {'stage': 'cs'}, {'_b': [('main0', True)]})   # function constants: the host specializes it, MTLDevice
        self.shader('c4', {'stage': 'cs', 'entry': 'k'}, {'_b': [('main0', False)]})
        self.shader('c5', {'stage': 'cs', 'chain': 'fedcba987654'}, {'_b': [('main0', False)]})
        self.shader('c6', {'stage': 'cs'}, {})
        self.shader('v', {'stage': 'vs'}, {'_b': [('main0', False)]})
        keys = ['c1_b', 'c2_b', 'c3_b', 'c4_b', 'c5_b', 'c6_b', 'v_b', 'gone_b', 'c1_b']
        pipes, left = po.plan_compute(self.d, CHAIN, keys)
        self.assertEqual(pipes, [('c1_b', 'main0'), ('c2_b', 'main0')])   # each once
        self.assertEqual(left, {'reflection': 3, 'library': 1, 'entry': 1, 'constants': 1})

    def test_border_constant_kernel_is_specialized(self):   # the border corrections' constant alone: specialized as cps_new looks up the common dispatch's pipeline (cps_new_b)
        self.shader('kb', {'stage': 'cs'}, {'_b': [('main0', ('dm_border_off',))]})
        self.shader('kbs', {'stage': 'cs'}, {'_b': [('main0', ('dm_border_off', 'spvLinearTextureAlignmentOverride'))]})
        pipes, left = po.plan_compute(self.d, CHAIN, ['kb_b', 'kbs_b'])
        self.assertEqual(pipes, [('kb_b', 'main0')]); self.assertEqual(left['constants'], 1)
        j = po.script(self.d, [], pipes)
        self.assertEqual(j['pipeline_descriptors']['compute_pipeline_descriptors'], [{'compute_function_descriptor': 'fnd:kb_b.main0.host'}])
        self.assertEqual(j['function_descriptors']['specialized_function_descriptors'][0]['constant_values'][0]['id'], {'data': 216})

    def test_pipelines_from_a_library_image(self):   # one-file libraries (b-<chain>.img, shader_lib.py), as dmsh_build.py writes them: read in place, as plan reads the bases
        self.shader('c1', {'stage': 'cs'}, {'_b': [('main0', False)]})
        self.shader('c3', {'stage': 'cs'}, {'_b': [('main0', True)]})
        self.assertTrue(po.shader_lib.pack(str(self.d), say=lambda m: None)); self.assertFalse(self.d.exists())
        pipes, left = po.plan_compute(po.shader_lib.view(str(self.d)), CHAIN, ['c1_b', 'c3_b'])
        self.assertEqual(pipes, [('c1_b', 'main0')])
        self.assertEqual(left, {'reflection': 0, 'library': 0, 'entry': 0, 'constants': 1})

    def test_script(self):
        j = po.script(self.d, [('v_bp', 'main0', 'f_b', 'main0')], [('c_b', 'main0'), ('d_b', 'main0')])
        self.assertEqual([l['label'] for l in j['libraries']], ['v_bp', 'f_b', 'c_b', 'd_b'])
        self.assertEqual(j['pipeline_descriptors']['compute_pipeline_descriptors'],   # the function alone: what the host's lookup names (cps_bundle)
                         [{'compute_function_descriptor': 'fnd:c_b.main0'}, {'compute_function_descriptor': 'fnd:d_b.main0'}])
        self.assertNotIn('compute_pipeline_descriptors', po.script(self.d, [('v_bp', 'main0', 'f_b', 'main0')])['pipeline_descriptors'])   # bases alone: the script as before

    def test_groups(self):
        b, c = [('v', f'e{i}', 'f', 'main0') for i in range(3)], [(f'c{i}_b', 'main0') for i in range(3)]
        self.assertEqual([(n, len(x), len(y)) for n, x, y in po.groups_of(b, c, 2, 2)], [('00001', 2, 0), ('00002', 1, 0), ('c00001', 0, 2), ('c00002', 0, 1)])
        self.assertEqual([(n, len(x), len(y)) for n, x, y in po.groups_of(b, [], 0, 0)], [('00001', 3, 0)])
        self.assertEqual([(n, len(x), len(y)) for n, x, y in po.groups_of([], c, 5, 0)], [('c00001', 0, 3)])


class BuildCase(unittest.TestCase):
    """A profile, its libraries and a fake metal-tt for main (Build, Prewarm)"""
    def setUp(self):
        self.t = Path(tempfile.mkdtemp()).resolve(); self.addCleanup(shutil.rmtree, self.t)
        self.lib = self.t / f'b-{CHAIN}'; self.lib.mkdir()
        for k, j, funcs in (('v', {'pull': {}, 'entry_z': 'main0_z'}, [('main0', False), ('main0_z', False)]), ('f', {}, [('main0', False)]), ('c', {}, [('main0', True)])):
            (self.lib / f'{k}_b.json').write_text(json.dumps(dict({'chain': CHAIN, 'entry': 'main0'}, **j)))
            (self.lib / f'{k}_b.metallib').write_bytes(metallib(funcs))
        (self.lib / 'v_bp.metallib').write_bytes(metallib([('main0', False), ('main0_z', False)]))
        (self.lib / 'inputs.tsv').write_text('game\tX\ninputs\tY\n')
        self.prof = self.t / 'games/0100000000001000'; self.prof.mkdir(parents=True)
        (self.prof / 'game.tsv').write_text('title_id\t0100000000001000\nname\tTest\ndepth_mode\tzero\n')
        (self.prof / 'shader_pairs.tsv').write_text('# pairs\nv\tf\nv\tc\n')
        self.out, self.work, self.calls = self.t / 'out', self.t / 'work', []
        self.compiler = 'AIR-TT 1.0'

    def fake_run(self, cmd, **kw):
        cmd = [str(c) for c in cmd]
        if '-version' in cmd: return subprocess.CompletedProcess(cmd, 0, self.compiler + '\n', '')
        if '--show-sdk-version' in cmd: return subprocess.CompletedProcess(cmd, 0, '27.1\n', '')
        self.calls.append(cmd); Path(cmd[cmd.index('-o') + 1]).write_bytes(b'archive ' + cmd[cmd.index('-arch') + 1].encode())
        return subprocess.CompletedProcess(cmd, 0)

    def main(self, *extra):
        args = ['--game', '0100000000001000', '--msl', str(self.lib), '--out', str(self.out), '--work', str(self.work), *extra]
        with mock.patch.object(po.subprocess, 'run', self.fake_run), mock.patch.dict(os.environ, DM_GAMES_DIR=str(self.prof.parent)), contextlib.redirect_stdout(io.StringIO()):
            return po.main(args)


class Build(BuildCase):
    """main: one archive per architecture, made again only when its key changes; the stamp, rest_pairs.tsv and archive.tsv beside them"""
    def test_large_script_memory_and_compatible_archive_key(self):
        # Share the two synthetic library files but give every base a distinct entry pair.
        bases = [('v_bp', f'vertex{i}', 'f_b', 'main0') for i in range(512)]
        at_compile = []
        compiler = self.fake_run
        def measure(cmd, **kwargs):
            if '-o' in cmd:
                at_compile.append(tracemalloc.get_traced_memory()[0])
            return compiler(cmd, **kwargs)
        with mock.patch.object(po, 'plan', return_value=(bases, [], {})), mock.patch.object(self, 'fake_run', measure):
            tracemalloc.start()
            try:
                self.main('--os-min', '27.0')
                peak = tracemalloc.get_traced_memory()[1]
            finally:
                tracemalloc.stop()
        path = self.work / 'bases.mtl4-json'
        self.assertFalse((self.work / 'bases.mtl4-json.partial').exists())
        self.assertLess(peak, 3 * path.stat().st_size, 'serializing descriptors must not retain multiple full JSON buffers')
        self.assertEqual(len(at_compile), 1)
        self.assertLess(at_compile[0], path.stat().st_size, 'the compiler only needs the script on disk')
        config = json.loads(path.read_bytes())
        self.assertEqual(len(config['pipeline_descriptors']['render_pipeline_descriptors']), len(bases))
        # Keys from earlier builds must continue to hit. Only library locations are normalized.
        for lib in config['libraries']:
            lib['path'] = lib['label'] + '.metallib'
        inputs = dict(format=1, configuration=config, libraries=po.library_digest(self.lib, bases),
                      arch='applegpu_g18p', platform='ios', os_min='27.0', toolchain=self.compiler)
        expected = hashlib.sha256(json.dumps(inputs, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        self.assertEqual((self.work / 'key.applegpu_g18p').read_text().strip(), expected)

    def test_build_reuse_and_stamp(self):
        self.assertEqual(self.main('--gpu', 'g18p'), 0)
        self.assertEqual(len(self.calls), 1)
        c = self.calls[0]
        self.assertEqual(c[:8], ['xcrun', '-sdk', 'iphoneos', 'metal-tt', '-arch', 'applegpu_g18p', '-ios-version-min', '27.0'])
        self.assertTrue(c[8].endswith('.mtl4-json'))   # the extension metal-tt takes the Metal 4 format from
        script = json.loads(Path(c[8]).read_text())
        self.assertEqual(script['pipeline_descriptors']['render_pipeline_descriptors'][0]['vertex_function_descriptor'], 'fnd:v_bp.main0_z')   # the profile's depth_mode zero
        self.assertEqual((self.out / 'applegpu_g18p.mtl4a').read_bytes(), b'archive applegpu_g18p')
        text = (self.prof / 'shader_pairs.tsv').read_bytes()
        self.assertEqual((self.out / 'prebuilt.tsv').read_text(), po.stamp(CHAIN, True, text))
        self.assertEqual((self.out / 'rest_pairs.tsv').read_text(), 'v\tc\n')
        self.assertIn('bases\t1\n', (self.out / 'archive.tsv').read_text())
        self.assertEqual(self.main('--gpu', 'g18p'), 0); self.assertEqual(len(self.calls), 1)   # unchanged: reused
        self.assertEqual(self.main('--gpu', 'g18p,applegpu_g17p'), 0)   # another architecture: only it is built
        self.assertEqual([x[5] for x in self.calls[1:]], ['applegpu_g17p'])
        self.assertEqual(self.main('--gpu', 'g17p', '--os-min', '26.0'), 0)   # another minimum: built again; g18p no longer asked for: removed
        self.assertEqual(self.calls[-1][7], '26.0')
        self.assertEqual(sorted(p.name for p in self.out.glob('*.mtl4a')), ['applegpu_g17p.mtl4a'])
        (self.prof / 'shader_pairs.tsv').write_text('v\tf\n')   # only a device-specialized pair removed: offline bases unchanged
        self.assertEqual(self.main('--gpu', 'g17p', '--os-min', '26.0'), 0)
        self.assertEqual(len(self.calls), 3)
        self.assertFalse((self.out / 'rest_pairs.tsv').exists())

    def reads(self, *args):   # a run → whether it read the library (plan), not only its name and time (run_key)
        with mock.patch.object(po, 'plan', side_effect=po.plan) as plan: self.assertEqual(self.main(*args), 0)
        return plan.called

    def test_a_library_image_and_lists_as_at_the_last_run_are_not_read_again(self):
        # Ocarina of Time: 26 s of every build that changed nothing went into reading 33,365 bases' libraries and reflections again to find the archive reused
        self.assertTrue(po.shader_lib.pack(str(self.lib), say=lambda m: None)); image = Path(po.shader_lib.image_of(str(self.lib)))
        self.assertTrue(self.reads()); self.assertEqual(len(self.calls), 1); self.assertTrue((self.work / 'run.key').is_file())
        state = lambda: {p.name: (p.stat().st_mtime_ns, p.read_bytes()) for p in sorted(self.out.iterdir())}
        before = state()
        with mock.patch.object(po, 'library_digest', side_effect=AssertionError('the libraries were read')): self.assertFalse(self.reads())
        self.assertEqual(len(self.calls), 1); self.assertEqual(state(), before)
        st = image.stat(); os.utime(image, ns=(st.st_atime_ns, st.st_mtime_ns + 1_000_000_000))   # the image written again: read, and found to hold the same libraries
        self.assertTrue(self.reads()); self.assertEqual(len(self.calls), 1)
        self.assertEqual(state(), before)   # the lists and stamps it would write again hold the same text: left as they are, with their times (the Mac app's packer keys them by those)
        self.assertFalse(self.reads())
        pairs = self.prof / 'shader_pairs.tsv'; pairs.write_text(pairs.read_text() + '# a comment\n')   # a pair list
        self.assertTrue(self.reads()); self.assertFalse(self.reads()); self.assertEqual(len(self.calls), 1)
        self.assertEqual((self.out / 'prebuilt.tsv').read_text(), po.stamp(CHAIN, True, pairs.read_bytes()))
        (self.prof / 'shader_map.tsv').write_text('p\tn\tcompute\tc\n')   # the compute shaders
        self.assertTrue(self.reads()); self.assertFalse(self.reads())
        self.compiler = 'AIR-TT 2.0'   # the compiler
        self.assertTrue(self.reads()); self.assertFalse(self.reads())
        self.assertTrue(self.reads('--gpu', 'g18p,g17p')); self.assertFalse(self.reads('--gpu', 'g18p,g17p')); self.assertTrue(self.reads())   # the architectures
        made = len(self.calls)
        for name, change in (('applegpu_g18p.mtl4a', Path.unlink), ('prebuilt.tsv', lambda f: f.write_text('chain\tother\n')), ('archive.tsv', Path.unlink)):   # an output not as the run left it
            with self.subTest(output=name):
                self.assertFalse(self.reads()); change(self.out / name); self.assertTrue(self.reads())
        self.assertEqual(len(self.calls), made + 1)   # the archive that was gone is built again
        (self.work / 'key.applegpu_g18p').unlink(); self.assertTrue(self.reads()); self.assertFalse(self.reads())
        (self.work / 'run.key').write_text('not a key'); self.assertTrue(self.reads()); self.assertFalse(self.reads())

    def test_a_library_folder_is_read_each_run(self):   # its files have each their own time: no run.key
        self.assertTrue(self.reads()); self.assertTrue(self.reads()); self.assertFalse((self.work / 'run.key').exists())

    def test_a_changed_image_with_preserved_size_and_mtime_is_read_again(self):
        self.assertTrue(po.shader_lib.pack(str(self.lib), say=lambda m: None))
        image = Path(po.shader_lib.image_of(str(self.lib)))
        self.assertTrue(self.reads()); self.assertFalse(self.reads()); self.assertEqual(len(self.calls), 1)
        before = image.stat(); original = image.read_bytes()
        changed = original.replace(b'BC\xc0\xde', b'BC\xc0\xdf')   # the used synthetic metallibs, same length; all image metadata and reflections stay valid
        self.assertNotEqual(changed, original)
        image.write_bytes(changed); os.utime(image, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.assertEqual((image.stat().st_size, image.stat().st_mtime_ns), (before.st_size, before.st_mtime_ns))
        self.assertTrue(self.reads()); self.assertEqual(len(self.calls), 2)
        self.assertFalse(self.reads())

    def test_an_architectures_prewarm_pipelines_stay(self):
        # the NSP script links them into the output folder after a run (mac_prebake) and takes them away: a run that removed them gave the files the app packs new change times
        self.main('--platform', 'macos', '--os-min', '27.0', '--gpu', 'g18p,g17p')
        for gpu in ('applegpu_g18p', 'applegpu_g17p'): (self.out / f'{gpu}-prewarm-00.mtl4a').write_bytes(b'bases'); (self.out / f'{gpu}-prewarm-00.keys').write_bytes(b'index')
        (self.out / 'prewarm.tsv').write_text('stamp\n')
        self.main('--platform', 'macos', '--os-min', '27.0', '--gpu', 'g18p,g17p')
        self.assertEqual(sorted(p.name for p in self.out.glob('*-prewarm-*.mtl4a')), ['applegpu_g17p-prewarm-00.mtl4a', 'applegpu_g18p-prewarm-00.mtl4a'])
        self.main('--platform', 'macos', '--os-min', '27.0', '--gpu', 'g18p')   # an architecture no longer asked for goes with its prewarm pipelines
        self.assertEqual(sorted(p.name for p in self.out.glob('*.mtl4a')), ['applegpu_g18p-prewarm-00.mtl4a', 'applegpu_g18p.mtl4a'])

    def test_chain_move_reuses_binary_and_refreshes_metadata_for_both_platforms(self):
        for platform in ('macos', 'ios'):
            with self.subTest(platform=platform):
                self.out, self.work = self.t / f'out-{platform}', self.t / f'work-{platform}'
                args = ('--platform', platform, '--os-min', '27.0')
                self.main(*args)
                archive = self.out / 'applegpu_g18p.mtl4a'
                identity = archive.stat().st_ino, archive.stat().st_mtime_ns, archive.read_bytes()
                before = len(self.calls)
                chain = 'fedcba987654' if platform == 'macos' else 'abcdef012345'
                moved = self.t / platform / f'b-{chain}'
                shutil.copytree(self.lib, moved)
                for p in moved.glob('*_b.json'):
                    j = json.loads(p.read_text()); j['chain'] = chain; p.write_text(json.dumps(j))
                (moved / 'inputs.tsv').write_text('game\tX\ninputs\tupdated-profile\n')
                self.lib = moved
                self.assertEqual(self.main(*args), 0)
                self.assertEqual(len(self.calls), before, 'chain-only move must not invoke metal-tt')
                self.assertEqual((archive.stat().st_ino, archive.stat().st_mtime_ns, archive.read_bytes()), identity)
                self.assertEqual((self.out / 'prebuilt.tsv').read_text(), po.stamp(chain, True, (self.prof / 'shader_pairs.tsv').read_bytes()))
                self.assertIn(str(moved), (self.work / 'bases.mtl4-json').read_text())

    def test_metadata_only_changes_reuse_but_refresh_runtime_lists(self):
        self.main()
        (self.lib / 'inputs.tsv').write_text('game\tX\ninputs\tnew-profile\n')
        pairs = b'# changed comment\nv\tf\nv\tf\n'
        (self.prof / 'shader_pairs.tsv').write_bytes(pairs)
        self.main()
        self.assertEqual(len(self.calls), 1)
        self.assertFalse((self.out / 'rest_pairs.tsv').exists())
        self.assertEqual((self.out / 'prebuilt.tsv').read_text(), po.stamp(CHAIN, True, pairs))

    def test_pipeline_entry_and_descriptor_changes_rebuild(self):
        self.main()
        self.main('--depth', 'gl')
        self.assertEqual(len(self.calls), 2)
        original = po.script
        def changed(lib_dir, bases, computes=()):
            config = original(lib_dir, bases, computes)
            config['pipeline_descriptors']['render_pipeline_descriptors'][0]['raster_sample_count'] = 4
            return config
        with mock.patch.object(po, 'script', changed):
            self.main('--depth', 'gl')
            self.assertEqual(len(self.calls), 3)
            self.main('--depth', 'gl')
            self.assertEqual(len(self.calls), 3)

    def test_platform_minimum_and_compiler_changes_rebuild(self):
        self.main('--platform', 'ios', '--os-min', '27.0')
        self.main('--platform', 'macos', '--os-min', '27.0')
        self.assertEqual(len(self.calls), 2)
        self.main('--platform', 'macos', '--os-min', '26.0')
        self.assertEqual(len(self.calls), 3)
        self.compiler = 'AIR-TT 2.0'
        self.main('--platform', 'macos', '--os-min', '26.0')
        self.assertEqual(len(self.calls), 4)

    def test_missing_archive_and_legacy_key_rebuild(self):
        self.main()
        (self.out / 'applegpu_g18p.mtl4a').unlink()
        self.main()
        self.assertEqual(len(self.calls), 2)
        (self.work / 'key.applegpu_g18p').write_text('legacy-cache-key\n')
        self.main()
        self.assertEqual(len(self.calls), 3)

    def test_stale_reflection_cannot_reuse_archive(self):
        self.main()
        p = self.lib / 'v_b.json'
        j = json.loads(p.read_text()); j['chain'] = 'stale'; p.write_text(json.dumps(j))
        with self.assertRaises(SystemExit): self.main()
        self.assertEqual(len(self.calls), 1)

    def test_macos_takes_this_macs_major_version(self):   # not the SDK's: Xcode 27.1's SDK is 27.0 on macOS 26.6, which cannot open a 27.0 archive (GitHub issue #673)
        def run(cmd, **kw):
            cmd = [str(c) for c in cmd]
            if cmd == ['sw_vers', '-productVersion']: return subprocess.CompletedProcess(cmd, 0, '26.6.2\n', '')
            return self.fake_run(cmd)
        with mock.patch.object(po.subprocess, 'run', run), mock.patch.dict(os.environ, DM_GAMES_DIR=str(self.prof.parent)), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(po.main(['--game', '0100000000001000', '--msl', str(self.lib), '--out', str(self.out), '--work', str(self.work), '--platform', 'macos', '--gpu', 'g16s']), 0)
        self.assertEqual(self.calls[0][:8], ['xcrun', '-sdk', 'macosx', 'metal-tt', '-arch', 'applegpu_g16s', '-macos-version-min', '26.0'])

    def test_depth_override_and_bad_key(self):
        self.main('--depth', 'gl', '--script-only')
        self.assertIn('depth\tnear=-w\n', (self.out / 'prebuilt.tsv').read_text()); self.assertEqual(self.calls, [])
        (self.prof / 'game.tsv').write_text('title_id\t0100000000001000\nname\tTest\ndepth_mode\tnear0\n')
        with self.assertRaises(SystemExit): self.main('--script-only')

    def test_profile_archives_match_observed_depth(self):
        # Issue #758 and its seven-game follow-up, then four found by reading the NSP script's check boots: these builds' NVN calls select near=0 (evidence in each game note).
        # Exercise the real profiles through the builder with synthetic pairs: an omitted key must not quietly build main0.
        profiles = ('01008CF01BAAC000', '010089C01E9CA000', '010056B015FE8000', '01007BB017812000',
                    '01003E601E324000', '010020400E1C2000', '0100429011144000', '010014C011146000',
                    '0100ACD0163D0000', '010040801A4C8000', '0100C49025D3E000', '0100A3900C3E2000',
                    '01006A800016E000',  # Super Smash Bros. Ultimate: a player's start log (its note)
                    '0400E88025B1A000',  # Star Fox: a Mac app's start log (its note)
                    '01005CA01580E000')  # Persona 5 Royal: an iPhone app's start log (its note)
        for gid in profiles:
            with self.subTest(game=gid):
                self.main('--game', gid, '--pairs', str(self.prof / 'shader_pairs.tsv'), '--script-only')
                self.assertIn('depth\tnear=0\n', (self.out / 'prebuilt.tsv').read_text())
                script = json.loads(next(self.work.glob('*.mtl4-json')).read_text())
                self.assertEqual(script['pipeline_descriptors']['render_pipeline_descriptors'][0]['vertex_function_descriptor'], 'fnd:v_bp.main0_z')

    def test_library_digest_is_the_names_and_bytes_in_order(self):   # read on a few threads: the key every earlier build's archive has
        bases, computes = [('v_bp', 'main0_z', 'f_b', 'main0'), ('v_b', 'main0', 'c_b', 'main0')], [('c_b', 'main0')]
        h = hashlib.sha256()
        for name in ('c_b', 'f_b', 'v_b', 'v_bp'): h.update(name.encode() + b'\0'); h.update(hashlib.sha256((self.lib / f'{name}.metallib').read_bytes()).digest())
        self.assertEqual(po.library_digest(self.lib, bases, computes), h.hexdigest())

    def test_library_content_invalidates_archive(self):
        self.main()
        for name in ('v_bp', 'f_b'):
            with self.subTest(library=name):
                lib = self.lib / f'{name}.metallib'
                lib.write_bytes(lib.read_bytes() + b'changed GPU program')   # same entries, reflection and profile inputs
                before = len(self.calls)
                self.main()
                self.assertEqual(len(self.calls), before + 1)
                self.main()
                self.assertEqual(len(self.calls), before + 1)   # unchanged bytes still reuse the archive

    def test_constant_only_pairs_are_left_to_device(self):
        self.main()   # remove the previous archive when no pair can be built offline anymore
        (self.prof / 'shader_pairs.tsv').write_text('v\tc\n')
        before = len(self.calls)
        self.assertEqual(self.main(), 0)
        self.assertEqual(len(self.calls), before)
        self.assertEqual(list(self.out.glob('*.mtl4a')), [])
        self.assertEqual((self.out / 'rest_pairs.tsv').read_text(), 'v\tc\n')
        self.assertEqual((self.out / 'prebuilt.tsv').read_text(), po.stamp(CHAIN, True, b'v\tc\n'))
        self.assertIn('bases\t0\n', (self.out / 'archive.tsv').read_text())

    def test_no_pairs_builds_nothing(self):   # GitHub issue #338: a header-only shader_pairs.tsv is nothing to build, not an error
        self.main()
        (self.prof / 'shader_pairs.tsv').write_text('# pairs\n')
        before = len(self.calls)
        self.assertEqual(self.main(), 0)
        self.assertEqual(len(self.calls), before)
        self.assertEqual(list(self.out.glob('*.mtl4a')), [])   # the old archive goes: mac_pso() then packs the app without one
        self.assertFalse((self.out / 'rest_pairs.tsv').exists())
        self.assertIn('bases\t0\n', (self.out / 'archive.tsv').read_text())

    def test_pairs_all_left_out_is_an_error(self):   # pairs whose libraries the host would not take: still a failure
        (self.prof / 'shader_pairs.tsv').write_text('x\ty\n')
        with self.assertRaises(SystemExit): self.main()

    def test_metal_tt_failure_leaves_no_archive(self):
        def fail(cmd, **kw):
            cmd = [str(c) for c in cmd]
            if '-version' in cmd or '--show-sdk-version' in cmd: return self.fake_run(cmd)
            return subprocess.CompletedProcess(cmd, 1)
        with mock.patch.object(po.subprocess, 'run', fail), mock.patch.dict(os.environ, DM_GAMES_DIR=str(self.prof.parent)), contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
            po.main(['--game', '0100000000001000', '--msl', str(self.lib), '--out', str(self.out), '--work', str(self.work)])
        self.assertEqual(list(self.out.glob('*.mtl4a')), [])

    def batched_runner(self, fail_compile=(), fail_merge=False):   # fail_compile: the vertex entries whose base metal-tt fails on
        original = self.fake_run
        observed = []
        def run(cmd, **kw):
            cmd = [str(c) for c in cmd]
            if 'metal-pack' in cmd:
                pieces = [Path(p) for p in cmd if p.endswith('.mtl4a')]
                observed.append(('merge', len(pieces)))
                Path(cmd[cmd.index('-o') + 1]).write_bytes(b''.join(p.read_bytes() for p in pieces))
                return subprocess.CompletedProcess(cmd, 1 if fail_merge else 0)
            if 'metal-tt' in cmd and '-o' in cmd:
                js = next(Path(p) for p in cmd if p.endswith('.mtl4-json'))
                config = json.loads(js.read_text())
                names = [f['name'] for f in config['function_descriptors']['library_function_descriptors'] if f['library'] == 'lib:v_bp']
                observed.append(('compile', names))
                if set(fail_compile) & set(names):
                    Path(cmd[cmd.index('-o') + 1]).write_bytes(b'partial')
                    return subprocess.CompletedProcess(cmd, 1)
            return original(cmd, **kw)
        return run, observed

    def test_batched_archive_reuses_pilot_and_normal_archive_key(self):
        bases = [('v_bp', f'vertex{i}', 'f_b', 'main0') for i in range(5)]
        runner, observed = self.batched_runner()
        with mock.patch.object(self, 'fake_run', runner), mock.patch.object(po, 'plan', return_value=(bases, [], {})):
            # An isolated pilot is the exact cache entry the complete build will consume.
            with mock.patch.object(po.subprocess, 'run', runner), contextlib.redirect_stdout(io.StringIO()):
                po.compile_batch(self.lib, bases[:2], self.work/'batches.applegpu_g18p/00001', 'applegpu_g18p', 'ios', '27.0', self.compiler)
            self.main('--batch-size', '2', '--os-min', '27.0')
            self.assertEqual(observed, [('compile',['vertex0','vertex1']),('compile',['vertex2','vertex3']),('compile',['vertex4']),('merge',3)])
            before = list(observed)
            self.main('--os-min', '27.0')
            self.assertEqual(observed, before, 'the normal packer must reuse the complete merged archive')
            self.assertIn('bases\t5\n', (self.out/'archive.tsv').read_text())

    def test_batch_failure_preserves_completed_work_and_publishes_nothing(self):
        bases = [('v_bp', f'vertex{i}', 'f_b', 'main0') for i in range(5)]
        runner, observed = self.batched_runner(fail_compile=('vertex2', 'vertex3'))   # nothing of the group compiles: metal-tt's failure, not one pipeline's
        with mock.patch.object(self, 'fake_run', runner), mock.patch.object(po, 'plan', return_value=(bases, [], {})), self.assertRaises(SystemExit):
            self.main('--batch-size','2','--os-min','27.0')
        self.assertEqual(observed[:2], [('compile',['vertex0','vertex1']),('compile',['vertex2','vertex3'])])
        self.assertTrue(all(set(names) <= {'vertex2', 'vertex3'} for _, names in observed[2:]))   # the search for the pipelines it stops on
        self.assertFalse((self.out/'prebuilt.tsv').exists())
        self.assertFalse((self.work/'key.applegpu_g18p').exists())
        self.assertFalse(list(self.out.glob('*.mtl4a')))
        self.assertFalse(list(self.work.rglob('*.partial')))
        first = self.work/'batches.applegpu_g18p/00001/applegpu_g18p.mtl4a'
        self.assertTrue(first.is_file()); identity = first.stat().st_mtime_ns
        runner, observed = self.batched_runner()
        with mock.patch.object(self, 'fake_run', runner), mock.patch.object(po, 'plan', return_value=(bases, [], {})):
            self.main('--batch-size','2','--os-min','27.0')
        self.assertEqual(first.stat().st_mtime_ns, identity)
        self.assertEqual(observed, [('compile',['vertex2','vertex3']),('compile',['vertex4']),('merge',3)])

    def test_a_base_metal_tt_cannot_compile_is_left_to_the_device(self):   # PRAGMATA: one pixel shader the GPU compiler stops on cost the whole archive
        bases = [('v_bp', f'vertex{i}', 'f_b', 'main0') for i in range(5)]
        runner, observed = self.batched_runner(fail_compile=('vertex2',))
        out = io.StringIO()
        with mock.patch.object(po.subprocess, 'run', runner), mock.patch.object(po, 'plan', return_value=(bases, [('v', 'c')], {})), \
                mock.patch.dict(os.environ, DM_GAMES_DIR=str(self.prof.parent)), contextlib.redirect_stdout(out):
            po.main(['--game', '0100000000001000', '--msl', str(self.lib), '--out', str(self.out), '--work', str(self.work), '--os-min', '27.0', '--no-compute'])
        compiles = [names for kind, names in observed if kind == 'compile']
        self.assertEqual(compiles[0], [f'vertex{i}' for i in range(5)])
        self.assertEqual(compiles[-1], ['vertex0', 'vertex1', 'vertex3', 'vertex4'])   # the archive: every base but the one that fails
        self.assertIn(['vertex2'], compiles)                                            # tried alone before it is named
        self.assertTrue((self.out / 'applegpu_g18p.mtl4a').is_file())
        self.assertEqual((self.out / 'rest_pairs.tsv').read_text(), 'v\tc\nv\tf\n')   # after the pairs with function constants
        self.assertIn('failed\t1 bases', (self.out / 'archive.tsv').read_text())
        self.assertRegex(out.getvalue(), r'\[RED\] pso_offline: applegpu_g18p: metal-tt cannot compile the base of v \+ f \(v_bp\.vertex2, f_b\.main0\)')
        self.assertEqual(out.getvalue().count('[RED]'), 1)
        n = len(observed); out = io.StringIO()   # a second run reuses the archive and still names the pair
        (self.out / 'rest_pairs.tsv').unlink()
        with mock.patch.object(po.subprocess, 'run', runner), mock.patch.object(po, 'plan', return_value=(bases, [('v', 'c')], {})), \
                mock.patch.dict(os.environ, DM_GAMES_DIR=str(self.prof.parent)), contextlib.redirect_stdout(out):
            po.main(['--game', '0100000000001000', '--msl', str(self.lib), '--out', str(self.out), '--work', str(self.work), '--os-min', '27.0', '--no-compute'])
        self.assertEqual(len(observed), n)
        self.assertEqual((self.out / 'rest_pairs.tsv').read_text(), 'v\tc\nv\tf\n')

    def test_a_failing_base_in_one_group_keeps_the_other_groups(self):
        bases = [('v_bp', f'vertex{i}', 'f_b', 'main0') for i in range(5)]
        runner, observed = self.batched_runner(fail_compile=('vertex3',))
        with mock.patch.object(self, 'fake_run', runner), mock.patch.object(po, 'plan', return_value=(bases, [], {})):
            self.main('--batch-size', '2', '--os-min', '27.0', '--no-compute')
        self.assertEqual(observed[0], ('compile', ['vertex0', 'vertex1']))
        self.assertEqual(observed[-1], ('merge', 3))
        self.assertIn(('compile', ['vertex2']), observed)   # the second group's archive, without the base that fails
        self.assertEqual((self.out / 'rest_pairs.tsv').read_text(), 'v\tf\n')
        self.assertEqual(po.read_failed(self.work / 'batches.applegpu_g18p/00002', 'applegpu_g18p'), ([('v_bp', 'vertex3', 'f_b', 'main0')], []))

    def test_more_failing_bases_than_the_limit_stop_the_build(self):
        bases = [('v_bp', f'vertex{i}', 'f_b', 'main0') for i in range(40)]
        runner, observed = self.batched_runner(fail_compile=[f'vertex{i}' for i in range(0, 40, 2)])
        with mock.patch.object(self, 'fake_run', runner), mock.patch.object(po, 'plan', return_value=(bases, [], {})), self.assertRaises(SystemExit) as e:
            self.main('--os-min', '27.0', '--no-compute')
        self.assertIn(f'more than {po.MAX_FAILED} pipelines', str(e.exception))
        self.assertFalse(list(self.out.glob('*.mtl4a')))

    def test_merge_failure_never_publishes_partial_archive(self):
        bases = [('v_bp', f'vertex{i}', 'f_b', 'main0') for i in range(3)]
        runner, _ = self.batched_runner(fail_merge=True)
        with mock.patch.object(self, 'fake_run', runner), mock.patch.object(po, 'plan', return_value=(bases, [], {})), self.assertRaises(SystemExit):
            self.main('--batch-size','2','--os-min','27.0')
        self.assertFalse(list(self.out.glob('*.mtl4a')))
        self.assertFalse((self.out/'prebuilt.tsv').exists())
        self.assertFalse((self.work/'key.applegpu_g18p').exists())
        self.assertFalse(list(self.work.rglob('*.partial')))
        runner, observed = self.batched_runner()
        with mock.patch.object(self, 'fake_run', runner), mock.patch.object(po, 'plan', return_value=(bases, [], {})):
            self.main('--batch-size','2','--os-min','27.0')
        self.assertEqual(observed, [('merge',2)])

    def test_groups_are_the_default_above_the_batch_size(self):
        bases = [('v_bp', f'vertex{i}', 'f_b', 'main0') for i in range(3)]
        for size, want in ((2, [('merge', 2)]), (3, [])):   # more bases than a group: groups, merged; as many: one compiler run, no merge
            shutil.rmtree(self.work, ignore_errors=True); shutil.rmtree(self.out, ignore_errors=True)
            runner, observed = self.batched_runner()
            with mock.patch.object(po, 'BATCH', size), mock.patch.object(po, 'plan', return_value=(bases, [], {})), mock.patch.object(self, 'fake_run', runner):
                self.main('--os-min', '27.0')
            self.assertEqual([x for x in observed if x[0] == 'merge'], want)
            self.assertTrue((self.out / 'applegpu_g18p.mtl4a').is_file())
        shutil.rmtree(self.work); shutil.rmtree(self.out)
        runner, observed = self.batched_runner()
        with mock.patch.object(po, 'BATCH', 2), mock.patch.object(po, 'plan', return_value=(bases, [], {})), mock.patch.object(self, 'fake_run', runner):
            self.main('--os-min', '27.0', '--batch-size', '0')   # 0: one compiler run
        self.assertEqual(observed, [('compile', ['vertex0', 'vertex1', 'vertex2'])])

    def test_groups_of_a_longer_list_are_removed(self):
        bases = [('v_bp', f'vertex{i}', 'f_b', 'main0') for i in range(4)]
        runner, _ = self.batched_runner()
        with mock.patch.object(po, 'plan', return_value=(bases, [], {})), mock.patch.object(self, 'fake_run', runner):
            self.main('--batch-size', '1', '--os-min', '27.0')
        groups = self.work / 'batches.applegpu_g18p'
        self.assertEqual(sorted(d.name for d in groups.iterdir()), ['00001', '00002', '00003', '00004'])
        with mock.patch.object(po, 'plan', return_value=(bases[:2], [], {})), mock.patch.object(self, 'fake_run', runner):
            self.main('--batch-size', '1', '--os-min', '27.0')
        self.assertEqual(sorted(d.name for d in groups.iterdir()), ['00001', '00002'])

    def test_compute_pipelines_are_groups_of_their_own(self):
        (self.prof / 'shader_map.tsv').write_text('package\tindex\tstage\tdm_ctl_sha16\tsource\ncs\t0\tcompute\tk\tx\ncs\t1\tcompute\tkc\tx\n')
        (self.prof / 'pso_manifest.tsv').write_text('# list\nC\tk_b\n')
        for k, const in (('k', False), ('kc', True)):
            (self.lib / f'{k}_b.json').write_text(json.dumps({'chain': CHAIN, 'stage': 'cs', 'entry': 'main0'}))
            (self.lib / f'{k}_b.metallib').write_bytes(metallib([('main0', const)]))
        runner, observed = self.batched_runner()
        def computes(): return [f['library'] for f in json.loads((self.work / 'bases.mtl4-json').read_text())['function_descriptors']['library_function_descriptors'] if f['library'].startswith('lib:k')]
        with mock.patch.object(self, 'fake_run', runner):
            self.main('--os-min', '27.0')
            self.assertEqual(observed, [('compile', ['main0_z']), ('compile', []), ('merge', 2)])   # the bases, the compute pipelines, one archive
            self.assertEqual(computes(), ['lib:k_b'])   # kc has function constants: left to the host
            self.assertIn('compute\t1\n', (self.out / 'archive.tsv').read_text())
            self.assertEqual(sorted(d.name for d in (self.work / 'batches.applegpu_g18p').iterdir()), ['00001', 'c00001'])
            observed.clear(); self.main('--os-min', '27.0'); self.assertEqual(observed, [])   # unchanged: the archive reused
            (self.prof / 'shader_pairs.tsv').write_text('v\tc\nv\tf\n')   # other pair lists, the same bases and compute pipelines: the archive reused
            observed.clear(); self.main('--os-min', '27.0'); self.assertEqual(observed, [])
            observed.clear(); self.main('--os-min', '27.0', '--depth', 'gl'); self.assertEqual(observed, [('compile', ['main0']), ('merge', 2)])   # other bases: the compute group kept
            (self.lib / 'k_b.metallib').write_bytes(metallib([('main0', False), ('other', False)]))   # another compute library: its group again, the bases kept
            observed.clear(); self.main('--os-min', '27.0', '--depth', 'gl'); self.assertEqual(observed, [('compile', []), ('merge', 2)])
            observed.clear(); self.main('--os-min', '27.0', '--depth', 'gl', '--no-compute'); self.assertEqual(observed, [('compile', ['main0'])])   # bases alone: one compiler run
            self.assertIn('compute\t0\n', (self.out / 'archive.tsv').read_text())

    def test_compute_pipelines_alone_make_an_archive(self):   # no base (every pair has function constants), but compute shaders: still an archive
        (self.prof / 'shader_pairs.tsv').write_text('v\tc\n')
        (self.prof / 'shader_map.tsv').write_text('p\ti\tstage\tkey\ts\ncs\t0\tcompute\tk\tx\n')
        (self.lib / 'k_b.json').write_text(json.dumps({'chain': CHAIN, 'stage': 'cs', 'entry': 'main0'})); (self.lib / 'k_b.metallib').write_bytes(metallib([('main0', False)]))
        self.assertEqual(self.main('--gpu', 'g18p'), 0)
        self.assertEqual(len(self.calls), 1)
        self.assertTrue((self.out / 'applegpu_g18p.mtl4a').is_file())
        self.assertEqual((self.out / 'rest_pairs.tsv').read_text(), 'v\tc\n')

    def test_only_changed_batch_is_recompiled(self):
        bases = [('v_bp', f'vertex{i}', 'f_b', 'main0') for i in range(5)]
        runner, observed = self.batched_runner()
        with mock.patch.object(self, 'fake_run', runner), mock.patch.object(po, 'plan', return_value=(bases, [], {})):
            self.main('--batch-size','2','--os-min','27.0')
            observed.clear(); bases[0] = ('v_bp','changed','f_b','main0')
            self.main('--batch-size','2','--os-min','27.0')
        self.assertEqual(observed, [('compile',['changed','vertex1']),('merge',3)])

    def test_low_disk_space_stops_before_metal_tt(self):   # Cruis'n Blast: a full volume made metal-tt fail with only "applegpu-nt command failed"
        self.work.mkdir(parents=True); (self.work / 'translate-0001').write_bytes(b'x'); (self.work / 'a.airp').write_bytes(b'x')
        with mock.patch.object(po.shutil, 'disk_usage', return_value=shutil._ntuple_diskusage(1 << 40, 1 << 40, 300 << 20)), mock.patch.object(po.vol_space, 'important_bytes', lambda p: None), \
                self.assertRaises(SystemExit) as e: self.main()
        self.assertIn('GB free', str(e.exception)); self.assertEqual(self.calls, [])
        self.assertEqual(sorted(p.name for p in self.work.iterdir()), ['bases.mtl4-json'])   # a failed run's temporaries cleared

    def failed_run(self, free_with_temporaries):   # metal-tt passes the check with plenty free, then fails having written its temporaries, which take the free space down until they are cleared; the error text and what is left in the work folder
        def fail(cmd, **kw):
            cmd = [str(c) for c in cmd]
            if '-version' in cmd or '--show-sdk-version' in cmd: return self.fake_run(cmd)
            (self.work / 'translate-0001').mkdir(); (self.work / 'translate-0001/x').write_bytes(b'x'); (self.work / 'a.airp').write_bytes(b'x')
            return subprocess.CompletedProcess(cmd, 1)
        usage = lambda _: shutil._ntuple_diskusage(1 << 40, 1 << 40, free_with_temporaries if (self.work / 'a.airp').exists() else 100 << 30)
        with mock.patch.object(po.subprocess, 'run', fail), mock.patch.object(po.shutil, 'disk_usage', usage), \
                mock.patch.dict(os.environ, DM_GAMES_DIR=str(self.prof.parent)), contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as e:
            po.main(['--game', '0100000000001000', '--msl', str(self.lib), '--out', str(self.out), '--work', str(self.work)])
        return str(e.exception), {p.name for p in self.work.iterdir()}

    def test_metal_tt_failure_on_a_full_volume_says_so(self):   # the free space as it failed, not after its temporaries are cleared: clearing them brings it back above the estimate
        text, left = self.failed_run(100 << 20)
        self.assertIn('0.1 GB free', text); self.assertIn('likely out of disk space', text)
        self.assertFalse({'translate-0001', 'a.airp'} & left)

    def test_metal_tt_failure_with_room_does_not_blame_the_disk(self):
        text, left = self.failed_run(10 << 30)
        self.assertNotIn('out of disk space', text); self.assertIn('metal-tt failed', text)
        self.assertFalse({'translate-0001', 'a.airp'} & left)


class Prewarm(BuildCase):
    """--prewarm-host: the host's prebuild tool makes the prewarm lists' R and M rows here (a fake host below, writing what the bundle form writes), from the profile's tables without the
    pair lists; metal-tt compiles each period's script for each --gpu as <arch>-prewarm-<NN>.mtl4a beside the period's index; both cached; a failure leaves the archive as it is"""
    MAC = 'applegpu_g16g'

    def setUp(self):
        super().setUp()
        (self.prof / 'shader_map.tsv').write_text('# map\n')
        (self.prof / 'c1_full.tsv').write_text('c1\n')
        (self.prof / 'pso_manifest.tsv').write_text('# list\nS\tv\t0\nR\tr1\nC\tc_b\nM\t-\tm1\nR\tr1\nR\tr2\n')
        self.host = self.t / 'dmhost'; self.host.write_bytes(b'host 1')
        self.vbp = hashlib.sha256((self.lib / 'v_bp.metallib').read_bytes()).hexdigest().upper()
        self.host_runs, self.tt, self.periods, self.host_exit, self.tt_fail = [], [], 2, 0, ()

    def fake_run(self, cmd, **kw):
        cmd = [str(c) for c in cmd]
        if cmd == ['sysctl', '-n', 'kern.osversion']: return subprocess.CompletedProcess(cmd, 0, '25A1\n', '')
        if cmd == [str(self.host)]:   # the bundle form: per period an archive for this Mac, its index and its script; the stamp
            env = kw['env']; self.host_runs.append(env); made = Path(env['DM_PSO_PREBUILD_OUT']); made.mkdir(parents=True, exist_ok=True)
            for k in range(self.periods if not self.host_exit else 0):
                (made / f'{self.MAC}-prewarm-{k:02d}.mtl4a').write_bytes(b'mac'); (made / f'{self.MAC}-prewarm-{k:02d}.keys').write_bytes(b'DMPSOIX2' + bytes([k]))
                rp = [{'vertex_function_descriptor': f'fnd:F{k}{i}', 'label': f'p{k}{i}'} for i in range(3)]
                (made / f'prewarm-{k:02d}.mtl4-json').write_text(json.dumps({'libraries': [{'label': self.vbp, 'path': '_Path_not_available_for_lib_from_data_with_UUID_' + self.vbp}],
                    'function_descriptors': {'library_function_descriptors': [{'label': f'F{k}{i}', 'name': 'main0', 'library': self.vbp} for i in range(3)]},
                    'pipeline_descriptors': {'render_pipeline_descriptors': rp, 'mesh_render_pipeline_descriptors': [{'mesh_function_descriptor': f'fnd:F{k}0', 'label': f'm{k}'}]}}))
            if not self.host_exit: (made / 'prewarm.tsv').write_text('chain\tX\n')
            print('[dm] [nvn-mtl] pipeline prebuild: the prewarm lists\' 3 lines in 1.0 s: 2 render pipelines', file=kw['stdout'])
            return subprocess.CompletedProcess(cmd, self.host_exit)
        if 'metal-tt' in cmd and '-ios-version-min' in cmd and 'pipelines.mtl4-json' in cmd[8]:
            d = json.loads(Path(cmd[8]).read_text()); labels = [x['label'] for k in po.PIPELINE_KINDS for x in d['pipeline_descriptors'].get(k, [])]
            self.tt.append((cmd[5], labels)); self.assertTrue(Path(d['libraries'][0]['path']).is_file())   # the library found by its bytes
            if set(labels) & set(self.tt_fail): return subprocess.CompletedProcess(cmd, 1)
        if 'metal-pack' in cmd: self.tt.append(('pack', len(cmd) - 8)); Path(cmd[cmd.index('-o') + 1]).write_bytes(b'packed'); return subprocess.CompletedProcess(cmd, 0)
        return super().fake_run(cmd, **kw)

    def main(self, *extra):
        return super().main('--os-min', '27.0', *extra)

    def test_rows(self):
        self.assertEqual(po.prewarm_rows([self.prof / 'pso_manifest.tsv', self.t / 'none.tsv']), 'R\tr1\nM\t-\tm1\nR\tr2\n')

    def test_cached_publication_preserves_ipa_reuse_metadata(self):
        for copy_only in (False, True):
            with self.subTest(copy_only=copy_only), contextlib.ExitStack() as stack:
                if copy_only: stack.enter_context(mock.patch.object(po.os, 'link', side_effect=OSError('no hard links')))
                self.main('--prewarm-host', str(self.host))
                def state():
                    return {p.name: (p.stat().st_size, p.stat().st_mtime_ns, p.stat().st_ctime_ns, p.stat().st_ino) for p in self.out.iterdir()}
                before = state()
                self.main('--prewarm-host', str(self.host))
                self.assertEqual(state(), before)
            shutil.rmtree(self.out)

    def test_prewarm_only_bundle_keeps_cached_files_without_pair_archives(self):
        (self.prof / 'shader_pairs.tsv').write_text('# no pulling bases\n')
        self.main('--no-compute', '--prewarm-host', str(self.host))
        archive = self.out / 'applegpu_g18p-prewarm-00.mtl4a'; before = archive.stat()
        self.main('--no-compute', '--prewarm-host', str(self.host))
        self.assertEqual((archive.stat().st_ino, archive.stat().st_ctime_ns), (before.st_ino, before.st_ctime_ns))

    def test_files_beside_the_archive_and_reuse(self):
        with mock.patch.dict(os.environ, DM_SAVE='/x/save', DM_PSO_PREBUILD_THREADS='3'):
            self.assertEqual(self.main('--gpu', 'g18p,g17p', '--prewarm-host', str(self.host)), 0)
        env = self.host_runs[0]
        self.assertEqual((env['DM_PSO_PREBUILD'], env['DM_PSO_BUNDLE'], env['DM_PSO_PREBUILD_DEPTH'], env['DM_MSL_DIR']), ('tool', 'off', 'zero', str(self.t)))
        self.assertEqual(Path(env['DM_PSO_MANIFEST']).read_text(), 'R\tr1\nM\t-\tm1\nR\tr2\n')   # the M rows' mesh pipelines too
        self.assertEqual(sorted(p.name for p in Path(env['DM_NVN_SHMAP']).parent.iterdir()), ['c1_full.tsv', 'shader_map.tsv'])   # no pair list: those are the archive's
        self.assertFalse(any(k in env for k in ('DM_SAVE', 'DM_PSO_USER')))
        self.assertEqual(env.get('DM_PSO_PREBUILD_THREADS'), '3')   # passed on: fewer workers on a small Mac
        names = sorted(p.name for p in self.out.iterdir())
        for arch in ('applegpu_g17p', 'applegpu_g18p'):
            for k in range(2):
                self.assertIn(f'{arch}-prewarm-{k:02d}.mtl4a', names)
                self.assertEqual((self.out / f'{arch}-prewarm-{k:02d}.keys').read_bytes(), b'DMPSOIX2' + bytes([k]))   # this Mac's index of the same period
        self.assertEqual((self.out / 'applegpu_g18p-prewarm-01.mtl4a').read_bytes(), b'archive applegpu_g18p')
        self.assertIn('applegpu_g18p.mtl4a', names); self.assertIn('prewarm.tsv', names)
        self.assertEqual(sorted(a for a, _ in self.tt), ['applegpu_g17p'] * 2 + ['applegpu_g18p'] * 2)
        self.assertEqual(self.tt[0][1], ['p00', 'p01', 'p02', 'm0'])
        self.assertIn('prewarm\t4 files', (self.out / 'archive.tsv').read_text())
        self.host_runs.clear(); self.tt.clear()
        self.main('--gpu', 'g18p,g17p', '--prewarm-host', str(self.host))
        self.assertEqual((self.host_runs, self.tt), ([], []))   # inputs unchanged: nothing runs
        self.main('--gpu', 'g18p', '--prewarm-host', str(self.host))
        self.assertEqual((self.host_runs, self.tt), ([], []))
        self.assertFalse(list(self.out.glob('applegpu_g17p*')))   # a GPU no longer asked for
        (self.prof / 'pso_manifest.tsv').write_text('R\tr3\n')
        self.main('--gpu', 'g18p', '--prewarm-host', str(self.host))
        self.assertEqual((len(self.host_runs), len(self.tt)), (1, 0))   # new rows: the host runs again; its scripts are the same bytes here, so metal-tt has nothing new
        self.main('--gpu', 'g18p')
        self.assertFalse(list(self.out.glob('*-prewarm-*')) + list(self.out.glob('prewarm.tsv')))   # without a host: none, not stale ones
        self.assertTrue((self.out / 'applegpu_g18p.mtl4a').is_file())

    def test_groups_and_a_pipeline_metal_tt_cannot_compile(self):
        self.periods, self.tt_fail = 1, ('p01',)
        self.assertEqual(self.main('--batch-size', '2', '--prewarm-host', str(self.host)), 0)
        compiled = [x for x in self.tt if x[0] != 'pack']
        self.assertEqual(compiled[0], ('applegpu_g18p', ['p00', 'p01']))
        self.assertIn(('applegpu_g18p', ['p00']), compiled); self.assertIn(('applegpu_g18p', ['p02', 'm0']), compiled)
        self.assertEqual(self.tt[-1], ('pack', 2))
        self.assertEqual((self.out / 'applegpu_g18p-prewarm-00.mtl4a').read_bytes(), b'packed')
        self.assertIn('1 pipelines left out', (self.out / 'archive.tsv').read_text())
        self.assertFalse((self.out / 'applegpu_g18p-prewarm-00.keys').exists())   # a complete index must not claim the omitted pipeline, shadowing its device capture
        self.tt.clear()
        self.main('--batch-size', '2', '--prewarm-host', str(self.host))
        self.assertEqual(self.tt, [])
        self.assertFalse((self.out / 'applegpu_g18p-prewarm-00.keys').exists())   # the cached partial archive has the same contract

    def test_library_changes_invalidate_host_cache_without_a_new_stamp(self):
        args = (self.host, 'R\tr1\n', [self.prof / 'shader_map.tsv'], self.lib, False, 'OS')
        key = po.prewarm_host_key(*args)
        lib = self.lib / 'v_bp.metallib'; st = lib.stat()
        lib.write_bytes(metallib([('main0', False), ('changed', False)]))
        os.utime(lib, ns=(st.st_atime_ns, st.st_mtime_ns))
        self.assertNotEqual(po.prewarm_host_key(*args), key)

    def test_relative_host_and_child_paths(self):
        with contextlib.chdir(self.t):
            self.assertEqual(self.main('--msl', self.lib.name, '--out', 'out', '--work', 'work', '--prewarm-host', 'dmhost'), 0)
        env = self.host_runs[0]
        for name in ('DM_PSO_ARCHIVE', 'DM_PSO_PREBUILD_OUT', 'DM_PSO_MANIFEST', 'DM_MSL_DIR', 'DM_NVN_SHMAP'):
            self.assertTrue(Path(env[name]).is_absolute(), name)

    def test_host_launch_error_keeps_pair_archive(self):
        runner = self.fake_run
        def fail(cmd, **kw):
            if list(map(str, cmd)) == [str(self.host)]: raise OSError('host cannot execute')
            return runner(cmd, **kw)
        with mock.patch.object(self, 'fake_run', fail):
            self.assertEqual(self.main('--prewarm-host', str(self.host)), 0)
        self.assertTrue((self.out / 'applegpu_g18p.mtl4a').is_file())
        self.assertFalse(list(self.out.glob('*-prewarm-*')))
        self.assertIn('prewarm\tnone (failed)', (self.out / 'archive.tsv').read_text())

    def test_pair_fast_path_still_builds_refreshes_repairs_and_removes_prewarm(self):
        po.shader_lib.pack(str(self.lib))
        self.main()   # populate the pair archive's image fast path without a prewarm host
        with mock.patch.object(po, 'plan', side_effect=AssertionError('pair archive must stay on its fast path')):
            self.main('--prewarm-host', str(self.host))
            self.assertEqual(len(self.host_runs), 1)
            self.host_runs.clear(); self.tt.clear()
            (self.prof / 'pso_manifest.tsv').write_text('C\tc_b\nR\tr3\nM\t-\tm2\n')
            self.main('--prewarm-host', str(self.host))
            self.assertEqual(len(self.host_runs), 1)
            archive = self.out / 'applegpu_g18p-prewarm-00.mtl4a'; archive.unlink()
            self.main('--prewarm-host', str(self.host))
            self.assertTrue(archive.is_file())
            self.assertEqual(self.tt, [])   # repaired from the prewarm cache
            self.main()
            self.assertFalse(list(self.out.glob('*-prewarm-*')) + list(self.out.glob('prewarm.tsv')))

    def test_mac_pair_step_preserves_separately_baked_prewarm(self):
        self.main('--platform', 'macos')
        archive = self.out / 'applegpu_g18p-prewarm-00.mtl4a'; archive.write_bytes(b'mac prewarm')
        keys = archive.with_suffix('.keys'); keys.write_bytes(b'index')
        (self.out / 'prewarm.tsv').write_text('mac stamp\n')
        self.main('--platform', 'macos')
        self.assertEqual(archive.read_bytes(), b'mac prewarm')
        self.assertEqual(keys.read_bytes(), b'index')

    def test_a_failed_host_leaves_the_archive(self):
        self.out.mkdir(); (self.out / 'applegpu_g18p-prewarm-07.mtl4a').write_bytes(b'old')
        self.host_exit = 1
        out = io.StringIO()
        with mock.patch.object(po.subprocess, 'run', self.fake_run), mock.patch.dict(os.environ, DM_GAMES_DIR=str(self.prof.parent)), contextlib.redirect_stdout(out):
            r = po.main(['--game', '0100000000001000', '--msl', str(self.lib), '--out', str(self.out), '--work', str(self.work), '--os-min', '27.0', '--prewarm-host', str(self.host)])
        self.assertEqual(r, 0)
        self.assertIn('[RED] pso_offline: prewarm: the host\'s prebuild tool made 0 files', out.getvalue())
        self.assertEqual(sorted(p.name for p in self.out.glob('*.mtl4a')), ['applegpu_g18p.mtl4a'])
        self.assertIn('prewarm\tnone (failed)', (self.out / 'archive.tsv').read_text())
        self.host_exit = 0; self.host_runs.clear()
        self.main('--prewarm-host', str(self.host))
        self.assertEqual(len(self.host_runs), 1)   # nothing kept from the failed run
        self.assertTrue((self.out / 'applegpu_g18p-prewarm-01.mtl4a').is_file())

    @unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode and Metal')
    def test_metal_tt_takes_a_mesh_pipeline(self):   # the M rows: a native mesh pipeline descriptor, as the host's script writes it, compiles for an iPhone's GPU
        src = ('#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\nusing M = metal::mesh<V, void, 3, 1, topology::triangle>;\n'
               '[[mesh]] void m0(M out, uint t [[thread_index_in_threadgroup]]) { if (t == 0) { out.set_primitive_count(1); for (uint i = 0; i < 3; i++) { V v; '
               'v.p = float4(float(i), 0, 0, 1); out.set_vertex(i, v); out.set_index(i, i); } } }\nfragment float4 f0() { return float4(1); }\n')
        (self.t / 'm.metal').write_text(src)
        r = subprocess.run(['xcrun', '-sdk', 'iphoneos', 'metal', '-c', str(self.t / 'm.metal'), '-o', str(self.t / 'm.air')], capture_output=True, text=True)
        if r.returncode: self.skipTest('no Metal compiler: ' + r.stderr[-200:])
        subprocess.run(['xcrun', '-sdk', 'iphoneos', 'metallib', str(self.t / 'm.air'), '-o', str(self.t / 'm.metallib')], check=True, capture_output=True)
        if subprocess.run(['xcrun', '-sdk', 'iphoneos', 'metal-tt', '-version'], capture_output=True).returncode: self.skipTest('no metal-tt')
        h = hashlib.sha256((self.t / 'm.metallib').read_bytes()).hexdigest().upper()
        cfg = {'libraries': [{'label': h, 'path': str(self.t / 'm.metallib')}],
               'function_descriptors': {'library_function_descriptors': [{'label': 'M', 'name': 'm0', 'library': h}, {'label': 'F', 'name': 'f0', 'library': h}]},
               'pipeline_descriptors': {'mesh_render_pipeline_descriptors': [{'mesh_function_descriptor': 'fnd:M', 'fragment_function_descriptor': 'fnd:F',
                                                                              'max_total_threads_per_mesh_threadgroup': 32, 'color_attachments': [{'pixel_format': 'RGBA8Unorm'}]}]}}
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(po.compile_script(cfg, self.t / 'm.mtl4a', self.t / 'mw', 'applegpu_g18p', 'ios', '27.0', 1000), [])
        self.assertGreater((self.t / 'm.mtl4a').stat().st_size, 0)

    def test_a_missing_host_is_a_warning(self):
        out = io.StringIO()
        with mock.patch.object(po.subprocess, 'run', self.fake_run), mock.patch.dict(os.environ, DM_GAMES_DIR=str(self.prof.parent)), contextlib.redirect_stdout(out):
            self.assertEqual(po.main(['--game', '0100000000001000', '--msl', str(self.lib), '--out', str(self.out), '--work', str(self.work), '--os-min', '27.0',
                                      '--prewarm-host', str(self.t / 'nothing')]), 0)
        self.assertIn('[RED] pso_offline: prewarm: no host at', out.getvalue())


class IpaFallback(unittest.TestCase):
    """build_ipa.sh's own archive step with a pso_offline.py that fails: a [RED] line, no archive folder, and the build goes on (an optimization: the device builds the bases)"""

    def step(self, python, host_game=None, make_exit=0, selected_python=None, **extra):
        lines = (REPO / 'dm/ios/tools/build_ipa.sh').read_text().splitlines()
        i = next(n for n, ln in enumerate(lines) if ln.startswith('if [ "$GFXAPI" = nvn ] && [ -n "$GPUS" ]'))
        j = next(n for n, ln in enumerate(lines) if n > i and ln.startswith('else [ "$GFXAPI" != nvn ]'))
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'bin').mkdir(); stub = Path(d) / 'bin/python3'; stub.write_text('#!/bin/sh\n' + python); stub.chmod(0o755)
            (Path(d) / 'shader_pairs.tsv').write_text('v\tf\n')
            if host_game:
                host = Path(d) / f'dm/host/gen/{host_game}/dmhost'; host.parent.mkdir(parents=True); host.write_text('#!/bin/sh\nexit 0\n'); host.chmod(0o755)
                make = Path(d) / 'bin/make'; make.write_text('#!/bin/sh\nprintf "%s\\n" "$*" > "$W/make.args"\nexit ' + str(make_exit) + '\n'); make.chmod(0o755)
            env = dict({k: v for k, v in os.environ.items() if k not in ('DM_PSO_HOST', 'DM_PYTHON')}, PATH=f'{d}/bin:{os.environ["PATH"]}', W=d, GD=d, GAME='G', DM='dm', MSL_IOS='m', V='0', GPUS='g18p', GFXAPI='nvn', **extra)
            if selected_python is not None:
                selected = Path(d) / 'selected python'
                selected.write_text('#!/bin/sh\n' + selected_python)
                selected.chmod(0o755)
                env['DM_PYTHON'] = str(selected)
            if host_game: env['DM'] = str(Path(d) / 'dm')
            script = 'die() { echo "FAIL: $*"; exit 1; }\nt() { echo "$*"; }\nDM_PSO_IOS_MIN=\n' + '\n'.join(lines[i:j]) + '\nfi\necho "PSO=[$PSO]"\n[ -d "$W/build/ios/pso/bundle" ] && echo folder\n[ ! -f "$W/make.args" ] || cat "$W/make.args"\necho went on\n'
            return subprocess.run(['bash', '-c', 'set -u\n' + script], env=env, capture_output=True, text=True).stdout

    def test_failed_archive_is_a_warning(self):
        out = self.step('mkdir -p "$W/build/ios/pso/bundle"; echo half; exit 1\n')
        self.assertIn('[RED] pipeline archive: pso_offline.py failed', out); self.assertIn('PSO=[]', out); self.assertNotIn('folder', out); self.assertIn('went on', out)
        self.assertNotIn('FAIL', out)

    def test_archive_uses_the_packagers_python(self):
        out = self.step('echo system Python; exit 1\n',
                        selected_python='echo selected Python; mkdir -p "$W/build/ios/pso/bundle"\n')
        self.assertIn('selected Python', out)
        self.assertIn('folder', out)
        self.assertNotIn('system Python', out)

    def test_prewarm_host(self):   # DM_PSO_HOST names the Mac host the prewarm files are made with; none, or no host of this game in the tree: none, with a line saying how
        out = self.step('echo "ARGS $*"; exit 0\n', DM_PSO_HOST='/x/dmhost')
        self.assertIn('--prewarm-host /x/dmhost', out); self.assertNotIn('no Mac host', out)
        for setting in ('none', None):
            out = self.step('echo "ARGS $*"; exit 0\n', DM_PSO_HOST=setting) if setting else self.step('echo "ARGS $*"; exit 0\n')
            self.assertNotIn('--prewarm-host', out); self.assertIn('prewarm pipelines: no Mac host of this game', out); self.assertNotIn('FAIL', out)

    def test_made_archive_goes_into_the_app(self):
        out = self.step('mkdir -p "$W/build/ios/pso/bundle"; exit 0\n')
        self.assertNotIn('[RED]', out); self.assertIn('/build/ios/pso/bundle]', out); self.assertIn('folder', out)

    def test_current_per_game_host_is_discovered_without_the_old_stamp(self):
        out = self.step('echo "ARGS $*"; exit 0\n', host_game='G')
        self.assertIn('/dm/host/gen/G/dmhost', out); self.assertIn('GAME=G gen/G/dmhost', out)
        self.assertNotIn('no Mac host', out)
        out = self.step('echo "ARGS $*"; exit 0\n', host_game='other')
        self.assertNotIn('--prewarm-host', out); self.assertIn('no Mac host', out)
        out = self.step('mkdir -p "$W/build/ios/pso/bundle"; exit 0\n', host_game='G', make_exit=1)
        self.assertIn('prewarm pipelines: make', out); self.assertNotIn('--prewarm-host', out); self.assertIn('folder', out)


class Default(unittest.TestCase):
    """Which games get an archive without DM_PSO_GPUS: build_ipa.sh's own lines, run with game.py's default form, so only a profile with pso_mtl4 on (the Metal 4 path, which alone
    reads the archive on iOS) gets one — g18p by default, the profile key pso_gpu's architectures when it names them (joined with commas) and g18p on a key the shell guard does not take —
    and an explicit DM_PSO_GPUS wins, also when empty"""

    def lines(self):
        lines = (REPO / 'dm/ios/tools/build_ipa.sh').read_text().splitlines()
        i = next(n for n, ln in enumerate(lines) if ln.startswith('PSO=; P4='))
        return 'die() { echo "FAIL: $*"; exit 1; }\nt() { echo "$*" >&2; }\n' + '\n'.join(lines[i:i + 4])   # the P4 / pso_gpu reads, the key's shell guard and the GPUS chain

    def gpus(self, profile, **env):
        with tempfile.TemporaryDirectory() as d:
            for k, v in profile.items(): Path(d, k).mkdir(); Path(d, k, 'game.tsv').write_text(v)
            e = {k: v for k, v in os.environ.items() if k != 'DM_PSO_GPUS'}
            e.update(env, DM=str(REPO / 'dm'), DM_GAMES_DIR=d, GD=d)
            return {gid: (r.stdout, r.stderr) for gid, r in
                    ((gid, subprocess.run(['bash', '-c', self.lines() + '\necho "$P4 $GPUS"'], env=dict(e, GAME=gid),
                                          capture_output=True, text=True)) for gid in profile)}

    def test_by_profile(self):
        games = {'0100000000F0F000': 'title_id\t0100000000F0F000\npso_mtl4\ton\n',
                 '0100000000F0F001': 'title_id\t0100000000F0F001\n',
                 '0100000000F0F002': 'title_id\t0100000000F0F002\npso_mtl4\toff\n',
                 '0100000000F0F003': 'title_id\t0100000000F0F003\npso_mtl4\ton\npso_gpu\tapplegpu_g19p\n',
                 '0100000000F0F004': 'title_id\t0100000000F0F004\npso_mtl4\ton\npso_gpu\tg18p   g19p\n',
                 '0100000000F0F005': 'title_id\t0100000000F0F005\npso_mtl4\ton\npso_gpu\tnone\n',                                   # none is no token of this key
                 '0100000000F0F006': 'title_id\t0100000000F0F006\npso_mtl4\ton\npso_gpu\tg15s g16s g17p g18p g19p\n',               # 5 architectures: more than the key takes
                 '0100000000F0F007': 'title_id\t0100000000F0F007\npso_mtl4\toff\npso_gpu\tg19p\n'}
        r = self.gpus(games)
        self.assertEqual({k: p[0].strip() for k, p in r.items()},
                         {'0100000000F0F000': 'on g18p', '0100000000F0F001': 'off none', '0100000000F0F002': 'off none',
                          '0100000000F0F003': 'on applegpu_g19p', '0100000000F0F004': 'on g18p,g19p', '0100000000F0F005': 'on g18p',
                          '0100000000F0F006': 'on g18p', '0100000000F0F007': 'off none'})
        self.assertEqual({k for k, p in r.items() if 'taking g18p' in p[1]}, {'0100000000F0F005', '0100000000F0F006'})   # a key the guard rejects keeps the default, and says so
        self.assertEqual({k: p[0].strip() for k, p in self.gpus(games, DM_PSO_GPUS='g17p').items()},
                         {k: v.split()[0] + ' g17p' for k, v in {k: p[0].strip() for k, p in r.items()}.items()})
        self.assertEqual(self.gpus(games, DM_PSO_GPUS='none')['0100000000F0F000'][0].strip(), 'on none')
        self.assertEqual(self.gpus(games, DM_PSO_GPUS='')['0100000000F0F000'][0].strip(), 'on')

    def test_nsp_defaults_follow_the_same_profile_gate(self):
        # Run the NSP script's actual choice block without extracting or building a game.
        script = (REPO / 'scripts/build_ipa_from_nsp.py').read_text()
        start = script.index("    gpus = (args.gpu or PSO_GPUS).strip()")
        end = script.index("    ipa_env['DM_PSO_GPUS'] = gpus", start)
        block = '\n'.join(line[4:] for line in script[start:end].splitlines())
        block = script[script.index('def ios_gpus('):script.index('def ios_wants_pulling(')] + block   # the choice itself, which the shader step shares
        for p4, pg, expected in [('on', '', 'g18p'), ('on', 'g18p   applegpu_g19p', 'g18p,applegpu_g19p'),
                                 ('off', '', 'none'), ('off', 'g19p', 'none')]:
            for explicit, setting in [('', ''), ('g17p', ''), ('none', ''), ('', 'g16p')]:
                with self.subTest(pso_mtl4=p4, pso_gpu=pg, gpu=explicit, setting=setting):
                    scope = {'args': types.SimpleNamespace(gpu=explicit), 'PSO_GPUS': setting,
                             'g': types.SimpleNamespace(get={'pso_mtl4': p4, 'pso_gpu': pg}.get),
                             're': re, 'BuildError': RuntimeError}
                    exec(block, scope)
                    self.assertEqual(scope['gpus'], explicit or setting or expected)

    def test_breath_of_the_wild(self):   # the profile that turns the Metal 4 path on: its app gets the archive
        r = subprocess.run([sys.executable, str(REPO / 'dm/games/game.py'), '01007EF00011E000', 'pso_mtl4', 'off'], capture_output=True, text=True)
        self.assertEqual(r.stdout.strip(), 'on')


if __name__ == '__main__':
    unittest.main()
