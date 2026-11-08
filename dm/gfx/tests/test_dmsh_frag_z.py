"""A fragment shader's depth against the depth the same surface stored (dmsh_build.py frag_z): the comparisons a same-surface pixel must fail take position.z one ULP down, and no others change."""
import ast
from pathlib import Path
import re
import struct
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
selected = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'frag_z']
scope = {'re': re}
exec(compile(ast.Module(body=selected, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
frag_z = scope['frag_z']

DOWN = 'as_type<float>(as_type<uint>({0}) - uint({0} > 0.0))'


def shader(line):
    return f'''fragment main0_out main0(float4 gl_FragCoord [[position]])
{{
    float _106 = gl_FragCoord.z;
    float _116 = abs(_106);
    bool _120 = _116 < 1.17e-38;
    float _122 = _120 ? 0.0 : _106;
    float _128 = _104;
    float _5 = _7 * 2.0;
    {line}
}}
'''


class FragZ(unittest.TestCase):
    def test_comparisons_that_take_z_down(self):
        for line, want in (('bool _130 = _122 > _128;', f'bool _130 = {DOWN.format("_122")} > _128;'),
                           ('bool _130 = _106 > _128;', f'bool _130 = {DOWN.format("_106")} > _128;'),
                           ('bool _130 = _122 <= _128;', f'bool _130 = {DOWN.format("_122")} <= _128;'),
                           ('bool _130 = _128 < _122;', f'bool _130 = _128 < {DOWN.format("_122")};'),
                           ('bool _130 = _128 >= _122;', f'bool _130 = _128 >= {DOWN.format("_122")};')):
            with self.subTest(line):
                self.assertIn(want, frag_z(shader(line)))

    def test_comparisons_that_already_give_the_maxwell_answer(self):
        for line in ('bool _130 = _122 >= _128;', 'bool _130 = _122 < _128;', 'bool _130 = _128 > _122;', 'bool _130 = _128 <= _122;'):
            with self.subTest(line):
                self.assertEqual(frag_z(shader(line)), shader(line))

    def test_values_that_are_not_the_depth(self):
        src = shader('bool _130 = _5 > _128;')
        self.assertEqual(frag_z(src), src)
        src = shader('bool _130 = _122 > _106;')   # both are the depth
        self.assertEqual(frag_z(src), src)
        src = shader('float _130 = _122 * _128;')   # a linearization
        self.assertEqual(frag_z(src), src)

    def test_a_same_surface_pixel_gets_the_maxwell_answer(self):
        # the rewritten expression, evaluated as Metal does with 32-bit floats: as_type<uint> / as_type<float> reinterpret the bits, uint(bool) is 0 or 1
        expr = re.search(r'bool _130 = (.*) > _128;', frag_z(shader('bool _130 = _106 > _128;'))).group(1)
        bits = lambda f: struct.unpack('<I', struct.pack('<f', f))[0]
        flt = lambda u: struct.unpack('<f', struct.pack('<I', u & 0xffffffff))[0]
        py = expr.replace('as_type<uint>(', 'bits(').replace('as_type<float>(', 'flt(').replace('uint(', 'int(').replace('_106', 'z')
        py = re.sub(r'\b(\d+)u\b', r'\1', py)   # an unsigned literal, should the expression use one
        for z in (0.0, flt(1), flt(0x00800000), 0.25, flt(bits(0.5) + 1), 1.0):   # 0, the smallest denormal and normal, a value one ULP above 0.5, the far plane
            for stored in (z, flt(bits(z) - 1) if z > 0.0 else z):   # what the rasterizer stored for the same surface: equal, or one ULP below
                d = eval(py, {'flt': flt, 'bits': bits, 'z': z})
                with self.subTest(z=z, stored=stored):
                    self.assertFalse(d > stored)   # z > stored must fail
                    self.assertTrue(d <= stored)   # z <= stored must pass
            if z > 0.0:   # a surface two ULP nearer is another surface: it must still fail z <= stored
                nearer = flt(bits(z) - 2) if bits(z) >= 2 else 0.0
                d = eval(py, {'flt': flt, 'bits': bits, 'z': z})
                with self.subTest(z=z, stored=nearer):
                    self.assertTrue(d > nearer or nearer == 0.0 == d)

    def test_a_shader_that_does_not_read_it(self):
        src = 'fragment main0_out main0()\n{\n    bool _1 = _2 > _3;\n}\n'
        self.assertEqual(frag_z(src), src)

    def test_a_copy_of_something_else_is_not_the_depth(self):
        src = shader('float _9 = _8 ? 0.0 : _5;\n    bool _130 = _9 > _128;')
        self.assertEqual(frag_z(src), src)


if __name__ == '__main__':
    unittest.main()
