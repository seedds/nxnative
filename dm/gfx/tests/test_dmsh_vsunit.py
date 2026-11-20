"""A vertex shader's libraries from one Metal compile (dmsh_side.vs_unit): small vertex shaders in the translator's MSL shape, made into the build's source as dmsh_build.py makes it
(dmsh_entry.vs_depth, dmsh_geometry.capture), no game needed. The unit must hold the body once as dm_vs_body, inlined into main0, main0_z and their pulling (main0_p, main0_zp) and
shadow-input (main0_v, main0_zv) versions, with one plain static copy for the wide-line entries and never a noinline; keep main0_gs and the rarer entry points whole copies; name
the side libraries' entry points as the reflection's `pull` and `vin` take them; refuse a source of another shape; compile for macOS and iOS and cut into <key>_b, <key>_bp and
<key>_bv with each library's own entry points, main0_gs's code byte for byte what the plain source compiles to (temporary files only in a temporary folder).
The build step (dmsh_build.py vs_libraries, run here with the Metal cache replaced): one compile a target, the reflection's names, a target without the pulling library, a
layer shader's shadow-input library, and each library compiled from its own source, with a red line, for a unit that does not build. t_vs_unit.m draws with the cut libraries on the GPU."""
import ast, collections, os, pathlib, shutil, subprocess, sys, tempfile, unittest, warnings
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_side, dmsh_pull, dmsh_vin, dmsh_entry, dmsh_geometry, pso_offline
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'ios/tools'))
import ios_metal   # noqa: E402  (the iPhone's deployment target and math mode)

BODY = 'float4 acc = u;'   # a line the body has once
CALL_GL = '    main0_out out = dm_vs_body(in, u);\n    ' + dmsh_side.VS_DEPTH + '\n    return out;\n}\n'
CALL_Z = '    return dm_vs_body(in, u);\n}\n'
TF = '\nvertex void main0_tf(main0_in in [[stage_in]], constant float4& u [[buffer(0)]], uint dm_vid [[vertex_id]], device uint* dm_tf0 [[buffer(8)]])\n{\n    dm_tf0[dm_vid] = as_type<uint>(in.m_0.x + u.x);\n}\n'
LAYER = ('\nstruct main0l_out\n{\n    float4 gl_Position [[position, invariant]];\n    uint gl_Layer [[render_target_array_index]];\n};\n'
         + ''.join(f'\nvertex main0l_out {n}(main0_in in [[stage_in]], constant float4& u [[buffer(0)]])\n{{\n    main0l_out out = {{}};\n    out.gl_Position = in.m_0 + u;\n    out.gl_Layer = 1u;\n    return out;\n}}\n'
                   for n in ('main0_l', 'main0_lz')))


def single(types, ids='', ret=True):
    """The translator's vertex shader, with main0 alone. types: the MSL type of each input in location order; ids: extra parameters of main0; ret: an interpolated output and an early
    return, so the body returns twice"""
    ins = ''.join(f'    {t} m_{l} [[attribute({l})]];\n' for l, t in enumerate(types))
    use = ''.join(f'    acc += float4(as_type<float4>(in.m_{l}));\n' if t != 'float4' else f'    acc += in.m_{l};\n' for l, t in enumerate(types))
    early = '    if (acc.x > 5.0)\n    {\n        return out;\n    }\n    out.c = acc;\n' if ret else ''
    outs = ('    float4 c [[user(locn0)]];\n' if ret else '') + '    float4 gl_Position [[position, invariant]];\n'
    return ('#include <metal_stdlib>\n#include <simd/simd.h>\n\nusing namespace metal;\n\nstruct main0_out\n{\n' + outs + '};\n\nstruct main0_in\n{\n' + ins.rstrip('\n') + '\n};\n'
            f'\nvertex main0_out main0(main0_in in [[stage_in]], constant float4& u [[buffer(0)]]{ids})\n{{\n    main0_out out = {{}};\n    float4 acc = u;\n{use}{early}    out.gl_Position = acc;\n    return out;\n}}\n')


def source(types, ids='', ret=True, tail=''):   # the source with every entry point, as dmsh_build.py's finish makes it: main0 with the depth line, main0_z, main0_gs, then the rarer ones
    src = single(types, ids, ret)
    return dmsh_entry.vs_depth(src)[1] + dmsh_geometry.capture(src) + tail


