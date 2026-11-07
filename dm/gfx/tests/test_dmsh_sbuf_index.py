#!/usr/bin/env python3
"""dmsh_build.py sbuf_scalar_index: a scalar storage-buffer load whose byte offset is computed and bounds-checked, then indexed at element 0, gets its index back
(Orbitals' GPUScene upload compute shaders scattered every primitive to slot 0: black 3D backgrounds). A plain element-0 read is left alone."""
from pathlib import Path
import ast
import re
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
selected = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name == 'sbuf_scalar_index')
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) == 'SBUF_SCALAR_INDEX' for t in n.targets))]
scope = {'re': re}
exec(compile(ast.Module(body=selected, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
sbuf_scalar_index = scope['sbuf_scalar_index']

# The shape SPIRV-Cross prints for the translator's miscompiled load (f0db173d46f9cdc5's PrimitiveUploadIndices[PrimitiveIndex])
LOST = """    int _120 = _118 * 4;
    bool _125 = uint(_120) >= as_type<uint>((*spvDescriptorSet0.cp_c1)._m0[2].z);
    bool _126 = !_125;
    int _127 = 0;
    if (_126)
    {
        uint _131 = as_type<uint>((*spvDescriptorSet0.cp_s0)._m0[0]);
"""


class ScalarIndex(unittest.TestCase):
    def test_lost_index_is_restored(self):
        out = sbuf_scalar_index(LOST)
        self.assertIn('(*spvDescriptorSet0.cp_s0)._m0[int(uint(_120) >> uint(2))]', out)
        self.assertNotIn('._m0[0])', out)

    def test_plain_element_zero_read_is_kept(self):
        plain = '    uint _9 = as_type<uint>((*spvDescriptorSet0.cp_s0)._m0[0]);\n'
        self.assertEqual(sbuf_scalar_index(plain), plain)

    def test_offset_not_checked_against_a_size_is_kept(self):
        other = LOST.replace('>= as_type<uint>((*spvDescriptorSet0.cp_c1)._m0[2].z)', '>= 16u')
        self.assertEqual(sbuf_scalar_index(other), other)


if __name__ == '__main__':
    unittest.main()
