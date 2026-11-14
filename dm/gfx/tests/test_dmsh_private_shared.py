"""Invocation-private shared storage and conservative refusal of communication."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from dmsh_private_shared import privatize
import dmsh_abi as abi
from dmsh_rewrite import max_threads, tg_spill

SOURCE = '''kernel void main0(uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    threadgroup spvUnsafeArray<uint, 11264> scratch;
    float x = as_type<float>(gl_LocalInvocationID.x);
    float y = as_type<float>(gl_LocalInvocationID.y);
    int row = as_type<int>(y) & 65535;
    int rowbase = row * 1024;
    int lane = rowbase + as_type<int>(x);
    int masked = lane & 65535;
    int base = masked * 44;
    int address = base + 40;
    uint index = uint(int(uint(address) >> uint(2)));
    scratch[int(index)] = 7u;
    uint value = scratch[int(index)];
}
'''


class PrivateShared(unittest.TestCase):
    def test_disjoint_slices(self):
        result = privatize(SOURCE, [1024, 1, 1])
        self.assertIn('spvUnsafeArray<uint, 11> scratch;', result)
        self.assertEqual(result.count('scratch[10]'), 2)
        self.assertNotIn('threadgroup spvUnsafeArray', result)
        # Every invocation's rewritten address names exactly its original word.
        for invocation in range(1024):
            self.assertEqual(((invocation * 44 + 40) >> 2), invocation * 11 + 10)

    def test_unknown_or_shared_access_unchanged(self):
        for source in (
            SOURCE.replace('base + 40', 'base + 44'),
            SOURCE.replace('base + 40', '40'),
            SOURCE.replace('base + 40', 'other_lane()'),
            SOURCE.replace('value = scratch[int(index)]', 'value = helper(scratch[int(index)])'),
            SOURCE.replace('uint value', 'uint& value'),
            SOURCE.replace('int base =', 'lane++;\n    int base ='),
            SOURCE.replace('int base =', 'mutate(lane);\n    int base ='),
            SOURCE.replace('scratch[int(index)] = 7u;', 'helper(scratch);'),
            SOURCE.replace('scratch[int(index)] = 7u;', 'scratch[int(index)] = helper(scratch[int(index)]);'),
            SOURCE.replace('as_type<int>(x)', 'int(x)'),
            SOURCE.replace('masked * 44', 'masked * 2147483647'),
        ):
            with self.subTest(source=source):
                self.assertEqual(privatize(source, [1024, 1, 1]), source)

    def test_group_dimensions_required(self):
        for size in (None, [1, 1, 1], [32, 32, 1], [1023, 1, 1]):
            self.assertEqual(privatize(SOURCE, size), SOURCE)


GPU_SOURCE = '''#include <metal_stdlib>
using namespace metal;
template<typename T, size_t N> struct spvUnsafeArray {
    T elements[N];
    thread T& operator [] (size_t pos) thread { return elements[pos]; }
    threadgroup T& operator [] (size_t pos) threadgroup { return elements[pos]; }
    device T& operator [] (size_t pos) device { return elements[pos]; }
};
kernel void main0(device uint* out [[buffer(1)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]], uint3 group [[threadgroup_position_in_grid]])
{
    threadgroup spvUnsafeArray<uint, 11264> scratch;
    int lane = int(gl_LocalInvocationID.x);
    int base = lane * 11;
    int first = base + 1;
    int last = base + 10;
    scratch[int(first)] = group.x * 100000u + gl_LocalInvocationID.x * 17u;
    scratch[int(last)] = group.x + gl_LocalInvocationID.x * 3u;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint a = scratch[int(first)];
    uint b = scratch[int(last)];
    out[group.x * 1024u + gl_LocalInvocationID.x] = a + b;
}
'''

RUNNER = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
int main(int argc, char **argv) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *e = nil;
    if (!dev) return 2;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];
    id<MTLComputePipelineState> ps = lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"main0"] error:&e] : nil;
    if (!ps || ps.maxTotalThreadsPerThreadgroup < 1024) { printf("pipeline: %s\n", e.description.UTF8String); return 1; }
    const uint32_t groups = 32;
    id<MTLBuffer> out = [dev newBufferWithLength:groups * 1024 * 4 options:MTLResourceStorageModeShared];
    memset(out.contents, 0xff, out.length);
    id<MTLBuffer> tg = [dev newBufferWithLength:MAX(16, (size_t)atoi(argv[2]) * groups) options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps]; [ce setBuffer:out offset:0 atIndex:1];
    if (atoi(argv[2])) [ce setBuffer:tg offset:0 atIndex:atoi(argv[3])];
    [ce dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(1024, 1, 1)];
    [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { puts(cb.error.description.UTF8String); return 1; }
    const uint32_t *v = out.contents;
    for (uint32_t g = 0; g < groups; g++) for (uint32_t lane = 0; lane < 1024; lane++) {
        if (v[g * 1024 + lane] != g * 100001u + lane * 20u) { printf("wrong invocation %u:%u\n", g, lane); return 1; }
    }
    puts("32768 invocations: PASS"); return 0;
} }
'''


@unittest.skipUnless(shutil.which('xcrun') and shutil.which('clang'), 'needs Xcode and Metal')
class OnGpu(unittest.TestCase):
    def test_private_and_spilled_storage_preserve_every_invocation(self):
        private = privatize(GPU_SOURCE, [1024, 1, 1])
        self.assertNotEqual(private, GPU_SOURCE)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'run.m').write_text(RUNNER)
            subprocess.run(['clang', '-fobjc-arc', '-O1', str(root / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(root / 'run')], check=True)
            for source, expected_spill in ((GPU_SOURCE, 45056), (private, 0)):
                with self.subTest(spill=expected_spill):
                    rewritten, spill = tg_spill(source)
                    self.assertEqual(spill, expected_spill)
                    (root / 'k.metal').write_text(max_threads(rewritten, [1024, 1, 1]))
                    subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-O1', str(root / 'k.metal'), '-o', str(root / 'k.metallib')], check=True)
                    result = subprocess.run([str(root / 'run'), str(root / 'k.metallib'), str(spill), str(abi.TG_SLOT)], capture_output=True, text=True, env=dict(os.environ, MTL_DEBUG_LAYER='1'))
                    if result.returncode == 2: self.skipTest('no Metal device')
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
