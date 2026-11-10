"""dmsh_rewrite.py frag_kill on synthetic MSL in the translator's shapes: a discard in main0's body followed by NVIDIA's own exit on the same predicate (Mario Tennis Fever's Donkey Kong fur,
ec0405e7fb48caf8: KILL, then the outputs zeroed and an early return) gets dm_killed set by the discard and the early return taken only while it is false; main0's last return stays as it was,
also in a body moved to dm_body_main0 and in an else branch. A shader without a discard, or with no return before its last one, and a discard outside main0's body stay as they were.
The rewritten source compiles where xcrun is."""
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


import dmsh_rewrite   # noqa: E402

NS = {'frag_kill': dmsh_rewrite.frag_kill}
HEAD = '''#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct spvDescriptorSetBuffer0
{
    texture2d<float> fp_t_tcb_E [[id(0)]];
    sampler fp_t_tcb_ESmplr [[id(1)]];
    constant float4* fp_c8 [[id(2)]];
};

struct main0_out
{
    float4 m_63 [[color(0)]];
};

struct main0_in
{
    float4 m_58 [[user(locn0)]];
};
'''
# Donkey Kong's fur (ec0405e7fb48caf8), cut down: the alpha test's KILL, then the same test's exit with zeroed outputs
FUR = HEAD + '''
fragment main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    float4 _191 = spvDescriptorSet0.fp_t_tcb_E.sample(spvDescriptorSet0.fp_t_tcb_ESmplr, in.m_58.xy);
    float _211 = _191.w * spvDescriptorSet0.fp_c8[0].w;
    bool _235 = _211 > spvDescriptorSet0.fp_c8[26].x;
    if (!_235)
    {
        discard_fragment();
    }
    if (!_235)
    {
        out.m_63.x = 0.0;
        out.m_63.y = 0.0;
        out.m_63.z = 0.0;
        out.m_63.w = 0.0;
        return out;
    }
    out.m_63 = float4(_191.xyz, _211);
    return out;
}
'''


def body_of(msl):
    e = dmsh_side.entry(msl, 'main0')
    return msl[msl.find('{', e['head']) + 1:e['end'] - 1]


class FragKill(unittest.TestCase):
    def test_fur(self):
        out, n = NS['frag_kill'](FUR)
        self.assertEqual(n, 1)
        body = body_of(out)
        self.assertTrue(body.startswith('\n    bool dm_killed = false;\n    main0_out out = {};'))
        self.assertIn('{ discard_fragment(); dm_killed = true; }', body)
        self.assertIn('        out.m_63.w = 0.0;\n        { if (!dm_killed) return out; }\n', body)
        self.assertTrue(body.endswith('\n    out.m_63 = float4(_191.xyz, _211);\n    return out;\n'))   # the last return as it was
        self.assertEqual(out.replace('\n    bool dm_killed = false;', '', 1).replace('{ discard_fragment(); dm_killed = true; }', 'discard_fragment();')
                         .replace('{ if (!dm_killed) return out; }', 'return out;'), FUR)   # nothing else changes

    def test_shared_body(self):
        out, n = NS['frag_kill'](dmsh_side.shared_body(FUR))
        self.assertEqual(n, 1)
        self.assertIn('main0_out dm_body_main0(', out)
        self.assertEqual(out.count('dm_killed = true;'), 1)
        self.assertIn('{\n    main0_out out = dm_body_main0(in, spvDescriptorSet0, gl_FragCoord);\n    return out;\n}', out)   # the calling entry point is not touched

    def test_braceless_if_and_else(self):
        msl = FUR.replace('    if (!_235)\n    {\n        out.m_63.x = 0.0;\n        out.m_63.y = 0.0;\n        out.m_63.z = 0.0;\n        out.m_63.w = 0.0;\n        return out;\n    }\n',
                          '    if (!_235) return out; else out.m_63.x = 1.0;\n')
        out, n = NS['frag_kill'](msl)
        self.assertEqual(n, 1)
        self.assertIn('if (!_235) { if (!dm_killed) return out; } else out.m_63.x = 1.0;', out)   # the else still belongs to the outer if

    def test_left_alone(self):
        cases = {
            'no discard': FUR.replace('        discard_fragment();\n', ''),
            'no early return': FUR.replace('        out.m_63.w = 0.0;\n        return out;\n', '        out.m_63.w = 0.0;\n'),
            'a discard outside main0': FUR.replace('        discard_fragment();\n', '        out.m_63.x = 2.0;\n') + '\nstatic void dm_other() { discard_fragment(); }\n',
        }
        for why, msl in cases.items():
            with self.subTest(why):
                self.assertEqual(NS['frag_kill'](msl), (msl, 0))

    @unittest.skipUnless(shutil.which('xcrun'), 'no xcrun')
    def test_compiles(self):
        with tempfile.TemporaryDirectory() as d:
            for name, msl in (('fur', FUR), ('fur_shared', dmsh_side.shared_body(FUR))):
                src = os.path.join(d, name + '.metal'); Path(src).write_text(NS['frag_kill'](msl)[0])
                r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', '-fwrapv', src, '-o', os.path.join(d, name + '.air')],
                                   capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr[-800:])


if __name__ == '__main__':
    unittest.main()
