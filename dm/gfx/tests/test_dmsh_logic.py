"""The logic operation (dmsh_logic.py, dmsh_side.py, dmsh_entry.fs_uint): main0's entry point, or main0_u under its constant, reads each colour output's attachment and combines it with what the body returned,
integer outputs bit for bit and float outputs through their normalized target's bits, the output stays a library that compiles for the Mac and the iPhone, and shaders without colour
outputs get none. With --fixture <dir>, writes the replay test's shaders (t_logic.m): the base and side libraries and their generated reflection"""
import ast, collections, json, os, pathlib, re, shutil, struct, subprocess, sys, tempfile, threading, types, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_abi, dmsh_atomic, dmsh_chain, dmsh_entry, dmsh_fetch, dmsh_logic, dmsh_reflect, dmsh_rewrite, dmsh_side, dmsh_wide
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))   # its sibling test module, also when the file runs alone (python -m unittest <file>, as CI does)
from test_dmsh_alpha import compile_lib   # noqa: E402

MAIN = '''#include <metal_stdlib>
using namespace metal;
struct spvDescriptorSetBuffer0
{
    constant uint4* fp_c3 [[id(0)]];
};

static inline float4 helper(float4 v) { return v; }

struct main0_out
{
    float4 m_0 [[color(0)]];
    float4 m_1 [[color(1)]];
};

fragment main0_out main0(float4 gl_FragCoord [[position]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    main0_out out = {};
    out.m_0 = helper(as_type<float4>(*spvDescriptorSet0.fp_c3));
    out.m_1 = float4(0.5);
    return out;
}
'''
BASE = MAIN + dmsh_entry.fs_uint(MAIN)   # main0_u as the build writes it (dmsh_entry.py), each colour a float or an integer by its function constant; the unit makes main0's body dm_body_main0
VERTEX = '\nvertex float4 vertex_main(uint i [[vertex_id]]) { float2 v[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)}; return float4(v[i], 0, 1); }\n'
STATE = f'constant dm_logic_state& dm_lop [[buffer({dmsh_abi.LOGIC_BUFFER})]]'
FB0 = 'uint4 dm_fb0 [[color(0), function_constant(dm_lfb0)]]'
USTATE = f'constant dm_logic_state& dm_lop [[buffer({dmsh_abi.LOGIC_BUFFER}), function_constant(dm_lo)]]'   # main0_u's
ALPHA = f'constant dm_alpha_state& dm_alpha [[buffer({dmsh_abi.ALPHA_BUFFER}), function_constant(dm_at)]]'


def variant(src):   # (the compile unit, its logic operation entry points)
    u, info = dmsh_side.unit(src)
    return u, info.get('logic')


