"""The scans of c1_guard.py and cb_extent.py give what the plain ones gave: definitions made only when an index asks for one, the assignment scan with its literal first, and a
binding's uses counted with str.find. No game files (synthetic sources)."""
from pathlib import Path
import re
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import c1_guard, cb_extent, dmsh_abi, dmsh_entry, dmsh_geometry

VERTEX = '''#include <metal_stdlib>
using namespace metal;
struct vp_c3_t { float4 _m0[4096]; };
struct spvDescriptorSetBuffer0 { constant vp_c3_t* vp_c3 [[id(0)]]; constant vp_c3_t* vp_c1 [[id(1)]]; constant vp_c3_t* vp_c5 [[id(2)]]; device vp_c3_t* vp_s0 [[id(3)]]; };
struct main0_out
{
    float4 out_attr0 [[user(locn0)]];
    float4 gl_Position [[position, invariant]];
};
struct main0_in { float4 in_attr0 [[attribute(0)]]; };
vertex main0_out main0(main0_in in, constant spvDescriptorSetBuffer0& spvDescriptorSet0, uint gl_VertexIndex [[vertex_id]])
{
    main0_out out = {};
    uint _5 = gl_VertexIndex & 3u;
    uint _7 = 1u;
    out.gl_Position = (*spvDescriptorSet0.vp_c3)._m0[_5 + 2u] + (*spvDescriptorSet0.vp_c1)._m0[_5] + (*spvDescriptorSet0.vp_s0)._m0[1];
    if (in.in_attr0.x < 0.0)
    {
        _7 = 2u;
        return out;
    }
    out.out_attr0 = (*spvDescriptorSet0.vp_c3)._m0[7] + (*spvDescriptorSet0.vp_c5)._m0[_7] + float4(vp_c5_scale);
    return out;
}
'''
def built(source):   # the three entry points as dmsh_build.py writes them
    capture = dmsh_geometry.capture(source)
    return dmsh_entry.vs_depth(source)[1] + capture


def plain_definitions(source):   # definitions_of as it was: every form of ASSIGNMENT, the declarations counted in a scan of their own
    writes, decls, counts = {}, {}, {}
    for m in c1_guard.ASSIGNMENT.finditer(source):
        name = m.group(1) or m.group(2) or m.group(3); writes[name] = writes.get(name, 0) + 1
    for kind, name, text in c1_guard.DEFINITION.findall(source): decls.setdefault(name, set()).add((kind, text.strip()))
    for _, name, _ in c1_guard.DEFINITION.findall(source): counts[name] = counts.get(name, 0) + 1
    return {name: next(iter(d)) for name, d in decls.items() if len(d) == 1 and writes.get(name, 0) == counts[name]}


class Scans(unittest.TestCase):
    def test_assignments_with_the_literal_first(self):
        for text in (built(VERTEX), 'uint _1 = 2u; _1 += 3u; a._2 = 1; x_3 = 2; _4 == 5; _6 <<= 1; bool _7 = _1 >= _6; _8\n = 1; int _9 = 0;',
                     'int _1 = 0; _1++; --_2; int _3 = _1-- - 2; uint _4 = 1u;', 'int _1 = 0; for (int _2 = 0; _2 < 4; _2++) { _1 = _2; }'):
            with self.subTest(text[:40]): self.assertEqual(c1_guard.definitions_of(text), plain_definitions(text))
        self.assertEqual(c1_guard.definitions_of('uint _1 = 2u; int _9 = 0; _1 += 3u;'), {'_9': ('int', '0')})

    def test_definitions_are_made_only_for_a_temporary(self):
        source = 'uint _5 = in.v & 3u;\nuint _6 = _5 + 1u;\n'
        with mock.patch.object(c1_guard, 'definitions_of', wraps=c1_guard.definitions_of) as made:
            definitions = c1_guard.Definitions(source)
            self.assertTrue(c1_guard.proven('3', definitions, 512)); self.assertFalse(c1_guard.proven('in.v', definitions, 512)); self.assertEqual(made.call_count, 0)
            self.assertTrue(c1_guard.proven('_6', definitions, 512)); self.assertTrue(c1_guard.proven('_5 + 4u', definitions, 512)); self.assertEqual(made.call_count, 1)
        self.assertEqual(c1_guard.Interval(c1_guard.Definitions(source)).expression('_6'), c1_guard.Interval(c1_guard.definitions_of(source)).expression('_6'))

    def test_uses_counted_with_find(self):
        source = 'vp_c1 [[id(0)]]; vp_c10 [[id(1)]]; xvp_c1 = (*s.vp_c1)._m0[1]; vp_c1\t [[id(2)]]; f(vp_c1); vp_c1_x; s.vp_c1[[id(3)]] évp_c1 vp_c1é vp_c1'
        for name in ('vp_c1', 'vp_c10', 'vp_c', 's.vp_c1', 'missing', 'c1)', ''):
            with self.subTest(name):
                want = len(re.findall(r'\b' + re.escape(name) + r'\b', source)), len(re.findall(r'\b' + re.escape(name) + r'\s*\[\[id\(', source))
                self.assertEqual(cb_extent.uses_of(source, name), want)
        self.assertEqual(cb_extent.uses_of(source, 'vp_c1'), (6, 3))


if __name__ == '__main__':
    unittest.main()
