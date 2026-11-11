"""Signed integer overflow wraps in translated shaders (-fwrapv in dmsh_build.py MAC_METAL, ios_metal.py METAL, vk_shaders.py and gl_shaders.py): SPIR-V, GLSL and the Switch's GPUs wrap,
while MSL leaves it undefined, and without the flag the compiler folds Ocarina of Time's low-half extraction (an IMAD by 65536, then a shift right by 16) to the whole word. On this Mac's GPU,
with the chain's own compile command, the idioms below give the wrapped results; the iPhone command keeps the shift."""
import ast
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / 'dm/gfx/tools'
sys.path.insert(0, str(ROOT / 'dm/ios/tools'))
import ios_metal  # noqa: E402


def mac_metal():   # dmsh_build.py's MAC_METAL, read without running the script
    for n in ast.parse((TOOLS / 'dmsh_build.py').read_text()).body:
        if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'MAC_METAL' for t in n.targets):
            sys.path.insert(0, str(TOOLS)); import dmsh_chain   # the line names its deployment target, dmsh_chain.MAC_MIN_FLAG
            return eval(compile(ast.Expression(n.value), 'dmsh_build.py', 'eval'), {'dmsh_chain': dmsh_chain})
    raise AssertionError('no MAC_METAL in dmsh_build.py')


# The low half sign-extended as Ocarina's mesh shader 35785cddc971290c takes it (a = as_type<int>(w) * 65536, then (a >> 31) << 16 | uint(a) >> 16; without -fwrapv the compiler takes the
# sign bits from bits 15-30 of w), an overflow test it may fold to false (x + C < x), and a doubling and halving it may fold to x
KERNEL = '''#include <metal_stdlib>
using namespace metal;
kernel void check(device const uint* input [[buffer(0)]], device uint* output [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    uint w = input[i];
    int a = as_type<int>(w) * 65536;
    int lo = ((a >> 31) << 16) | int(uint(int(uint(a) >> uint(16))));
    int b = as_type<int>(w) + 2147483647;
    int c = as_type<int>(w) * 2 / 2;
    device uint* o = output + i * 3;
    o[0] = as_type<uint>(lo); o[1] = b < as_type<int>(w) ? 1u : 0u; o[2] = as_type<uint>(c);
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
    id<MTLBuffer> in = [d newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [d newBufferWithLength:n * 12 options:MTLResourceStorageModeShared];
    uint32_t edge[] = {0, 1, 0x7fff, 0x8000, 0xffff, 0x10000, 0x12345678, 0x3fffffff, 0x40000000, 0x7fffffff, 0x80000000, 0x80000001, 0xc0000000, 0xfffffffe, 0xffffffff};
    unsigned ne = sizeof(edge) / sizeof(edge[0]);
    uint32_t *a = in.contents, state = 0x12345678;
    for (unsigned i = 0; i < n; i++) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; a[i] = i < ne ? edge[i] : state; }
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
    [en setComputePipelineState:ps]; [en setBuffer:in offset:0 atIndex:0]; [en setBuffer:out offset:0 atIndex:1];
    [en dispatchThreadgroups:MTLSizeMake(n/64,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
    [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.status == MTLCommandBufferStatusError) { fprintf(stderr, "%s\n", cb.error.localizedDescription.UTF8String); return 1; }
    const uint32_t *b = out.contents;
    for (unsigned i = 0; i < n; i++) {
        uint32_t w = a[i];
        uint32_t want[3] = {(uint32_t)(int32_t)(int16_t)(w & 0xffff), (int32_t)(w + 0x7fffffffu) < (int32_t)w, (uint32_t)((int32_t)(w * 2u) / 2)};
        for (unsigned k = 0; k < 3; k++)
            if (b[3*i+k] != want[k]) { fprintf(stderr, "result %u at %u (%08x): %08x, want %08x\n", k, i, w, b[3*i+k], want[k]); return 1; }
    }
    return 0;
 }
}
'''


class WrappingOverflow(unittest.TestCase):
    def test_every_compile_wraps(self):
        self.assertIn('-fwrapv', mac_metal())
        self.assertIn('-fwrapv', ios_metal.METAL)
        for tool in ('vk_shaders.py', 'gl_shaders.py'):
            calls = re.findall(r"\['xcrun', '-sdk', sdk, 'metal', '-c'[^\]]*\]", (TOOLS / tool).read_text())
            self.assertTrue(calls, tool)
            for c in calls: self.assertIn("'-fwrapv'", c, tool)

    @unittest.skipUnless(shutil.which('xcrun') and shutil.which('clang'), 'needs Xcode and Metal')
    def test_gpu_wraps(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'test.metal').write_text(KERNEL)
            (root / 'run.m').write_text(RUNNER)
            subprocess.run(mac_metal() + [str(root / 'test.metal'), '-o', str(root / 'test.air')], check=True, capture_output=True)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(root / 'test.air'), '-o', str(root / 'test.metallib')], check=True, capture_output=True)
            subprocess.run(['clang', '-fobjc-arc', '-O1', str(root / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(root / 'run')], check=True, capture_output=True)
            r = subprocess.run([str(root / 'run'), str(root / 'test.metallib')], capture_output=True, text=True)
            if r.returncode == 2: self.skipTest('no Metal device')
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    @unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode and Metal')
    def test_ios_keeps_the_shift(self):
        # the iPhone command's IR keeps the shift left by 16 that the sign extension starts from (it compiles for no device here)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'test.metal').write_text(KERNEL.replace('int b =', 'int b = 0; //').replace('int c =', 'int c = 0; //'))
            subprocess.run(ios_metal.METAL + ['-S', '-emit-llvm', str(root / 'test.metal'), '-o', str(root / 'test.ll')], check=True, capture_output=True)
            body = (root / 'test.ll').read_text()
            body = body[body.index('@check('):]
            body = body[:body.index('\n}\n')]
            self.assertRegex(body, r'shl i32 %\S+, 16\n')


if __name__ == '__main__':
    unittest.main()
