"""A store only a dispatch's first invocation makes (dmsh_build.py first_store): recognised exactly, taken out of the shader for the host; anything else stays. The pattern is Splatoon Raiders' Hoian_Proc 029d9c3ec1fcb39f."""
import ast
from pathlib import Path
import re
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
selected = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name == 'first_store')
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) in ('FIRST_STORE', 'FIRST_STORE_2D') for t in n.targets))]
scope = {'re': re}
exec(compile(ast.Module(body=selected, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
first_store = scope['first_store']

BODY = """    float _59 = as_type<float>(gl_WorkGroupID.x);
    float _61 = as_type<float>(gl_LocalInvocationID.x);
    int _63 = as_type<int>(_59) << %d;
    int _65 = ~as_type<int>(_61);
    int _67 = _65 + 1;
    bool _71 = _67 %s _63;
    if (!_71)
    {
        (*spvDescriptorSet0.cp_s0)._m0[1] = 0u;
    }
    int _73 = as_type<int>(_61) + _63;
"""


class FirstStore(unittest.TestCase):
    def test_the_pattern(self):
        out, stores = first_store(BODY % (5, '!='), [32, 1, 1])
        self.assertEqual(stores, [('cp_s0', 1, 0)])
        self.assertNotIn('_m0[1] = 0u', out)
        self.assertNotIn('if (!_71)', out)
        self.assertIn('int _73 = as_type<int>(_61) + _63;', out)   # what the invocations compute from these goes on
        self.assertIn('bool _71 = _67 != _63;', out)

    def test_two_stores(self):
        src = (BODY % (5, '!=')).replace('= 0u;', '= 0u;\n        (*spvDescriptorSet0.cp_s2)._m0[3] = 7u;')
        self.assertEqual(first_store(src, [32, 1, 1])[1], [('cp_s0', 1, 0), ('cp_s2', 3, 7)])

    def test_other_shapes_stay(self):
        for src, local in ((BODY % (4, '!='), [32, 1, 1]),   # the shift is not the local size: not one invocation
                           (BODY % (5, '=='), [32, 1, 1]),   # the opposite condition: all but the first
                           (BODY % (5, '!='), [32, 2, 1]),   # a local size with rows: more than one invocation
                           (BODY % (5, '!='), None)):
            self.assertEqual(first_store(src, local), (src, []))
        computed = (BODY % (5, '!=')).replace('= 0u;', '= _63;')   # a value only the shader knows
        self.assertEqual(first_store(computed, [32, 1, 1]), (computed, []))


SPLIT = """    float _53 = as_type<float>(gl_WorkGroupID.x);
    float _55 = as_type<float>(gl_LocalInvocationID.x);
    int _57 = as_type<int>(_53) << 6;
    int _59 = ~as_type<int>(_55);
    int _61 = _59 + 1;
    bool _65 = _61 != _57;
    int _67 = _61;
    if (!_65)
    {
        _67 = 1;
    }
    int _69 = _67;
    if (!_65)
    {
        (*spvDescriptorSet0.cp_s0)._m0[0] = 0u;
    }
    if (!_65)
    {
        (*spvDescriptorSet0.cp_s0)._m0[2] = uint(_69);
    }
    if (!_65)
    {
        (*spvDescriptorSet0.cp_s0)._m0[3] = uint(_69);
    }
    if (!_65)
    {
        (*spvDescriptorSet0.cp_s0)._m0[1] = 0u;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    int _71 = as_type<int>(_55) + _57;
"""


class SplitBlocks(unittest.TestCase):
    """Mario Kart World's 2ce9071c2f22d74f and 5fea9b7ddb30e8a8: one guarded block a store, two of the values a local set under the same guard. Missed, a workgroup that appended before the
    reset lost its entries: the scenery list swung between rebuilds (406, 202, 797, 104) and whole groups of trees vanished"""

    def test_the_pattern(self):
        out, stores = first_store(SPLIT, [64, 1, 1])
        self.assertEqual(stores, [('cp_s0', 0, 0), ('cp_s0', 2, 1), ('cp_s0', 3, 1), ('cp_s0', 1, 0)])   # in the shader's order
        self.assertNotIn('_m0[', out)
        self.assertIn('int _69 = _67;', out)   # the local stays: it costs nothing and may be read later
        self.assertIn('threadgroup_barrier', out)
        self.assertIn('int _71 = as_type<int>(_55) + _57;', out)

    def test_a_value_only_the_shader_knows_leaves_it_all(self):
        src = SPLIT.replace('[2] = uint(_69)', '[2] = uint(_71)')
        self.assertEqual(first_store(src, [64, 1, 1]), (src, []))

    def test_a_guarded_store_further_on_leaves_it_all(self):
        src = SPLIT + '    if (!_65)\n    {\n        (*spvDescriptorSet0.cp_s0)._m0[9] = 2u;\n    }\n'
        self.assertEqual(first_store(src, [64, 1, 1]), (src, []))

    def test_more_than_the_host_keeps_leaves_it_all(self):
        src = SPLIT.replace('[1] = 0u;', '[1] = 0u;\n        (*spvDescriptorSet0.cp_s0)._m0[4] = 0u;')
        self.assertEqual(first_store(src, [64, 1, 1]), (src, []))

    def test_the_local_size_still_decides(self):
        self.assertEqual(first_store(SPLIT, [32, 1, 1]), (SPLIT, []))


ROWS = """    float _74 = as_type<float>(gl_WorkGroupID.y);
    float _76 = as_type<float>(gl_LocalInvocationID.y);
    float _78 = as_type<float>(gl_LocalInvocationID.x);
    float _80 = as_type<float>(gl_WorkGroupID.x);
    float _82 = as_type<float>(gl_WorkGroupID.z);
    int _84 = as_type<int>(_74) << 3;
    int _86 = ~as_type<int>(_76);
    int _88 = _86 + 1;
    bool _92 = _88 != _84;
    int _94 = as_type<int>(_80) << 3;
    int _96 = ~as_type<int>(_78);
    int _98 = _96 + 1;
    bool _100 = _98 == _94;
    bool _102 = !_92;
    bool _104 = _100 && _102;
    bool _106 = as_type<int>(_82) == 0;
    bool _108 = _106 && _104;
    int _110 = as_type<int>(_82) * 24;
    int _112 = _110 + 8;
    if (_108)
    {
        (*spvDescriptorSet0.cp_s0)._m0[1] = 0u;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
"""
SETUP = """    float _78 = as_type<float>(gl_WorkGroupID.y);
    float _80 = as_type<float>(gl_LocalInvocationID.y);
    float _82 = as_type<float>(gl_LocalInvocationID.x);
    float _84 = as_type<float>(gl_WorkGroupID.x);
    float _86 = as_type<float>(gl_WorkGroupID.z);
    int _88 = as_type<int>(_78) << 3;
    int _90 = ~as_type<int>(_80);
    int _91 = _90 + 1;
    bool _94 = _91 != _88;
    int _96 = as_type<int>(_84) << 3;
    int _97 = ~as_type<int>(_82);
    int _98 = _97 + 1;
    bool _100 = _98 == _96;
    bool _102 = !_94;
    bool _104 = _100 && _102;
    bool _106 = 0 == as_type<int>(_86);
    bool _108 = _106 && _104;
    bool _110 = !_108;
    if (!_110)
    {
        (*spvDescriptorSet0.cp_s0)._m0[0] = 0u;
        bool _112 = 0u < as_type<uint>((*spvDescriptorSet0.cp_c4)._m0[0].x);
    }
"""


class Rows(unittest.TestCase):
    """A local size with rows, Mario Kart World's 0823b08c2027a38d: the test on y and x and workgroup z zero is one invocation too. 649e36162cad9e79 does more than stores there: it stays"""

    def test_the_pattern(self):
        out, stores = first_store(ROWS, [8, 8, 1])
        self.assertEqual(stores, [('cp_s0', 1, 0)])
        self.assertNotIn('_m0[', out)
        self.assertIn('int _112 = _110 + 8;', out)
        self.assertIn('threadgroup_barrier', out)

    def test_the_shifts_must_be_the_local_size(self):
        for local in ([8, 4, 1], [4, 8, 1], [8, 8, 2], [64, 1, 1]):
            self.assertEqual(first_store(ROWS, local), (ROWS, []), local)

    def test_more_than_stores_stays(self):
        self.assertEqual(first_store(SETUP, [8, 8, 1]), (SETUP, []))


if __name__ == '__main__':
    unittest.main()
