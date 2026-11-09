"""NoContraction helpers (dmsh_rewrite.py no_contraction): spirv-cross's optnone spvFAdd / spvFSub / spvFMul become always inline with their fma bodies, the compiled kernel has
no call left for them, and on this Mac's GPU the inlined forms still give the separately rounded results (a product rounded, then added), where a plain a * b + c fuses."""
import ast
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
import sys
sys.path.insert(0, str(TOOLS))
import dmsh_rewrite
no_contraction = dmsh_rewrite.no_contraction

# spirv-cross's helpers as it writes them
HEAD = '''#include <metal_stdlib>
using namespace metal;

template<typename T>
[[clang::optnone]] T spvFAdd(T l, T r)
{
    return fma(T(1), l, r);
}

template<typename T>
[[clang::optnone]] T spvFSub(T l, T r)
{
    return fma(T(-1), r, l);
}

template<typename T>
[[clang::optnone]] T spvFMul(T l, T r)
{
    return fma(l, r, T(0));
}

'''
# a product into a NoContraction add, both sides of a subtract, a NoContraction product into a plain add, a plain a * b + c (shares the product, so it is not checked), and a vector add
KERNEL = '''kernel void check(device const float4* input [[buffer(0)]], device float* output [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    float4 x = input[i];
    device float* o = output + i * 6;
    o[0] = spvFAdd(x.x * x.y, x.z);
    o[1] = spvFSub(x.x * x.y, x.w);
    o[2] = spvFSub(x.w, x.x * x.y);
    o[3] = spvFMul(x.x, x.y) + x.z;
    o[4] = x.x * x.y + x.z;
    o[5] = spvFAdd(float2(x.x) * x.yx, x.zz).x;
}
'''
# the control: the same sums without the helpers, which Apple's GPU compiler fuses into one fma (so the inputs do tell the two apart)
CONTROL = '''#include <metal_stdlib>
using namespace metal;
kernel void check(device const float4* input [[buffer(0)]], device float* output [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    float4 x = input[i];
    device float* o = output + i * 6;
    o[0] = x.x * x.y + x.z;
    o[1] = o[2] = o[3] = o[4] = o[5] = 0.0f;
}
'''
RUNNER = r'''#import <Metal/Metal.h>
#include <stdint.h>
#include <stdio.h>
#pragma clang fp contract(off)
int main(int argc, char **argv) {
 @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    if (!d) return 2;
    NSError *err = nil;
    id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&err];
    id<MTLComputePipelineState> ps = lib ? [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"check"] error:&err] : nil;
    if (!ps) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 1; }
    const unsigned n = 65536;
    id<MTLBuffer> in = [d newBufferWithLength:n * 16 options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [d newBufferWithLength:n * 24 options:MTLResourceStorageModeShared];
    float *a = in.contents; uint32_t s = 0x12345678;
    for (unsigned i = 0; i < n; i++) {
        if (i < 8192) {   // u * u rounds; adding back minus the rounded product gives 0 separately rounded, the rounding error fused
            float u = 1.0f + (float)(i + 1) * 0x1p-14f;
            volatile float p = u * u;
            a[4*i] = u; a[4*i+1] = u; a[4*i+2] = -p; a[4*i+3] = p;
        } else for (int j = 0; j < 4; j++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; a[4*i+j] = (float)(s >> 8) * 0x1p-20f - 8.0f; }
    }
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
    [en setComputePipelineState:ps]; [en setBuffer:in offset:0 atIndex:0]; [en setBuffer:out offset:0 atIndex:1];
    [en dispatchThreadgroups:MTLSizeMake(n/64,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
    [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.status == MTLCommandBufferStatusError) { fprintf(stderr, "%s\n", cb.error.localizedDescription.UTF8String); return 1; }
    const float *b = out.contents; unsigned bad[6] = {0};
    for (unsigned i = 0; i < n; i++) {
        volatile float p = a[4*i] * a[4*i+1];
        float want[6] = {p + a[4*i+2], p - a[4*i+3], a[4*i+3] - p, p + a[4*i+2], p + a[4*i+2], p + a[4*i+2]};
        for (int k = 0; k < 6; k++) if (b[6*i+k] != want[k]) bad[k]++;
    }
    printf("%u %u %u %u %u %u\n", bad[0], bad[1], bad[2], bad[3], bad[4], bad[5]);
    return 0;
 }
}
'''
METAL = ['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', '-fwrapv']   # dmsh_build.py MAC_METAL


