"""Extended multiplication (dmsh_build.py mul_extended): scalar spvMulExtended calls reach overloads outside spirv-cross's optnone template, and on this Mac's GPU their low and
high halves match 64-bit products for literal (power of two or not) and variable multipliers, unsigned and signed, including overflow; vector calls keep the template."""
import ast
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'mul_extended']
scope = {'re': re}
exec(compile(ast.Module(body=nodes, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
mul_extended = scope['mul_extended']

# spirv-cross's helper as it writes it, and its result structs
HEAD = '''#include <metal_stdlib>
using namespace metal;
struct U { uint lo; uint hi; };
struct S { int lo; int hi; };
struct U2 { uint2 lo; uint2 hi; };

template<typename T, typename U, typename V>
[[clang::optnone]] T spvMulExtended(V l, V r)
{
    return T{U(l * r), U(mulhi(l, r))};
}

'''
# multiplies by 4042322161 (a division constant), 3, 16 (high word only, as Ocarina's address math), 0x80000000, a variable, signed by -7 and a variable, and a vector by a variable
KERNEL = '''kernel void check(device const uint2* input [[buffer(0)]], device uint* output [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    uint2 x = input[i];
    U a = spvMulExtended<U, uint>(uint(x.x), 4042322161u);
    U b = spvMulExtended<U, uint>(as_type<uint>(x.y), 3u);
    uint c = (spvMulExtended<U, uint>(uint(x.x), 16u)).hi;
    U d = spvMulExtended<U, uint>(x.y, 2147483648u);
    U e = spvMulExtended<U, uint>(x.x, x.y);
    S f = spvMulExtended<S, int>(as_type<int>(x.x), -7);
    S g = spvMulExtended<S, int>(as_type<int>(x.x), as_type<int>(x.y));
    U2 h = spvMulExtended<U2, uint2>(x, x.yx);
    device uint* o = output + i * 16;
    o[0] = a.lo; o[1] = a.hi; o[2] = b.lo; o[3] = b.hi; o[4] = c; o[5] = d.lo; o[6] = d.hi; o[7] = e.lo; o[8] = e.hi;
    o[9] = as_type<uint>(f.lo); o[10] = as_type<uint>(f.hi); o[11] = as_type<uint>(g.lo); o[12] = as_type<uint>(g.hi); o[13] = h.lo.x; o[14] = h.hi.x; o[15] = h.hi.y;
}
'''
RUNNER = r'''#import <Metal/Metal.h>
#include <stdint.h>
#include <stdio.h>
int main(int argc, char **argv) {
 @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    if (!d) return 2;
    NSError *err = nil;
    id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&err];
    id<MTLComputePipelineState> ps = lib ? [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"check"] error:&err] : nil;
    if (!ps) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 1; }
    const unsigned n = 65536;
    id<MTLBuffer> in = [d newBufferWithLength:n * 8 options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [d newBufferWithLength:n * 64 options:MTLResourceStorageModeShared];
    uint32_t edge[] = {0, 1, 2, 3, 0x7fffffff, 0x80000000, 0x80000001, 0xfffffffe, 0xffffffff, 0x00010000, 0x0000ffff, 0xffff0000, 4042322161u, 2290649225u, 954437177u};
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
        uint32_t x = a[2*i], y = a[2*i+1];
        uint64_t pa = (uint64_t)x * 4042322161u, pb = (uint64_t)y * 3u, pc = (uint64_t)x * 16u, pd = (uint64_t)y * 0x80000000u, pe = (uint64_t)x * y;
        uint64_t pf = (uint64_t)((int64_t)(int32_t)x * -7), pg = (uint64_t)((int64_t)(int32_t)x * (int32_t)y), ph = (uint64_t)y * x;
        uint32_t want[16] = {(uint32_t)pa, (uint32_t)(pa >> 32), (uint32_t)pb, (uint32_t)(pb >> 32), (uint32_t)(pc >> 32), (uint32_t)pd, (uint32_t)(pd >> 32),
                             (uint32_t)pe, (uint32_t)(pe >> 32), (uint32_t)pf, (uint32_t)(pf >> 32), (uint32_t)pg, (uint32_t)(pg >> 32), (uint32_t)pe, (uint32_t)(pe >> 32), (uint32_t)(ph >> 32)};
        for (unsigned k = 0; k < 16; k++)
            if (b[16*i+k] != want[k]) { fprintf(stderr, "result %u at %u (%08x, %08x): %08x, want %08x\n", k, i, x, y, b[16*i+k], want[k]); return 1; }
    }
    return 0;
 }
}
'''


class ExtendedMultiply(unittest.TestCase):
    def test_overloads_follow_the_helper(self):
        out = mul_extended(HEAD + KERNEL)
        self.assertIn('[[clang::optnone]] T spvMulExtended(V l, V r)', out)
        self.assertIn('T spvMulExtended(uint l, uint r) { ulong p = ulong(l) * ulong(r); return T{U(p), U(p >> 32)}; }', out)
        self.assertIn('T spvMulExtended(int l, int r) { long p = long(l) * long(r); return T{U(p), U(p >> 32)}; }', out)
        self.assertLess(out.index('mulhi(l, r)'), out.index('spvMulExtended(uint l'))
        self.assertTrue(out.endswith(KERNEL))   # calls are left as they are
        self.assertNotIn('mulhi', out.replace('U(mulhi(l, r))', ''))
        self.assertEqual(mul_extended(out), out)

    def test_no_helper_changed_helper_or_two_helpers_unchanged(self):
        for src in (KERNEL, HEAD.replace('U(mulhi(l, r))', 'U(mulhi(l, r) + 1)') + KERNEL, HEAD + HEAD + KERNEL,
                    HEAD.replace('[[clang::optnone]] ', '') + KERNEL):
            self.assertEqual(mul_extended(src), src)

    @unittest.skipUnless(shutil.which('xcrun') and shutil.which('clang'), 'needs Xcode and Metal')
    def test_gpu_matches_64_bit_products(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'test.metal').write_text(mul_extended(HEAD + KERNEL))
            (root / 'run.m').write_text(RUNNER)
            for sdk in ('macosx', 'iphoneos'):
                subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-O2', '-fpreserve-invariance', str(root / 'test.metal'), '-o', str(root / f'{sdk}.air')], check=True, capture_output=True)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(root / 'macosx.air'), '-o', str(root / 'test.metallib')], check=True, capture_output=True)
            subprocess.run(['clang', '-fobjc-arc', '-O1', str(root / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(root / 'run')], check=True, capture_output=True)
            r = subprocess.run([str(root / 'run'), str(root / 'test.metallib')], capture_output=True, text=True)
            if r.returncode == 2: self.skipTest('no Metal device')
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    @unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode and Metal')
    def test_overloads_are_called(self):
        # the scalar calls reach the overloads: the compiled kernel has no optnone call left for them, only the vector one
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'test.metal').write_text(mul_extended(HEAD + KERNEL))
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-S', '-emit-llvm', '-O2', str(root / 'test.metal'), '-o', str(root / 'test.ll')], check=True, capture_output=True)
            ll = (root / 'test.ll').read_text()
            body = ll[ll.index('@check('):]
            body = body[:body.index('\n}\n')]
            calls = re.findall(r'call [^@]*@(\S*spvMulExtended\S*)\(', body)
            self.assertEqual(len(calls), 1, calls)
            self.assertIn('Dv2_j', calls[0])   # uint2


if __name__ == '__main__':
    unittest.main()
