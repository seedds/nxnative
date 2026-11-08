"""dmsh_build.py frag_z_snap on synthetic MSL in the translator's shapes: a gl_FragCoord.z read of main0 that meets a value read from a texture in one comparison (Ocarina of Time's mist:
z > d after the denormal flush; its foliage: z * k > d * k; a body moved to dm_body_main0) takes dm_zsnap(gl_FragCoord.z, d, dm_zw), with dm_zw = fwidth(gl_FragCoord.z) first in the body
and the helper once; z against constants or uniforms, a texture value read after z or in a block z does not see, and z read outside main0's body stay as they were. The rewritten source
compiles where xcrun is. With --fixture <dir>, writes the helper as zsnap.h for t_zsnap.m (make check_t_zsnap), which tests it on the GPU against the depth the rasterizer stores."""
import ast
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_side   # noqa: E402


def load():
    tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
    nodes: list[ast.stmt] = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name == 'frag_z_snap')
                             or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) in ('ZSNAP', 'ZSNAP_WIDTH') for t in n.targets))]
    ns = {'re': re, 'dmsh_side': dmsh_side}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), ns)
    return ns


NS = load()
HEAD = '''#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct spvDescriptorSetBuffer0
{
    texture2d<float> fp_t_tcb_A [[id(0)]];
    sampler fp_t_tcb_ASmplr [[id(1)]];
    constant float4* fp_c3 [[id(2)]];
};

struct main0_out
{
    float4 m_16 [[color(0)]];
};
'''
# Ocarina of Time's mist (5ad65c0eefd09c7f), cut down: its own depth test against the depth its depth-only pass stored
MIST = HEAD + '''
fragment main0_out main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    float2 _7217 = float2(gl_FragCoord.x / 768.0, gl_FragCoord.y / 432.0);
    float _112 = spvDescriptorSet0.fp_t_tcb_A.sample(spvDescriptorSet0.fp_t_tcb_ASmplr, _7217, level(0.0)).x;
    float _114 = gl_FragCoord.z;
    float _124 = abs(_114);
    bool _128 = _124 < 1.1754943508222875079687365372222e-38;
    float _130 = _128 ? 0.0 : _114;
    float _132 = abs(_112);
    bool _134 = _132 < 1.1754943508222875079687365372222e-38;
    float _136 = _134 ? 0.0 : _112;
    bool _138 = _130 > _136;
    if (_138)
    {
        discard_fragment();
    }
    out.m_16 = float4(1.0);
    return out;
}
'''
# Its foliage (0419a82951b7ab81), as a library holds it: both sides scaled by one uniform, the body in dm_body_main0 (dmsh_side.shared_body)
FOLIAGE = HEAD + '''
static __attribute__((always_inline)) main0_out dm_body_main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0, float4 gl_FragCoord)
{
    main0_out out = {};
    float2 _432 = float2(gl_FragCoord.x / 768.0, gl_FragCoord.y / 432.0);
    float _71 = spvDescriptorSet0.fp_t_tcb_A.sample(spvDescriptorSet0.fp_t_tcb_ASmplr, _432).x;
    if (_71 > 2.0)
    {
        out.m_16 = float4(0.0);
        return out;
    }
    float _308 = gl_FragCoord.z;
    float _310 = _71 * spvDescriptorSet0.fp_c3[188].w;
    float _312 = _308 * spvDescriptorSet0.fp_c3[188].w;
    bool _326 = _312 > _310;
    if (_326)
    {
        out.m_16 = float4(1.0);
        return out;
    }
    out.m_16 = float4(0.5);
    return out;
}

fragment main0_out main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], float4 gl_FragCoord [[position]])
{
    main0_out out = dm_body_main0(spvDescriptorSet0, gl_FragCoord);
    return out;
}
'''


def body_of(msl):
    e = dmsh_side.entry(msl, 'main0')
    return msl[msl.find('{', e['head']) + 1:e['end'] - 1]


