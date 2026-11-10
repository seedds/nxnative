"""Loop guard (dmsh_rewrite.py loop_guard): every loop of a shader also ends once its function has run LOOP_CAP iterations in all, so a loop whose bound never comes (a wrong
translation, an unfilled value) cannot hang the GPU. Each loop form spirv-cross writes gets the cap, each function body one counter, the result compiles, and on this Mac's GPU a
loop with no end stops at the cap and a later loop of the same function runs no more."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
from dmsh_rewrite import LOOP_CAP, loop_guard   # noqa: E402

# The loop forms of spirv-cross's MSL, a struct (no counter) and a helper function; out[0] counts the first loop's iterations, out[1] the second's
KERNEL = '''#include <metal_stdlib>
using namespace metal;
struct S
{
    uint n;
};

static inline uint helper(uint v)
{
    uint r = 0u;
    for (uint i = 0u; i < v; i++)
    {
        r += i;
    }
    return r;
}

kernel void main0(device uint* out [[buffer(0)]], constant S& s [[buffer(1)]])
{
    uint a = 0u;
    do
    {
        a++;
        if (a == s.n)
        {
            break;
        }
    } while (true);
    out[0] = a;
    uint b = 0u;
    while (b < s.n)
    {
        b++;
    }
    out[1] = b;
    for (;;)
    {
        break;
    }
    out[2] = helper(4u);
}
'''

RUNNER = r'''#import <Metal/Metal.h>
int main(int argc, char **argv) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); if (!d) return 2;
    NSError *e = nil; id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e]; if (!lib) { fprintf(stderr, "%s\n", e.description.UTF8String); return 1; }
    id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"main0"] error:&e]; if (!ps) return 1;
    id<MTLBuffer> out = [d newBufferWithLength:16 options:MTLResourceStorageModeShared], s = [d newBufferWithLength:4 options:MTLResourceStorageModeShared];
    *(uint32_t *)s.contents = 0;   // a == 0 never comes after a++: the first loop has no end
    id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps]; [ce setBuffer:out offset:0 atIndex:0]; [ce setBuffer:s offset:0 atIndex:1];
    [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) return 1;
    uint32_t *o = out.contents; printf("%u %u %u\n", o[0], o[1], o[2]); return 0;
} }
'''


class LoopGuard(unittest.TestCase):
    def test_every_loop_form_gets_the_cap(self):
        msl = loop_guard(KERNEL)
        cap = f'++dm_lg < {LOOP_CAP}u'
        self.assertIn(f'}} while ((true) && {cap});', msl)
        self.assertIn(f'while ((b < s.n) && {cap})', msl)
        self.assertIn(f'for (; {cap};)', msl)
        self.assertIn(f'for (uint i = 0u; (i < v) && {cap}; i++)', msl)
        self.assertEqual(msl.count('    uint dm_lg = 0u;\n'), 2)   # helper and main0, not the struct

    def test_a_shader_with_no_loop_is_unchanged(self):
        src = 'kernel void main0(device uint* out [[buffer(0)]])\n{\n    out[0] = 1u;\n}\n'
        self.assertEqual(loop_guard(src), src)

    def test_a_mesh_kernel_keeps_its_stage_memory_findable(self):   # a task or mesh shader with a loop failed dmsh_meshstage.to_stage (Nintendo Switch Sports Resort)
        import dmsh_meshstage
        src = ('kernel void main0(uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])\n{\n    threadgroup spvUnsafeArray<uint, 64> _18;\n'
               '    for (uint i = 0u; i < 4u; i++)\n    {\n        _18[i] = i;\n    }\n}\n')
        self.assertIsNotNone(dmsh_meshstage._KERNEL.search(loop_guard(src)))

    @unittest.skipUnless(shutil.which('xcrun') and shutil.which('clang'), 'needs Xcode and Metal')
    def test_gpu_a_loop_with_no_end_stops_at_the_cap(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'test.metal').write_text(loop_guard(KERNEL))
            (root / 'run.m').write_text(RUNNER)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O2', str(root / 'test.metal'), '-o', str(root / 'test.air')], check=True, capture_output=True)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(root / 'test.air'), '-o', str(root / 'test.metallib')], check=True, capture_output=True)
            subprocess.run(['clang', '-fobjc-arc', '-O1', str(root / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(root / 'run')], check=True, capture_output=True)
            r = subprocess.run([str(root / 'run'), str(root / 'test.metallib')], capture_output=True, text=True, timeout=60)
            if r.returncode == 2: self.skipTest('no Metal device')
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            first, second, helper = map(int, r.stdout.split())
            self.assertEqual(first, LOOP_CAP)   # the cap's last increment ends the loop
            self.assertEqual(second, 0)         # the function's count is spent: a later loop runs no more
            self.assertEqual(helper, 0 + 1 + 2 + 3)   # another function counts on its own


if __name__ == '__main__':
    unittest.main()