def unit(full, pull=True, vin=True):
    return dmsh_side.vs_unit(full, dmsh_pull.variant(full)[0] if pull else None, dmsh_vin.variant(full)[0] if vin else None)


class Unit(unittest.TestCase):
    def test_the_body_once(self):
        full = source(['float4', 'uint4', 'int4'])
        pv, pinfo = dmsh_pull.variant(full); vv, _ = dmsh_vin.variant(full)
        self.assertEqual((full.count(BODY), pv.count(BODY), vv.count(BODY)), (3, 4, 3))   # the three sources: main0, main0_z, main0_gs; the pulling pair and a body function for each wide-line entry; the shadow-input library's three
        u, pn, vn = dmsh_side.vs_unit(full, pv, vv)
        self.assertEqual(u.count(BODY), 2)   # dm_vs_body, and main0_gs's whole copy
        self.assertEqual(pn, {'entry': 'main0_p', 'entry_z': 'main0_zp', 'entry_wl': 'main0_wl', 'entry_z_wl': 'main0_z_wl'})
        self.assertEqual(vn, {'entry': 'main0_v', 'entry_z': 'main0_zv'})
        self.assertEqual(set(pn), {k for k in pinfo if k.startswith('entry')})   # the keys of the reflection's `pull` that name entry points
        self.assertEqual(u.count('static __attribute__((always_inline)) main0_out dm_vs_body(main0_in in, constant float4& u)\n{\n'), 1)
        self.assertEqual(u.count('static main0_out dm_vs_body_o(main0_in in, constant float4& u)\n{\n    return dm_vs_body(in, u);\n}\n'), 1)
        self.assertNotIn('noinline', u)   # a noinline body changed the capture entry's results when that entry shared it: the out-of-line copy is a plain static function
        es = dmsh_side.vs_entries(u)
        self.assertEqual(sorted(es), ['main0', 'main0_gs', 'main0_p', 'main0_v', 'main0_wl', 'main0_z', 'main0_z_wl', 'main0_zp', 'main0_zv'])
        body = lambda n: u[es[n]['body']:es[n]['end']]
        self.assertEqual(body('main0'), CALL_GL)   # the depth line once, after the call, where the copy had it before each of its two returns
        self.assertEqual(body('main0_z'), CALL_Z)
        self.assertEqual(es['main0']['params'], 'main0_in in [[stage_in]], constant float4& u [[buffer(0)]]')
        self.assertIn('    float4 m_0 [[attribute(0)]];\n    uint4 m_1 [[attribute(1)]];', u)   # main0_in keeps its attributes for them

    def test_main0_gs_and_the_rarer_entry_points_stay_whole_copies(self):
        full = source(['float4', 'uint4'], tail=TF + LAYER)
        u, pn, vn = unit(full)
        fe, ue = dmsh_side.vs_entries(full), dmsh_side.vs_entries(u)
        for n in ('main0_gs', 'main0_tf', 'main0_l', 'main0_lz'):
            self.assertEqual(u[ue[n]['start']:ue[n]['end']], full[fe[n]['start']:fe[n]['end']], n)
        self.assertEqual(u.count('{ dm_cw('), full.count('{ dm_cw('))   # the capture epilogue at each of the body's two returns, not once after a call
        self.assertEqual(full.count('{ dm_cw('), 2)
        self.assertFalse({'main0_gs', 'main0_tf', 'main0_l', 'main0_lz'} & (set(pn.values()) | set(vn.values())))   # they stay in <key>_b

    def test_pulling_entry_points(self):
        u, pn, _ = unit(source(['float4', 'uint4', 'int4']))
        es = dmsh_side.vs_entries(u)
        body = lambda n: u[es[n]['body']:es[n]['end']]
        for n in ('main0_p', 'main0_zp'):
            self.assertNotIn('[[stage_in]]', es[n]['params'])   # no stage input, although the unit's main0_in has attributes: a pipeline of it takes no vertex descriptor
            self.assertEqual(es[n]['params'], 'constant float4& u [[buffer(0)]], constant dm_vlayout& dm_lay [[buffer(16)]], uint dm_vid [[vertex_id]], uint dm_iid [[instance_id]], uint dm_bi [[base_instance]]')
            self.assertTrue(body(n).startswith('    main0_in in;\n    in.m_0 = as_type<float4>(dm_fetch(dm_lay, 0u, dm_vid, dm_iid, dm_bi));\n    in.m_1 = as_type<uint4>(dm_fetch(dm_lay, 1u, dm_vid, dm_iid, dm_bi));\n'))
        self.assertTrue(body('main0_p').endswith('dm_bi));\n' + CALL_GL)); self.assertTrue(body('main0_zp').endswith('dm_bi));\n' + CALL_Z))
        self.assertEqual(u.count('struct dm_vlayout'), 1); self.assertEqual(u.count('struct dm_wline'), 1)
        # each wide-line entry runs the body for both endpoints of its segment: through the out-of-line copy, which the two share
        self.assertIn('static inline main0_out dm_wlb_main0(main0_in in, constant float4& u, constant dm_vlayout& dm_lay, uint dm_vid, uint dm_iid, uint dm_bi)\n{\n'
                      '    main0_out out = dm_vs_body_o(in, u);\n    ' + dmsh_side.VS_DEPTH + '\n    return out;\n}\n', u)
        self.assertIn('static inline main0_out dm_wlb_main0_z(main0_in in, constant float4& u, constant dm_vlayout& dm_lay, uint dm_vid, uint dm_iid, uint dm_bi)\n{\n    return dm_vs_body_o(in, u);\n}\n', u)
        self.assertIn('main0_out out = dm_wlb_main0(dm_wl_in(dm_lay, dm_e.x, dm_iid, dm_bi), u, dm_lay, dm_e.x, dm_iid, dm_bi), dm_o = dm_wlb_main0(dm_wl_in(dm_lay, dm_e.y, dm_iid, dm_bi)', u)

    def test_shadow_input_entry_points(self):
        u, _, vn = unit(source(['float4', 'uint4', 'int4']))
        es = dmsh_side.vs_entries(u)
        body = lambda n: u[es[n]['body']:es[n]['end']]
        self.assertEqual(u.count('struct main0_in_v\n'), 1)
        for n in ('main0_v', 'main0_zv'):
            self.assertEqual(es[n]['params'], 'main0_in_v dm_in [[stage_in]], constant float4& u [[buffer(0)]]')
            self.assertTrue(body(n).startswith('    main0_in in = {};\n    in.m_0 = dm_in.m_0;\n    in.m_1 = dm_in.m_1 | as_type<uint4>(dm_in.m_1_vf) | as_type<uint4>(dm_in.m_1_vo);\n'))
        self.assertTrue(body('main0_v').endswith(';\n' + CALL_GL)); self.assertTrue(body('main0_zv').endswith(';\n' + CALL_Z))

    def test_the_ids_the_shader_declares_are_passed_on(self):
        u, _, _ = unit(source(['float4', 'uint4'], ids=', uint gl_InstanceIndex [[instance_id]], uint gl_VertexIndex [[vertex_id]]'))
        self.assertEqual(u.count('static __attribute__((always_inline)) main0_out dm_vs_body(main0_in in, constant float4& u, uint gl_InstanceIndex, uint gl_VertexIndex)\n'), 1)
        self.assertEqual(u.count('dm_vs_body(in, u, gl_InstanceIndex, gl_VertexIndex);'), 7)   # main0, main0_z, their pulling and shadow-input versions, and the out-of-line copy
        self.assertEqual(u.count('dm_vs_body_o(in, u, gl_InstanceIndex, gl_VertexIndex);'), 2)   # the two wide-line body functions

    def test_a_target_without_the_pulling_library(self):   # DMSH_IOS_PULL=0: the iPhone's unit holds <key>_b and <key>_bv
        full = source(['float4', 'uint4'])
        u, pn, vn = unit(full, pull=False)
        self.assertEqual((pn, vn), ({}, {'entry': 'main0_v', 'entry_z': 'main0_zv'}))
        self.assertEqual(sorted(dmsh_side.vs_entries(u)), ['main0', 'main0_gs', 'main0_v', 'main0_z', 'main0_zv'])
        for absent in ('dm_fetch', 'dm_vlayout', 'dm_vs_body_o', 'dm_wl'): self.assertNotIn(absent, u)
        self.assertEqual(u.count(BODY), 2)

    def test_no_wide_line_entries_no_out_of_line_copy(self):
        full = source(['float4'], ids=', uint gl_ViewIndex [[amplification_id]]')   # a parameter a wide-line entry cannot pass on
        self.assertNotIn('entry_wl', dmsh_pull.variant(full)[1])
        u, pn, vn = unit(full)
        self.assertEqual((pn, vn), ({'entry': 'main0_p', 'entry_z': 'main0_zp'}, {}))   # and no integer input: no shadow-input entry points
        self.assertNotIn('dm_vs_body_o', u); self.assertNotIn('dm_wline', u)
        self.assertEqual(u.count(BODY), 2)

    def test_refuses_another_shape(self):
        full = source(['float4', 'uint4'])
        pv, vv = dmsh_pull.variant(full)[0], dmsh_vin.variant(full)[0]
        same = full.replace('    ' + dmsh_side.VS_DEPTH + '\n', '')   # main0 without its depth line
        for bad, why in ((same, 'depth line'), (full.replace('vertex main0_out main0_z(', 'vertex main0_out main0_y('), 'main0_z'),
                         (full.replace('main0_z(main0_in in [[stage_in]], ', 'main0_z(main0_in in [[stage_in]], uint x [[vertex_id]], '), 'differ'),
                         (full + '\nvertex main0_out main0_p(main0_in in [[stage_in]])\n{\n    main0_out out = {};\n    return out;\n}\n', 'main0_p already'),
                         (full.replace('constant float4& u [[buffer(0)]]', 'constant float4& u [[buffer(0), function_constant(c)]]'), 'parameter')):
            with self.subTest(why=why), self.assertRaisesRegex(ValueError, why): dmsh_side.vs_unit(bad, pv, vv)
        with self.assertRaisesRegex(ValueError, 'copies of the body'): dmsh_side.vs_unit(full, pv.replace(BODY, 'float4 acc = u + 1.0;'), vv)   # a variant whose body is not the source's
        with self.assertRaisesRegex(ValueError, 'copies of the body'): dmsh_side.vs_unit(full, pv, vv.replace(BODY, 'float4 acc = u + 1.0;'))
        with self.assertRaisesRegex(ValueError, 'main0_in_v'): dmsh_side.vs_unit(full, pv, full)


