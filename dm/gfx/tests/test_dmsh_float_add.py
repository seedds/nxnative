#!/usr/bin/env python3
"""dmsh_rewrite.py float_add_msl: a float add to storage memory (RED.F32's compare-and-swap loop, InstEmitMemory EmitAtomicAddF32) as one native atomic_fetch_add on a device
atomic_float. The loop as spirv-cross writes it is replaced and any other loop is left alone; a later read of its retry flag still gets false. On this Mac's GPU 8,192 threads
adding into the same four words, through the loop and through the rewrite, reach the exact sums. test_dmsh_ampere_alu.py FloatRed checks that the translator's own loop is the one rewritten (PR #1930)."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(TOOLS))
import dmsh_rewrite
float_add_msl = dmsh_rewrite.float_add_msl

# The loop of 6e28359aa3558f61 (Ocarina of Time), on word w of buffer b, adding v
def loop(b, w, v, n):
    return f"""    bool _{n};
    do
    {{
        float _{n + 1} = as_type<float>({b}._m0[{w}]);
        float _{n + 2} = _{n + 1} + {v};
        uint _{n + 3} = as_type<uint>(_{n + 1});
        uint _{n + 4};
        do
        {{
            _{n + 4} = _{n + 3};
        }} while (!atomic_compare_exchange_weak_explicit((device atomic_uint*)&{b}._m0[{w}], &_{n + 4}, as_type<uint>(_{n + 2}), memory_order_relaxed, memory_order_relaxed) && _{n + 4} == _{n + 3});
        uint _{n + 5} = _{n + 4};
        _{n} = int(_{n + 5}) != as_type<int>(_{n + 1});
    }} while (_{n});
