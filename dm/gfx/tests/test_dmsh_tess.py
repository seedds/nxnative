"""Tessellation without a Metal tessellator (nvn_tess.inc, dmsh_tess.py): the host's expansion kernel and the domain prologue share one
text of the layout (dmsh_tess.TESS_MSL), and with DM_TEST_TESS (the t_tess executable) the layout and the expansion kernel are checked on
the GPU. `python3 test_dmsh_tess.py --fixture <dir>` writes <dir>/tess_test.metal for t_tess (make check_t_tess)."""
import ast
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

GFX = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GFX / 'tools'))
import dmsh_tess   # noqa: E402

HEAD = '#include <metal_stdlib>\nusing namespace metal;\n'
# Test kernels: every triangle of a patch in mode md (dm_tcount entries of 6 vertices, at most 4096 entries a patch) and the cull decision
TEST_KERNELS = r'''
kernel void t_tris(device const half* lev [[buffer(0)]], device float4* out [[buffer(1)]], device uint* cnt [[buffer(2)]],
    constant float& lmax [[buffer(3)]], constant uint& md [[buffer(4)]], uint p [[thread_position_in_grid]])
{
    device const half* L = lev + p * 6u;
    uint c = dm_tcount(L, lmax, md); cnt[p] = c;
    for (uint e = 0; e < c && e < 4096u; e++) for (uint k = 0; k < 6u; k++) out[p * 24576u + e * 6u + k] = float4(dm_tvert(e, k, L, lmax, md), 0.0f);
}
kernel void t_cull(device const half* lev [[buffer(0)]], device uint* out [[buffer(1)]], constant uint& md [[buffer(2)]], uint p [[thread_position_in_grid]]) { out[p] = dm_tcull(lev + p * 6u, md) ? 1u : 0u; }
'''


def host_kernel():
    """The text of nvn_tess.inc's k_tess_msl, as the host compiles it."""
    inc = (GFX / 'nvn' / 'nvn_tess.inc').read_text()
    m = re.search(r'static const char \*k_tess_msl = (.*?);\n', inc, re.S)
    return ''.join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', m.group(1)))


def fixture(out):
    path = Path(out, 'tess_test.metal')
    path.write_text(host_kernel() + TEST_KERNELS)
    return path


class TessText(unittest.TestCase):
    def test_host_kernel_starts_with_the_shared_layout(self):
        self.assertTrue(host_kernel().startswith(HEAD + dmsh_tess.TESS_MSL),
                        'nvn_tess.inc k_tess_msl must start with dmsh_tess.TESS_MSL: the expansion kernel and the domain prologue would disagree on the layout')

    def test_kernel_and_helpers_are_there(self):
        k = host_kernel()
        for name in ('kernel void dm_tess_expand', 'dm_tcount', 'dm_tvert', 'dm_tcull'):
            self.assertIn(name, k)


@unittest.skipUnless(os.environ.get('DM_TEST_TESS'), 'set DM_TEST_TESS to the t_tess executable (make check_t_tess)')
class TessGpu(unittest.TestCase):
    def test_layout_and_expansion_on_the_gpu(self):
        with tempfile.TemporaryDirectory() as d:
            for seed in ('20261104', '7'):
                r = subprocess.run([os.environ['DM_TEST_TESS'], str(fixture(d)), seed], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertIn('ALL PASSED', r.stdout)


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--fixture':
        fixture(sys.argv[2])
        sys.exit(0)
    unittest.main()
