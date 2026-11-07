"""Both shader-pair tables must feed vertex padding, its cache dependencies and every vertex variant (#283, #276)."""
import ast
import collections
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import types
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS.parents[1] / 'tools'))
import dmsh_chain
import dmsh_pull
import pso_offline
import shader_cache


def builder():
    # Use production functions without the CLI's translator/profile initialization.
    names = {'load_pairs', 'partner_inputs', 'vs_pad', 'shader_key', 'pair_keyed', 'output', 'label_dir'}
    tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
    import dmsh_entry
    ns = dict(collections=collections, os=os, re=re, sys=sys, hashlib=hashlib, vs_depth=dmsh_entry.vs_depth)
    exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names],
                            type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), ns)
    return ns


VS = '''#include <metal_stdlib>
using namespace metal;
struct main0_out
{
    float4 gl_Position [[position]];
    float4 v0 [[user(locn0)]];
};
struct main0_in
{
    float4 m_0 [[attribute(0)]];
};
vertex main0_out main0(main0_in in [[stage_in]], uint vid [[vertex_id]])
{
    main0_out out = {};
    out.gl_Position = float4(float2((vid << 1) & 2, vid & 2) * 2.0 - 1.0, 0.0, 1.0);
    out.v0 = in.m_0;
    return out;
}
'''


def pixel(loc):
    return f'''#include <metal_stdlib>
using namespace metal;
struct main0_in
{{
    float4 v0 [[user(locn0)]];
    float4 missing [[user(locn{loc})]];
}};
fragment float4 main0(main0_in in [[stage_in]]) {{ return in.v0 + in.missing; }}
'''


