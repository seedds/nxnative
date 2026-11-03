"""Compute shared memory past Apple's threadgroup memory (dmsh_build.py tg_spill): a kernel whose threadgroup arrays exceed dmsh_abi.TGMEM_LIMIT gets its largest array as a
slice of the device buffer at compute buffer dmsh_abi.TGMEM_BUFFER, one slice per threadgroup, with its barriers and atomics on device memory; a kernel within the limit is unchanged.
On this Mac's GPU, a kernel with 48 KB of shared memory and a barrier, rewritten, runs and gives every threadgroup its own memory."""
import ast
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_abi as abi  # noqa: E402

tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
selected = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name == 'tg_spill')
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) == 'TG_ELEM' for t in n.targets))]
scope = {'re': re, 'abi': abi}
exec(compile(ast.Module(body=selected, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
tg_spill = scope['tg_spill']

HEAD = '''#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];

    thread T& operator [] (size_t pos) thread { return elements[pos]; }
    constexpr const thread T& operator [] (size_t pos) const thread { return elements[pos]; }
    device T& operator [] (size_t pos) device { return elements[pos]; }
    constexpr const device T& operator [] (size_t pos) const device { return elements[pos]; }
    threadgroup T& operator [] (size_t pos) threadgroup { return elements[pos]; }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup { return elements[pos]; }
};
'''
# The shape spirv-cross gives Mario Kart World's DrivePassShaderDefault kernels (2a89b6d4df731b8d: 64 threads, a 12,288-word array, threadgroup_barrier on it in half of them):
# each thread fills 192 words, then after the barrier reads its neighbour's
KERNEL = '''
static inline __attribute__((always_inline))
uint sum_of(threadgroup spvUnsafeArray<uint, %(n)d>& _9, thread const int& _10)
{
    uint s = 0u;
    for (int i = 0; i < 192; i++) s += _9[_10 * 192 + i];
    return s;
}

kernel void main0(device uint* out [[buffer(1)]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    threadgroup spvUnsafeArray<uint, %(n)d> _37;
    threadgroup uint _38;
    int t = int(gl_LocalInvocationID.x);
    uint g = gl_WorkGroupID.y * 16u + gl_WorkGroupID.x;
    for (int i = 0; i < 192; i++) _37[t * 192 + i] = g * 100000u + uint(t) * 1000u + uint(i);
    if (t == 0) _38 = 0u;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint _40 = atomic_fetch_add_explicit((threadgroup atomic_uint*)&_37[0], 0u, memory_order_relaxed);
    int nb = (t + 1) & 63;
    out[g * 64u + uint(t)] = sum_of(_37, nb) + (_40 - _40);
}
'''
BIG, SMALL = KERNEL % {'n': 12288}, KERNEL % {'n': 4096}


class Rule(unittest.TestCase):
    def test_over_the_limit_goes_to_a_device_slice(self):
        out, slice_ = tg_spill(HEAD + BIG)
        self.assertEqual(slice_, 49152)
        self.assertIn(f'device uchar* dmsh_tgmem [[buffer({abi.TGMEM_BUFFER})]], uint3 dmsh_tgn [[threadgroups_per_grid]])', out)   # the existing position is reused
        self.assertIn(f'device spvUnsafeArray<uint, 12288>& _37 = *(device spvUnsafeArray<uint, 12288>*)(dmsh_tgmem + {abi.TGMEM_HEAD} + dmsh_tgl * 49152u);', out)
        self.assertIn('dmsh_tgl = (gl_WorkGroupID.z * dmsh_tgn.y + gl_WorkGroupID.y) * dmsh_tgn.x + gl_WorkGroupID.x', out)
        self.assertIn('    threadgroup uint _38;', out)                                        # the small one stays
        self.assertIn('uint sum_of(device spvUnsafeArray<uint, 12288>& _9,', out)              # a helper taking it
        self.assertIn('(device atomic_uint*)&_37[0]', out)
        self.assertIn('threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);', out)
        self.assertNotIn('threadgroup spvUnsafeArray<uint, 12288>', out)

    def test_within_the_limit_unchanged(self):
        src = HEAD + SMALL
        self.assertEqual(tg_spill(src), (src, 0))

    def test_within_the_limit_with_other_types_unchanged(self):   # types the rule cannot size do not fail a kernel that needs no move
        src = HEAD + SMALL.replace('    threadgroup uint _38;\n', '    threadgroup atomic_uint _38;\n    threadgroup uint3 _39[16];\n    threadgroup S _41;\n')
        self.assertEqual(tg_spill(src), (src, 0))

    def test_position_added_when_the_kernel_has_none(self):
        src = HEAD + BIG.replace(', uint3 gl_WorkGroupID [[threadgroup_position_in_grid]]', '').replace('gl_WorkGroupID', 'uint3(0u)')
        out, _ = tg_spill(src)
        self.assertIn('uint3 dmsh_tgp [[threadgroup_position_in_grid]])', out)
        self.assertIn('(dmsh_tgp.z * dmsh_tgn.y + dmsh_tgp.y) * dmsh_tgn.x + dmsh_tgp.x', out)

    def test_rest_over_the_limit_refused(self):
        src = HEAD + BIG.replace('    threadgroup uint _38;\n', '    threadgroup uint _38[9000];\n')
        with self.assertRaises(ValueError): tg_spill(src)


@unittest.skipUnless(shutil.which('xcrun') and shutil.which('clang'), 'needs Xcode')
class OnGpu(unittest.TestCase):
    RUNNER = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *e = nil;
        if (!dev) { printf("no Metal device\n"); return 2; }
        NSData *d = [NSData dataWithContentsOfFile:@(argv[1])];
        id<MTLLibrary> lib = [dev newLibraryWithData:dispatch_data_create(d.bytes, d.length, nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT) error:&e];
        if (!lib) { printf("library: %s\n", e.localizedDescription.UTF8String); return 1; }
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"main0"] error:&e];
        if (!ps) { printf("pipeline: %s\n", e.localizedDescription.UTF8String); return 1; }
        const uint32_t slice = (uint32_t)atoi(argv[2]), head = (uint32_t)atoi(argv[3]), idx = (uint32_t)atoi(argv[4]), slices = (uint32_t)atoi(argv[5]), gx = 16, gy = 16, n = gx * gy * 64;
        id<MTLBuffer> tgm = [dev newBufferWithLength:head + (size_t)slice * slices options:MTLResourceStorageModeShared], out = [dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
        uint32_t *h = tgm.contents; h[0] = slice * slices; h[1] = 0;
        id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:ps]; [ce setBuffer:out offset:0 atIndex:1]; [ce setBuffer:tgm offset:0 atIndex:idx];
        [ce dispatchThreadgroups:MTLSizeMake(gx, gy, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (cb.error) { printf("command buffer: %s\n", cb.error.localizedDescription.UTF8String); return 1; }
        const uint32_t *o = out.contents; long bad = 0;
        for (uint32_t g = 0; g < gx * gy; g++) for (uint32_t t = 0; t < 64; t++) {
            uint32_t nb = (t + 1) & 63, s = 0;
            for (uint32_t i = 0; i < 192; i++) s += g * 100000u + nb * 1000u + i;
            bad += o[g * 64 + t] != s;
        }
        printf("%u threadgroups, %ld wrong outputs, flag %u\n", gx * gy, bad, h[1]);
        return bad != 0 || h[1] != 0;
    }
}
'''

    def test_rewritten_kernel_runs(self):
        out, slice_ = tg_spill(HEAD + BIG)
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / 't.metal').write_text(out)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', str(d / 't.metal'), '-o', str(d / 't.air')], check=True)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(d / 't.air'), '-o', str(d / 't.metallib')], check=True)
            (d / 'run.m').write_text(self.RUNNER)
            subprocess.run(['clang', '-fobjc-arc', '-O1', str(d / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(d / 'run')], check=True)
            r = subprocess.run([str(d / 'run'), str(d / 't.metallib'), str(slice_), str(abi.TGMEM_HEAD), str(abi.TGMEM_BUFFER), '256'], capture_output=True, text=True)
            if r.returncode == 2: self.skipTest(r.stdout.strip())
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            print(r.stdout.strip())


if __name__ == '__main__':
    unittest.main()
