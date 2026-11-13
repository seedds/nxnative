#!/usr/bin/env python3
"""dmsh_rewrite.py per_vertex: the flat inputs the translator declares for Ampere's LDTRAM (dm_pv<L>_<v> at user(locn<64 + 3L + v>)) become one vertex_value<float4> at user(locn<L>),
each read that vertex's get(); a shader without them is unchanged, and one that also interpolates attribute L is refused. ios_metal.compile takes MSL 4.0 for such a source only.
With DMSH_TEST_METAL=1 the result also compiles with the Metal compilers (the Mac's deployment target, and the iPhone's with its MSL 4.0 flag)."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'dm/gfx/tools'))
sys.path.insert(0, str(ROOT / 'dm/ios/tools'))
import dmsh_chain  # noqa: E402
import dmsh_rewrite  # noqa: E402
import ios_metal  # noqa: E402

# The shape SPIRV-Cross gives Resident Evil 4's pixel shader 5b7f35045f5c8614: attribute 2 read at all three vertices, weighed with the barycentric coordinates' y and z
SRC = """#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct main0_out
{
    float4 m_83 [[color(0)]];
};

struct main0_in
{
    float4 m_72 [[user(locn0)]];
    float4 dm_pv2_0 [[user(locn70), flat]];
    float4 dm_pv2_1 [[user(locn71), flat]];
    float4 dm_pv2_2 [[user(locn72), flat]];
    float3 gl_BaryCoordEXT [[barycentric_coord, center_perspective]];
};

static inline __attribute__((always_inline))
void body(thread main0_out& out, thread main0_in& in)
{
    float _147 = in.dm_pv2_0.w;
    float _149 = in.dm_pv2_1.x;
    float _199 = in.dm_pv2_2.x;
    float _255 = in.gl_BaryCoordEXT.y;
    float _289 = in.gl_BaryCoordEXT.z;
    out.m_83 = float4(_147 * (1.0 - _255 - _289) + _149 * _255 + _199 * _289) + in.m_72;
}

fragment main0_out main0(main0_in in [[stage_in]])
{
    main0_out out = {};
    body(out, in);
    return out;
}
"""


class PerVertex(unittest.TestCase):
    def test_the_three_inputs_become_one_vertex_value(self):
        out = dmsh_rewrite.per_vertex(SRC)
        self.assertNotIn('dm_pv2_', out)
        self.assertEqual(out.count('vertex_value<float4> dm_pv2 [[user(locn2)]];'), 1)
        for v, name in enumerate(('first', 'second', 'third')):
            self.assertIn(f'in.dm_pv2.get(vertex_index::{name})', out)
        self.assertIn('float4 m_72 [[user(locn0)]];', out)   # the attribute it interpolates stays

    def test_two_attributes_and_a_vertex_not_read(self):
        src = SRC.replace('    float4 dm_pv2_2 [[user(locn72), flat]];\n', '    float4 dm_pv3_0 [[user(locn73), flat]];\n').replace('in.dm_pv2_2.x', 'in.dm_pv3_0.x')
        out = dmsh_rewrite.per_vertex(src)
        self.assertIn('vertex_value<float4> dm_pv2 [[user(locn2)]];', out)
        self.assertIn('vertex_value<float4> dm_pv3 [[user(locn3)]];', out)
        self.assertIn('in.dm_pv3.get(vertex_index::first).x', out)
        self.assertNotIn('vertex_index::third', out)

    def test_a_shader_without_them_is_unchanged(self):
        src = SRC.replace('dm_pv2_', 'm_pv2_')
        self.assertEqual(dmsh_rewrite.per_vertex(src), src)

    def test_an_attribute_also_interpolated_is_refused(self):
        with self.assertRaises(ValueError):
            dmsh_rewrite.per_vertex(SRC.replace('user(locn0)', 'user(locn2)'))

    def test_a_location_off_the_translators_is_refused(self):
        with self.assertRaises(ValueError):
            dmsh_rewrite.per_vertex(SRC.replace('locn71', 'locn75'))

    def test_only_a_vertex_value_source_compiles_as_msl_4(self):
        calls = []
        real_compile, real_run = ios_metal.dmsh_pch.compile, subprocess.run
        try:
            ios_metal.dmsh_pch.compile = lambda metal, msl, air: calls.append(metal)
            subprocess.run = lambda *a, **k: None
            with tempfile.TemporaryDirectory() as d:
                for name, text in (('pv', dmsh_rewrite.per_vertex(SRC)), ('plain', SRC.replace('dm_pv2_', 'm_pv2_'))):
                    p = Path(d) / f'{name}.metal'; p.write_text(text)
                    ios_metal.compile(str(p), str(Path(d) / f'{name}.air'), str(Path(d) / f'{name}.metallib'))
        finally:
            ios_metal.dmsh_pch.compile, subprocess.run = real_compile, real_run
        self.assertIn(ios_metal.METAL4_FLAG, calls[0])
        self.assertNotIn(ios_metal.METAL4_FLAG, calls[1])

    @unittest.skipUnless(os.environ.get('DMSH_TEST_METAL') and shutil.which('xcrun'), 'set DMSH_TEST_METAL=1 to compile with the Metal compilers')
    def test_the_result_compiles(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'pv.metal'; p.write_text(dmsh_rewrite.per_vertex(SRC))
            for metal in (dmsh_chain.MAC_METAL, ios_metal.METAL + [ios_metal.METAL4_FLAG]):
                r = subprocess.run([*metal, str(p), '-o', str(Path(d) / 'pv.air')], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == '__main__':
    unittest.main()
