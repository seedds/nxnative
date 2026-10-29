"""Integer OR shader variants retain the original COPY path."""
import ast
from pathlib import Path
import re
import unittest
import subprocess
import sys
import tempfile

source = (Path(__file__).resolve().parents[1] / 'tools/dmsh_build.py').read_text()
node = next(n for n in ast.parse(source).body if isinstance(n, ast.FunctionDef) and n.name == 'fs_uint')
early = next((n for n in ast.parse(source).body if isinstance(n, ast.FunctionDef) and n.name == 'fs_early'), None)
ns = {'re': re}
if early: exec(compile(ast.Module(body=[early], type_ignores=[]), '<fs_early>', 'exec'), ns)
exec(compile(ast.Module(body=[node], type_ignores=[]), '<fs_uint>', 'exec'), ns)

class LogicOrTests(unittest.TestCase):
    def test_early_fragment_tests_are_opt_in_and_idempotent(self):
        src = 'fragment float4 main0() { return float4(1); }\n'
        self.assertIn('fs_early', ns)
        self.assertEqual(ns['fs_early'](src, False), src)
        result = ns['fs_early'](src, True)
        self.assertIn('[[early_fragment_tests]] fragment', result)
        self.assertEqual(ns['fs_early'](result, True), result)

    def test_integer_color_zero_gets_separate_or_entry(self):
        src = '''using namespace metal;
struct main0_out { float4 value [[color(0)]]; };
fragment main0_out main0(uint v [[primitive_id]])
{
    main0_out out = {};
    out.value = as_type<float4>(uint4(v));
    return out;
}
'''
        result, _ = ns['fs_uint'](src)
        self.assertIn('main0_u_or(', result)
        self.assertIn('uint4 dm_logic_previous [[color(0)]]', result)
        self.assertIn('as_type<uint4>(out.value) | dm_logic_previous', result)
        self.assertIn('return main0_out_u{as_type<uint4>(out.value)};', result)
        self.assertTrue(result.startswith(src))

    def test_preserves_early_fragment_test_attribute(self):
        src = """struct main0_out { float4 value [[color(0)]]; };
[[early_fragment_tests]] fragment main0_out main0()
{
    main0_out out = {};
    out.value = as_type<float4>(uint4(1));
    return out;
}
"""
        result, _ = ns['fs_uint'](src)
        self.assertIn('[[early_fragment_tests]] fragment main0_out_u main0_u_or(', result)

    def test_multiline_early_marker_survives_integer_clones(self):
        src = """struct main0_out { float4 value [[color(0)]]; };
[[early_fragment_tests]]
fragment main0_out main0()
{
    main0_out out = {};
    out.value = as_type<float4>(uint4(1));
    return out;
}
"""
        result, _ = ns['fs_uint'](ns['fs_early'](src, True))
        self.assertEqual(result.count('[[early_fragment_tests]]'), 3)

    @unittest.skipUnless(sys.platform == 'darwin', 'Metal GPU test')
    def test_rejected_depth_does_not_write_storage(self):
        src = """#include <metal_stdlib>
using namespace metal;
vertex float4 vs(uint v [[vertex_id]]) { return float4(v==1?3.0:-1.0,v==2?3.0:-1.0,0.5,1); }
fragment float4 main0(device atomic_uint *counter [[buffer(0)]])
{
    atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
    return float4(1);
}
"""
        with tempfile.TemporaryDirectory(prefix='early-fragment-') as tmp:
            root = Path(tmp)
            (root/'probe.metal').write_text(ns['fs_early'](src, True))
            commands = [
                ['xcrun','-sdk','macosx','metal','-c',str(root/'probe.metal'),'-o',str(root/'probe.air')],
                ['xcrun','-sdk','macosx','metallib',str(root/'probe.air'),'-o',str(root/'probe.metallib')],
                ['clang','-fobjc-arc',str(Path(__file__).with_name('t_early_fragment.m')),'-framework','Foundation','-framework','Metal','-o',str(root/'probe')],
                [str(root/'probe'),str(root/'probe.metallib')],
            ]
            for command in commands:
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_other_output_locations_do_not_admit_color_zero_fetch(self):
        src = '''struct main0_out { float4 value [[color(2)]]; };
fragment main0_out main0()
{
    main0_out out = {};
    out.value = as_type<float4>(uint4(1));
    return out;
}
'''
        result, _ = ns['fs_uint'](src)
        self.assertNotIn('main0_u_or(', result)

if __name__ == '__main__': unittest.main()
