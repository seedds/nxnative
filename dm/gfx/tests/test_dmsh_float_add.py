#!/usr/bin/env python3
"""A float add to storage memory (RED.F32's compare-and-swap loop, InstEmitMemory EmitAtomicAddF32) rewritten as one native atomic_fetch_add on a device atomic_float
(dmsh_build.py float_add_msl): the loop as spirv-cross writes it is replaced and any other loop is left alone; and on this Mac's GPU 8,192 threads adding into the same four words,
through the loop and through the rewrite, reach the exact sums."""
import ast
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
NAMES = {'FLOAT_ADD', 'float_add_msl'}
scope = {'re': __import__('re')}
exec(compile(ast.Module(body=[n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name in NAMES) or
                              (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) in NAMES for t in n.targets))], type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
float_add_msl = scope['float_add_msl']

# The loop of 6e28359aa3558f61 (Ocarina of Time), on word w of buffer b, adding v
def loop(b, w, v, n):
    return f"""    bool _{n};
    do
    {{
        float _{n + 1} = as_type<float>({b}._m0[{w}]);
        float _{n + 2} = _{n + 1} + {v};
        uint _{n + 3} = as_type<uint>(_{n + 1});
        uint _{n + 4};
        do
        {{
            _{n + 4} = _{n + 3};
        }} while (!atomic_compare_exchange_weak_explicit((device atomic_uint*)&{b}._m0[{w}], &_{n + 4}, as_type<uint>(_{n + 2}), memory_order_relaxed, memory_order_relaxed) && _{n + 4} == _{n + 3});
        uint _{n + 5} = _{n + 4};
        _{n} = int(_{n + 5}) != as_type<int>(_{n + 1});
    }} while (_{n});
"""

KERNEL = """#include <metal_stdlib>
using namespace metal;
struct B { uint _m0[1]; };
kernel void k(device B& b [[buffer(0)]], uint id [[thread_position_in_grid]])
{
    float v = float(id & 7u) * 0.25f, w = 1.0f;
%s}
"""


class FloatAdd(unittest.TestCase):
    def test_loop_becomes_one_atomic_add(self):
        src = loop('(*spvDescriptorSet0.cp_s4)', 0, '_2580', 10) + loop('(*spvDescriptorSet0.cp_s4)', 'int(_7)', '_2572', 20)
        out = float_add_msl(src)
        self.assertEqual(out, '    atomic_fetch_add_explicit((device atomic_float*)&(*spvDescriptorSet0.cp_s4)._m0[0], _2580, memory_order_relaxed);\n'
                              '    atomic_fetch_add_explicit((device atomic_float*)&(*spvDescriptorSet0.cp_s4)._m0[int(_7)], _2572, memory_order_relaxed);\n')

    def test_other_words_and_shapes_left_alone(self):
        other = loop('b', 0, 'v', 10).replace('&b._m0[0], &', '&b._m0[1], &')   # the swap on another word than the read
        self.assertEqual(float_add_msl(other), other)
        used = loop('b', 0, 'v', 10) + '    out = _11;\n'   # a loop whose read is used after it is the same text: the rewrite drops only the do-block's own names
        self.assertNotIn('_11 = as_type<float>', float_add_msl(used))

    @unittest.skipUnless(shutil.which('xcrun') and sys.platform == 'darwin', 'needs Metal')
    def test_sums_on_gpu(self):
        body = loop('b', 0, 'v', 10) + loop('b', 1, 'w', 20) + loop('b', 2, 'v', 30) + loop('b', 3, 'w', 40)
        runner = r'''#import <Metal/Metal.h>
#include <stdio.h>
int main(int ac, char **av) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); NSError *e = nil;
    for (int k = 1; k < ac; k++) {
        id<MTLLibrary> l = [d newLibraryWithURL:[NSURL fileURLWithPath:@(av[k])] error:&e]; if (!l) { puts(e.description.UTF8String); return 2; }
        id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:[l newFunctionWithName:@"k"] error:&e]; if (!ps) { puts(e.description.UTF8String); return 2; }
        id<MTLBuffer> b = [d newBufferWithLength:16 options:MTLResourceStorageModeShared]; memset(b.contents, 0, 16);
        id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLCommandBuffer> c = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [c computeCommandEncoder];
        [ce setComputePipelineState:ps]; [ce setBuffer:b offset:0 atIndex:0]; [ce dispatchThreads:MTLSizeMake(8192, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)]; [ce endEncoding]; [c commit]; [c waitUntilCompleted];
        const float *f = b.contents; printf("%g %g %g %g\n", f[0], f[1], f[2], f[3]);
    }
    return 0;
} }
'''
        with tempfile.TemporaryDirectory() as w:
            w = Path(w); libs = []
            for name, src in (('loop', KERNEL % body), ('native', KERNEL % float_add_msl(body))):
                (w / f'{name}.metal').write_text(src)
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', str(w / f'{name}.metal'), '-o', str(w / f'{name}.air')], check=True, capture_output=True)
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(w / f'{name}.air'), '-o', str(w / f'{name}.metallib')], check=True)
                libs.append(str(w / f'{name}.metallib'))
            self.assertIn('atomic_float', (w / 'native.metal').read_text())
            (w / 'r.m').write_text(runner)
            subprocess.run(['clang', '-fobjc-arc', '-framework', 'Metal', '-framework', 'Foundation', str(w / 'r.m'), '-o', str(w / 'r')], check=True, capture_output=True)
            out = subprocess.run([str(w / 'r')] + libs, capture_output=True, text=True)
            want = f'{sum((i & 7) * 0.25 for i in range(8192)):g} 8192 {sum((i & 7) * 0.25 for i in range(8192)):g} 8192'
            self.assertEqual(out.stdout.split('\n')[:2], [want, want], out.stdout + out.stderr)


if __name__ == '__main__':
    unittest.main()
