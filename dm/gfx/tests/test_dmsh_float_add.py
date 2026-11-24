#!/usr/bin/env python3
"""dmsh_rewrite.py float_add_msl: a float add to storage memory (RED.F32's compare-and-swap loop, InstEmitMemory EmitAtomicAddF32) as one native atomic_fetch_add on a device
atomic_float, called through a noinline leaf helper so Apple's AGX3 backend (M5 / M5 Pro on macOS 26.x, M5 iPad Pro / iPhone 17 on iOS 26.x) does not hoist
@llvm.agx3.disable.smashing.float into agc.main.constant_program where GlobalISel cannot select it (GitHub issue #2825). The loop as spirv-cross writes it is replaced and any
other loop is left alone; a later read of its retry flag still gets false. On this Mac's GPU 8,192 threads adding into the same four words, through the loop and through the
rewrite, reach the exact sums. With DMSH_TEST_METAL=1, metal-tt checks the unrewritten inline add fails on applegpu_g17g (macOS 26.6) and the rewritten add compiles for g17g,
g17s (macOS 26.6) and g18p (iOS 26.0). test_dmsh_ampere_alu.py FloatRed checks that the translator's own loop is the one rewritten (PR #1930)."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(TOOLS))
import dmsh_chain
import dmsh_rewrite
float_add_msl = dmsh_rewrite.float_add_msl
HEAD = 'using namespace metal;\n'
HELPER = dmsh_rewrite.FLOAT_ADD_HELPER


def has_metal_tt():
    if not shutil.which('xcrun') or sys.platform != 'darwin': return False
    return subprocess.run(['xcrun', '-sdk', 'macosx', 'metal-tt', '-version'], capture_output=True).returncode == 0


def compile_compute_tt(msl, flags, platform, arch, osmin):
    """(ok, stderr) of compiling msl's kernel `k` through metal, metallib and metal-tt."""
    with tempfile.TemporaryDirectory() as w:
        w = Path(w); (w / 'k.metal').write_text(msl)
        r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-w', *flags, str(w / 'k.metal'), '-o', str(w / 'k.air')], capture_output=True, text=True)
        if r.returncode: raise AssertionError('metal: ' + r.stderr)
        r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(w / 'k.air'), '-o', str(w / 'k.metallib')], capture_output=True, text=True)
        if r.returncode: raise AssertionError('metallib: ' + r.stderr)
        js = {'libraries': [{'label': 'L', 'path': str(w / 'k.metallib')}],
              'function_descriptors': {'library_function_descriptors': [{'label': 'L.k', 'library': 'lib:L', 'name': 'k'}]},
              'pipeline_descriptors': {'compute_pipeline_descriptors': [{'compute_function_descriptor': 'fnd:L.k'}]}}
        (w / 'k.mtl4-json').write_text(json.dumps(js))
        r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal-tt', '-arch', arch, f'-{platform}-version-min', osmin, str(w / 'k.mtl4-json'), '-o', str(w / 'k.mtl4a')],
                           capture_output=True, text=True)
        return r.returncode == 0, r.stdout + r.stderr


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
        src = HEAD + loop('(*spvDescriptorSet0.cp_s4)', 0, '_2580', 10) + loop('(*spvDescriptorSet0.cp_s4)', 'int(_7)', '_2572', 20)
        out = float_add_msl(src)
        self.assertEqual(out, HEAD + HELPER
                              + '    dm_atomic_add_f32((device atomic_float*)&(*spvDescriptorSet0.cp_s4)._m0[0], _2580);\n'
                              + '    bool _10 = false;\n'
                              + '    dm_atomic_add_f32((device atomic_float*)&(*spvDescriptorSet0.cp_s4)._m0[int(_7)], _2572);\n'
                              + '    bool _20 = false;\n')
        self.assertEqual(float_add_msl(out), out)   # idempotent

    def test_exact_add_too(self):   # the translator's exact form (XLATE_REDUCED=0): spvFAdd(old, v)
        src = HEAD + loop('b', 0, 'v', 10).replace('_11 + v;', 'spvFAdd(_11, v);')
        self.assertEqual(float_add_msl(src), HEAD + HELPER
                                           + '    dm_atomic_add_f32((device atomic_float*)&b._m0[0], v);\n'
                                           + '    bool _10 = false;\n')

    def test_live_retry_flag_is_false_after_add(self):
        tail = '    b._m0[0] = uint(_10);\n'
        for add in ('_11 + v', 'spvFAdd(_11, v)'):
            with self.subTest(add=add):
                src = HEAD + loop('b', 0, 'v', 10).replace('_11 + v', add) + tail
                out = float_add_msl(src)
                self.assertNotIn('atomic_compare_exchange_weak_explicit', out)
                self.assertIn('    bool _10 = false;\n', out)   # the do-while can only finish with this value
                self.assertTrue(out.endswith(tail))

    def test_other_words_and_shapes_left_alone(self):
        other = loop('b', 0, 'v', 10).replace('&b._m0[0], &', '&b._m0[1], &')   # the swap on another word than the read
        self.assertEqual(float_add_msl(other), other)

    def test_missing_using_namespace_raises(self):
        with self.assertRaises(ValueError):
            float_add_msl(loop('b', 0, 'v', 10))

    def test_bounded_loop_becomes_guarded_atomic_add(self):
        src = (
            HEAD
            + '    bool _46;\n'
            + '    do\n'
            + '    {\n'
            + '        bool _71 = (uint(0) + (*spvDescriptorSet0.cp_c27)._m0[0].x) < (*spvDescriptorSet0.cp_c27)._m0[0].y;\n'
            + '        float _48 = as_type<float>(_71 ? (*spvDescriptorSet0.cp_s0)._m0[_71 ? 0 : 0] : 0u);\n'
            + '        float _50 = _48 + _42;\n'
            + '        bool _87 = (uint(0) + (*spvDescriptorSet0.cp_c27)._m0[0].x) < (*spvDescriptorSet0.cp_c27)._m0[0].y;\n'
            + '        uint _91 = as_type<uint>(_48);\n'
            + '        uint _100;\n'
            + '        if (_87)\n'
            + '        {\n'
            + '            uint _97;\n'
            + '            do\n'
            + '            {\n'
            + '                _97 = _91;\n'
            + '            } while (!atomic_compare_exchange_weak_explicit((device atomic_uint*)&(*spvDescriptorSet0.cp_s0)._m0[_87 ? 0 : 0], &_97, as_type<uint>(_50), memory_order_relaxed, memory_order_relaxed) && _97 == _91);\n'
            + '            _100 = _97;\n'
            + '        }\n'
            + '        else\n'
            + '        {\n'
            + '            _100 = 0u;\n'
            + '        }\n'
            + '        uint _53 = _100;\n'
            + '        _46 = int(_53) != as_type<int>(_48);\n'
            + '    } while (_46);\n'
        )
        out = float_add_msl(src)
        self.assertEqual(out, HEAD + HELPER
                              + '    if ((uint(0) + (*spvDescriptorSet0.cp_c27)._m0[0].x) < (*spvDescriptorSet0.cp_c27)._m0[0].y)\n'
                              + '    {\n'
                              + '        dm_atomic_add_f32((device atomic_float*)&(*spvDescriptorSet0.cp_s0)._m0[0], _42);\n'
                              + '    }\n'
                              + '    bool _46 = false;\n')
        mismatch = src.replace('_87 ? 0 : 0', '_87 ? 1 : 0')
        self.assertEqual(float_add_msl(mismatch), mismatch)

    @unittest.skipUnless(shutil.which('xcrun') and sys.platform == 'darwin', 'needs Metal')
    def test_sums_on_gpu(self):
        body = loop('b', 0, 'v', 10) + loop('b', 1, 'w', 20) + loop('b', 2, 'v', 30) + loop('b', 3, 'w', 40)
        body += '    if (_10 || _20 || _30 || _40) { b._m0[0] = as_type<uint>(-1.0f); }\n'
        import metal_kernel, struct
        want = [sum((i & 7) * 0.25 for i in range(8192)), 8192.0] * 2
        for name, src in (('loop', KERNEL % body), ('native', float_add_msl(KERNEL % body))):
            if name == 'native':
                self.assertIn('dm_atomic_add_f32', src)
                self.assertIn('atomic_float', src)
            got = [struct.unpack('<f', struct.pack('<I', x))[0] for x in metal_kernel.run(src, 'k', [0, 0, 0, 0], 8192, per_group=256)]
            self.assertEqual(got, want, name)

    @unittest.skipUnless(shutil.which('xcrun') and sys.platform == 'darwin', 'needs Metal')
    def test_compiles_for_iphone(self):   # atomic_float at the iPhone libraries' deployment target and flags (ios_metal.METAL)
        sys.path.insert(0, str(TOOLS.parents[1] / 'ios/tools')); import ios_metal
        with tempfile.TemporaryDirectory() as w:
            body = loop('b', 0, 'v', 10) + '    if (_10) { b._m0[0] = 0u; }\n'
            src = Path(w) / 'n.metal'; src.write_text(float_add_msl(KERNEL % body))
            r = subprocess.run([*ios_metal.METAL, str(src), '-o', str(Path(w) / 'n.air')], capture_output=True, text=True)
            if 'SDK "iphoneos" cannot be located' in r.stderr: self.skipTest('no iPhone SDK')
            self.assertEqual(r.returncode, 0, r.stderr)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_METAL') and has_metal_tt(), 'set DMSH_TEST_METAL=1 to compile with metal-tt')
    def test_agx3_constant_program_workaround(self):
        chain = list(dmsh_chain.MAC_METAL[5:])
        inline_msl = KERNEL % '    atomic_fetch_add_explicit((device atomic_float*)&b._m0[0], v, memory_order_relaxed);\n'
        ok, log = compile_compute_tt(inline_msl, chain, 'macos', 'applegpu_g17g', '26.6')
        if ok: self.skipTest('this toolchain compiles the inline atomic_float add on applegpu_g17g')
        self.assertIn('disable.smashing.float', log)
        rewritten = float_add_msl(KERNEL % loop('b', 0, 'v', 10))
        for flags in (chain, ['-O2', *chain[1:]]):
            for plat, arch, osmin in (('macos', 'applegpu_g17g', '26.6'), ('macos', 'applegpu_g17s', '26.6'), ('ios', 'applegpu_g18p', '26.0')):
                ok, log = compile_compute_tt(rewritten, flags, plat, arch, osmin)
                self.assertTrue(ok, f'{flags} {plat} {arch} {osmin}: {log}')


if __name__ == '__main__':
    unittest.main()
