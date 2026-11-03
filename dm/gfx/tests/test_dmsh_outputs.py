"""Constant material outputs retain bits in both float and integer MRT layouts."""
import ast
from pathlib import Path
import re
import unittest

builder = Path(__file__).resolve().parents[1] / 'tools/dmsh_build.py'
scope = {'re': re}
exec(compile(ast.Module(body=[n for n in ast.parse(builder.read_text()).body if isinstance(n, ast.FunctionDef) and n.name == 'fs_uint'], type_ignores=[]), str(builder), 'exec'), scope)

SOURCE = '''struct main0_out
{
    float4 color [[color(0)]];
    float4 normal [[color(1)]];
    float4 material [[color(3)]];
};
fragment main0_out main0(float4 value [[position]])
{
    main0_out out = {};
    out.color = value;
    out.normal = as_type<float4>(uint4(3u));
    out.material.x = 4.4435174303739949318991465086123e-41;
    out.material.y = 0.0;
    out.material.z = 0.0;
    out.material.w = 0.0;
    return out;
}
'''


class ConstantOutputs(unittest.TestCase):
    def test_mixed_constant_keeps_both_layouts(self):
        source, mask = scope['fs_uint'](SOURCE)
        self.assertEqual(mask, 2)
        self.assertIn('fragment main0_out_m main0_m(', source)
        self.assertIn('fragment main0_out_mc0a main0_mc0a(', source)
        self.assertIn('as_type<uint4>(out.material)', source)
        self.assertIn('4.4435174303739949318991465086123e-41', source)

    def test_variable_float_output_is_not_optional(self):
        source, _ = scope['fs_uint'](SOURCE.replace('out.material.x = 4.4435174303739949318991465086123e-41;', 'out.material.x = value.x;'))
        self.assertNotIn('main0_mc', source)

    def test_multiple_constant_outputs_cover_independent_masks(self):
        source, mask = scope['fs_uint'](SOURCE.replace('out.color = value;', 'out.color = float4(0.0);'))
        self.assertEqual(mask, 2)
        self.assertIn('main0_mc03(', source)
        self.assertIn('main0_mc0a(', source)
        self.assertNotIn('main0_mc0b(', source)  # all-uint already has main0_u


if __name__ == '__main__':
    unittest.main()
