"""The float16 output limit library (dmsh_limit16.py): colour outputs written straight from a constant buffer are limited in every entry point, computed outputs and shaders
without such an output are left alone, and the output compiles for the macOS and iOS SDKs. With --fixture <dir>, writes the replay test's shaders (t_limit16.m)"""
import pathlib, shutil, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_limit16

BASE = '''#include <metal_stdlib>
using namespace metal;
struct _16 { float4 _m0[4]; };
struct spvDescriptorSetBuffer0 { constant _16* fp_c4 [[id(0)]]; };
struct main0_out
{
    float4 m_0 [[color(0)]];
    float4 m_2 [[color(2)]];
    uint4 m_3 [[color(3)]];
};

fragment main0_out main0(float4 gl_FragCoord [[position]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    main0_out out = {};
    float _7 = (*spvDescriptorSet0.fp_c4)._m0[2].x;
    out.m_0.x = gl_FragCoord.x;
    out.m_0.y = _7;
    out.m_2.x = (*spvDescriptorSet0.fp_c4)._m0[1].y;
    out.m_2.y = 0.0;
    return out;
}

struct main0_out_u
{
    uint4 m_0 [[color(0)]];
};
fragment main0_out_u main0_u(float4 gl_FragCoord [[position]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    main0_out out = {};
    out.m_2.x = (*spvDescriptorSet0.fp_c4)._m0[1].y;
    return main0_out_u{uint4(out.m_2)};
}
'''
COMPUTED = BASE.replace('out.m_2.x = (*spvDescriptorSet0.fp_c4)._m0[1].y;', 'out.m_2.x = gl_FragCoord.y;').replace('out.m_0.y = _7;', 'out.m_0.y = _7 * 2.0;')


def compile_lib(src, d, name, sdk='macosx'):
    metal, lib = d / (name + '.metal'), d / (name + '.metallib')
    metal.write_text(src)
    subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-w', str(metal), '-o', str(lib)], check=True, capture_output=True, text=True)
    return lib


class Limit(unittest.TestCase):
    def test_limits_uniform_outputs_in_every_entry_point(self):
        src, entries = dmsh_limit16.variant(BASE)
        self.assertEqual(entries, ['main0', 'main0_u'])
        self.assertEqual(src.count('out.m_2.x = dm_limit16((*spvDescriptorSet0.fp_c4)._m0[1].y);'), 2)
        self.assertIn('out.m_0.y = dm_limit16(_7);', src)
        self.assertIn('out.m_0.x = gl_FragCoord.x;', src)
        self.assertIn('out.m_2.y = 0.0;', src)
        self.assertEqual(dmsh_limit16.colors(BASE), 1 << 0 | 1 << 2)

    def test_computed_outputs_get_no_library(self):
        self.assertEqual(dmsh_limit16.variant(COMPUTED)[0], None)
        self.assertEqual(dmsh_limit16.colors(COMPUTED), 0)

    @unittest.skipUnless(shutil.which('xcrun'), 'needs Metal')
    def test_compiles_for_both_platforms(self):
        with tempfile.TemporaryDirectory() as d:
            for sdk in ('macosx', 'iphoneos'):
                with self.subTest(sdk=sdk):
                    try: compile_lib(dmsh_limit16.variant(BASE)[0], pathlib.Path(d), 'v_' + sdk, sdk)
                    except subprocess.CalledProcessError as e: self.fail(e.stderr)


def fixture(out):   # t_limit16.m's shaders: base.metal (compiled by the test) and <key>_bs.metallib (the library the host loads)
    out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
    (out / 'base.metal').write_text(BASE)
    compile_lib(dmsh_limit16.variant(BASE)[0], out, 'lim').rename(out / f'{"0" * 16}_bs.metallib')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--fixture': fixture(sys.argv[2])
    else: unittest.main()
