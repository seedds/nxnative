"""Indexed NVN2 compute c0 handles use the host's combined texture table."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'gfx/tools'))
import dmsh_abi as abi
import dmsh_reflect as reflect

SOURCE = """struct spvDescriptorSetBuffer0
{
    device uint* cp_s0 [[id(0)]];
    array<texture2d<float>, 8192> cp_t2d_cb0_0 [[id(1)]];
    array<sampler, 8192> cp_t2d_cb0_0Smplr [[id(8193)]];
    device uint* result [[id(16385)]];
};
kernel void main0()
{
    float4 v = args.cp_t2d_cb0_0[indices[group]].sample(args.cp_t2d_cb0_0Smplr[indices[group]], uv);
}
"""


class ComputeHandleArrays(unittest.TestCase):
    def test_tennis_layout_and_reflection(self):
        out = reflect.compute_handle_arrays(SOURCE, [0, 1, 32, 288, 0, 0, 0])
        count = (abi.C2_SAMP + abi.C2_ROW * abi.C2_SEP_SLOTS) // abi.C2_ROW
        self.assertIn(f'array<texture2d<float>, {count}> cp_t2d_cb2_0 [[id(1)]];', out)
        self.assertIn(f'cp_t2d_cb2_0Smplr [[id({count + 1})]];', out)
        self.assertIn(f'result [[id({2 * count + 1})]];', out)
        self.assertIn(f'args.cp_t2d_cb2_0[min(uint(indices[group]), {count - 1}u)]', out)
        self.assertIn(f'args.cp_t2d_cb2_0Smplr[min(uint(indices[group]), {count - 1}u)]', out)
        bindings = reflect.reflect('test', 'cs', out, {'sbufs': []})
        self.assertEqual([b['kind'] for b in bindings[1:3]], ['tex_c2a', 'samp_c2a'])
        self.assertEqual([b['off'] for b in bindings[1:3]], [0, 0])

    def test_layout_offset_matches_scalar_mapping(self):
        for ubos, ssbos, push in ((0, 0, 0), (1, 2, 5), (3, 1, 32)):
            start = 16 + ((push + 15) & ~15) + 16 * (ubos + ssbos)
            out = reflect.compute_handle_arrays(SOURCE, [ubos, ssbos, 16, start + 128, push, 0, 0])
            shift = (abi.C2_TEX - start) // abi.C2_ROW
            self.assertIn(f'(uint(indices[group]) + uint({shift}))', out)
            for slot in (0, 1, 15):
                self.assertEqual((start + 8 * slot) // 8 + shift, abi.C2_TEX // 8 + slot)

    def test_other_shader_bindings_stay(self):
        self.assertEqual(reflect.compute_handle_arrays(SOURCE, None), SOURCE)
        other = SOURCE.replace('_cb0_0', '_cb3_0')
        self.assertEqual(reflect.compute_handle_arrays(other, [0, 1, 32, 288, 0]), other)

    def test_unsupported_layout_and_array_use_fail(self):
        for layout in ([0, 1, 0, 32, 0], [0, 1, 32, 128, 0], [0, 1, 32, 296, 0, 1, 0]):
            with self.subTest(layout=layout), self.assertRaises(ValueError):
                reflect.compute_handle_arrays(SOURCE, layout)
        with self.assertRaises(ValueError):
            reflect.compute_handle_arrays(SOURCE + 'f(args.cp_t2d_cb0_0);', [0, 1, 32, 288, 0])
        with self.assertRaises(ValueError):
            reflect.compute_handle_arrays(SOURCE + 'texture2d<float> cp_t2d_cb2_0;', [0, 1, 32, 288, 0])


if __name__ == '__main__':
    unittest.main()