class LogicVariant(unittest.TestCase):
    def test_integer_entry_points(self):
        v, entries = variant(BASE)
        self.assertEqual(entries, [])   # main0_u holds the operation under its constant DMSH_TYPED_LOGIC_FC, and main0 gets none beside it (dmsh_side.unit)
        # main0_u: each colour reads its attachment as a float or as an unsigned integer, whichever member the pipeline's constants leave, only when it is attached and the operation is
        # on; a signed one passes through (no bits to combine: lop_key), with no attachment read; the state is an argument only with the operation
        pair = lambda n: f'float4 dm_fb{n}_dm_uf{n} [[color({n}), function_constant(dm_lfb{n}_dm_uf{n})]], uint4 dm_fb{n}_dm_ui{n} [[color({n}), function_constant(dm_lfb{n}_dm_ui{n})]]'
        self.assertIn('[[buffer(0)]], ' + ALPHA + ', ' + pair(0) + ', ' + pair(1) + ', ' + USTATE + ')', v)
        self.assertEqual(v.count(f'constant bool dm_lo [[function_constant({dmsh_abi.TYPED_LOGIC_FC})]];'), 1)
        for n in (0, 1):
            self.assertEqual(v.count(f'constant bool dm_lfb{n} [[function_constant({dmsh_abi.LOGIC_FC + n})]];'), 1)
            for fc in (f'dm_uf{n}', f'dm_ui{n}'): self.assertEqual(v.count(f'constant bool dm_lfb{n}_{fc} = dm_lfb{n} && {fc} && dm_lo;'), 1)
            self.assertEqual(v.count(f'if (dm_lfb{n}_dm_ui{n}) r.m_{n}_ui = uint4(dm_logic(uint4(r.m_{n}_ui), dm_fb{n}_dm_ui{n}, dm_lop, {n}u));'), 1)
            self.assertEqual(v.count(f'if (dm_lfb{n}_dm_uf{n}) r.m_{n} = float4(dm_logic_norm(float4(r.m_{n}), dm_fb{n}_dm_uf{n}, dm_lop, {n}u));'), 1)
            self.assertNotIn(f'dm_lfb{n}_dm_si{n}', v); self.assertNotIn(f'r.m_{n}_si = int', v)
        self.assertLess(v.index('main0_out_u r = {};'), v.index('if (dm_lfb0_dm_uf0) r.m_0'))   # on the typed outputs

    def test_float_outputs_go_through_the_targets_bits(self):
        v, entries = variant(MAIN)
        self.assertEqual(entries, ['main0_l'])
        self.assertIn('float4 dm_fb0 [[color(0), function_constant(dm_lfb0)]], float4 dm_fb1 [[color(1), function_constant(dm_lfb1)]], ' + STATE + ')', v)
        self.assertEqual(v.count('if (dm_lfb0) dm_r.m_0 = float4(dm_logic_norm(float4(dm_r.m_0), dm_fb0, dm_lop, 0u));'), 1)

    def test_only_the_entry_points_the_host_takes(self):   # the dual-source entry point stays out: a logic operation takes the place of blending
        dual = BASE + '\nstruct main0_out_d\n{\n    float4 m_0 [[color(0), index(0)]];\n    float4 m_1 [[color(0), index(1)]];\n};\n\nfragment main0_out_d main0_d(float4 gl_FragCoord [[position]])\n{\n    return main0_out_d{float4(0.0), float4(1.0)};\n}\n'
        v, entries = variant(dual)
        self.assertEqual(entries, []); self.assertNotIn('main0_d_l', v)
        self.assertEqual(variant(MAIN)[1], ['main0_l'])   # main0's, in a shader without main0_u

    def test_helper_after_the_shaders_own(self):
        v, _ = variant(BASE)
        self.assertLess(v.index('static inline float4 helper'), v.index('struct dm_logic_state'))
        self.assertLess(v.index('dm_body_main0('), v.index('struct dm_logic_state'))
        self.assertLess(v.index('struct dm_logic_state'), v.index('constant bool dm_lfb0'))
        self.assertLess(v.index('constant bool dm_lfb1'), v.index('fragment main0_out_u main0_u('))

    def test_narrow_integer_outputs(self):
        narrow = MAIN.replace('float4 m_0 [[color(0)]];\n    float4 m_1', 'float m_0 [[color(0)]];\n    float2 m_1').replace('out.m_0 = helper(as_type<float4>(*spvDescriptorSet0.fp_c3));', 'out.m_0 = as_type<float>((*spvDescriptorSet0.fp_c3).x);').replace('out.m_1 = float4(0.5);', 'out.m_1 = float2(0.5);')
        v = dmsh_entry.fs_uint(narrow)
        self.assertIn('if (dm_lfb0_dm_ui0) r.m_0_ui = uint(dm_logic(uint4(r.m_0_ui, 0u, 0u, 0u), dm_fb0_dm_ui0, dm_lop, 0u).x);', v)
        self.assertIn('if (dm_lfb1_dm_ui1) r.m_1_ui = uint2(dm_logic(uint4(r.m_1_ui, 0u, 0u), dm_fb1_dm_ui1, dm_lop, 1u).xy);', v)

    def test_narrow_float_outputs(self):
        v, _ = variant(MAIN.replace('float4 m_0 [[color(0)]];\n    float4 m_1', 'float m_0 [[color(0)]];\n    float2 m_1').replace('out.m_0 = helper(as_type<float4>(*spvDescriptorSet0.fp_c3));', 'out.m_0 = 0.5;').replace('out.m_1 = float4(0.5);', 'out.m_1 = float2(0.5);'))
        self.assertIn('if (dm_lfb0) dm_r.m_0 = float(dm_logic_norm(float4(dm_r.m_0, 0.0f, 0.0f, 0.0f), dm_fb0, dm_lop, 0u).x);', v)
        self.assertIn('if (dm_lfb1) dm_r.m_1 = float2(dm_logic_norm(float4(dm_r.m_1, 0.0f, 0.0f), dm_fb1, dm_lop, 1u).xy);', v)

    def test_no_colour_output_no_entry_point(self):
        depth_only = MAIN.replace('float4 m_0 [[color(0)]];\n    float4 m_1 [[color(1)]];', 'float gl_FragDepth [[depth(any)]];').replace('    out.m_0 = helper(as_type<float4>(*spvDescriptorSet0.fp_c3));\n    out.m_1 = float4(0.5);\n', '    out.gl_FragDepth = 0.5;\n')
        self.assertIsNone(variant(depth_only)[0])

    def test_taken_buffer_or_fetch_no_entry_point(self):
        self.assertFalse(variant(BASE.replace('[[buffer(0)]])\n{\n    main0_out out = {};\n    out.m_0 = helper', f'[[buffer(0)]], constant float& x [[buffer({dmsh_abi.LOGIC_BUFFER})]])\n{{\n    main0_out out = {{}};\n    out.m_0 = helper', 1))[1])
        self.assertFalse(variant(BASE.replace('float4 gl_FragCoord [[position]],', 'float4 gl_FragCoord [[position]], float4 fb [[color(0)]],', 1))[1])

    @unittest.skipUnless(shutil.which('xcrun'), 'requires Metal compilers')
    def test_compiles_on_both_platforms(self):
        with tempfile.TemporaryDirectory() as d:
            for sdk in ('macosx', 'iphoneos'):
                with self.subTest(sdk=sdk):
                    try: compile_lib(variant(BASE)[0], pathlib.Path(d), 'logic_' + sdk, sdk)
                    except subprocess.CalledProcessError as e: self.fail(e.stderr)