@unittest.skipUnless(shutil.which('xcrun'), 'requires Metal compilers')
class Libraries(unittest.TestCase):
    FLAGS = (('macosx', ()), ('iphoneos', tuple(ios_metal.IOS_FLAGS)))

    def compile(self, d, name, src, sdk, flags):   # → (the .air, the library), with the chain's flags that matter here
        msl = d / f'{name}.metal'; air = d / f'{name}.{sdk}.air'; lib = d / f'{name}.{sdk}.metallib'; msl.write_text(src)
        subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-O1', '-w', '-fpreserve-invariance', '-fwrapv', *flags, str(msl), '-o', str(air)], check=True)
        subprocess.run(['xcrun', '-sdk', sdk, 'metallib', str(air), '-o', str(lib)], check=True)
        return air, lib

    def one(self, d, air, sdk, name):   # the library of one function of a compile, as the compiler's own tool cuts it
        out = d / 'one.metallib'
        subprocess.run(['xcrun', '-sdk', sdk, 'metallib', str(air), f'--function-name={name}', '-o', str(out)], check=True)
        return out.read_bytes()

    def test_cut_into_the_three_libraries(self):
        full = source(['float4', 'uint4', 'int4'], tail=TF)
        u, pn, vn = unit(full)
        with tempfile.TemporaryDirectory() as t:
            d = pathlib.Path(t)
            for sdk, flags in self.FLAGS:
                with self.subTest(sdk=sdk):
                    _, lib = self.compile(d, 'u', u, sdk, flags)
                    pieces = dmsh_side.cut(lib.read_bytes(), set(pn.values()), set(vn.values()))   # as vs_libraries cuts it
                    got = []
                    for name, data in zip(('b', 'bp', 'bv'), pieces): (d / f'{name}.metallib').write_bytes(data); got.append(pso_offline.functions(d / f'{name}.metallib'))
                    self.assertEqual(got, [{'main0': False, 'main0_z': False, 'main0_gs': False, 'main0_tf': False},
                                           {'main0_p': False, 'main0_zp': False, 'main0_wl': False, 'main0_z_wl': False}, {'main0_v': False, 'main0_zv': False}])

    def test_main0_gs_is_the_plain_sources_code(self):   # the capture entry is the copy it was: byte for byte the function the plain source compiles to. main0 is not (the depth line once)
        full = source(['float4', 'uint4', 'int4'])
        u, _, _ = unit(full)
        with tempfile.TemporaryDirectory() as t:
            d = pathlib.Path(t)
            for sdk, flags in self.FLAGS:
                with self.subTest(sdk=sdk):
                    plain, _ = self.compile(d, 'plain', full, sdk, flags); cut, _ = self.compile(d, 'unit', u, sdk, flags)
                    self.assertEqual(self.one(d, cut, sdk, 'main0_gs'), self.one(d, plain, sdk, 'main0_gs'))

    def test_a_unit_without_the_pulling_library_and_one_without_wide_lines(self):
        with tempfile.TemporaryDirectory() as t:
            d = pathlib.Path(t)
            for name, (u, pn, vn), want in (('nopull', unit(source(['float4', 'uint4']), pull=False), [{'main0': False, 'main0_z': False, 'main0_gs': False}, {'main0_v': False, 'main0_zv': False}]),
                                            ('nowide', unit(source(['float4'], ids=', uint gl_ViewIndex [[amplification_id]]')), [{'main0': False, 'main0_z': False, 'main0_gs': False}, {'main0_p': False, 'main0_zp': False}])):
                for sdk, flags in self.FLAGS:
                    with self.subTest(unit=name, sdk=sdk):
                        _, lib = self.compile(d, name, u, sdk, flags)
                        pieces = dmsh_side.cut(lib.read_bytes(), *[set(n.values()) for n in (pn, vn) if n])
                        got = []
                        for i, data in enumerate(pieces): (d / f'{i}.metallib').write_bytes(data); got.append(pso_offline.functions(d / f'{i}.metallib'))
                        self.assertEqual(got, want)