class FragZSnap(unittest.TestCase):
    def test_mist(self):
        out, n = NS['frag_z_snap'](MIST)
        self.assertEqual(n, 1)
        self.assertIn('float _114 = dm_zsnap(gl_FragCoord.z, _112, dm_zw);', out)
        self.assertTrue(body_of(out).lstrip().startswith('float dm_zw = fwidth(gl_FragCoord.z);'))
        self.assertEqual(out.count('float dm_zsnap('), 1)
        self.assertEqual(out.replace(NS['ZSNAP'], '').replace('\n    float dm_zw = fwidth(gl_FragCoord.z);', '').replace('dm_zsnap(gl_FragCoord.z, _112, dm_zw)', 'gl_FragCoord.z'),
                         MIST.replace('using namespace metal;\n', 'using namespace metal;\n\n'))   # nothing else changes

    def test_scaled_in_shared_body(self):
        out, n = NS['frag_z_snap'](FOLIAGE)
        self.assertEqual(n, 1)
        self.assertIn('float _308 = dm_zsnap(gl_FragCoord.z, _71, dm_zw);', out)
        self.assertIn('{\n    float dm_zw = fwidth(gl_FragCoord.z);\n    main0_out out = {};\n    float2 _432', out)

    def test_left_alone(self):
        cases = {
            'a uniform': MIST.replace('float _112 = spvDescriptorSet0.fp_t_tcb_A.sample(spvDescriptorSet0.fp_t_tcb_ASmplr, _7217, level(0.0)).x;', 'float _112 = spvDescriptorSet0.fp_c3[3].x;'),
            'a constant': MIST.replace('bool _138 = _130 > _136;', 'bool _138 = _130 > 0.5;'),
            'a texture value read after z': MIST.replace('    float _112 = spvDescriptorSet0.fp_t_tcb_A.sample(spvDescriptorSet0.fp_t_tcb_ASmplr, _7217, level(0.0)).x;\n    float _114 = gl_FragCoord.z;\n',
                                                         '    float _114 = gl_FragCoord.z;\n    float _112 = spvDescriptorSet0.fp_t_tcb_A.sample(spvDescriptorSet0.fp_t_tcb_ASmplr, _7217, level(0.0)).x;\n'),
            'a texture value read after z, in a block': MIST.replace('    float _112 = spvDescriptorSet0.fp_t_tcb_A.sample(spvDescriptorSet0.fp_t_tcb_ASmplr, _7217, level(0.0)).x;\n    float _114 = gl_FragCoord.z;\n',
                                                                    '    float _114 = gl_FragCoord.z;\n').replace('    float _132 = abs(_112);\n',
                                                                    '    if (_7217.x > 0.5)\n    {\n    float _112 = spvDescriptorSet0.fp_t_tcb_A.sample(spvDescriptorSet0.fp_t_tcb_ASmplr, _7217, level(0.0)).x;\n    float _132 = abs(_112);\n').replace(
                                                                    '        discard_fragment();\n    }\n', '        discard_fragment();\n    }\n    }\n'),
            'a texture value with z in it': MIST.replace('float _136 = _134 ? 0.0 : _112;', 'float _136 = _134 ? 0.0 : (_112 + _114);'),
            'no gl_FragCoord.z': MIST.replace('gl_FragCoord.z', 'gl_FragCoord.w'),
        }
        for why, msl in cases.items():
            with self.subTest(why):
                self.assertEqual(NS['frag_z_snap'](msl), (msl, 0))

    @unittest.skipUnless(shutil.which('xcrun'), 'no xcrun')
    def test_compiles(self):
        with tempfile.TemporaryDirectory() as d:
            for name, msl in (('mist', MIST), ('foliage', FOLIAGE), ('mist_shared', dmsh_side.shared_body(MIST))):
                src = os.path.join(d, name + '.metal'); Path(src).write_text(NS['frag_z_snap'](msl)[0])
                r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', src, '-o', os.path.join(d, name + '.air')], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr[-800:])


def fixture(out):
    os.makedirs(out, exist_ok=True)
    Path(out, 'zsnap.h').write_text(NS['ZSNAP'])


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--fixture': fixture(sys.argv[2])
    else: unittest.main()
