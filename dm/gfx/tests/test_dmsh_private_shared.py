"""Invocation-private shared storage and conservative refusal of communication."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from dmsh_private_shared import privatize

SOURCE = '''kernel void main0(uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    threadgroup spvUnsafeArray<uint, 11264> scratch;
    float x = as_type<float>(gl_LocalInvocationID.x);
    float y = as_type<float>(gl_LocalInvocationID.y);
    int row = as_type<int>(y) & 65535;
    int rowbase = row * 1024;
    int lane = rowbase + as_type<int>(x);
    int masked = lane & 65535;
    int base = masked * 44;
    int address = base + 40;
    uint index = uint(int(uint(address) >> uint(2)));
    scratch[int(index)] = 7u;
    uint value = scratch[int(index)];
}
'''


class PrivateShared(unittest.TestCase):
    def test_disjoint_slices(self):
        result = privatize(SOURCE, [1024, 1, 1])
        self.assertIn('spvUnsafeArray<uint, 11> scratch;', result)
        self.assertEqual(result.count('scratch[10]'), 2)
        self.assertNotIn('threadgroup spvUnsafeArray', result)
        # Every invocation's rewritten address names exactly its original word.
        for invocation in range(1024):
            self.assertEqual(((invocation * 44 + 40) >> 2), invocation * 11 + 10)

    def test_unknown_or_shared_access_unchanged(self):
        for source in (
            SOURCE.replace('base + 40', 'base + 44'),
            SOURCE.replace('base + 40', '40'),
            SOURCE.replace('base + 40', 'other_lane()'),
            SOURCE.replace('value = scratch[int(index)]', 'value = helper(scratch[int(index)])'),
            SOURCE.replace('uint value', 'uint& value'),
            SOURCE.replace('int base =', 'lane++;\n    int base ='),
            SOURCE.replace('int base =', 'mutate(lane);\n    int base ='),
            SOURCE.replace('scratch[int(index)] = 7u;', 'helper(scratch);'),
            SOURCE.replace('scratch[int(index)] = 7u;', 'scratch[int(index)] = helper(scratch[int(index)]);'),
            SOURCE.replace('as_type<int>(x)', 'int(x)'),
            SOURCE.replace('masked * 44', 'masked * 2147483647'),
        ):
            with self.subTest(source=source):
                self.assertEqual(privatize(source, [1024, 1, 1]), source)

    def test_group_dimensions_required(self):
        for size in (None, [1, 1, 1], [32, 32, 1], [1023, 1, 1]):
            self.assertEqual(privatize(SOURCE, size), SOURCE)


if __name__ == '__main__':
    unittest.main()
