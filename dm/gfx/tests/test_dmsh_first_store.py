"""A store only a dispatch's first invocation makes (dmsh_build.py first_store): recognised exactly, taken out of the shader for the host; anything else stays. Patterns cover Splatoon Raiders' single reset and Mario Kart World's four split resets."""
import ast
from pathlib import Path
import re
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
selected = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name == 'first_store')
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) == 'FIRST_STORE' for t in n.targets))]
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

    def test_split_stores_with_conditional_constant(self):
        # MKW uses one predicate for a constant assignment, then four stores.
        src = (BODY % (6, '!=')).replace('    if (!_71)', '    int v = _67;\n    if (!_71) { v = 1; }\n    int copy = v;\n    if (!_71)', 1)
        src = src.replace('._m0[1] = 0u;', '._m0[0] = 0u;')
        at = src.index('    int _73')
        src = src[:at] + '    if (!_71) { (*spvDescriptorSet0.cp_s0)._m0[2] = uint(copy); }\n    if (!_71) { (*spvDescriptorSet0.cp_s0)._m0[3] = uint(copy); }\n    if (!_71) { (*spvDescriptorSet0.cp_s0)._m0[1] = 0u; }\n    threadgroup_barrier(mem_flags::mem_threadgroup);\n' + src[at:]
        out, stores = first_store(src, [64, 1, 1])
        self.assertEqual(stores, [('cp_s0', 0, 0), ('cp_s0', 2, 1), ('cp_s0', 3, 1), ('cp_s0', 1, 0)])
        self.assertNotIn('._m0[', out)
        self.assertIn('if (!_71) { v = 1; }', out)
        self.assertIn('threadgroup_barrier', out)
        # Refuse the entire rewrite if one store depends on an unknown value.
        bad = src.replace('v = 1;', 'v = external;')
        self.assertEqual(first_store(bad, [64, 1, 1]), (bad, []))
        bad = src.replace('uint(copy)', 'uint(unknown)')
        self.assertEqual(first_store(bad, [64, 1, 1]), (bad, []))
        # The reflection has room for four, never silently drop a fifth store.
        bad = src.replace('threadgroup_barrier', 'if (!_71) { (*spvDescriptorSet0.cp_s1)._m0[0] = 8u; }\n    threadgroup_barrier')
        self.assertEqual(first_store(bad, [64, 1, 1]), (bad, []))

    def test_other_shapes_stay(self):
        for src, local in ((BODY % (4, '!='), [32, 1, 1]),   # the shift is not the local size: not one invocation
                           (BODY % (5, '=='), [32, 1, 1]),   # the opposite condition: all but the first
                           (BODY % (5, '!='), [32, 2, 1]),   # a local size with rows: more than one invocation
                           (BODY % (5, '!='), None)):
            self.assertEqual(first_store(src, local), (src, []))
        computed = (BODY % (5, '!=')).replace('= 0u;', '= _63;')   # a value only the shader knows
        self.assertEqual(first_store(computed, [32, 1, 1]), (computed, []))
        read = (BODY % (5, '!=')).replace('    if (!_71)', '    uint prior = (*args.cp_s0)._m0[1];\n    if (!_71)', 1)
        self.assertEqual(first_store(read, [32, 1, 1]), (read, []))


if __name__ == '__main__':
    unittest.main()
