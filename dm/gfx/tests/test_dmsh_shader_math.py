"""Profile keys shader_math and shader_nofm (dmsh_build.py): with Metal's default fast maths the compiler assumes no NaN and folds a translated shader's x != x (Maxwell's NaN test) to false.
shader_math safe (-fno-fast-math on every Mac library of the game) and shader_nofm (the safe maths pragma on single shaders) keep the compare. Checked on the AIR the chain's own compile
command writes; the control case (fast maths, no pragma) folds it, so the test fails if either key stops reaching the compiler."""
import ast
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / 'dm/gfx/tools/dmsh_build.py'

KERNEL = '''#include <metal_stdlib>
using namespace metal;
kernel void check(device float* io [[buffer(0)]], uint i [[thread_position_in_grid]])
{
    float x = io[i];
    io[i] = x != x ? 1.0f : 0.0f;
}
'''


def build_defs():
    """dmsh_build.py's MAC_METAL, SHADER_MATH, NOFM_PRAGMA and nofm_msl, read without running the script (it needs a game profile)"""
    tree = ast.parse(BUILD.read_text()); ns = {}
    for n in tree.body:
        if isinstance(n, ast.Assign) and isinstance(n.targets[0], ast.Name) and n.targets[0].id in ('MAC_METAL', 'SHADER_MATH', 'NOFM_PRAGMA'):
            ns[n.targets[0].id] = ast.literal_eval(n.value)
        elif isinstance(n, ast.FunctionDef) and n.name == 'nofm_msl':
            exec(compile(ast.Module(body=[n], type_ignores=[]), str(BUILD), 'exec'), ns)
    return ns


def keeps_nan_test(src, flags):
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / 'k.metal'; p.write_text(src)
        r = subprocess.run(flags + ['-S', '-emit-llvm', '-o', '-', str(p)], capture_output=True, text=True)
        if r.returncode: raise AssertionError(r.stderr)
        return 'fcmp uno' in r.stdout or 'fcmp une' in r.stdout


class ShaderMath(unittest.TestCase):
    def setUp(self):
        self.d = build_defs()
        self.d['SHADER_NOFM'] = {'0123456789abcdef'}

    def test_nofm_pragma_placed_once(self):
        out = self.d['nofm_msl']('0123456789abcdef0', KERNEL)
        self.assertIn('using namespace metal;\n' + self.d['NOFM_PRAGMA'], out)
        self.assertEqual(self.d['nofm_msl']('0123456789abcdef0', out), out)
        self.assertEqual(self.d['nofm_msl']('fedcba9876543210', KERNEL), KERNEL)

    def test_modes(self):
        self.assertEqual(self.d['SHADER_MATH'], {'fast': [], 'relaxed': ['-fmetal-math-mode=relaxed'], 'safe': ['-fno-fast-math']})


@unittest.skipUnless(shutil.which('xcrun') and subprocess.run(['xcrun', '-sdk', 'macosx', '-f', 'metal'], capture_output=True).returncode == 0, 'no Metal compiler')
class ShaderMathCompiles(unittest.TestCase):
    def setUp(self):
        self.d = build_defs(); self.d['SHADER_NOFM'] = {'0123456789abcdef'}

    def test_fast_folds_nan_test(self):   # the control: without either key the compare is gone
        self.assertFalse(keeps_nan_test(KERNEL, self.d['MAC_METAL']))

    def test_safe_keeps_nan_test(self):
        self.assertTrue(keeps_nan_test(KERNEL, self.d['MAC_METAL'] + self.d['SHADER_MATH']['safe']))

    def test_nofm_keeps_nan_test(self):
        self.assertTrue(keeps_nan_test(self.d['nofm_msl']('0123456789abcdef0', KERNEL), self.d['MAC_METAL']))


if __name__ == '__main__':
    unittest.main()
