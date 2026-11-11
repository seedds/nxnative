#!/usr/bin/env python3
# dmsh_rewrite.flag_reads: a game uniform read as an integer at a fixed offset goes through dm_vl (a volatile load), so Metal's backend keeps each read a load of its own
# (docs/reference/shaders.md, Integer reads of game uniforms and Metal's compile time). Float reads, the driver's banks and computed offsets are left as they are
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parent.parent / 'tools'
sys.path.insert(0, str(TOOLS))
from dmsh_rewrite import flag_reads   # noqa: E402

HEAD = '#include <metal_stdlib>\nusing namespace metal;\n'
SHADER = HEAD + '''struct _10 { float4 _m0[4096]; };
struct spvDescriptorSetBuffer0 { constant _10* fp_c0 [[id(0)]]; constant _10* fp_c5 [[id(1)]]; constant _10* fp_c12 [[id(2)]]; };
struct main0_out { float4 m_0 [[color(0)]]; };
fragment main0_out main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    main0_out out = {};
    bool _1 = 0 != as_type<int>((*spvDescriptorSet0.fp_c5)._m0[40].x);
    uint _2 = as_type<uint>((*spvDescriptorSet0.fp_c12)._m0[3].w);
    float _3 = (*spvDescriptorSet0.fp_c5)._m0[41].y;
    int _4 = as_type<int>((*spvDescriptorSet0.fp_c0)._m0[2].z);
    int _5 = as_type<int>((*spvDescriptorSet0.fp_c5)._m0[int(_2)].x);
    out.m_0 = float4(_1 ? _3 : 0.0, float(_2), float(_4), float(_5));
    return out;
}
'''


class FlagReads(unittest.TestCase):
    def test_integer_reads_of_game_slots_are_volatile(self):
        out = flag_reads(SHADER)
        self.assertIn('as_type<int>(dm_vl((*spvDescriptorSet0.fp_c5)._m0[40]).x)', out)
        self.assertIn('as_type<uint>(dm_vl((*spvDescriptorSet0.fp_c12)._m0[3]).w)', out)
        self.assertEqual(out.count('dm_vl('), 3)   # the two reads and the helper
        self.assertLess(out.index('using namespace metal;'), out.index('T dm_vl(constant T& r)'))

    def test_other_reads_stay(self):
        out = flag_reads(SHADER)
        self.assertIn('float _3 = (*spvDescriptorSet0.fp_c5)._m0[41].y;', out)                 # a float read
        self.assertIn('as_type<int>((*spvDescriptorSet0.fp_c0)._m0[2].z)', out)                 # a driver bank
        self.assertIn('as_type<int>((*spvDescriptorSet0.fp_c5)._m0[int(_2)].x)', out)           # a computed offset

    def test_a_shader_with_none_is_unchanged(self):
        src = HEAD + 'fragment float4 main0() { return float4(0.0); }\n'
        self.assertEqual(flag_reads(src), src)

    @unittest.skipUnless(shutil.which('xcrun') and subprocess.run(['xcrun', '-sdk', 'macosx', '-f', 'metal'], capture_output=True).returncode == 0, 'no Metal compiler')
    def test_compiles(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d, 's.metal'); f.write_text(flag_reads(SHADER))
            r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', '-fwrapv', str(f), '-o', str(Path(d, 's.air'))], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == '__main__':
    unittest.main()
