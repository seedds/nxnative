import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'gfx/tools'))
from vk_inputs import integer_inputs


class IntegerInputs(unittest.TestCase):
    def test_signed_and_unsigned_vectors(self):
        source = '''using namespace metal;
constant uint user [[function_constant(7)]];
struct I { int3 a [[attribute(1)]]; uint b [[attribute(4)]]; float2 uv [[attribute(5)]]; };
vertex float4 vk_test(I in [[stage_in]]) { return float4(in.a, in.b) + in.uv.x; }
'''
        result = integer_inputs(source)
        self.assertIn('uint3 a [[attribute(1)]]', result)
        self.assertIn('float2 uv [[attribute(5)]]', result)
        self.assertIn('int3(as_type<int3>(in.a << dm_vk_input_shift_1) >> dm_vk_input_shift_1)', result)
        self.assertIn('uint(as_type<int>(in.b << dm_vk_input_shift_4) >> dm_vk_input_shift_4)', result)
        self.assertEqual(re.findall(r'function_constant\((\d+)\)', result), ['7', '8', '9'])

    def test_no_integer_inputs(self):
        for source in ['kernel void test() {}', 'struct I { float4 p [[attribute(0)]]; }; vertex float4 test(I in [[stage_in]]) { return in.p; }']:
            self.assertEqual(integer_inputs(source), source)

    def test_index_overflow(self):
        source = 'constant uint x [[function_constant(65535)]]; struct I { int a [[attribute(0)]]; }; vertex int test(I in [[stage_in]]) { return in.a; }'
        with self.assertRaisesRegex(ValueError, 'indices left'):
            integer_inputs(source)


if __name__ == '__main__':
    unittest.main()
