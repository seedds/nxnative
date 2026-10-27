"""Synthetic geometry lowering, compiler and optional native GPU regressions."""
import ast
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_geometry

RASTER_CHECK = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
int main(int argc, const char **argv) { @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { fprintf(stderr, "no Metal device\n"); return 77; }
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
    MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
    pipeline.vertexFunction = [library newFunctionWithName:@(argv[2])];
    pipeline.fragmentFunction = [library newFunctionWithName:@"pixel"];
    pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:pipeline error:&error];
    if (!state) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:td];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    uint32_t masks[] = {0, 1, 2, 3, 0x80000000u, 0xffffffffu};
    for (unsigned clamp = 0; clamp < 2; clamp++)
    for (uint32_t points = 0; points < 2; points++)
    for (unsigned i = 0; i < sizeof(masks) / sizeof(*masks); i++) {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:state];
        [encoder setDepthClipMode:clamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
        [encoder setVertexBytes:&masks[i] length:sizeof(masks[i]) atIndex:0];
        [encoder setVertexBytes:&points length:sizeof(points) atIndex:1];
        [encoder drawPrimitives:points ? MTLPrimitiveTypePoint : MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:points ? 1 : 3];
        [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
        if (command.error) { fprintf(stderr, "%s\n", command.error.description.UTF8String); return 1; }
        uint8_t pixel[4];
        [texture getBytes:pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(8, 8, 1, 1) mipmapLevel:0];
        if ((pixel[0] != 0) != ((masks[i] & 1u) != 0)) {
            fprintf(stderr, "mask %#x, points=%u, depth clamp=%u: red=%u\n", masks[i], points, clamp, pixel[0]); return 1;
        }
    }
    return 0;
} }
'''


def mask_reader():
    # GenerateVertexPassthroughForCompute treats ViewportMask (int[1]) as a
    # scalar output. SPIRV-Cross emits this invalid array-copy expression.
    return '''#include <metal_stdlib>