class NoContraction(unittest.TestCase):
    def test_helpers_inline_with_their_bodies(self):
        out = no_contraction(HEAD + KERNEL)
        self.assertNotIn('optnone', out)
        for name, body in (('spvFAdd', 'fma(T(1), l, r)'), ('spvFSub', 'fma(T(-1), r, l)'), ('spvFMul', 'fma(l, r, T(0))')):
            self.assertIn(f'static inline __attribute__((always_inline)) T {name}(T l, T r)\n{{\n    return {body};\n}}', out)
        self.assertTrue(out.endswith(KERNEL))   # calls are left as they are
        self.assertEqual(no_contraction(out), out)

    def test_other_bodies_and_helpers_unchanged(self):
        for src in (KERNEL, HEAD.replace('fma(T(1), l, r)', 'l + r') + KERNEL,
                    '#include <metal_stdlib>\nusing namespace metal;\ntemplate<typename T, typename U, typename V>\n[[clang::optnone]] T spvMulExtended(V l, V r)\n{\n    return T{U(l * r), U(mulhi(l, r))};\n}\n'):
            out = no_contraction(src)
            if 'l + r' in src: self.assertIn('[[clang::optnone]] T spvFAdd', out)
            else: self.assertEqual(out, src)

    @unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode and Metal')
    def test_no_call_left(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'test.metal').write_text(no_contraction(HEAD + KERNEL))
            subprocess.run([*METAL[:4], '-S', '-emit-llvm', *METAL[5:], str(root / 'test.metal'), '-o', str(root / 'test.ll')], check=True, capture_output=True)
            ll = (root / 'test.ll').read_text()
            body = ll[ll.index('@check('):]
            body = body[:body.index('\n}\n')]
            self.assertEqual(re.findall(r'call [^@]*@(\S*spvF\S*)\(', body), [])
            self.assertEqual(len(re.findall(r'@air\.fma\.', body)), 5)   # each helper's fma is kept (o[4] has none)

    @unittest.skipUnless(shutil.which('xcrun') and shutil.which('clang'), 'needs Xcode and Metal')
    def test_gpu_keeps_products_rounded(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'run.m').write_text(RUNNER)
            subprocess.run(['clang', '-fobjc-arc', '-O1', '-ffp-contract=off', str(root / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(root / 'run')],
                           check=True, capture_output=True)
            for math in ([], ['-fno-fast-math']):   # the default (fast) and a profile's exact maths
                for src, control in ((no_contraction(HEAD + KERNEL), False), (CONTROL, True)):
                    (root / 'test.metal').write_text(src)
                    subprocess.run([*METAL, *math, str(root / 'test.metal'), '-o', str(root / 'test.air')], check=True, capture_output=True)
                    subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(root / 'test.air'), '-o', str(root / 'test.metallib')], check=True, capture_output=True)
                    r = subprocess.run([str(root / 'run'), str(root / 'test.metallib')], capture_output=True, text=True)
                    if r.returncode == 2: self.skipTest('no Metal device')
                    self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                    bad = [int(v) for v in r.stdout.split()]
                    if control: self.assertGreater(bad[0], 0, (math, r.stdout))   # a plain a * b + c fuses on this GPU
                    else: self.assertEqual(bad[:4] + bad[5:], [0, 0, 0, 0, 0], (math, r.stdout))   # NoContraction: the product rounded first

if __name__ == '__main__':
    unittest.main()