def fixture(out):   # Actual build reflection and libraries, starting at the synthetic MSL boundary (no translator/profile CLI initialization)
    out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
    key = '1' * 16; work = out / 'work'; work.mkdir()
    (work / (key + '.metal')).write_text(MAIN)
    (work / (key + '.res.json')).write_text(json.dumps({'cbufs': [{'slot': 3, 'fixed': 16}], 'sbufs': [], 'textures': [], 'images': [], 'notes': []}))
    code = work / 'code'; code.write_bytes(b'')

    def compile_msl(target, source, dest):
        pathlib.Path(dest).write_bytes(compile_lib(pathlib.Path(source).read_text() + VERTEX, work, 'fixture').read_bytes())

    def compile_split(target, source, dests, split):
        for dest, data in zip(dests, split(compile_lib(pathlib.Path(source).read_text() + VERTEX, work, 'fixture').read_bytes())):
            pathlib.Path(dest).write_bytes(data)

    ns = dict(globals(), loc={key: ('synthetic', 0)}, pc_of={key: []}, out=str(out), w=str(work), CHAIN=dmsh_chain.version(),
              IOSOUT=None, TARGETS={'mac'}, BF_WANT=set(), DUAL_WANT=set(), SIDE_STATS=collections.Counter(), BF_STATS=collections.Counter(),
              SPIRV_CROSS='synthetic-msl', XC=[], clone=shutil.copyfile,
              subprocess=types.SimpleNamespace(run=lambda *a, **kw: subprocess.CompletedProcess(a, 0), CalledProcessError=subprocess.CalledProcessError),
              MTL=types.SimpleNamespace(compile=compile_msl, compile_split=compile_split, with_compiler=lambda target, refl: refl))
    tree = ast.parse((pathlib.Path(dmsh_entry.__file__).parent / 'dmsh_build.py').read_text())
    tree.body = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in {'finish', 'ps_libraries', 'ps_omap', 'write_refl', 'atomic_json'}]
    exec(compile(tree, 'dmsh_build.py', 'exec'), ns)
    status, reason, refl = ns['finish'](key, str(work), {'stage': 'ps', 'code': str(code)}, b'')
    if status != 'ok': raise AssertionError((status, reason))
    assert refl['entry_u_logic'] and refl['entry_u_side'] and 'logic' not in refl   # the typed entry replaces main0_l: has_logic stays false


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--fixture': fixture(sys.argv[2])
    else: unittest.main()
