"""A vertex shader's far fact (dm/gfx/tools/dmsh_reflect.py far_plane, raster_facts): whether main0 puts every vertex on the far plane (gl_Position z = w), which the host draws
with a depth bias towards the far value and depth clamping (nvn_mtl.m draw; docs/reference/rendering.md, Depth on the far plane). Mario Kart 8 Deluxe's sky in Sky-High Sundae
showed red blocks where Apple's rasterizer gave it depths just below 1. No game files, no Metal compiler."""
from pathlib import Path
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[3] / 'dm/gfx/tools'
sys.path.insert(0, str(TOOLS))
import dmsh_reflect  # noqa: E402

GL = '    out.gl_Position.z = fma(out.gl_Position.z, 0.5, out.gl_Position.w * 0.5);\n'


def shader(body):
    return ('#include <metal_stdlib>\nusing namespace metal;\n\nstruct main0_out\n{\n    float4 gl_Position [[position]];\n};\n\n'
            'struct main0_in\n{\n    float4 vin_attr0 [[attribute(0)]];\n};\n\n'
            'vertex main0_out main0(main0_in in [[stage_in]])\n{\n    main0_out out = {};\n' + body + '    return out;\n}\n')


def far(body):
    return dmsh_reflect.raster_facts('vs', shader(body)).get('far', 0)


XY = '    out.gl_Position.x = in.vin_attr0.x;\n    out.gl_Position.y = in.vin_attr0.y;\n'


class FarPlane(unittest.TestCase):
    def test_register_on_z_and_w(self):
        self.assertEqual(far('    float _12 = in.vin_attr0.w;\n' + XY + '    out.gl_Position.z = _12;\n    out.gl_Position.w = _12;\n'), 1)

    def test_gl_depth_convention_keeps_it(self):
        self.assertEqual(far('    float _12 = in.vin_attr0.w;\n' + XY + '    out.gl_Position.z = _12;\n    out.gl_Position.w = _12;\n' + GL), 1)

    def test_constant_one(self):
        self.assertEqual(far(XY + '    out.gl_Position.z = 1.0;\n    out.gl_Position.w = 1.0;\n'), 1)

    def test_zero_is_not_the_far_plane(self):
        self.assertEqual(far(XY + '    out.gl_Position.z = 0.0;\n    out.gl_Position.w = 0.0;\n'), 0)

    def test_different_z_and_w(self):
        self.assertEqual(far(XY + '    out.gl_Position.z = in.vin_attr0.z;\n    out.gl_Position.w = in.vin_attr0.w;\n'), 0)

    def test_register_written_twice(self):
        self.assertEqual(far('    float _12 = in.vin_attr0.w;\n    _12 = in.vin_attr0.z;\n' + XY + '    out.gl_Position.z = _12;\n    out.gl_Position.w = _12;\n'), 0)

    def test_branch(self):
        self.assertEqual(far('    float _12 = in.vin_attr0.w;\n    if (_12 > 0.0) { _12 = 1.0; }\n' + XY + '    out.gl_Position.z = _12;\n    out.gl_Position.w = _12;\n'), 0)

    def test_whole_position_store(self):
        self.assertEqual(far('    out.gl_Position = in.vin_attr0;\n    out.gl_Position.z = 1.0;\n    out.gl_Position.w = 1.0;\n'), 0)

    def test_pixel_shader_has_no_far_fact(self):
        self.assertNotIn('far', dmsh_reflect.raster_facts('ps', shader('')))


if __name__ == '__main__':
    unittest.main()
