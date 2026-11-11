"""tools/dmsh_rscale.py: the internal-resolution facts the shader build writes into the reflection, and the checker that compares them with the MSL.
Synthetic inputs written in the shape the translator and spirv-cross produce; no game data. Run: python3 -I dm/gfx/tests/test_rscale.py"""
import os
import sys
import unittest
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import dmsh_reflect   # noqa: E402
import dmsh_rscale as rs   # noqa: E402

RES = {'textures': [{'binding': 3, 'cbuf_slot': -1, 'handle': 8, 'array': 1, 'flags': 2},
                    {'binding': 5, 'cbuf_slot': 2, 'handle': 0x40000000, 'array': 32, 'flags': 0},
                    {'binding': 6, 'cbuf_slot': -1, 'handle': 0xE, 'array': 1, 'flags': 0}],
       'images': [{'binding': 4, 'cbuf_slot': -1, 'handle': 0x48, 'array': 1, 'flags': 5}],
       'uses_frag_coord': True}
BINDS = [{'msl_name': 'support_buffer', 'kind': 'support', 'arg_id': 0},
         {'msl_name': 'fp_t_tcb_8', 'kind': 'tex', 'arg_id': 2},
         {'msl_name': 'fp_t_tcb_8Smplr', 'kind': 'samp_tex', 'arg_id': 3},
         {'msl_name': 'fp_i_tcb_48_rgba8', 'kind': 'img', 'arg_id': 4},
         {'msl_name': 'fp_i_tcb_48_rgba8_atomic', 'kind': 'img', 'arg_id': 5},
         {'msl_name': 'fp_t_cb2_40000000', 'kind': 'tex', 'arg_id': 6},
         {'msl_name': 'fp_t_tcb_E', 'kind': 'tex', 'arg_id': 7}]

SUPPORT = '''struct _10
{
    uint _m0;
    uint4 _m1[8];
    float4 _m2;
    float4 _m3;
    int _m4;
    float4 _m5[73];
    int4 _m6;
    int _m7;
};
'''
HELPERS = '''
static inline __attribute__((always_inline))
int _40(thread const int& _1, thread const int& _2, constant _10& support_buffer)
{
    int _3 = 1 + _2;
    float _4 = support_buffer._m5[_3].x;
    float _5 = abs(_4);
    return int(float(_1) / _5);
}

static inline __attribute__((always_inline))
int _42(thread const int& _6, thread const int& _7, thread const int& _8, constant _10& support_buffer, thread float4& gl_FragCoord)
{
    int _9 = 1 + _7;
    float _11 = support_buffer._m5[_9].x;
    return int(float(_6) * _11);
}
'''


def msl(fetch_idx=0, size_idx=0, struct=SUPPORT):
    return struct + '''
struct spvDescriptorSetBuffer0
{
    constant _10* support_buffer [[id(0)]];
    texture2d<float> fp_t_tcb_8 [[id(2)]];
};
''' + HELPERS + f'''
fragment main0_out main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], float4 gl_FragCoord [[position]])
{{
    float _20 = gl_FragCoord.x / (*spvDescriptorSet0.support_buffer)._m5[0].x;
    int _21 = int2(spvDescriptorSet0.fp_t_tcb_8.get_width(), spvDescriptorSet0.fp_t_tcb_8.get_height()).x;
    int _22 = _21;
    int _23 = {size_idx};
    int _24 = _40(_22, _23, (*spvDescriptorSet0.support_buffer));
    int _25 = {fetch_idx};
    int _26 = _25;
    int _27 = 0;
    int _28 = _42(_24, _26, _27, (*spvDescriptorSet0.support_buffer), gl_FragCoord);
    int _29 = _28;
    float4 _30 = spvDescriptorSet0.fp_t_tcb_8.read(uint2(int2(_29, _29)), 0);
}}
'''


