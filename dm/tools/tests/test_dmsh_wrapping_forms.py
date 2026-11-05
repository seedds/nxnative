"""MSL pattern matchers of the build accept both spellings of integer add / multiply: the signed one (`_5 + 1`, `as_type<int>(_9) + 1568`) and the wrapping
one the translator writes since xcheck-fixes 5eba12cf (`int(uint(_5) + 1u)`, `int(as_type<uint>(_9) + 1568u)`). With only the first, 65 PRAGMATA pixel
shaders lost drv_reads sample_table; the storage-index repair (sbuf_scalar_index), the first-thread store test, the wide loads (dmsh_wide) and the lean
geometry kernel (dmsh_geometry.lean) matched the same way."""
import ast
from pathlib import Path
import re
import sys
import unittest
from typing import Any

TOOLS = Path(__file__).resolve().parents[2] / 'gfx/tools'
sys.path.insert(0, str(TOOLS))
import dmsh_abi as abi   # noqa: E402
import dmsh_wide   # noqa: E402
import dmsh_geometry   # noqa: E402

FUNCS = {'drv_reads', 'sbuf_scalar_index'}
NAMES = {'SBUF_SCALAR_INDEX', 'FIRST_STORE', 'FIRST_STORE_2D'}


def load():
    tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
    nodes: list[ast.stmt] = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name in FUNCS)
                             or (isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id in NAMES for t in n.targets))]
    ns: dict[str, Any] = {'re': re, 'abi': abi}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), ns)
    assert FUNCS | NAMES <= set(ns), (FUNCS | NAMES) - set(ns)
    return ns


NS = load()
T = abi.C0_SAMPLE_TABLE


def sample_ps(add):
    return f'''fragment main0_out main0(uint gl_SampleID [[sample_id]])
{{
    float _104 = as_type<float>(gl_SampleID);
    int _124 = {add};
    float4 _130 = (*spvDescriptorSet0.fp_c0)._m0[uint(_124) >> 4u];
}}
'''


class SampleTable(unittest.TestCase):
    def test_both_spellings(self):
        for add in (f'as_type<int>(_104) + {T}', f'int(as_type<uint>(_104) + {T}u)'):
            self.assertEqual(NS['drv_reads'](sample_ps(add)), {'0': {'vec4_offsets': [], 'dynamic': True, 'sample_table': True}}, add)

    def test_other_offset(self):
        self.assertNotIn('sample_table', NS['drv_reads'](sample_ps(f'int(as_type<uint>(_104) + {T + 16}u)'))['0'])


SBUF = '''    int _40 = {mul};
    bool _41 = uint(_40) >= as_type<uint>((*spvDescriptorSet0.cp_c3)._m0[2].z);
    bool _42 = !_41;
    int _43 = 0;
    if (_42)
    {{
        float _44 = as_type<float>((*spvDescriptorSet0.cp_s0)._m0[0]);
'''


class StorageIndex(unittest.TestCase):
    def test_both_spellings(self):
        for mul in ('_39 * 4', 'int(uint(_39) * 4u)'):
            self.assertIn('cp_s0)._m0[int(uint(_40) >> uint(2))]);', NS['sbuf_scalar_index'](SBUF.format(mul=mul)), mul)


FIRST = '''float _10 = as_type<float>(gl_WorkGroupID.x);
    float _11 = as_type<float>(gl_LocalInvocationID.x);
    int _12 = as_type<int>(_10) << 6;
    int _13 = ~as_type<int>(_11);
    int _14 = {inc};
    bool _15 = _14 != _12;
'''


class FirstStore(unittest.TestCase):
    def test_both_spellings(self):
        for inc in ('_13 + 1', 'int(uint(_13) + 1u)'):
            self.assertTrue(NS['FIRST_STORE'].search(FIRST.format(inc=inc)), inc)


WIDE = '''struct spvDescriptorSetBuffer0
{{
    const device _20* cp_s1 [[id(0)]];
}};
kernel void main0()
{{
    int _50 = {a4};
    int _51 = {a8};
    uint _60 = uint(int(uint(_49) >> uint(2)));
    uint _61 = uint(int(uint(_50) >> uint(2)));
    uint _62 = uint(int(uint(_51) >> uint(2)));
    float _70 = as_type<float>((*spvDescriptorSet0.cp_s1)._m0[int(_60)]);
    float _71 = as_type<float>((*spvDescriptorSet0.cp_s1)._m0[int(_61)]);
    float _72 = as_type<float>((*spvDescriptorSet0.cp_s1)._m0[int(_62)]);
}}
'''


class WideLoads(unittest.TestCase):
    def test_both_spellings(self):
        for a4, a8 in (('_49 + 4', '_49 + 8'), ('int(uint(_49) + 4u)', 'int(uint(_49) + 8u)'), ('int(uint(_49) + 4u)', 'int(uint(_50) - 4294967292u)')):
            out = dmsh_wide.wide_loads(WIDE.format(a4=a4, a8=a8))
            self.assertIn('packed_uint3 _dmw0 = ', out, (a4, a8))
            self.assertIn('as_type<float>(_dmw0.z)', out, (a4, a8))

    def test_negative_constant(self):
        self.assertEqual(dmsh_wide._add('    int _5 = int(uint(_4) + 4294967292u);'), ('_5', '_4', -4))
        self.assertEqual(dmsh_wide._add('    int _5 = int(_4 - 32u);'), ('_5', '_4', -32))
        self.assertEqual(dmsh_wide._add('    int _5 = _4 + -32;'), ('_5', '_4', -32))


GEO = '''struct spvDescriptorSetBuffer0
{{
    device _9* vertex_output [[id(0)]];
    device _9* geometry_vb_output [[id(1)]];
}};
kernel void main0(uint3 gl_GlobalInvocationID [[thread_position_in_grid]])
{{
    int _10 = int(gl_GlobalInvocationID.x);
    int _20 = {mul};
    int _21 = {add_out};
    int _30 = {mul_in};
    int _31 = {add_in};
    uint _40 = (*spvDescriptorSet0.vertex_output)._m0[_31];
    (*spvDescriptorSet0.geometry_vb_output)._m0[_21] = _40;
}}
'''


class LeanGeometry(unittest.TestCase):
    def test_both_spellings(self):
        S, W, M = 8, dmsh_geometry.WORDS, dmsh_geometry.MASK_WORD
        forms = (dict(mul=f'_10 * {S}', add_out=f'_20 + {M}', mul_in=f'_10 * {W}', add_in='_30 + 7'),
                 dict(mul=f'int(uint(_10) * {S}u)', add_out=f'int(uint(_20) + {M}u)', mul_in=f'int(uint(_10) * {W}u)', add_in='int(uint(_30) + 7u)'))
        outs = []
        for f in forms:
            src, reads = dmsh_geometry.lean(GEO.format(**f), S)
            self.assertEqual(reads, [7], f)
            outs.append(src)
        self.assertIn('int _31 = _30 + 0;', outs[0]); self.assertIn('int _30 = _10 * 1;', outs[0])
        self.assertIn('int _31 = int(uint(_30) + 0u);', outs[1]); self.assertIn('int _30 = int(uint(_10) * 1u);', outs[1])


if __name__ == '__main__':
    unittest.main()
