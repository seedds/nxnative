"""Pulling vertex inputs (dm/gfx/tools/dmsh_pull.py): small vertex shaders in the translator's MSL shape, no game needed. The variant must drop [[stage_in]] from main0 and main0_z,
add the layout block at buffer 16 and the vertex / instance / base instance ids it lacks (keeping the ones the shader declares), fill every input through dm_fetch with as_type to
its declared type, add the wide-line entries (main0_wl, main0_z_wl: the body as a function run for both endpoints of the corner's segment, its outputs interpolated when the
segment is clipped; no entries when a parameter or an output is one it cannot handle), leave the other entry points out, decline shaders it does not fit, and compile with the
Metal compiler for macOS and iOS (temporary files only in a temporary folder). t_wline.m draws with the wide-line entry on the GPU."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_pull

def shader(types, extra='', ids='', tf=False, gs=False, ret=False):
    """types: the MSL type of each input in location order ('float4' / 'uint4' / 'int4'); ids: extra parameters of main0 / main0_z (e.g. a [[vertex_id]]); ret: more outputs
    (an interpolated one before the position, a flat one, clip distances) and an early return"""
    ins = ''.join(f'    {t} m_{l} [[attribute({l})]];\n' for l, t in enumerate(types))
    use = ''.join(f'    acc += float4(as_type<float4>(in.m_{l}));\n' if t != 'float4' else f'    acc += in.m_{l};\n' for l, t in enumerate(types))
    early = '    if (acc.x > 5.0)\n    {\n        return out;\n    }\n    out.c = acc;\n    out.f = acc;\n    out.gl_ClipDistance[1] = acc.y;\n' if ret else ''
    main = lambda name: (f'\nvertex main0_out {name}(main0_in in [[stage_in]], constant float4& u [[buffer(0)]]{ids})\n{{\n    main0_out out = {{}};\n    float4 acc = u;\n{use}{early}'
                         f'    out.gl_Position = acc;\n    return out;\n}}\n')
    outs = ('    float4 c [[user(locn0)]];\n    float4 gl_Position [[position, invariant]];\n    float4 f [[user(locn1), flat]];\n    float gl_ClipDistance [[clip_distance]] [2];\n'
            if ret else '    float4 gl_Position [[position, invariant]];\n')
    src = ('#include <metal_stdlib>\n#include <simd/simd.h>\n\nusing namespace metal;\n\nstruct main0_out\n{\n' + outs + '};\n\n'
           'struct main0_in\n{\n' + ins.rstrip('\n') + '\n};\n' + extra)
    src += main('main0') + main('main0_z')
    if tf: src += '\nvertex void main0_tf(main0_in in [[stage_in]], constant float4& u [[buffer(0)]], uint dm_vid [[vertex_id]], device uint* dm_tf0 [[buffer(8)]])\n{\n    dm_tf0[dm_vid] = 0u;\n}\n'
    if gs: src += '\nvertex void main0_gs(main0_in in [[stage_in]], constant float4& u [[buffer(0)]], uint dm_vid [[vertex_id]])\n{\n    (void)in;\n}\n'
    return src

class PullingInputs(unittest.TestCase):
    def compiles(self, msl):
        with tempfile.TemporaryDirectory(prefix='dmsh_pull_test_') as d:
            p = Path(d) / 'v.metal'; p.write_text(msl)
            for sdk in ('macosx', 'iphoneos'):
                r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-fpreserve-invariance', str(p), '-o', str(Path(d) / f'v.{sdk}.air')], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, f'{sdk}: {r.stderr[:600]}')

    def test_inputs_through_the_layout_block(self):
        v, info = dmsh_pull.variant(shader(['float4', 'uint4', 'int4']))
        self.assertEqual(info, {'entry': 'main0', 'entry_z': 'main0_z', 'slot': 16, 'inputs': [0, 1, 2], 'entry_wl': 'main0_wl', 'entry_z_wl': 'main0_z_wl'})
        self.assertNotIn('[[stage_in]]', v)
        self.assertNotIn('[[attribute(', v)
        self.assertEqual(v.count('constant dm_vlayout& dm_lay [[buffer(16)]]'), 4)   # main0 and main0_z, and their wide-line entries
        self.assertEqual(v.count('    in.m_1 = as_type<uint4>(dm_fetch(dm_lay, 1u, dm_vid, dm_iid, dm_bi));'), 2)
        self.assertEqual(v.count('    in.m_2 = as_type<int4>(dm_fetch(dm_lay, 2u, dm_vid, dm_iid, dm_bi));'), 2)
        self.assertEqual(v.count('    in.m_2 = as_type<int4>(dm_fetch(dm_lay, 2u, dm_v, dm_i, dm_b));'), 1)   # dm_wl_in, which both wide-line entries call
        self.assertEqual(v.count('uint dm_vid [[vertex_id]], uint dm_iid [[instance_id]], uint dm_bi [[base_instance]]'), 2)
        self.compiles(v)

    def test_keeps_the_ids_the_shader_declares(self):
        v, info = dmsh_pull.variant(shader(['float4'], ids=', uint gl_InstanceIndex [[instance_id]], uint gl_BaseInstance [[base_instance]], uint gl_VertexIndex [[vertex_id]]'))
        self.assertEqual(v.count('dm_fetch(dm_lay, 0u, gl_VertexIndex, gl_InstanceIndex, gl_BaseInstance)'), 2)
        self.assertEqual(v.count('dm_wlb_main0(dm_wl_in(dm_lay, dm_e.x, gl_InstanceIndex, gl_BaseInstance), u, gl_InstanceIndex, gl_BaseInstance, dm_e.x, dm_lay)'), 1)   # the shader's own ids, its vertex ID the endpoint's
        self.assertNotIn('uint dm_vid [[vertex_id]]', v); self.assertNotIn('uint dm_iid [[instance_id]]', v); self.assertNotIn('uint dm_bi [[base_instance]]', v)   # no second id
        self.compiles(v)

    def test_wide_line_entry(self):
        v, info = dmsh_pull.variant(shader(['float4', 'uint4'], ret=True))
        self.assertEqual((info['entry_wl'], info['entry_z_wl']), ('main0_wl', 'main0_z_wl'))
        for e in ('main0', 'main0_z'):
            f = v[v.index(f'vertex main0_out {e}_wl('):]; f = f[:f.index('\n}\n')]
            self.assertIn('uint dm_wcorner [[vertex_id]]', f); self.assertEqual(f.count('[[vertex_id]]'), 1)   # the corner, not the endpoint
            self.assertIn('constant dm_wline& dm_wl [[buffer(15)]]', f)
            self.assertIn(f'main0_out out = dm_wlb_{e}(dm_wl_in(dm_lay, dm_e.x, dm_iid, dm_bi), u, dm_lay, dm_e.x, dm_iid, dm_bi), dm_o = dm_wlb_{e}(dm_wl_in(dm_lay, dm_e.y, dm_iid, dm_bi)', f)
            self.assertIn('if (dm_t.x > 0.0f) out = dm_wl_mix(out, dm_o, dm_t.x);', f)
            self.assertIn('out.gl_Position = dm_wl_offset(out.gl_Position, mix(dm_q, dm_p, dm_t.y), dm_wl, dm_e.z);', f)
            g = v[v.index(f'static inline main0_out dm_wlb_{e}(main0_in in, constant float4& u, constant dm_vlayout& dm_lay, uint dm_vid, uint dm_iid, uint dm_bi)\n{{'):]
            self.assertEqual(g[:g.index('\n}\n')].count('return out;'), 2)   # the body unchanged, its early return included
        self.assertIn('    r.c = mix(a.c, b.c, t);\n    r.gl_Position = mix(a.gl_Position, b.gl_Position, t);\n', v)
        self.assertIn('for (int i = 0; i < 2; i++) r.gl_ClipDistance[i] = mix(a.gl_ClipDistance[i], b.gl_ClipDistance[i], t);', v)
        self.assertNotIn('r.f =', v)   # a flat output keeps its own endpoint's value
        self.assertEqual(v.count('struct dm_wline'), 1); self.assertEqual(v.count('static inline main0_in dm_wl_in('), 1)
        self.compiles(v)

    def test_wide_line_template_parameter(self):
        v, info = dmsh_pull.variant(shader(['float4'], ids=', array<texture2d<float>, 2> t [[texture(0)]], sampler s [[sampler(0)]]'))
        self.assertEqual(info['entry_wl'], 'main0_wl')
        self.assertIn('constant float4& u, array<texture2d<float>, 2> t, sampler s, constant dm_vlayout& dm_lay', v)
        self.compiles(v)

    def test_no_wide_line_entry(self):
        v, info = dmsh_pull.variant(shader(['float4'], ids=', uint gl_ViewIndex [[amplification_id]]'))   # a parameter the entry cannot pass on
        self.assertEqual(info['entry'], 'main0'); self.assertNotIn('entry_wl', info); self.assertNotIn('_wl(', v)
        self.compiles(v)

    def test_other_entry_points_left_out(self):
        v, info = dmsh_pull.variant(shader(['float4', 'float4'], tf=True, gs=True))
        self.assertNotIn('main0_tf', v); self.assertNotIn('main0_gs', v)
        self.compiles(v)

    def test_declined(self):
        no_input = shader(['float4']).replace('struct main0_in\n{\n    float4 m_0 [[attribute(0)]];\n};\n', '').replace('main0_in in [[stage_in]], ', '')
        self.assertEqual(dmsh_pull.variant(no_input), (None, 'no stage input'))
        self.assertEqual(dmsh_pull.variant(shader(['float4'], extra='constant float4& x [[buffer(16)]];\n'))[1], 'buffer 16 is taken')
        self.assertEqual(dmsh_pull.variant(shader(['float4']).replace('[[attribute(0)]]', '[[attribute(17)]]'))[1], 'an input at attribute 16 or above')

if __name__ == '__main__':
    unittest.main()
