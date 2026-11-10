"""A vertex shader's layer_out (dm/gfx/tools/dmsh_build.py layer_out): whether main0 writes the layer, the one fact the host (shader_load, pre_desc) and pso_offline.py read for it.
Layer entry points (vs_layer_entry) name the layer in the library without main0 writing it: Ocarina of Time's effect vertex shaders were left out of the pipeline archive while the
layer's name anywhere in the library counted. No game files, no Metal compiler."""
import ast
from pathlib import Path
import re
from typing import Any
import unittest

TOOLS = Path(__file__).resolve().parents[3] / 'dm/gfx/tools'
ns: dict[str, Any] = {'re': re}   # dmsh_build.py is a script that runs a build when imported
exec(compile(ast.Module(body=[n for n in ast.parse((TOOLS / 'dmsh_build.py').read_text()).body if isinstance(n, ast.FunctionDef)
                              and n.name in {'layer_out', 'vs_layer', 'vs_layer_entry', 'drop_point_size', 'vs_depth'}], type_ignores=[]), 'dmsh_build.py', 'exec'), ns)
layer_out, vs_layer, vs_layer_entry, vs_depth = ns['layer_out'], ns['vs_layer'], ns['vs_layer_entry'], ns['vs_depth']

HEAD = '#include <metal_stdlib>\nusing namespace metal;\n\n'


def shader(outputs, body):
    return (HEAD + 'struct main0_out\n{\n' + ''.join(f'    {o};\n' for o in outputs) + '};\n\n'
            'vertex main0_out main0(uint gl_InstanceIndex [[instance_id]])\n{\n    main0_out out = {};\n' + body + '    return out;\n}\n')


PLAIN = shader(['float4 gl_Position [[position]]', 'float gl_PointSize [[point_size]]'], '    out.gl_Position = float4(0.0);\n    out.gl_PointSize = 1.0;\n')
LAYERED = shader(['float4 gl_Position [[position]]', 'uint gl_Layer [[render_target_array_index]]', 'float gl_PointSize [[point_size]]'],
                 '    out.gl_Position = float4(0.0);\n    out.gl_Layer = gl_InstanceIndex;\n    out.gl_PointSize = 1.0;\n')
SLICE = shader(['float4 gl_Position [[position]]', 'float4 m_1 [[user(locn1)]]', 'float gl_PointSize [[point_size]]'],
               '    out.gl_Position = float4(0.0);\n    out.m_1.x = as_type<float>(int(gl_InstanceIndex));\n    out.gl_PointSize = 1.0;\n')


class LayerOut(unittest.TestCase):
    def test_main0_without_the_layer(self):
        self.assertFalse(layer_out(PLAIN))

    def test_main0_writing_the_layer(self):
        self.assertTrue(layer_out(LAYERED))
        self.assertTrue(layer_out(vs_layer(LAYERED)))   # without its point size (vs_layer), still the layer

    def test_layer_entry_points_do_not_count(self):
        single, full = vs_depth(SLICE)
        full, lay = vs_layer_entry(full)
        self.assertEqual(lay, {'loc': 1, 'comp': 'x'})
        self.assertIn('[[render_target_array_index]]', full)   # the library names the layer (main0_l, main0_lz) ...
        self.assertFalse(layer_out(single))   # ... main0, from which the reflection takes it, writes none
        self.assertFalse(layer_out(full))

    def test_vs_layer_drops_the_point_size_only_beside_the_layer(self):
        self.assertEqual(vs_layer(PLAIN), PLAIN)
        self.assertNotIn('[[point_size]]', vs_layer(LAYERED))

    def test_no_output_struct(self):
        self.assertFalse(layer_out(HEAD + 'kernel void main0() {}\n'))


if __name__ == '__main__':
    unittest.main()
