"""fs_uint gives a pixel shader that computes only floats the integer entry point main0_u only when the profile lists it (uint_want.tsv):
Tears of the Kingdom's e3fb0bb58a691e10 draws Recall's R16Float mask and Ultrahand's R8Uint one, so its main0 must stay float and main0_u must exist beside it."""
import ast
import collections
from pathlib import Path
import re
from typing import Any
import unittest

REPO = Path(__file__).resolve().parents[3]
TOOLS = REPO / 'dm/gfx/tools'

FLOAT_ONLY = '''struct main0_out
{
    float4 m_64 [[color(0)]];
};

fragment main0_out main0(float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    float _10 = gl_FragCoord.x * 0.5;
    out.m_64.x = _10;
    out.m_64.y = 0.0;
    out.m_64.z = 0.0;
    out.m_64.w = 0.0;
    return out;
}
'''


class FsUintWant(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
        nodes: list[ast.stmt] = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ('fs_uint', 'int_outputs', 'in_word')]
        cls.ns: dict[str, Any] = {'re': re, 'collections': collections}
        exec(compile(ast.Module(body=nodes, type_ignores=[]), 'dmsh_build.py', 'exec'), cls.ns)

    def test_float_only_shader_gets_no_integer_entry_unless_listed(self):
        self.assertEqual(self.ns['fs_uint'](FLOAT_ONLY), (None, 0))

    def test_listed_shader_keeps_float_main0_and_gets_main0_u(self):
        src, mask = self.ns['fs_uint'](FLOAT_ONLY, True)
        self.assertEqual(mask, 0)
        self.assertIn('fragment main0_out main0(', src)
        self.assertIn('float4 m_64 [[color(0)]];', src.split('struct main0_out_u')[0], 'main0 stays float for float targets')
        self.assertRegex(src, r'struct main0_out_u\s*\{\s*uint4 m_64 \[\[color\(0\)\]\];')
        self.assertIn('fragment main0_out_u main0_u(', src)
        self.assertIn('as_type<uint4>(out.m_64)', src, 'main0_u returns the bits of the float outputs, as the hardware writes them to an integer target')


if __name__ == '__main__':
    unittest.main()
