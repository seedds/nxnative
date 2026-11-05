"""Integer attachments reinterpret fragment output bits, irrespective of dataflow.

The fixtures contain no game bytes. Set DMSH_TEST_GPU=1 to render flat-varying
copies into R8Uint/R32Uint and arithmetic results into R32Uint on a Mac.
"""
import ast
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from typing import Any
import unittest


TOOLS = Path(__file__).resolve().parents[1] / 'tools'
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
node = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'fs_uint')
ns: dict[str, Any] = {'re': re}
exec(compile(ast.Module(body=[node], type_ignores=[]), 'fs_uint', 'exec'), ns)
fs_uint = ns['fs_uint']

BASE = '''#include <metal_stdlib>
using namespace metal;
struct main0_out
{
    float4 color [[color(0)]];
};
struct main0_in
{
    float4 raw [[user(locn0), flat]];
};
fragment main0_out main0(main0_in in [[stage_in]])
{
    main0_out out = {};
    float copied = in.raw.x;
    out.color.x = copied;
    return out;
}
'''
VERTEX = '''
struct vertex_out {
    float4 position [[position]];
    float4 raw [[user(locn0), flat]];
};
vertex vertex_out vertex_main(uint i [[vertex_id]], constant uint& bits [[buffer(0)]]) {
    float2 p[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return vertex_out{float4(p[i], 0, 1), as_type<float4>(uint4(bits, 0, 0, 0))};
}
'''
ARITHMETIC = BASE.replace('out.color.x = copied;', 'out.color.x = copied * 2.0f;')
MIXED = BASE.replace('float4 color [[color(0)]];', 'float4 color [[color(0)]];\n    float4 other [[color(3)]];').replace(
    'out.color.x = copied;', 'out.color.x = as_type<float>(17u);\n    out.other = float4(copied);')

RUNNER = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 3) return 2;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return 77;
    NSError *error = nil;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    unsigned cases = 0;
    for (int arithmetic = 0; arithmetic < 2; arithmetic++) {
        id<MTLLibrary> lib = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[arithmetic + 1])] error:&error];
        if (!lib) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
        for (int narrow = 0; narrow < (arithmetic ? 1 : 2); narrow++) {
            MTLPixelFormat format = narrow ? MTLPixelFormatR8Uint : MTLPixelFormatR32Uint;
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:2 height:2 mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
            id<MTLTexture> texture = [device newTextureWithDescriptor:td];
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [lib newFunctionWithName:@"vertex_main"];
            pd.fragmentFunction = [lib newFunctionWithName:@"main0_u"];
            pd.colorAttachments[0].pixelFormat = format;
            id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
            if (!pipeline) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
            const uint32_t values[] = {0x13u, 0x7fu, 0x80u, 0xffu, 0x12345678u, 0xdeadbeefu};
            for (unsigned i = 0; i < (arithmetic ? 1u : narrow ? 4u : 6u); i++) {
                uint32_t input = arithmetic ? 0x3fc00000u : values[i]; // 1.5f -> 3.0f
                uint32_t expected = arithmetic ? 0x40400000u : input;
                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
                pass.colorAttachments[0].texture = texture;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
                [enc setRenderPipelineState:pipeline];
                [enc setVertexBytes:&input length:sizeof input atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
                if (cb.error) { fprintf(stderr, "%s\n", cb.error.description.UTF8String); return 1; }
                uint32_t pixels[4] = {0};
                [texture getBytes:pixels bytesPerRow:2 * (narrow ? 1 : 4) fromRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0];
                for (unsigned p = 0; p < 4; p++) {
                    uint32_t actual = narrow ? ((uint8_t *)pixels)[p] : pixels[p];
                    if (actual != expected) {
                        fprintf(stderr, "arithmetic=%d narrow=%d pixel=%u: got=%08x expected=%08x\n", arithmetic, narrow, p, actual, expected);
                        return 1;
                    }
                }
                cases++;
            }
        }
    }
    printf("integer fragment output: %u native bit-exact cases passed\n", cases);
    return 0;
} }
'''


class IntegerFragmentOutput(unittest.TestCase):
    def test_flat_passthrough_has_integer_entry(self):
        full, mask = fs_uint(BASE)
        self.assertEqual(mask, 0)
        self.assertTrue(full.startswith(BASE))
        self.assertIn('uint4 color [[color(0)]];', full)
        self.assertIn('return main0_out_u{as_type<uint4>(out.color)};', full)
        self.assertEqual(full.count('out.color.x = copied;'), 2)

    def test_arithmetic_has_integer_entry(self):
        full, mask = fs_uint(ARITHMETIC)
        self.assertEqual(mask, 0)
        self.assertIn('fragment main0_out_u main0_u(', full)
        self.assertEqual(full.count('out.color.x = copied * 2.0f;'), 2)

    def test_mixed_mask_retains_existing_inference(self):
        full, mask = fs_uint(MIXED)
        self.assertEqual(mask, 1)
        self.assertIn('fragment main0_out_m main0_m(', full)
        self.assertIn('return main0_out_m{as_type<uint4>(out.color), out.other};', full)
        # A raw varying's runtime target type remains unknown for mixed layouts.
        self.assertNotIn('main0_m(', fs_uint(MIXED.replace('as_type<float>(17u)', 'copied'))[0])

    def test_early_returns_and_noncolor_outputs_preserved(self):
        source = BASE.replace('float4 color [[color(0)]];', 'float color [[color(0)]];\n    float depth [[depth(any)]];').replace(
            'out.color.x = copied;', 'out.color = copied;\n    out.depth = 0.25f;\n    if (copied < 0.0f) return out;')
        full, _ = fs_uint(source)
        self.assertEqual(full.count('return main0_out_u{as_type<uint>(out.color), out.depth};'), 2)
        self.assertIn('float depth [[depth(any)]];', full)

    def test_no_float_colors_needs_no_variant(self):
        self.assertEqual(fs_uint(BASE.replace('[[color(0)]]', '[[depth(any)]]')), (None, 0))
        self.assertEqual(fs_uint(BASE.replace('float4 color', 'uint4 color')), (None, 0))

    @unittest.skipUnless(shutil.which('xcrun'), 'requires the Metal compiler')
    def test_compiles_mac_and_ios(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for i, source in enumerate((BASE, ARITHMETIC, MIXED)):
                path = root / f'{i}.metal'
                path.write_text(fs_uint(source)[0] + VERTEX)
                for sdk in ('macosx', 'iphoneos'):
                    subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-std=metal3.1', '-c', str(path), '-o', str(root / f'{i}-{sdk}.air')], check=True, capture_output=True)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') == '1' and shutil.which('xcrun'), 'set DMSH_TEST_GPU=1 on a Mac')
    def test_native_integer_target_bits(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'runner.m').write_text(RUNNER)
            runner = root / 'runner'
            subprocess.run(['clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', str(root / 'runner.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(runner)], check=True)
            libraries = []
            for i, source in enumerate((BASE, ARITHMETIC)):
                path = root / f'{i}.metal'
                path.write_text(fs_uint(source)[0] + VERTEX)
                air, lib = root / f'{i}.air', root / f'{i}.metallib'
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-std=metal3.1', '-c', str(path), '-o', str(air)], check=True, capture_output=True)
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(air), '-o', str(lib)], check=True, capture_output=True)
                libraries.append(str(lib))
            result = subprocess.run([str(runner), *libraries], capture_output=True, text=True)
            if result.returncode == 77:
                self.skipTest('no Metal device')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