class Facts(unittest.TestCase):
    def test_indices_follow_the_translator_rule(self):
        out = {b['msl_name']: b for b in rs.facts(BINDS, RES)}
        self.assertEqual(out['fp_t_tcb_8']['sidx'], 0)              # texture 0
        self.assertNotIn('sidx', out['fp_t_cb2_40000000'])          # an array: never scaled
        self.assertEqual(out['fp_t_tcb_E']['sidx'], 2)              # texture 2 (arrays keep their place in the list)
        self.assertEqual(out['fp_i_tcb_48_rgba8']['sidx'], 2)       # image 0 after the two non-array textures
        self.assertEqual(out['fp_t_tcb_8']['sfl'], 2)
        self.assertEqual(out['fp_i_tcb_48_rgba8']['sfl'], 5)

    def test_other_members_get_nothing(self):
        out = {b['msl_name']: b for b in rs.facts(BINDS, RES)}
        for n in ('support_buffer', 'fp_t_tcb_8Smplr', 'fp_i_tcb_48_rgba8_atomic'):
            self.assertNotIn('sidx', out[n]); self.assertNotIn('sfl', out[n])

    def test_input_not_changed(self):
        rs.facts(BINDS, RES)
        self.assertTrue(all('sidx' not in b for b in BINDS))

    def test_same_handle_twice_matches_in_binding_order(self):
        res = {'textures': [{'binding': 1, 'cbuf_slot': -1, 'handle': 8, 'array': 1, 'flags': 0},
                            {'binding': 2, 'cbuf_slot': -1, 'handle': 8, 'array': 1, 'flags': 2}]}
        out = rs.facts([{'msl_name': 'fp_t_tcb_8'}, {'msl_name': 'fp_t_tcb_8_1'}], res)
        self.assertEqual([b['sidx'] for b in out], [0, 1])

    def test_no_index_past_render_scale(self):
        res = {'textures': [{'binding': i, 'cbuf_slot': -1, 'handle': i, 'array': 1, 'flags': 0} for i in range(74)]}
        out = rs.facts([{'msl_name': f'fp_t_tcb_{i:X}'} for i in range(74)], res)
        self.assertEqual(out[71]['sidx'], 71)
        self.assertNotIn('sidx', out[72])


    def test_reflect_writes_the_facts(self):   # dmsh_reflect.reflect, the production reflection, carries them per binding
        src = 'struct spvDescriptorSetBuffer0\n{\n    texture2d<float> fp_t_tcb_8 [[id(0)]];\n    sampler fp_t_tcb_8Smplr [[id(1)]];\n};\n'
        got = dmsh_reflect.reflect('synthetic', 'ps', src, {'sbufs': [], 'textures': [{'binding': 3, 'cbuf_slot': -1, 'handle': 8, 'array': 1, 'flags': 2}]})
        self.assertEqual([(b['msl_name'], b.get('sidx'), b.get('sfl')) for b in got], [('fp_t_tcb_8', 0, 2), ('fp_t_tcb_8Smplr', None, None)])


class Check(unittest.TestCase):
    def refl(self, sidx=0, frag=1):
        return {'frag_coord': frag, 'bindings': [{'msl_name': 'support_buffer'}, {'msl_name': 'fp_t_tcb_8', 'sidx': sidx}]}

    def test_agreeing_facts_pass(self):
        self.assertEqual(rs.check(msl(), self.refl()), [])

    def test_wrong_index_is_reported_per_helper(self):
        bad = rs.check(msl(fetch_idx=1), self.refl())
        self.assertEqual(len(bad), 1)
        self.assertIn('render_scale[1 + 1]', bad[0])
        self.assertEqual(len(rs.check(msl(), self.refl(sidx=3))), 2)   # both helpers disagree

    def test_missing_frag_coord_is_reported(self):
        self.assertEqual(len(rs.check(msl(), self.refl(frag=0))), 1)

    def test_unpadded_support_layout_is_reported(self):
        packed = SUPPORT.replace('uint4 _m1[8]', 'uint _m1[8]').replace('float4 _m5[73]', 'float _m5[73]')
        bad = rs.check(msl(struct=packed), self.refl())
        self.assertTrue(any('byte 192' in b for b in bad))

    def test_shader_without_scale_code_passes(self):
        self.assertEqual(rs.check('fragment main0_out main0() {}', {'bindings': []}), [])


if __name__ == '__main__':
    unittest.main()