"""

KERNEL = """#include <metal_stdlib>
using namespace metal;
struct B { uint _m0[1]; };
kernel void k(device B& b [[buffer(0)]], uint id [[thread_position_in_grid]])
{
    float v = float(id & 7u) * 0.25f, w = 1.0f;
%s}
"""


class FloatAdd(unittest.TestCase):
    def test_loop_becomes_one_atomic_add(self):
        src = loop('(*spvDescriptorSet0.cp_s4)', 0, '_2580', 10) + loop('(*spvDescriptorSet0.cp_s4)', 'int(_7)', '_2572', 20)
        out = float_add_msl(src)
        self.assertEqual(out, '    atomic_fetch_add_explicit((device atomic_float*)&(*spvDescriptorSet0.cp_s4)._m0[0], _2580, memory_order_relaxed);\n'
                              '    bool _10 = false;\n'
                              '    atomic_fetch_add_explicit((device atomic_float*)&(*spvDescriptorSet0.cp_s4)._m0[int(_7)], _2572, memory_order_relaxed);\n'
                              '    bool _20 = false;\n')

    def test_exact_add_too(self):   # the translator's exact form (XLATE_REDUCED=0): spvFAdd(old, v)
        src = loop('b', 0, 'v', 10).replace('_11 + v;', 'spvFAdd(_11, v);')
        self.assertEqual(float_add_msl(src), '    atomic_fetch_add_explicit((device atomic_float*)&b._m0[0], v, memory_order_relaxed);\n'
                                           '    bool _10 = false;\n')

    def test_live_retry_flag_is_false_after_add(self):
        tail = '    b._m0[0] = uint(_10);\n'
        for add in ('_11 + v', 'spvFAdd(_11, v)'):
            with self.subTest(add=add):
                src = loop('b', 0, 'v', 10).replace('_11 + v', add) + tail
                out = float_add_msl(src)
                self.assertNotIn('atomic_compare_exchange_weak_explicit', out)
                self.assertIn('    bool _10 = false;\n', out)   # the do-while can only finish with this value
                self.assertTrue(out.endswith(tail))

    def test_other_words_and_shapes_left_alone(self):
        other = loop('b', 0, 'v', 10).replace('&b._m0[0], &', '&b._m0[1], &')   # the swap on another word than the read
        self.assertEqual(float_add_msl(other), other)

    def test_bounded_loop_becomes_guarded_atomic_add(self):
        src = (
            '    bool _46;\n'
            '    do\n'
            '    {\n'
            '        bool _71 = (uint(0) + (*spvDescriptorSet0.cp_c27)._m0[0].x) < (*spvDescriptorSet0.cp_c27)._m0[0].y;\n'
            '        float _48 = as_type<float>(_71 ? (*spvDescriptorSet0.cp_s0)._m0[_71 ? 0 : 0] : 0u);\n'
            '        float _50 = _48 + _42;\n'
            '        bool _87 = (uint(0) + (*spvDescriptorSet0.cp_c27)._m0[0].x) < (*spvDescriptorSet0.cp_c27)._m0[0].y;\n'
            '        uint _91 = as_type<uint>(_48);\n'
            '        uint _100;\n'
            '        if (_87)\n'
            '        {\n'
            '            uint _97;\n'
            '            do\n'
            '            {\n'
            '                _97 = _91;\n'
            '            } while (!atomic_compare_exchange_weak_explicit((device atomic_uint*)&(*spvDescriptorSet0.cp_s0)._m0[_87 ? 0 : 0], &_97, as_type<uint>(_50), memory_order_relaxed, memory_order_relaxed) && _97 == _91);\n'
            '            _100 = _97;\n'
            '        }\n'
            '        else\n'
            '        {\n'
            '            _100 = 0u;\n'
            '        }\n'
            '        uint _53 = _100;\n'
            '        _46 = int(_53) != as_type<int>(_48);\n'
            '    } while (_46);\n'
        )
        out = float_add_msl(src)
        self.assertEqual(out, '    if ((uint(0) + (*spvDescriptorSet0.cp_c27)._m0[0].x) < (*spvDescriptorSet0.cp_c27)._m0[0].y)\n'
                              '    {\n'
                              '        atomic_fetch_add_explicit((device atomic_float*)&(*spvDescriptorSet0.cp_s0)._m0[0], _42, memory_order_relaxed);\n'
                              '    }\n'
                              '    bool _46 = false;\n')
        mismatch = src.replace('_87 ? 0 : 0', '_87 ? 1 : 0')
        self.assertEqual(float_add_msl(mismatch), mismatch)

    @unittest.skipUnless(shutil.which('xcrun') and sys.platform == 'darwin', 'needs Metal')
    def test_sums_on_gpu(self):
        body = loop('b', 0, 'v', 10) + loop('b', 1, 'w', 20) + loop('b', 2, 'v', 30) + loop('b', 3, 'w', 40)
        body += '    if (_10 || _20 || _30 || _40) { b._m0[0] = as_type<uint>(-1.0f); }\n'
        import metal_kernel, struct
        want = [sum((i & 7) * 0.25 for i in range(8192)), 8192.0] * 2
        for name, src in (('loop', KERNEL % body), ('native', KERNEL % float_add_msl(body))):
            if name == 'native': self.assertIn('atomic_float', src)
            got = [struct.unpack('<f', struct.pack('<I', x))[0] for x in metal_kernel.run(src, 'k', [0, 0, 0, 0], 8192, per_group=256)]
            self.assertEqual(got, want, name)


    @unittest.skipUnless(shutil.which('xcrun') and sys.platform == 'darwin', 'needs Metal')
    def test_compiles_for_iphone(self):   # atomic_float at the iPhone libraries' deployment target and flags (ios_metal.METAL)
        sys.path.insert(0, str(TOOLS.parents[1] / 'ios/tools')); import ios_metal
        with tempfile.TemporaryDirectory() as w:
            body = loop('b', 0, 'v', 10) + '    if (_10) { b._m0[0] = 0u; }\n'
            src = Path(w) / 'n.metal'; src.write_text(KERNEL % float_add_msl(body))
            r = subprocess.run([*ios_metal.METAL, str(src), '-o', str(Path(w) / 'n.air')], capture_output=True, text=True)
            if 'SDK "iphoneos" cannot be located' in r.stderr: self.skipTest('no iPhone SDK')
            self.assertEqual(r.returncode, 0, r.stderr)

if __name__ == '__main__':
    unittest.main()
