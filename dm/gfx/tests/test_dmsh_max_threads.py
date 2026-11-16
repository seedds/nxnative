"""A compute shader's local size as the kernel's thread limit (dmsh_rewrite.py max_threads): on the M1 and M2 a compute pipeline allows fewer threads per threadgroup the more
registers its code takes, so a kernel compiled without the limit can get a dispatch of the Switch shader's local size that its pipeline does not allow, and no invocation runs (Ocarina of
Time's froxel light injection 9ed48d1011f1b48b, 8x8x8, issue #2425). The rewrite names the local size on the kernel; with dmsh_build.py's and ios_metal.py's compile commands the front
end's IR then carries it as the function's work-group size, which the GPU compiler fits the registers to."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / 'dm/gfx/tools'
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(ROOT / 'dm/ios/tools'))
import dmsh_chain  # noqa: E402
import dmsh_rewrite  # noqa: E402
import ios_metal  # noqa: E402

SHADER = """#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct _16
{
    float4 _m0[4096];
};

kernel void main0(constant _16* cp_c3 [[buffer(0)]], texture3d<float, access::write> cp_i0 [[texture(0)]], uint3 gl_GlobalInvocationID [[thread_position_in_grid]])
{
    cp_i0.write(cp_c3->_m0[gl_GlobalInvocationID.x & 15u], gl_GlobalInvocationID);
}
"""


class MaxThreads(unittest.TestCase):
    def test_the_local_size_goes_on_the_kernel(self):
        got = dmsh_rewrite.max_threads(SHADER, [8, 8, 8])
        self.assertIn('[[max_total_threads_per_threadgroup(512)]]\nkernel void main0(', got)
        self.assertEqual(got.replace('[[max_total_threads_per_threadgroup(512)]]\n', ''), SHADER)   # nothing else changes
        self.assertIn('[[max_total_threads_per_threadgroup(1024)]]\n', dmsh_rewrite.max_threads(SHADER, [32, 32, 1]))
        self.assertIn('[[max_total_threads_per_threadgroup(448)]]\n', dmsh_rewrite.max_threads(SHADER, [7, 8, 8]))

    def test_once(self):
        once = dmsh_rewrite.max_threads(SHADER, [8, 8, 8])
        self.assertEqual(dmsh_rewrite.max_threads(once, [8, 8, 8]), once)

    def test_no_size_no_change(self):
        for local in (None, [], [8, 8], [0, 1, 1], [64, 32, 1], [8.0, 8, 8], [64, 1, 1], [384, 1, 1], [8, 8, 6]):   # missing, malformed, empty, past Metal's 1,024, and sizes every pipeline allows
            with self.subTest(local=local):
                self.assertEqual(dmsh_rewrite.max_threads(SHADER, local), SHADER)

    def test_only_the_kernel(self):
        src = SHADER.replace('kernel void main0(', 'fragment float4 main0(')
        self.assertEqual(dmsh_rewrite.max_threads(src, [8, 8, 8]), src)

    def work_group_size(self, command, source):   # the front end's IR of source with command (its -c compile, emitting LLVM IR instead): main0's air.max_work_group_size, or None
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'test.metal').write_text(source)
            r = subprocess.run(command + ['-w', '-S', '-emit-llvm', str(root / 'test.metal'), '-o', str(root / 'test.ll')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            for line in (root / 'test.ll').read_text().splitlines():
                if '"air.max_work_group_size"' in line: return int(line.rsplit('i32', 1)[1].strip(' }'))
            return None

    @unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode and Metal')
    def test_the_gpu_compiler_gets_the_size(self):
        for name, command in (('Mac', dmsh_chain.MAC_METAL), ('iPhone', ios_metal.METAL)):
            with self.subTest(platform=name):
                self.assertIsNone(self.work_group_size(command, SHADER), 'the plain kernel names a size: the test no longer shows the problem')
                self.assertEqual(self.work_group_size(command, dmsh_rewrite.max_threads(SHADER, [8, 8, 8])), 512)


if __name__ == '__main__':
    unittest.main()