using namespace metal;
template<typename T, uint N>
inline void spvArrayCopyFromStackToStack(thread T (&dst)[N], thread const T (&src)[N])
{ for (uint i = 0; i < N; i++) dst[i] = src[i]; }
struct main0_out
{
    float4 gl_Position [[position]];
    float gl_PointSize [[point_size]];
};
vertex main0_out main0(uint vid [[vertex_id]], constant uint& mask [[buffer(0)]], constant uint& points [[buffer(1)]])
{
    main0_out out = {};
    float2 positions[] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    out.gl_Position = float4(points ? float2(0.0) : positions[vid], 0.0, 1.0);
    out.gl_PointSize = 64.0;
    float bits = as_type<float>(mask);
    spvArrayCopyFromStackToStack(gl_BuiltIn_5253, as_type<int>(bits).elements);
    return out;
}
fragment float4 pixel() { return float4(1.0, 0.0, 0.0, 1.0); }
'''


def depth_entries(src):
    # Exercise the same post-reader depth conversion as geometry_finish without
    # importing the builder's command-line and game-profile initialization.
    builder = Path(dmsh_geometry.__file__).with_name('dmsh_build.py').read_text()
    node = next(n for n in ast.parse(builder).body if isinstance(n, ast.FunctionDef) and n.name == 'vs_depth')
    ns = {'re': re, 'sys': sys}
    exec(ast.get_source_segment(builder, node), ns)
    # The production helper locates the translated vertex entry at file end.
    vertex, pixel = src.split('fragment float4 pixel()', 1)
    return ns['vs_depth'](vertex)[1] + 'fragment float4 pixel()' + pixel


class GeometryReader(unittest.TestCase):
    def test_capture_loads_preserve_input_reads(self):
        source = 'device _22* vertex_output [[id(1)]]; device _22* geometry_vb_output [[id(2)]];'
        self.assertEqual(dmsh_geometry.capture_input(source),
                         'device volatile _22* vertex_output [[id(1)]]; device _22* geometry_vb_output [[id(2)]];')
        self.assertEqual(dmsh_geometry.capture_input('kernel void main0() {}'), 'kernel void main0() {}')
        with self.assertRaisesRegex(ValueError, 'capture-input declaration'):
            dmsh_geometry.capture_input('unknown vertex_output;')

    def test_ordinary_reader_is_unchanged(self):
        src = mask_reader().replace('    spvArrayCopyFromStackToStack(gl_BuiltIn_5253, as_type<int>(bits).elements);\n', '')
        self.assertEqual(dmsh_geometry.reader(src), src)

    def test_unknown_mask_expression_is_rejected(self):
        for expression in ('gl_BuiltIn_5253[0] = 1;',
                           'spvArrayCopyFromStackToStack(gl_BuiltIn_5253, as_type<int>(bits + 1.0).elements);'):
            with self.subTest(expression=expression), self.assertRaisesRegex(ValueError, 'viewport-mask expression'):
                dmsh_geometry.reader(mask_reader().replace(
                    'spvArrayCopyFromStackToStack(gl_BuiltIn_5253, as_type<int>(bits).elements);', expression))

    @unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'requires Metal on macOS')
    def test_mask_controls_viewport_zero_visibility(self):
        with tempfile.TemporaryDirectory(prefix='dmsh_geometry_raster_') as d:
            root = Path(d)
            source = root / 'reader.metal'; source.write_text(depth_entries(dmsh_geometry.reader(mask_reader())))
            library = root / 'reader.metallib'
            compiler = ['xcrun', '-sdk', 'macosx', 'metal', '-fmodules-cache-path=' + d + '/modules']
            r = subprocess.run([*compiler, '-c', str(source), '-o', str(root / 'reader.air')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(root / 'reader.air'), '-o', str(library)], check=True, capture_output=True)
            executable = root / 'raster'
            subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-x', 'objective-c', '-', '-o', str(executable),
                            '-framework', 'Foundation', '-framework', 'Metal'], input=RASTER_CHECK,
                           text=True, check=True, capture_output=True)
            for entry in ('main0', 'main0_z'):
                with self.subTest(entry=entry):
                    r = subprocess.run([str(executable), str(library), entry], capture_output=True, text=True)
                    if r.returncode == 77:
                        self.skipTest(r.stderr.strip())
                    self.assertEqual(r.returncode, 0, r.stderr)

    @unittest.skipUnless(shutil.which('xcrun'), 'requires the Metal compiler')
    def test_viewport_mask_reader_compiles(self):
        with tempfile.TemporaryDirectory(prefix='dmsh_geometry_test_') as d:
            src = Path(d) / 'reader.metal'
            src.write_text(dmsh_geometry.reader(mask_reader()))
            for sdk in ('macosx', 'iphoneos'):
                with self.subTest(sdk=sdk):
                    r = subprocess.run(['xcrun', '-sdk', sdk, 'metal',
                                        '-fmodules-cache-path=' + d + '/modules', '-fsyntax-only', str(src)],
                                       capture_output=True, text=True)
                    self.assertEqual(r.returncode, 0, r.stderr)


def geometry_header(passthrough=True, vertices=1, topology=7, invocations=1):
    return struct.pack('<5I', int(passthrough) << 24, 0, invocations << 24, topology << 24, vertices)


class GeometryOutputLayout(unittest.TestCase):
    def test_passthrough_uses_input_primitive_vertex_count(self):
        layouts = [dmsh_geometry.output_layout(geometry_header(), t) for t in range(5)]
        self.assertEqual([g['vertices'] for g in layouts], [1, 2, 2, 3, 3])
        self.assertEqual([g['index_stride'] for g in layouts], [2, 4, 4, 6, 6])
        self.assertEqual([g['output'] for g in layouts], [1, 6, 6, 7, 7])

    def test_regular_geometry_retains_declared_bound(self):
        for t in range(5):
            self.assertEqual(dmsh_geometry.output_layout(geometry_header(False, 12, 6, 3), t),
                             {'vertices': 12, 'invocations': 3, 'output': 6, 'index_stride': 24})

    def test_invalid_header_or_topology_is_rejected(self):
        for code, t in ((b'', 0), (geometry_header(), 5), (geometry_header(), -1),
                        (geometry_header(False, vertices=0), 0), (geometry_header(invocations=0), 0),
                        (geometry_header(False, topology=3), 0)):
            with self.subTest(code=code, topology=t), self.assertRaises(ValueError):
                dmsh_geometry.output_layout(code, t)

    @unittest.skipUnless(os.environ.get('DM_TEST_GEOMETRY'), 'set DM_TEST_GEOMETRY to the native t_geometry executable')
    def test_native_passthrough_canaries_and_compaction(self):
        layouts = [dmsh_geometry.output_layout(geometry_header(), t) for t in range(5)]
        with tempfile.TemporaryDirectory(prefix='geometry_bounds_') as d:
            path = Path(d) / 'layouts.json'; path.write_text(json.dumps(layouts))
            r = subprocess.run([os.environ['DM_TEST_GEOMETRY'], '--passthrough-layouts', str(path)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class PassthroughCompiler(unittest.TestCase):
    def test_passthrough_emits_positions_and_generic_attributes(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent))   # a sibling module: the CI runs each test file from the repository root
        from test_dmsh_reflect import REFLECT
        root = Path(__file__).resolve().parents[3]
        table = (root / 'dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/Decoders/InstTable.cs').read_text()
        pattern = re.search(r'Add\("([01x]{64})", InstName\.Exit\s*,', table)[1]
        exit_instruction = int(pattern.replace('x', '0'), 2) | (7 << 16) | 15
        with tempfile.TemporaryDirectory(prefix='geometry_passthrough_') as d:
            work = Path(d); jobs = []
            for t in range(5):
                header = bytearray(0x50)
                struct.pack_into('<5I', header, 0, (1 << 24) | (4 << 10) | (3 << 5) | 1,
                                 0, 1 << 24, 7 << 24, 1)
                code = work / f'pass{t}.bin'; code.write_bytes(header + struct.pack('<QQ', 0, exit_instruction))
                jobs.append(dict(key=f'pass{t}', stage='gs', code=str(code), geometry_topology=t,
                                 tex={}, img={}, attr={}, fout={}, debug=True, reduced=True))
            job_file = work / 'jobs.json'; job_file.write_text(json.dumps(jobs))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(job_file), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            for t, job in enumerate(jobs):
                with self.subTest(topology=t):
                    spv = work / f'pass{t}.spv'; dmsh_geometry.flatten_sets(spv)
                    r = subprocess.run([str(Path(__file__).resolve().parents[1] / 'tools/spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', '--msl-argument-buffers',
                                        '--msl-texture-buffer-native', str(spv)], capture_output=True, text=True)
                    self.assertEqual(r.returncode, 0, r.stderr)
                    src = dmsh_geometry.capture_input(dmsh_geometry.topology_pointer(r.stdout))
                    self.assertTrue('geometry_vb_output' in src, 'passthrough kernel discarded its raster output')
                    res = json.loads((work / f'pass{t}.res.json').read_text())
                    layout = dmsh_geometry.output_layout((work / f'pass{t}.bin').read_bytes(), t)
                    self.assertEqual(res['geometry_stride'], 133)   # position, point size, all 32 float4 generic outputs
                    reflection = dict(route='B', bindings=REFLECT('synthetic', 'gs', src, res),
                                      geometry=dict(layout, stride=res['geometry_stride']))
                    (work / f'pass{t}.json').write_text(json.dumps(reflection))
                    metal = work / f'pass{t}.metal'; metal.write_text(src)
                    for sdk in ('macosx', 'iphoneos'):
                        air = work / f'pass{t}.{sdk}.air'
                        r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-fmodules-cache-path=' + d + '/modules',
                                            '-c', str(metal), '-o', str(air)], capture_output=True, text=True)
                        self.assertEqual(r.returncode, 0, r.stderr)
                        subprocess.run(['xcrun', '-sdk', sdk, 'metallib', str(air), '-o', str(work / f'pass{t}.{sdk}.metallib')], check=True, capture_output=True)
            if os.environ.get('DM_TEST_GEOMETRY'):
                r = subprocess.run([os.environ['DM_TEST_GEOMETRY'], '--passthrough-kernels', d], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