class Pairing(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.profile = self.root / 'profile'; self.profile.mkdir()
        self.package = self.root / 'shader/pkg'; self.package.mkdir(parents=True)
        self.g = types.SimpleNamespace(table=lambda n: self.profile / n, rows=[])
        self.g.table('shader_pairs.tsv').write_text('# BNSH\nv\tf0\n')
        self.extra = self.package.parent / 'pso_pairs.tsv'
        self.extra.write_text('# BAHS\nv\tf1\nv\tf6\nv\tf1\n\n')
        self.ns = builder()
        self.store = shader_cache.Store(self.root / 'cas.db')
        self.addCleanup(self.store.close)
        self.ns.update(out=str(self.root), PAIRS=self.ns['load_pairs'](self.g, self.package),
                       STORE=self.store, STAGE='stage/test', want=set())
        for key, loc in [('f0', 0), ('f1', 1), ('f6', 6)]:
            src = pixel(loc)
            if not loc: src = src.replace('    float4 missing [[user(locn0)]];\n', '').replace('in.v0 + in.missing', 'in.v0')
            (self.root / f'{key}_b.metal').write_text(src)

    def test_split_fallback_union_and_defaults(self):
        self.assertEqual(self.ns['PAIRS'], {'v': {'f0', 'f1', 'f6'}})
        src, pad = self.ns['vs_pad']('v', VS)
        self.assertEqual(pad, [1, 6])
        for loc in pad:
            self.assertIn(f'float4 dm_pad{loc} [[user(locn{loc})]];', src)
            self.assertIn(f'out.dm_pad{loc} = float4(0.0, 0.0, 0.0, 1.0);', src)
        self.assertEqual(src.count('float4 v0 [[user(locn0)]];'), 1)
        self.assertEqual(self.ns['vs_pad']('v', src), (src, []))

    def test_padding_reads_staged_pixel_outputs(self):
        for path in self.root.glob('*_b.metal'):
            self.store.put(f'stage/test/mac/{path.name}', path.read_bytes())
            path.unlink()
        self.test_split_fallback_union_and_defaults()

    def test_profile_table_wins_and_optional_table(self):
        self.g.table('pso_pairs.tsv').write_text('v\tf6\n')
        self.assertEqual(self.ns['load_pairs'](self.g, self.package), {'v': {'f0', 'f6'}})
        self.g.table('pso_pairs.tsv').unlink(); self.extra.unlink()
        self.assertEqual(self.ns['load_pairs'](self.g, self.package), {'v': {'f0'}})
        self.assertEqual(self.ns['vs_pad']('unpaired', VS), (VS, []))

    def test_profile_input_stamp_changes(self):
        missing = dmsh_chain.inputs(self.g)
        self.g.table('pso_pairs.tsv').write_text('v\tf6\n')
        present = dmsh_chain.inputs(self.g)
        self.g.table('pso_pairs.tsv').write_text('v\tf1\n')
        self.assertNotEqual(missing, present)
        self.assertNotEqual(present, dmsh_chain.inputs(self.g))

    def test_vertex_cache_depends_on_extra_pairs_and_pixel_code(self):
        self.ns.update(FACTS={}, MAP_ROWS={}, MAP_CONFLICT_ROWS={}, TOOL_FP=b'tools', CODE_FP='code',
                       PROFILE_ROWS=b'', DUAL_WANT=set(), shader_rule_source=lambda name: b'')
        key = self.ns['shader_key']
        v1 = key('v', 'pkg.bin', b'vertex')
        self.ns['PAIRS']['v'].remove('f6')
        self.assertNotEqual(v1, key('v', 'pkg.bin', b'vertex'))
        f1, f2 = key('f1', 'pkg.bin', b'pixel'), key('f1', 'pkg.bin', b'changed pixel')
        self.assertNotEqual(self.ns['pair_keyed'](v1, [f1]), self.ns['pair_keyed'](v1, [f2]))
        self.ns['DUAL_WANT'].add('f1')
        dual = key('f1', 'pkg.bin', b'pixel')
        self.assertNotEqual(self.ns['pair_keyed'](v1, [f1]), self.ns['pair_keyed'](v1, [dual]))

    @unittest.skipUnless(shutil.which('xcrun'), 'needs the Metal compiler')
    def test_metal_archive_with_padded_regular_and_pulling_entries(self):
        probe = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal-tt', '-version'], capture_output=True)
        if probe.returncode: self.skipTest('no Metal toolchain')
        src, _ = self.ns['vs_pad']('v', VS)
        _, full = self.ns['vs_depth'](src)
        pull, meta = dmsh_pull.variant(full)
        self.assertIsNotNone(pull, meta)
        for name, text in [('v_b', full), ('v_bp', pull), ('f1_b', pixel(1)), ('f6_b', pixel(6))]:
            msl, air, lib = [self.root / (name + ext) for ext in ('.metal', '.air', '.metallib')]
            msl.write_text(text)
            for sdk in ('iphoneos', 'macosx'):
                r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-fpreserve-invariance', '-fmodules-cache-path=' + str(self.root / 'modules'), msl, '-o', air], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)
            # The archive below consumes the Mac libraries.
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', air, '-o', lib], check=True, capture_output=True)
        bases = [(v, entry, f, 'main0') for v in ('v_b', 'v_bp') for entry in ('main0', 'main0_z') for f in ('f1_b', 'f6_b')]
        plan = pso_offline.script(self.root, bases)
        for base, desc in zip(bases, plan['pipeline_descriptors']['render_pipeline_descriptors']):
            if base[0] == 'v_b':
                desc['vertex_descriptor'] = {'attributes': [{'format': 'Float4', 'offset': 0, 'buffer_index': 0}],
                                             'layouts': [{'stride': 16, 'step_function': 'PerVertex', 'step_rate': 1}]}
        script = self.root / 'bases.mtl4-json'; script.write_text(json.dumps(plan))
        archive = self.root / 'pairs.mtl4a'
        osmin = pso_offline.os_min_default('macos')
        r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal-tt', '-arch', 'applegpu_g13s', '-macos-version-min', osmin, script, '-o', archive],
                           env=dict(os.environ, TMPDIR=str(self.root)), capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(archive.is_file())


class VariationPairs(unittest.TestCase):
    def test_mesh_pairs_with_its_pixel_shader(self):   # NVN2: Ocarina of Time's foliage variations hold a mesh and a pixel shader, no vertex shader
        import shader_split as split
        sh = lambda tag: (bytes([tag]) * 0x40, bytes([tag]) * 0x40)
        found = [('a.bfsha#BNSH@0x10#var0', 'vs', *sh(1)), ('a.bfsha#BNSH@0x10#var0', 'ps', *sh(2)),
                 ('a.bfsha#BNSH@0x10#var1', 'ms', *sh(3)), ('a.bfsha#BNSH@0x10#var1', 'ps', *sh(4)),
                 ('a.bfsha#BNSH@0x10#var2', 'ms', *sh(5))]   # no pixel shader: no pair
        k = lambda tag: split.skey(*sh(tag))
        self.assertEqual(split.var_pairs(found), {(k(1), k(2)), (k(3), k(4))})


if __name__ == '__main__': unittest.main()
