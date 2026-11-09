"""Compute shared memory past Metal's 32 KB (dmsh_build.py tg_spill): a kernel whose threadgroup arrays exceed abi.TG_MAX takes them from the device buffer at abi.TG_SLOT, one slice per
workgroup at its index in the whole grid, with barriers that order device memory; a kernel within the limit is left as it is. The shape is Mario Kart World's DrivePassShaderDefault
compute shaders (spvUnsafeArray<uint, 12288>, 48 KB). When the Metal compiler is installed, the rewritten kernel is compiled too, and with Xcode it runs over 1,024 workgroups on this Mac's
GPU, more than run at once: no workgroup may see another's data in its shared memory, as with the index modulo 512 they did."""
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
import dmsh_abi as abi
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
selected = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name == 'tg_spill')
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) in ('TG_DECL', 'TG_BYTES') for t in n.targets))]
scope = {'re': re, 'abi': abi}
exec(compile(ast.Module(body=selected, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
tg_spill = scope['tg_spill']

ARRAY = """template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    thread T& operator [] (size_t pos) thread { return elements[pos]; }
    device T& operator [] (size_t pos) device { return elements[pos]; }
    threadgroup T& operator [] (size_t pos) threadgroup { return elements[pos]; }
};
"""
KERNEL = """#include <metal_stdlib>
using namespace metal;
""" + ARRAY + """
struct B { uint _m0[1]; };
kernel void main0(device B& out [[buffer(1)]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    threadgroup spvUnsafeArray<uint, %d> _39;
    threadgroup float4 _40[4];
    _39[gl_LocalInvocationID.x] = gl_WorkGroupID.x;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    _40[0] = float4(1.0);
    out._m0[0] = _39[(gl_LocalInvocationID.x + 1u) %% 64u] + uint(_40[0].x);
}
"""


class TgSpill(unittest.TestCase):
    def test_past_the_limit(self):
        out, n = tg_spill(KERNEL % 12288)
        self.assertEqual(n, 12288 * 4 + 4 * 16)
        self.assertIn(f'device uchar* dm_tg [[buffer({abi.TG_SLOT})]]', out)
        self.assertIn('uint3 dm_ntg [[threadgroups_per_grid]]', out)
        self.assertNotIn('dm_tgid', out)   # the kernel's own workgroup position is used
        self.assertIn(f'ulong dm_tgbase = ((ulong(gl_WorkGroupID.z) * dm_ntg.y + gl_WorkGroupID.y) * dm_ntg.x + gl_WorkGroupID.x) * {n}ul;', out)   # the whole grid, in 64 bits
        self.assertIn('device spvUnsafeArray<uint, 12288>& _39 = *(device spvUnsafeArray<uint, 12288>*)(dm_tg + dm_tgbase + 0);', out)
        self.assertIn(f'device float4 (&_40)[4] = *(device float4 (*)[4])(dm_tg + dm_tgbase + {12288 * 4});', out)
        self.assertIn('threadgroup_barrier(mem_flags::mem_device | mem_flags::mem_threadgroup);', out)
        self.assertIn('threadgroup T& operator [] (size_t pos) threadgroup', out)   # the template keeps its overloads
        self.assertNotRegex(out, r'^\s*threadgroup (spvUnsafeArray|float4) ', 'no threadgroup array is left')

    def test_within_the_limit(self):
        src = KERNEL % 4096
        self.assertEqual(tg_spill(src), (src, 0))

    def test_no_position_parameter(self):
        src = (KERNEL % 12288).replace(', uint3 gl_WorkGroupID [[threadgroup_position_in_grid]]', '').replace('= gl_WorkGroupID.x', '= 0u')
        out, n = tg_spill(src)
        self.assertTrue(n)
        self.assertIn('uint3 dm_tgid [[threadgroup_position_in_grid]]', out)
        self.assertIn('((ulong(dm_tgid.z) * dm_ntg.y + dm_tgid.y) * dm_ntg.x + dm_tgid.x)', out)

    def test_unknown_element_type(self):
        with self.assertRaises(ValueError):
            tg_spill((KERNEL % 12288).replace('threadgroup float4 _40[4];', 'threadgroup S _40[4];'))

    @unittest.skipUnless(shutil.which('xcrun') and subprocess.run(['xcrun', '-sdk', 'macosx', '-f', 'metal'], capture_output=True).returncode == 0, 'no Metal compiler')
    def test_compiles(self):
        out, n = tg_spill(KERNEL % 12288)
        with tempfile.TemporaryDirectory() as d:
            src = Path(d, 'k.metal'); src.write_text(out)
            r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', str(src), '-o', str(Path(d, 'k.air'))], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-2000:])


# Each of 64 threads fills 192 words of the 48 KB array with values of its workgroup, and after the barrier sums its neighbour's (the shape of the PR #1242 test)
GPU_KERNEL = """#include <metal_stdlib>
using namespace metal;
""" + ARRAY + """
kernel void main0(device uint* out [[buffer(1)]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    threadgroup spvUnsafeArray<uint, 12288> _37;
    uint t = gl_LocalInvocationID.x;
    uint g = gl_WorkGroupID.y * 16u + gl_WorkGroupID.x;
    for (uint i = 0u; i < 192u; i++) { _37[t * 192u + i] = g * 100000u + t * 1000u + i; }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint nb = (t + 1u) & 63u, s = 0u;
    for (uint i = 0u; i < 192u; i++) { s += _37[nb * 192u + i]; }
    out[g * 64u + t] = s;
}
"""
RUNNER = r"""#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
int main(int argc, char **argv) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *e = nil;
    if (!dev) { printf("no Metal device\n"); return 2; }
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];
    id<MTLComputePipelineState> ps = lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"main0"] error:&e] : nil;
    if (!ps) { printf("pipeline: %s\n", e.localizedDescription.UTF8String); return 1; }
    const uint32_t slice = (uint32_t)atoi(argv[2]), slot = (uint32_t)atoi(argv[3]), gw = 16, gh = 64, groups = gw * gh;
    id<MTLBuffer> tg = [dev newBufferWithLength:(size_t)slice * groups options:MTLResourceStorageModeShared];   // one slice per workgroup, as nvn_mtl.m dispatch binds
    id<MTLBuffer> out = [dev newBufferWithLength:groups * 64 * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    long wrong = 0;
    for (int run = 0; run < 4; run++) {
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:ps]; [ce setBuffer:out offset:0 atIndex:1]; [ce setBuffer:tg offset:0 atIndex:slot];
        [ce dispatchThreadgroups:MTLSizeMake(gw, gh, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (cb.error) { printf("command buffer: %s\n", cb.error.localizedDescription.UTF8String); return 1; }
        const uint32_t *v = out.contents;
        for (uint32_t g = 0; g < groups; g++) for (uint32_t t = 0; t < 64; t++) {
            uint32_t nb = (t + 1) & 63, want = 0;
            for (uint32_t i = 0; i < 192; i++) want += g * 100000u + nb * 1000u + i;
            wrong += v[g * 64 + t] != want;
        }
    }
    printf("%u workgroups x 4 runs, %ld wrong sums\n", groups, wrong);
    return wrong != 0;
} }
"""


@unittest.skipUnless(shutil.which('xcrun') and shutil.which('clang'), 'needs Xcode')
class OnGpu(unittest.TestCase):
    def test_workgroups_keep_their_own_memory(self):
        out, n = tg_spill(GPU_KERNEL)
        self.assertEqual(n, 49152)
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / 'k.metal').write_text(out)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', str(d / 'k.metal'), '-o', str(d / 'k.air')], check=True)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(d / 'k.air'), '-o', str(d / 'k.metallib')], check=True)
            (d / 'run.m').write_text(RUNNER)
            subprocess.run(['clang', '-fobjc-arc', '-O1', str(d / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(d / 'run')], check=True)
            r = subprocess.run([str(d / 'run'), str(d / 'k.metallib'), str(n), str(abi.TG_SLOT)], capture_output=True, text=True)
            if r.returncode == 2: self.skipTest(r.stdout.strip())
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
