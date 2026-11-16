"""tap_grid (dm/gfx/tools/dmsh_rewrite.py): a pixel shader that writes out 49 taps of one texture in a 7x7 grid becomes a loop of the same taps.
Apple's GPU compiler hoists the fetches of 49 written-out taps and runs out of registers: Echoes of Wisdom's save and file select screens' ArchiveShader-Blur pass took 48.8 ms and the
screen fell from 60 to 20 fps, while the same taps in an unrolled-off loop take 1.2-2.9 ms. No game files, no Metal compiler."""
from pathlib import Path
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
from dmsh_rewrite import tap_grid  # noqa: E402

HEAD = ('#include <metal_stdlib>\nusing namespace metal;\n\n'
        'struct spvDescriptorSetBuffer0\n{\n'
        '    constant _16* fp_c3 [[id(0)]];\n'
        '    texture2d<float> fp_t [[id(2)]];\n'
        '    sampler fp_tSmplr [[id(3)]];\n'
        '    sampler fp_tSmplr_w [[id(4)]];\n'
        '    constant dm_border_t* dm_border [[id(5)]];\n'
        '};\n\n'
        'struct main0_out\n{\n    float4 m_30 [[color(0)]];\n};\n\n'
        'struct main0_in\n{\n    float4 m_27 [[user(locn1)]];\n};\n\n')
STEP_X = '(*spvDescriptorSet0.fp_c3)._m0[9].x'
STEP_Y = '(*spvDescriptorSet0.fp_c3)._m0[9].y'


def shader(taps=49, tex='fp_t', samp='fp_tSmplr', vary=True, out_ty='float4', fragment=True, guard=True, steps=3):
    """The blur as spirv-cross writes it, cut down: the coordinate, 49 taps whose coordinates are fma(step, k, coordinate) chains, four weighted sums of their channels, a blend"""
    n = 10
    body = '    float _1 = in.m_27.x;\n    float _2 = in.m_27.y;\n' if vary else '    float _1 = 0.5;\n    float _2 = 0.5;\n'
    for t in range(taps):
        k, j = (t % (2 * steps + 1) - steps, t // (2 * steps + 1) - steps) if taps == (2 * steps + 1) ** 2 else (0, 0)
        x = f'fma({STEP_X}, {k}.0, _1)' if k else '_1'
        y = f'fma({STEP_Y}, {j}.0, _2)' if j else '_2'
        body += f'    float2 _{n} = float2({x}, {y});\n'
        uv = f'select(_{n}, float2(0.0), isnan(_{n}))' if guard else f'_{n}'
        body += (f'    float4 _{n + 1} = dm_bs(spvDescriptorSet0.{tex}.sample(spvDescriptorSet0.{samp}, {uv}), spvDescriptorSet0.dm_border, 0u, '
                 f'spvDescriptorSet0.{tex}, spvDescriptorSet0.{samp}_w, {uv});\n')
        n += 2
    body += '    out.m_30.x = _11.x;\n    out.m_30.y = _11.y;\n    out.m_30.z = _11.z;\n    out.m_30.w = _11.w;\n'
    body += '    dm_blend0(out, dm_blend);\n    return out;\n' if fragment else '    return out;\n'
    head = ('fragment main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], constant dm_blend_state& dm_blend [[buffer(28)]])\n'
            if fragment else 'kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])\n')
    out = HEAD.replace('float4 m_30 [[color(0)]]', f'{out_ty} m_30 [[color(0)]]')
    return out + head + '{\n    main0_out out = {};\n' + body + '}\n'


class TapGrid(unittest.TestCase):
    def test_49_taps_become_a_loop(self):
        msl = tap_grid(shader())
        self.assertIn(f'float2 st = float2({STEP_X}, {STEP_Y});', msl)   # the body's own steps
        self.assertIn('#pragma clang loop unroll(disable)\n    for (int j = -3; j <= 3; ++j)', msl)
        self.assertIn('#pragma clang loop unroll(disable)\n        for (int k = -3; k <= 3; ++k)', msl)
        self.assertIn('float2 uv = in.m_27.xy + st * float2((float)k, (float)j);', msl)
        self.assertIn('acc += dm_bs(spvDescriptorSet0.fp_t.sample(spvDescriptorSet0.fp_tSmplr, suv), spvDescriptorSet0.dm_border, 0u, '
                      'spvDescriptorSet0.fp_t, spvDescriptorSet0.fp_tSmplr_w, suv);', msl)
        self.assertIn('out.m_30 = acc * (1.0 / 49.0);', msl)
        self.assertEqual(msl.count('dm_bs('), 1)   # 49 calls, written once

    def test_the_nan_guard_and_the_statements_after_the_writes_stay(self):
        msl = tap_grid(shader())
        self.assertIn('select(uv, float2(0.0), isnan(uv))', msl)
        self.assertIn('    out.m_30 = acc * (1.0 / 49.0);\n    dm_blend0(out, dm_blend);\n    return out;\n}', msl)

    def test_a_body_that_does_not_fit_comes_back_unchanged(self):
        for msl in (shader(taps=48), shader(taps=25), shader(steps=1), shader(out_ty='float'), shader(fragment=False), shader(guard=False), shader(vary=False)):
            self.assertEqual(tap_grid(msl), msl)

    def test_two_textures_are_not_one_grid(self):
        msl = shader().replace('spvDescriptorSet0.fp_t.sample', 'spvDescriptorSet0.other.sample', 1)
        self.assertEqual(tap_grid(msl), msl)

    def test_a_plain_fragment_is_left_alone(self):
        plain = HEAD + 'fragment float4 main0()\n{\n    return float4(1.0);\n}\n'
        self.assertEqual(tap_grid(plain), plain)


if __name__ == '__main__':
    unittest.main()
