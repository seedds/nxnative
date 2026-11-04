"""The coordinate NaN guard as fmax (dmsh_build.py nan_coord_fmax, profile key shader_nan_coord fmax): 2D and array coordinates get fmax(c, -FLT_MAX), a cube direction keeps
no guard, a cube array keeps it on its layer only, whether the guard is the sample's argument or a temporary the sample reads; other selects stay; the result compiles for macOS."""
import ast
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
selected = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name == 'nan_coord_fmax')
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) in ('NAN_COORD', 'NAN_CUBE_TEX', 'NAN_CUBE_USE') for t in n.targets))]
scope = {'re': re}
exec(compile(ast.Module(body=selected, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
nan_coord_fmax = scope['nan_coord_fmax']

# A pixel shader as spirv-cross writes the translator's output, cut down: one 2D, one 2D array, one cube and one cube-array sample, each through the guard
SRC = '''#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct spvDescriptorSetBuffer0
{
    texture2d<float> fp_t_tcb_4 [[id(0)]];
    sampler fp_t_tcb_4Smplr [[id(1)]];
    texture2d_array<float> fp_t_tcb_5 [[id(2)]];
    sampler fp_t_tcb_5Smplr [[id(3)]];
    texturecube<float> fp_t_tcb_6 [[id(4)]];
    sampler fp_t_tcb_6Smplr [[id(5)]];
    texturecube_array<float> fp_t_tcb_8 [[id(6)]];
    sampler fp_t_tcb_8Smplr [[id(7)]];
};

struct main0_out
{
    float4 m_32 [[color(0)]];
};

struct main0_in
{
    float4 m_20 [[user(locn0)]];
};

fragment main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    main0_out out = {};
    float2 _10 = in.m_20.xy;
    float4 _11 = spvDescriptorSet0.fp_t_tcb_4.sample(spvDescriptorSet0.fp_t_tcb_4Smplr, select(_10, float2(0.0), isnan(_10)));
    float3 _12 = in.m_20.xyz;
    float3 _13 = select(_12, float3(0.0), isnan(_12));
    float4 _14 = spvDescriptorSet0.fp_t_tcb_5.sample(spvDescriptorSet0.fp_t_tcb_5Smplr, _13.xy, uint(rint(_13.z)));
    float4 _15 = spvDescriptorSet0.fp_t_tcb_6.sample(spvDescriptorSet0.fp_t_tcb_6Smplr, select(_12, float3(0.0), isnan(_12)), level(0.0));
    float4 _16 = in.m_20;
    float4 _17 = select(_16, float4(0.0), isnan(_16));
    float4 _18 = spvDescriptorSet0.fp_t_tcb_8.sample(spvDescriptorSet0.fp_t_tcb_8Smplr, _17.xyz, uint(rint(_17.w)), level(0.0));
    float _19 = select(_16.x, 0.0, isnan(_16.y));
    out.m_32 = _11 + _14 + _15 + _18 + float4(_19);
    return out;
}
'''


class NanCoordFmax(unittest.TestCase):
    def setUp(self):
        self.out = nan_coord_fmax(SRC)

    def test_2d_coordinate(self):
        self.assertIn('fp_t_tcb_4Smplr, fmax(_10, float2(-FLT_MAX)))', self.out)

    def test_array_coordinate_through_a_temporary(self):
        self.assertIn('float3 _13 = fmax(_12, float3(-FLT_MAX));', self.out)

    def test_cube_direction_unguarded(self):
        self.assertIn('fp_t_tcb_6Smplr, _12, level(0.0))', self.out)

    def test_cube_array_layer_only(self):
        self.assertIn('float4 _17 = float4(_16.xyz, fmax(_16.w, -FLT_MAX));', self.out)

    def test_no_select_of_a_coordinate_left(self):
        self.assertNotRegex(self.out, r'select\(_\d+, float[234]\(0\.0\), isnan')

    def test_other_selects_kept(self):
        self.assertIn('float _19 = select(_16.x, 0.0, isnan(_16.y));', self.out)

    def test_source_without_guards_unchanged(self):
        plain = SRC.replace('select(_10, float2(0.0), isnan(_10))', '_10')
        plain = re.sub(r'select\((_\d+), float[234]\(0\.0\), isnan\(\1\)\)', r'\1', plain)
        self.assertEqual(nan_coord_fmax(plain), plain)

    @unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode')
    def test_compiles_for_macos(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / 'nan_coord.metal'
            src.write_text(self.out)
            r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', str(src), '-o', str(Path(d) / 'nan_coord.air')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-2000:])


if __name__ == '__main__':
    unittest.main()
