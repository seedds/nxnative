"""The typed-output entry point main0_u (dmsh_entry.fs_uint) as the build cuts it (dmsh_build.py ps_libraries): every pixel shader of a known shape gets it, in the side library <key>_bx
with its alpha test and logic operation entry points, and the base <key>_b keeps main0 alone, with no function constant. With --fixture <dir>, writes the GPU test's libraries
(t_typed_outputs.m: every mix of float, unsigned and signed targets, and the alpha test of a float colour 0 beside integer targets, through pso_desc, with the bits read back)"""
import pathlib, shutil, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import dmsh_entry, dmsh_side, dmsh_reflect, pso_offline   # noqa: E402
from test_dmsh_alpha import compile_lib   # noqa: E402
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'ios/tools'))
import ios_metal   # noqa: E402

# Colours 0 to 2 written from bits that are a normal float, so a float target keeps them too (no NaN to canonicalize, no denormal to flush): 0.25, -0.5, 1.0, 3.0 and -3.0. Colour 3 holds
# bits no float target would keep, as the BC block encoders write (agl's BC4 encoder in Breath of the Wild and Tears of the Kingdom): a NaN with a payload, all ones, the smallest denormal
# and a negative denormal, which an integer target takes through main0's float output as they are
MAIN = '''#include <metal_stdlib>
using namespace metal;

struct main0_out
{
    float4 m_0 [[color(0)]];
    float4 m_1 [[color(1)]];
    float m_2 [[color(2)]];
    float4 m_3 [[color(3)]];
};

fragment main0_out main0(float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    out.m_0 = float4(0.25, -0.5, 1.0, 0.25);
    out.m_1 = as_type<float4>(uint4(0x3e800000u, 0xbf000000u, 0x3f800000u, 0x40400000u));
    out.m_2 = as_type<float>(int(-1069547520));
    out.m_3 = as_type<float4>(uint4(0x7fc00001u, 0xffffffffu, 0x00000001u, 0x807fffffu));
    return out;
}
'''
VERTEX = '\nvertex float4 vertex_main(uint i [[vertex_id]]) { float2 v[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)}; return float4(v[i], 0, 1); }\n'
KEY = '2' * 16


def libraries(src, sdk='macosx', root=None):   # (the base library's bytes, the side library's bytes, the side's function names), as ps_libraries cuts them
    full = dmsh_side.shared_body(src) + dmsh_entry.fs_uint(src)
    unit, info = dmsh_side.unit(full)
    side = set(info['alpha'] + info['logic']) | {'main0_u'}
    with tempfile.TemporaryDirectory() as t:
        lib = compile_lib(unit + VERTEX, pathlib.Path(root or t), 'typed_' + sdk, sdk)
        base, bx = dmsh_side.cut(lib.read_bytes(), side)
    return base, bx, side


class Typed(unittest.TestCase):
    def test_every_entry_of_the_unit(self):
        full = dmsh_side.shared_body(MAIN) + dmsh_entry.fs_uint(MAIN)
        _, info = dmsh_side.unit(full)
        self.assertEqual(info, {'alpha': ['main0_a'], 'logic': []})
        self.assertEqual(dmsh_entry.typed_ops(full), {'alpha': True, 'logic': True, 'dual': True})
        self.assertIn('float4 m_1_du [[color(0), index(1), function_constant(dm_du)]];', full)
        self.assertIn('constant bool dm_uf1 = !dm_ui1 && !dm_si1 && !dm_du;', full)
        self.assertIn('constant bool dm_uf2 = !dm_ui2 && !dm_si2 && !dm_du;', full)

    def test_dual_source_gates(self):   # dual-source requires matching widths on colours 0 and 1 and no untyped colour >= 1
        one = MAIN.replace('    float4 m_1 [[color(1)]];\n', '').replace('    out.m_1 = as_type<float4>(uint4(0x3e800000u, 0xbf000000u, 0x3f800000u, 0x40400000u));\n', '')
        self.assertFalse(dmsh_entry.typed_ops(dmsh_entry.fs_uint(one))['dual'])
        mismatch = MAIN.replace('float4 m_1 [[color(1)]];', 'float2 m_1 [[color(1)]];')
        self.assertFalse(dmsh_entry.typed_ops(dmsh_entry.fs_uint(mismatch))['dual'])
        half_c2 = MAIN.replace('float m_2 [[color(2)]];', 'half m_2 [[color(2)]];')
        self.assertFalse(dmsh_entry.typed_ops(dmsh_entry.fs_uint(half_c2))['dual'])

    def test_reflection_classes(self):   # what main0 declares, which the host compares with the pipeline's targets (pso_cls)
        r = dmsh_reflect.raster_facts('ps', MAIN.replace('float4 m_1', 'uint4 m_1').replace('float m_2', 'int m_2'))
        self.assertEqual((r['float_colors'], r['uint_colors'], r['sint_colors']), (9, 2, 4))

    @unittest.skipUnless(shutil.which('xcrun'), 'requires Metal compilers')
    def test_base_keeps_main0_alone_on_both_platforms(self):   # the base every shader loads, and the pipeline archive compiles, has no function constant (pso_offline.py)
        for sdk in ('macosx', 'iphoneos'):
            with self.subTest(sdk=sdk), tempfile.TemporaryDirectory() as t:
                base, bx, side = libraries(MAIN, sdk)
                (pathlib.Path(t) / 'b.metallib').write_bytes(base); (pathlib.Path(t) / 'bx.metallib').write_bytes(bx)
                self.assertEqual(pso_offline.functions(pathlib.Path(t) / 'b.metallib'), {'main0': False, 'vertex_main': False})
                self.assertEqual(set(pso_offline.functions(pathlib.Path(t) / 'bx.metallib')), side)


def fixture(out):   # t_typed_outputs.m's libraries: <key>_b.metallib (main0, vertex_main) and <key>_bx.metallib (main0_u and the alpha test and logic operation entry points)
    out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
    base, bx, _ = libraries(MAIN, root=out)
    (out / (KEY + '_b.metallib')).write_bytes(base); (out / (KEY + '_bx.metallib')).write_bytes(bx)


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--fixture': fixture(sys.argv[2])
    else: unittest.main()
