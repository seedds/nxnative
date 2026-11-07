"""Constant unsigned extended multiplication: exact low/high halves, including overflow, on the GPU."""
import ast
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'extended_multiply_msl']
scope = {'re': re}
exec(compile(ast.Module(body=nodes, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
rewrite = scope['extended_multiply_msl']
HEAD = '''#include <metal_stdlib>
using namespace metal;
struct U { uint lo; uint hi; };
struct S { int lo; int hi; };
template<typename T, typename U, typename V>
[[clang::optnone]] T spvMulExtended(V l, V r) { return T{U(l * r), U(mulhi(l, r))}; }
'''
KERNEL = '''
kernel void check(device const uint2* input [[buffer(0)]], device uint4* output [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    uint2 x = input[i];
    U u = spvMulExtended<U, uint>(uint(x.x), 4042322161u);
    U s = spvMulExtended<U, uint>(as_type<uint>(x.y), 3u);
    output[i] = uint4(u.lo, u.hi, as_type<uint>(s.lo), as_type<uint>(s.hi));
}
'''
RUNNER = r'''
#import <Metal/Metal.h>
#include <stdint.h>
#include <stdio.h>
int main(int argc, char **argv) {
 @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    if (!d) return 2;
    NSError *err = nil;
    id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&err];
    id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"check"] error:&err];
    if (!ps) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 1; }
    const unsigned n = 65536;
    id<MTLBuffer> in = [d newBufferWithLength:n * 8 options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [d newBufferWithLength:n * 16 options:MTLResourceStorageModeShared];
    uint32_t edge[] = {0, 1, 2, 0x7fffffff, 0x80000000, 0xfffffffe, 0xffffffff, 0x00010000,
                      0x0000ffff, 0xffff0000, 4042322161u, 2290649225u, 954437177u};
    unsigned ne = sizeof(edge) / sizeof(edge[0]);
    uint32_t *a = in.contents, state = 0x12345678;
    for (unsigned i = 0; i < n; i++) {
        for (unsigned j = 0; j < 2; j++) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; a[2*i+j] = state; }
        if (i < ne*ne) { a[2*i] = edge[i/ne]; a[2*i+1] = edge[i%ne]; }
    }
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
    [en setComputePipelineState:ps]; [en setBuffer:in offset:0 atIndex:0]; [en setBuffer:out offset:0 atIndex:1];
    [en dispatchThreadgroups:MTLSizeMake(n/64,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
    [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.status == MTLCommandBufferStatusError) { fprintf(stderr, "%s\n", cb.error.localizedDescription.UTF8String); return 1; }
    const uint32_t *b = out.contents;
    for (unsigned i = 0; i < n; i++) {
        uint64_t u = (uint64_t)a[2*i] * 4042322161u;
        uint64_t s = (uint64_t)a[2*i+1] * 3u;
        if (b[4*i] != (uint32_t)u || b[4*i+1] != (uint32_t)(u>>32) || b[4*i+2] != (uint32_t)s || b[4*i+3] != (uint32_t)(s>>32)) {
            fprintf(stderr, "product mismatch at %u: %08x * %08x\n", i, a[2*i], a[2*i+1]); return 1;
        }
    }
    return 0;
 }
}
'''

class ExtendedMultiply(unittest.TestCase):
    def test_other_specializations_unchanged(self):
        source = HEAD + 'spvMulExtended<U, uint2>(a, b); spvMulExtended<U, ulong>(a, b);'
        self.assertEqual(rewrite(source), source)

    def test_missing_insertion_point_is_unchanged(self):
        source = KERNEL
        self.assertEqual(rewrite(source), source)

    def test_unsigned_literal_products(self):
        result = rewrite(HEAD + KERNEL)
        self.assertIn('ulong p = ulong(l) * ulong(r)', result)
        self.assertIn('dmsh_mul_extended_uint<U>(uint(x.x), 4042322161u)', result)
        self.assertIn('dmsh_mul_extended_uint<U>(as_type<uint>(x.y), 3u)', result)
        self.assertIn('[[clang::optnone]] T spvMulExtended', result)
        self.assertEqual(rewrite(result), result)

    def test_signed_dynamic_and_powers_of_two_unchanged(self):
        for call in ['spvMulExtended<S, int>(int(x), 3)', 'spvMulExtended<U, uint>(uint(x), y)',
                     'spvMulExtended<U, uint>(x, 3u)', 'spvMulExtended<U, uint>(uint(x), 4294967296u)'] + [
                         f'spvMulExtended<U, uint>(uint(x), {n}u)' for n in [0, 1] + [1 << p for p in range(1, 32)]]:
            with self.subTest(call=call): self.assertEqual(rewrite(HEAD + call), HEAD + call)

    def test_changed_or_ambiguous_helper_is_unchanged(self):
        for source in [HEAD.replace('U(mulhi(l, r))', 'U(mulhi(l, r) + 1)') + KERNEL,
                       HEAD + HEAD + KERNEL]:
            self.assertEqual(rewrite(source), source)

    @unittest.skipUnless(shutil.which('xcrun') and shutil.which('clang'), 'needs Xcode and Metal')
    def test_gpu_matches_full_width_integer_products(self):
        for left, right in [(4042322161, 3), (2290649225, 954437177), (6, 4294967295)]:
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root/'test.metal').write_text(rewrite(HEAD + KERNEL.replace('4042322161u', f'{left}u').replace('3u', f'{right}u')))
                (root/'run.m').write_text(RUNNER.replace('4042322161u', f'{left}u').replace('* 3u', f'* {right}u'))
                for sdk in ('macosx', 'iphoneos'):
                    subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-O1', '-fpreserve-invariance', str(root/'test.metal'), '-o', str(root/f'{sdk}.air')], check=True, capture_output=True)
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(root/'macosx.air'), '-o', str(root/'test.metallib')], check=True, capture_output=True)
                subprocess.run(['clang', '-fobjc-arc', '-O1', str(root/'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(root/'run')], check=True, capture_output=True)
                result = subprocess.run([str(root/'run'), str(root/'test.metallib')], capture_output=True, text=True)
                if result.returncode == 2: self.skipTest('no Metal device')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
