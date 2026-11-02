"""A material's zero-period off state is black; nonzero expressions and perspective remain unchanged."""
import ast
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from typing import Any

TOOLS = Path(__file__).resolve().parents[2] / 'gfx/tools'
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
nodes: list[ast.stmt] = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'zero_period_off']
namespace: dict[str, Any] = {'re': re}
exec(compile(ast.Module(body=nodes, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), namespace)
off = namespace['zero_period_off']
SPEC = '4:9.w:10.x:1'

# Synthetic source, not a game shader. Position/UV are included for the GPU comparison.
MSL = '''#include <metal_stdlib>
using namespace metal;
struct Bank { float4 _m0[11]; };
struct Descriptors { constant Bank* vp_c4; };
struct main0_out {
    float4 position [[position]];
    float4 uv [[user(locn0)]];
    float4 color [[user(locn1)]];
};
main0_out main0(constant Bank& bank)
{
    Descriptors spvDescriptorSet0 = {&bank};
    main0_out out = {};
    float reciprocal = 1.0 / (*spvDescriptorSet0.vp_c4)._m0[9].w;
    float phase = reciprocal * bank._m0[0].y;
    float wave = cos(6.2831855 * (phase - floor(phase)));
    float pulse = wave * (*spvDescriptorSet0.vp_c4)._m0[10].x;
    float red = bank._m0[1].x;
    float green = bank._m0[1].y;
    float blue = bank._m0[1].z;
    float alpha = bank._m0[1].w;
    float r = fma(pulse, red, red);
    float g = fma(pulse, green, green);
    float b = fma(pulse, blue, blue);
    float a = fma(pulse, alpha, alpha);
    out.color.x = r;
    out.color.y = g;
    out.color.z = b;
    out.color.w = a;
    out.position = bank._m0[2];
    out.uv = bank._m0[3];
    return out;
}
'''

GPU = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
int main(int argc, const char **argv) { @autoreleasepool {
    (void)argc;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); if (!dev) return 77;
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];
    if (!lib) { fprintf(stderr,"%s\n",e.description.UTF8String); return 1; }
    id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"compare"] error:&e];
    if (!ps) { fprintf(stderr,"%s\n",e.description.UTF8String); return 1; }
    NSData *input = [NSData dataWithContentsOfFile:@(argv[2])]; NSUInteger n = input.length / 176;
    id<MTLBuffer> a = [dev newBufferWithBytes:input.bytes length:input.length options:MTLResourceStorageModeShared];
    id<MTLBuffer> b = [dev newBufferWithLength:n*96 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [[dev newCommandQueue] commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:ps]; [enc setBuffer:a offset:0 atIndex:0]; [enc setBuffer:b offset:0 atIndex:1];
    [enc dispatchThreads:MTLSizeMake(n,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { fprintf(stderr,"%s\n",cb.error.description.UTF8String); return 1; }
    return [[NSData dataWithBytes:b.contents length:b.length] writeToFile:@(argv[3]) atomically:YES] ? 0 : 1;
}}
'''


class ZeroPeriodOff(unittest.TestCase):
    def test_black_carrier_preserves_perspective_and_original_nonzero_expressions(self):
        result = off(MSL, SPEC)
        self.assertIn('float4 color [[user(locn1)]];', result)
        self.assertNotIn('flat', result)
        self.assertNotIn('isfinite', result)
        self.assertIn('bool dm_tv_off = (as_type<uint>((*spvDescriptorSet0.vp_c4)._m0[9].w) & 0x7fffffffu) == 0u;', result)
        for component, value, original in [('x', '0.0', 'r'), ('y', '0.0', 'g'), ('z', '0.0', 'b'), ('w', '1.0', 'a')]:
            self.assertIn(f'out.color.{component} = dm_tv_off ? {value} : {original};', result)
        for line in MSL.splitlines():
            if line.strip().startswith(('float wave', 'float pulse', 'float r =', 'float g =', 'float b =', 'float a =', 'out.position', 'out.uv')):
                self.assertIn(line, result)

    def test_explicit_fields_location_and_generated_names(self):
        source = MSL.replace('vp_c4', 'vp_c7').replace('[9].w', '[13].y').replace('[10].x', '[14].z')
        source = source.replace('color', 'renamed').replace('locn1', 'locn3')
        result = off(source, '7:13.y:14.z:3')
        self.assertIn('bool dm_tv_off = (as_type<uint>((*spvDescriptorSet0.vp_c7)._m0[13].y) & 0x7fffffffu) == 0u;', result)
        self.assertIn('out.renamed.w = dm_tv_off ? 1.0 : a;', result)

    def test_changed_ambiguous_and_repeated_shapes_fail_closed(self):
        for source in (MSL.replace('1.0 / ', '2.0 / '), MSL.replace('wave * ', 'wave + '), MSL + MSL,
                       MSL.replace('out.color.w = a;', ''), MSL.replace('out.color.w = a;', 'out.color.z = a;'),
                       MSL.replace('fma(pulse, alpha, alpha)', 'fma(pulse, alpha, blue)'),
                       MSL.replace('return out;', 'out.color *= 0.5;\n    return out;'),
                       MSL.replace('locn1', 'locn2'), off(MSL, SPEC)):
            with self.subTest(source=source[:40]), self.assertRaises(ValueError):
                off(source, SPEC)

    def test_invalid_specs_fail_closed(self):
        for spec in ('4:9.q:10.x:1', '4:9.w:10.x', '4:9.w:10.x:1:extra', '4:-1.w:10.x:1', '4:9.w:10.x:-1'):
            with self.subTest(spec=spec), self.assertRaises(ValueError):
                off(MSL, spec)

    @unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun') and os.environ.get('DMSH_TEST_GPU') == '1',
                         'set DMSH_TEST_GPU=1 on a Mac with Metal')
    def test_gpu_zero_is_black_and_other_outputs_match_original(self):
        prefix, body = MSL.split('main0_out main0(', 1)
        guarded = off(MSL, SPEC).split('main0_out main0(', 1)[1]
        source = prefix + 'main0_out original(' + body + 'main0_out guarded(' + guarded + '''
kernel void compare(constant Bank* banks [[buffer(0)]], device float4* values [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    main0_out a = original(banks[i]); main0_out b = guarded(banks[i]);
    values[i*6] = a.position; values[i*6+1] = a.uv; values[i*6+2] = a.color;
    values[i*6+3] = b.position; values[i*6+4] = b.uv; values[i*6+5] = b.color;
}
'''
        subnormal = [struct.unpack('<f', struct.pack('<I', bits))[0] for bits in (1, 0x80000001, 0x7fffff, 0x807fffff)]
        periods = [0.0, -0.0, 1.5, -1.5, 3.0, 0.25, 1e-20, -1e-20, 1e20, -1e20, math.nan, math.inf, -math.inf] + subnormal
        cases = [(period, time) for period in periods for time in (0.0, 0.125, 1.0, 2.75, 10.0)]
        inputs = bytearray()
        for period, time in cases:
            values = [0.0] * 44
            values[1] = time
            values[4:8] = [0.2, 0.4, 0.8, 0.5]
            values[8:12] = [0.1, -0.3, 0.6, 1.25]
            values[12:16] = [0.25, 0.75, 0.125, 0.5]
            values[39], values[40] = period, 0.1
            inputs.extend(struct.pack('<44f', *values))
        with tempfile.TemporaryDirectory(prefix='dmsh_tv_off_') as directory:
            work = Path(directory)
            (work / 'check.metal').write_text(source); (work / 'input.bin').write_bytes(inputs)
            commands = [
                ['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance',
                 '-fmodules-cache-path=' + str(work / 'modules'), str(work / 'check.metal'), '-o', str(work / 'check.air')],
                ['xcrun', '-sdk', 'macosx', 'metallib', str(work / 'check.air'), '-o', str(work / 'check.metallib')],
                ['xcrun', 'clang', '-fobjc-arc', '-x', 'objective-c', '-', '-framework', 'Foundation', '-framework', 'Metal', '-o', str(work / 'check')],
                [str(work / 'check'), str(work / 'check.metallib'), str(work / 'input.bin'), str(work / 'output.bin')]]
            for command in commands:
                result = subprocess.run(command, input=GPU if '-x' in command else None, text=True, capture_output=True)
                if result.returncode == 77: self.skipTest('no Metal device')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            data = (work / 'output.bin').read_bytes()
        self.assertEqual(len(data), len(cases) * 96)
        for i, (period, time) in enumerate(cases):
            row = data[i * 96:(i + 1) * 96]
            with self.subTest(period=period, time=time):
                self.assertEqual(row[:32], row[48:80], 'position/UV changed')
                if period == 0.0:
                    self.assertEqual(struct.unpack('<4f', row[80:96]), (0.0, 0.0, 0.0, 1.0))
                else:
                    for a, b in zip(struct.iter_unpack('<f', row[32:48]), struct.iter_unpack('<f', row[80:96])):
                        self.assertTrue((math.isnan(a[0]) and math.isnan(b[0])) or struct.pack('<f', a[0]) == struct.pack('<f', b[0]))


if __name__ == '__main__':
    unittest.main()
