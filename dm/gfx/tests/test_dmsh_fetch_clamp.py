"""A texel fetch past the edge of a sampled 2D texture (dmsh_rewrite.py fetch_clamp): it reads the nearest texel inside, and fetches of other kinds stay as they were."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_abi
from dmsh_reflect import fetch1d_names
from dmsh_rewrite import fetch_clamp

HEAD = '''#include <metal_stdlib>
using namespace metal;
struct A
{
    texture2d<float> fp_t_tcb_8;
    depth2d<float> fp_t_tcb_12;
    texture2d<float> fp_t_tcb_10;
    texture2d_ms<float> fp_t_tcb_A;
    texture2d_array<float> fp_t_tcb_C;
    texture3d<float> fp_t_tcb_E;
    texture2d<float, access::read_write> fp_i_tcb_48_rgba8;
    texture2d<float> fp_i_tcb_4A_;
};
'''


def shader(body):
    return HEAD + ('fragment float4 main0(constant A& d [[buffer(0)]], float4 p [[position]])\n{\n    int a = 1; int b = 2; int l = 0; uint4 u = uint4(0u); constant A* q = &d;\n    '
                   + body + '\n}\n')


def clamped(t, coord, lod):
    return f'{t}.read(uint2(clamp(int2({coord}), int2(0), int2({t}.get_width(uint({lod})), {t}.get_height(uint({lod}))) - 1)), {lod})'


def compiles(test, text):
    with tempfile.TemporaryDirectory() as t:
        path = Path(t) / 's.metal'; path.write_text(text)
        r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', str(path), '-o', str(Path(t) / 's.air')], capture_output=True, text=True)
        test.assertEqual(r.returncode, 0, r.stderr)


class FetchClamp(unittest.TestCase):
    def test_sampled_2d(self):
        for src, want in (('d.fp_t_tcb_8.read(uint2(int2(a, b)), 0).x', clamped('d.fp_t_tcb_8', 'uint2(int2(a, b))', '0') + '.x'),
                          ('d.fp_t_tcb_8.read(uint2(int2(a, b)), l).x', clamped('d.fp_t_tcb_8', 'uint2(int2(a, b))', 'l') + '.x'),
                          ('d.fp_t_tcb_8.read(uint2(int2(a, b))).x', clamped('d.fp_t_tcb_8', 'uint2(int2(a, b))', '0') + '.x'),
                          ('d.fp_t_tcb_8.read(uint2(int2(a, b)) + uint2(int2(-1, 0)), 0).x', clamped('d.fp_t_tcb_8', 'uint2(int2(a, b)) + uint2(int2(-1, 0))', '0') + '.x'),
                          ('d.fp_t_tcb_12.read(uint2(int2(a, b)), 0)', clamped('d.fp_t_tcb_12', 'uint2(int2(a, b))', '0'))):
            with self.subTest(src):
                self.assertIn(want, fetch_clamp(shader(f'float x = {src}; return float4(x);')))

    def test_other_kinds_stay(self):
        for src in ('d.fp_t_tcb_A.read(uint2(int2(a, b)), 0).x', 'd.fp_t_tcb_C.read(uint2(u.xy), uint(u.z), 0).x', 'd.fp_t_tcb_E.read(uint3(int3(a, b, 0)), 0).x',
                    'd.fp_i_tcb_48_rgba8.read(uint2(int2(a, b))).x', 'd.fp_i_tcb_4A_.read(uint2(int2(a, b)), 0).x'):
            with self.subTest(src):
                text = shader(f'float x = {src}; return float4(x);')
                self.assertEqual(fetch_clamp(text), text)

    def test_other_texture_expressions_stay(self):   # only a name or a member chain names the texture; through a pointer or a bracket it is left as written
        for src in ('q->fp_t_tcb_8.read(uint2(int2(a, b)), 0).x', '(*q).fp_t_tcb_8.read(uint2(int2(a, b)), 0).x'):
            with self.subTest(src):
                text = shader(f'float x = {src}; return float4(x);')
                self.assertEqual(fetch_clamp(text), text)

    def test_kept_names_stay(self):   # the caller's names: 1D integer fetches the translator reads as 2D rows (fetch1d_names)
        text = shader('float x = d.fp_t_tcb_10.read(uint2(int2(a, b)), 0).x; float y = d.fp_t_tcb_8.read(uint2(int2(a, b)), 0).x; return float4(x + y);')
        out = fetch_clamp(text, {'fp_t_tcb_10'})
        self.assertIn('d.fp_t_tcb_10.read(uint2(int2(a, b)), 0).x', out)
        self.assertIn(clamped('d.fp_t_tcb_8', 'uint2(int2(a, b))', '0'), out)

    def test_read_inside_a_read(self):   # the outer read changes; a read in its coordinate or its lod stays as written
        inner = 'd.fp_t_tcb_8.read(uint2(int2(a, b)), 0)'
        for src in (f'd.fp_t_tcb_8.read(uint2(int2({inner}.xy)), 0).x', f'd.fp_t_tcb_8.read(uint2(int2(a, b)), uint({inner}.x)).x'):
            with self.subTest(src):
                out = fetch_clamp(shader(f'float x = {src}; return float4(x);'))
                self.assertEqual(out.count('clamp(int2('), 1)
                self.assertIn(inner, out)
                compiles(self, out)

    def test_no_fetch(self):
        text = shader('return float4(0.0);')
        self.assertEqual(fetch_clamp(text), text)

    def test_two_fetches_both_change(self):
        out = fetch_clamp(shader('float x = d.fp_t_tcb_8.read(uint2(int2(a, b)), 0).x; float y = d.fp_t_tcb_8.read(uint2(int2(b, a)), 0).x; return float4(x + y);'))
        self.assertEqual(out.count('clamp(int2('), 2)

    def test_it_compiles(self):
        compiles(self, fetch_clamp(shader('float x = d.fp_t_tcb_8.read(uint2(int2(a, b)) + uint2(int2(-1, 0)), l).x; float y = d.fp_t_tcb_8.read(uint2(int2(a, b))).x; '
                                          'float z = d.fp_t_tcb_12.read(uint2(int2(a, b)), 0); return float4(x + y + z);')))


class Fetch1DNames(unittest.TestCase):
    def test_names_of_the_fetch1d_slots(self):
        def name(slot): return f'fp_t_tcb_{(dmsh_abi.C2_TEX + slot * dmsh_abi.C2_ROW) // 4:X}'   # a texture handle in the c2 table's row of that slot
        msl = f'struct A\n{{\n    texture2d<uint> {name(3)};\n    texture2d<float> {name(5)};\n}};\n'
        self.assertEqual(fetch1d_names(msl, {'fetch1d': [3]}), {name(3)})
        self.assertEqual(fetch1d_names(msl, {'fetch1d': [3, 5]}), {name(3), name(5)})
        self.assertEqual(fetch1d_names(msl, {}), set())


if __name__ == '__main__':
    unittest.main()