class FakeMetal:
    """The Metal cache as vs_libraries, pull_finish and vin_finish call it: records each compile and writes a file where a library would be. fail: the libraries (by suffix) whose
    compile fails, 'unit' for a unit's"""
    def __init__(self, fail=()): self.calls, self.fail, self.units = [], set(fail), {}

    def with_compiler(self, target, refl):
        return dict(refl, chain_compiler=target)

    def compile(self, target, msl, lib):
        suffix = os.path.basename(lib)[1:-len('.metallib')]; self.calls.append((target, suffix))
        if suffix in self.fail: raise subprocess.CalledProcessError(1, ['metal'], stderr='error')
        pathlib.Path(lib).write_text(f'{target} alone')

    def compile_split(self, target, msl, libs, split):
        self.calls.append((target, [os.path.basename(l)[1:-len('.metallib')] for l in libs], split.__defaults__[0])); self.units[target] = pathlib.Path(msl).read_text()
        if 'unit' in self.fail or (target, 'unit') in self.fail: raise subprocess.CalledProcessError(1, ['metal'], stderr='error')
        for l in libs: pathlib.Path(l).write_text(f'{target} cut')


class BuildStep(unittest.TestCase):
    PULLED, SHADOWED = {'main0_p', 'main0_zp', 'main0_wl', 'main0_z_wl'}, {'main0_v', 'main0_zv'}

    def setUp(self):
        tree = ast.parse((pathlib.Path(__file__).resolve().parents[1] / 'tools/dmsh_build.py').read_text())
        self.code = compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ('vs_libraries', 'pull_finish', 'vin_finish', 'write_refl')], type_ignores=[]), '<vertex libraries>', 'exec')
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        for d in ('mac', 'ios', 'w'): (self.root / d).mkdir()
        quiet = warnings.catch_warnings(); quiet.__enter__(); self.addCleanup(quiet.__exit__, None, None, None)
        warnings.simplefilter('ignore', ResourceWarning)   # the build writes a source with open(...).write(...), which CPython closes at once

    def run_step(self, full, ios=True, ios_pull=True, fail=()):   # → (what vs_libraries returns or the error it raises, the Metal cache's calls, the lines printed, the namespace)
        lines = []; mtl = FakeMetal(fail); written = {}
        ns = dict(os=os, re=__import__('re'), subprocess=subprocess, out=str(self.root / 'mac'), IOSOUT=str(self.root / 'ios') if ios else None, w=str(self.root / 'w'),
                  TARGETS={'mac', 'ios'} if ios else {'mac'}, IOS_PULL=ios_pull, MTL=mtl, dmsh_pull=dmsh_pull, dmsh_vin=dmsh_vin, dmsh_side=dmsh_side,
                  PULL_STATS=collections.Counter(), VIN_STATS=collections.Counter(), print=lambda *a, **k: lines.append(' '.join(map(str, a))),
                  written=written, atomic_json=lambda path, refl: written.update({pathlib.Path(path).parent.name: refl}))
        exec(self.code, ns)
        msl = self.root / 'w' / 'k_b.metal'; msl.write_text(full)
        try:
            got, cut_targets = ns['vs_libraries']('k', str(msl), full)
            ns['write_refl']('k', got, cut_targets)
        except subprocess.CalledProcessError as e: got = e
        self.assertEqual(sorted(p.name for p in (self.root / 'w').iterdir()), ['k_b.metal'])   # the units' files are gone
        return got, mtl, lines, ns

    def test_one_compile_a_target(self):
        got, mtl, lines, ns = self.run_step(source(['float4', 'uint4']))
        self.assertEqual(mtl.calls, [(t, ['_b', '_bp', '_bv'], [self.PULLED, self.SHADOWED]) for t in ('mac', 'ios')])   # where there were three compiles a target
        self.assertEqual(mtl.units['mac'], mtl.units['ios'])
        self.assertEqual(got, {'pull': {'entry': 'main0_p', 'entry_z': 'main0_zp', 'slot': 16, 'inputs': [0, 1], 'entry_wl': 'main0_wl', 'entry_z_wl': 'main0_z_wl'},
                               'vin': {'entry': 'main0_v', 'entry_z': 'main0_zv', 'shadow': [{'location': 1, 'float': 16, 'sint': 17}]}})
        self.assertEqual(lines, [])
        self.assertEqual((dict(ns['PULL_STATS']), dict(ns['VIN_STATS'])), ({'generated': 1, 'unit': 1}, {'generated': 1, 'inputs': 1, 'float only': 0}))
        self.assertTrue(all(ns['written'][t]['chain_metal_cut'] for t in ('mac', 'ios')))

    def test_no_integer_input(self):
        got, mtl, lines, _ = self.run_step(source(['float4']), ios=False)
        self.assertEqual(mtl.calls, [('mac', ['_b', '_bp'], [self.PULLED])])
        self.assertEqual(sorted(got), ['pull']); self.assertEqual(lines, [])

    def test_a_target_without_the_pulling_library(self):   # DMSH_IOS_PULL=0: the reflection, one for both targets, names the same entry points
        got, mtl, _, _ = self.run_step(source(['float4', 'uint4']), ios_pull=False)
        self.assertEqual(mtl.calls, [('mac', ['_b', '_bp', '_bv'], [self.PULLED, self.SHADOWED]), ('ios', ['_b', '_bv'], [self.SHADOWED])])
        self.assertNotIn('dm_fetch', mtl.units['ios']); self.assertIn('dm_fetch', mtl.units['mac'])
        self.assertEqual((got['pull']['entry'], got['vin']['entry']), ('main0_p', 'main0_v'))
        got, mtl, _, ns = self.run_step(source(['float4']), ios_pull=False)   # and with no shadow-input library either, nothing shares the plain source's compile there
        self.assertEqual(mtl.calls, [('mac', ['_b', '_bp'], [self.PULLED]), ('ios', '_b')])
        self.assertEqual(got['pull']['entry'], 'main0_p')
        self.assertTrue(ns['written']['mac']['chain_metal_cut'])
        self.assertNotIn('chain_metal_cut', ns['written']['ios'])

    def test_a_layer_shader_keeps_its_shadow_input_library_a_compile_of_its_own(self):   # that library repeats main0_l for the host (vin_fn), and one unit cannot hold it twice
        got, mtl, lines, _ = self.run_step(source(['float4', 'uint4'], tail=LAYER), ios=False)
        self.assertEqual(mtl.calls, [('mac', ['_b', '_bp'], [self.PULLED]), ('mac', '_bv')])
        self.assertEqual((got['pull']['entry'], got['vin']), ('main0_p', {'entry': 'main0', 'entry_z': 'main0_z', 'shadow': [{'location': 1, 'float': 16, 'sint': 17}]}))
        self.assertEqual(lines, [])

    def test_a_unit_that_does_not_build_is_compiled_library_by_library(self):
        got, mtl, lines, ns = self.run_step(source(['float4', 'uint4']), fail={('ios', 'unit')})
        units = [(t, ['_b', '_bp', '_bv'], [self.PULLED, self.SHADOWED]) for t in ('mac', 'ios')]
        self.assertEqual(mtl.calls, units + [('mac', '_b'), ('ios', '_b'), ('mac', '_bv'), ('ios', '_bv'), ('mac', '_bp'), ('ios', '_bp')])
        self.assertEqual((got['pull']['entry'], got['pull']['entry_wl'], got['vin']['entry']), ('main0', 'main0_wl', 'main0'))   # each library's own names
        self.assertEqual(len(lines), 1); self.assertRegex(lines[0], r'^\[RED\] k: its libraries do not build from one compile \(.*\): each is compiled from its own source$')
        self.assertEqual((ns['PULL_STATS']['unit'], ns['PULL_STATS']['unit failed'], ns['PULL_STATS']['generated'], ns['VIN_STATS']['generated']), (0, 1, 1, 1))
        self.assertEqual({p.name: p.read_text() for p in (self.root / 'mac').iterdir()}, {f'k{s}.metallib': 'mac alone' for s in ('_b', '_bp', '_bv')})   # none of a unit's pieces left
        self.assertTrue(all('chain_metal_cut' not in ns['written'][t] for t in ('mac', 'ios')))

    def test_a_source_of_another_shape_is_compiled_library_by_library(self):
        full = source(['float4']).replace('    ' + dmsh_side.VS_DEPTH + '\n', '')
        got, mtl, lines, ns = self.run_step(full, ios=False)
        self.assertEqual(mtl.calls, [('mac', '_b'), ('mac', '_bp')])   # no unit was compiled
        self.assertEqual(got['pull']['entry'], 'main0'); self.assertEqual(len(lines), 1); self.assertIn('main0 is not main0_z with the depth line', lines[0])
        self.assertEqual(ns['PULL_STATS']['unit failed'], 1)

    def test_a_source_that_does_not_compile_is_the_callers_failure(self):   # finish builds it again without its extras, or reports the shader: no red line of the unit's
        got, mtl, lines, ns = self.run_step(source(['float4']), ios=False, fail={'unit', '_b'})
        self.assertIsInstance(got, subprocess.CalledProcessError)
        self.assertEqual(mtl.calls, [('mac', ['_b', '_bp'], [self.PULLED]), ('mac', '_b')]); self.assertEqual(lines, []); self.assertEqual(ns['PULL_STATS']['unit failed'], 0)

    def test_no_pulling_library_no_unit(self):
        full = source(['float4', 'uint4']).replace('[[attribute(1)]]', '[[attribute(32)]]')   # an input the layout block does not hold
        got, mtl, lines, ns = self.run_step(full, ios=False)
        self.assertEqual(mtl.calls, [('mac', '_b')])   # and no shadow-input library: its input is past attribute 15
        self.assertEqual(got, {}); self.assertEqual(ns['PULL_STATS']['unit failed'], 0)
        self.assertEqual([l[:40] for l in lines], ['[RED] k: no shadow-input library (an inp', '[RED] k: no pulling library (an input at'])


if __name__ == '__main__':
    unittest.main()
