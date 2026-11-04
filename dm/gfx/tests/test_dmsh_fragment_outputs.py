"""Non-finite floating shader outputs (dmsh_build.py): every return of a floating colour or user varying is wrapped, including early and struct-literal returns;
integer, bit-pattern, position and layer members stay; finite_fragment_base restores the source dmsh_fetch.py needs."""
import ast
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / 'dm/gfx/tools/dmsh_build.py'
FUNCS = {'_finite_component_helpers', '_finite_outputs', 'finite_fragment_outputs', 'finite_vertex_outputs', 'finite_fragment_strip', 'finite_fragment_base'}


def load():
    tree = ast.parse(BUILD.read_text())
    nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in FUNCS]
    ns = {'re': re}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(BUILD), 'exec'), ns)
    return ns


NS = load()
fragment, vertex = NS['finite_fragment_outputs'], NS['finite_vertex_outputs']


class FragmentOutputs(unittest.TestCase):
    def test_returns_are_sanitized_and_fetch_gets_its_source_back(self):
        source = '''using namespace metal;
struct main0_out
{
    float4 color [[color(0)]];
    float4 bits [[color(1)]];
    uint4 id [[color(2)]];
};
fragment main0_out main0()
{
    main0_out out = {};
    out.bits = as_type<float4>(uint4(1u));
    if (out.id.x == 0) return out;
    return main0_out{out.color, out.bits, out.id};
}
struct main0_out_d
{
    float4 color [[color(0)]];
};
'''
        result = fragment(source)
        self.assertEqual(result.count('return dm_finite_out_main0_out('), 2)
        self.assertIn('v.color = dm_finite_output(v.color);', result)
        self.assertNotIn('v.bits = dm_finite_output(v.bits);', result)
        self.assertNotIn('v.id = dm_finite_output(v.id);', result)
        self.assertLess(result.index('static inline main0_out dm_finite_out_main0_out'), result.index('fragment main0_out main0'))
        base = NS['finite_fragment_base'](result)
        self.assertIn('return out;', base)
        self.assertNotIn('main0_out_d', base)
        self.assertNotIn('dm_finite_output', base)


class VertexOutputs(unittest.TestCase):
    def test_varyings_are_sanitized_but_positions_and_layers_are_not(self):
        source = '''using namespace metal;
struct main0_out
{
    float4 color [[user(locn0)]];
    float4 gl_Position [[position]];
    uint id [[user(locn1)]];
};
struct main0l_out
{
    float4 color [[user(locn0)]];
    uint gl_Layer [[render_target_array_index]];
};
vertex main0_out main0()
{
    main0_out out = {};
    if (out.id.x == 0) return out;
    return out;
}
vertex main0l_out main0_l()
{
    main0l_out out = {};
    return out;
}
'''
        result = vertex(source)
        self.assertEqual(result.count('return dm_finite_vout_main0_out(out);'), 2)
        self.assertIn('return dm_finite_vout_main0l_out(out);', result)
        self.assertIn('v.color = dm_finite_vertex(v.color);', result)
        self.assertNotIn('v.gl_Position', result)
        self.assertNotIn('v.id = dm_finite_vertex(v.id);', result)
        self.assertNotIn('v.gl_Layer', result)


if __name__ == '__main__':
    unittest.main()
