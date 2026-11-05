"""The alpha test entry points (dmsh_alpha.py, dmsh_side.py): every fragment entry point gets one that takes the state and tests colour 0's alpha after the body, the output stays
a library that compiles for the Mac and the iPhone, and shaders without a float colour 0 alpha get none. With --fixture <dir>, writes the replay test's shaders (t_alpha.m): the base
sources and their side libraries, compiled"""
import pathlib, re, shutil, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_abi, dmsh_alpha, dmsh_side

BASE = '''#include <metal_stdlib>
using namespace metal;
struct main0_out
{
    float4 m_0 [[color(0)]];
    float4 m_1 [[color(1)]];
};

fragment main0_out main0(float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    out.m_0 = float4(1.0, 0.0, 0.0, gl_FragCoord.x / 8.0);
    if (gl_FragCoord.y < 0.0)
        return out;
    out.m_1 = out.m_0;
    return out;
}

struct main0_out_u
{
    uint4 m_0 [[color(0)]];
    uint4 m_1 [[color(1)]];
};

fragment main0_out_u main0_u(float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    out.m_0 = float4(1.0, 0.0, 0.0, gl_FragCoord.x / 8.0);
    return main0_out_u{as_type<uint4>(out.m_0), as_type<uint4>(out.m_1)};
}
'''
VERTEX = '\nvertex float4 vertex_main(uint i [[vertex_id]]) { float2 v[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)}; return float4(v[i], 0, 1); }\n'
ARGS = BASE.split('\nstruct main0_out_u')[0].replace('fragment main0_out main0(float4 gl_FragCoord [[position]])',
    'struct spvDescriptorSetBuffer0 { constant float* width [[id(0)]]; };\n\nfragment main0_out main0(float4 gl_FragCoord [[position]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])').replace('gl_FragCoord.x / 8.0', 'gl_FragCoord.x / *spvDescriptorSet0.width')
TEST = 'if (!dm_alpha_pass(float(dm_r.m_0.w), dm_alpha)) discard_fragment();'
PARAM = f'constant dm_alpha_state& dm_alpha [[buffer({dmsh_abi.ALPHA_BUFFER})]]'


def compile_lib(src, root, name, sdk='macosx'):   # → the library path; CalledProcessError with the compiler's message
    msl = root / (name + '.metal'); msl.write_text(src); air = root / (name + '.' + sdk + '.air'); lib = root / (name + '.metallib')
    for cmd in (['xcrun', '-sdk', sdk, 'metal', '-c', '-fmodules-cache-path=' + str(root / 'mc'), str(msl), '-o', str(air)], ['xcrun', '-sdk', sdk, 'metallib', str(air), '-o', str(lib)]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode: raise subprocess.CalledProcessError(r.returncode, cmd, r.stdout, r.stderr)
    return lib


def variant(src):   # (the compile unit, its alpha test entry points)
    u, info = dmsh_side.unit(src)
    return u, info.get('alpha')


class AlphaVariant(unittest.TestCase):
    def test_every_entry_point(self):
        v, entries = variant(BASE)
        self.assertEqual(entries, ['main0_a', 'main0_u_a'])
        self.assertEqual(v.count(PARAM), 2)
        self.assertEqual(v.count(TEST), 1)   # main0_a
        self.assertEqual(v.count('if (!dm_alpha_pass(as_type<float>(dm_r.m_0.w), dm_alpha)) discard_fragment();'), 1)   # main0_u_a: colour 0 is the float's bits
        self.assertIn('main0_out dm_r = dm_body_main0(gl_FragCoord);', v)

    def test_the_body_stays_as_written(self):
        v, _ = variant(BASE)
        self.assertIn('static main0_out dm_body_main0(float4 gl_FragCoord)\n{\n    main0_out out = {};\n', v)
        self.assertIn('if (gl_FragCoord.y < 0.0)\n        return out;', v)
        self.assertIn('fragment main0_out main0(float4 gl_FragCoord [[position]])\n{\n    return dm_body_main0(gl_FragCoord);\n}', v)

    def test_parameter_after_attributes_with_parentheses(self):
        v, _ = variant(ARGS)
        self.assertIn('[[buffer(0)]], ' + PARAM + ')', v)
        self.assertIn('dm_body_main0(gl_FragCoord, spvDescriptorSet0)', v)

    def test_early_fragment_tests_kept(self):
        for attr in ('[[early_fragment_tests]]', '[[ early_fragment_tests ]]'):   # the second: SPIR-V Cross's spelling
            with self.subTest(attr=attr):
                v, entries = variant(BASE.replace('fragment main0_out main0(', attr + ' fragment main0_out main0('))
                self.assertEqual(entries, ['main0_a', 'main0_u_a'])
                self.assertIn(attr + ' fragment main0_out main0_a(float4 gl_FragCoord [[position]], ' + PARAM + ')', v)
                self.assertIn(attr + ' fragment main0_out main0(float4 gl_FragCoord [[position]])', v)
                self.assertNotIn(attr + ' static', v)

    def test_half_alpha(self):
        v, _ = variant(BASE.replace('float4 m_0 [[color(0)]]', 'half4 m_0 [[color(0)]]', 1))
        self.assertIn(TEST, v)

    def test_no_float_alpha_no_entry_point(self):
        for src, why in ((BASE.replace('[[color(0)]]', '[[color(2)]]'), 'no colour 0 output'), (BASE.replace('float4 m_0 [[color(0)]]', 'uint4 m_0 [[color(0)]]'), 'colour 0 has no float alpha'),
                         (BASE.replace('float4 m_0 [[color(0)]]', 'float3 m_0 [[color(0)]]'), 'colour 0 has no float alpha'), (BASE.split('struct main0_out')[0], 'no colour output')):
            with self.subTest(why=why):
                self.assertEqual(dmsh_alpha.refused(src), why)
                self.assertFalse(variant(src)[1])

    def test_taken_buffer_no_entry_point(self):
        self.assertFalse(variant(BASE.replace('float4 gl_FragCoord [[position]])', f'float4 gl_FragCoord [[position]], constant float& x [[buffer({dmsh_abi.ALPHA_BUFFER})]])', 1))[1])

    @unittest.skipUnless(shutil.which('xcrun'), 'requires Metal compilers')
    def test_compiles_on_both_platforms(self):
        with tempfile.TemporaryDirectory() as d:
            for name, src in (('base', BASE), ('args', ARGS), ('early', BASE.replace('fragment main0_out main0(', '[[ early_fragment_tests ]] fragment main0_out main0('))):
                for sdk in ('macosx', 'iphoneos'):
                    with self.subTest(shader=name, sdk=sdk):
                        try: compile_lib(variant(src)[0], pathlib.Path(d), name + '_' + sdk, sdk)
                        except subprocess.CalledProcessError as e: self.fail(e.stderr)


def fixture(out):   # t_alpha.m's shaders: <name>.metal (the base, compiled by the test) and <key>_bx.metallib (the side library the host loads), for key 0 and 1
    out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
    for i, src in enumerate((BASE.split('\nstruct main0_out_u')[0] + '\n', ARGS)):
        (out / f'base{i}.metal').write_text(src + VERTEX)
        u, info = dmsh_side.unit(src)
        lib = compile_lib(u + VERTEX, out, f'a{i}')
        (out / f'{str(i) * 16}_bx.metallib').write_bytes(dmsh_side.cut(lib.read_bytes(), set(info['alpha'] + info['logic']))[1])


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--fixture': fixture(sys.argv[2])
    else: unittest.main()
