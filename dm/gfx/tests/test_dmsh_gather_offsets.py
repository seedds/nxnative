"""textureGatherOffsets (dmsh_build.py gather_offsets): each call of spirv-cross's spvGatherConstOffsets / spvGatherCompareConstOffsets becomes its four gathers with their offsets written out,
in plain and array forms, and on this Mac's GPU a compute shader's rewritten gathers match texel reads in every lane, on two runs."""
import ast
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
selected = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name == 'gather_offsets')
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) in ('GATHER4', 'MAC_METAL') for t in n.targets))]
scope = {'re': re}
exec(compile(ast.Module(body=selected, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
gather_offsets, MAC_METAL = scope['gather_offsets'], scope['MAC_METAL']

HEAD = '''#include <metal_stdlib>
using namespace metal;
template<typename T, size_t Num> struct spvUnsafeArray { T elements[Num ? Num : 1]; constexpr const constant T& operator [] (size_t pos) const constant { return elements[pos]; } };
template<typename Tex, typename... Tp>
using spvGatherReturn = decltype(declval<Tex>().gather(declval<sampler>(), declval<Tp>()...));
// Wrapper function that processes a device texture gather with a constant offset array.
template<typename Tex, typename Toff, typename... Tp>
inline spvGatherReturn<Tex, Tp...> spvGatherConstOffsets(const thread Tex& t, sampler s, Toff coffsets, component c, Tp... params) METAL_CONST_ARG(c)
{
    spvGatherReturn<Tex, Tp...> rslts[4];
    for (uint i = 0; i < 4; i++)
    {
        switch (c)
        {
            case component::x: rslts[i] = t.gather(s, params..., coffsets[i], component::x); break;
            case component::y: rslts[i] = t.gather(s, params..., coffsets[i], component::y); break;
            case component::z: rslts[i] = t.gather(s, params..., coffsets[i], component::z); break;
            case component::w: rslts[i] = t.gather(s, params..., coffsets[i], component::w); break;
        }
    }
    return spvGatherReturn<Tex, Tp...>(rslts[0].w, rslts[1].w, rslts[2].w, rslts[3].w);
}
constant spvUnsafeArray<int2, 4> _9 = spvUnsafeArray<int2, 4>({ int2(0), int2(1, 0), int2(0, 1), int2(1) });
'''
KERNEL = '''
kernel void k(texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], device atomic_uint* bad [[buffer(0)]], uint3 id [[thread_position_in_grid]])
{
    float2 c = (float2(id.xy) + 1.0) / float2(t.get_width(), t.get_height());
    for (uint n = 0; n < 4; n++)
    {
        float4 a = spvGatherConstOffsets(t, s, _9, component::y, select(c, float2(0.0), isnan(c)));
        int2 i = int2(id.xy);
        float4 r = float4(t.read(uint2(i)).y, t.read(uint2(i + int2(1, 0))).y, t.read(uint2(i + int2(0, 1))).y, t.read(uint2(i + int2(1, 1))).y);
        if (any(a != r)) atomic_fetch_add_explicit(bad, 1u, memory_order_relaxed);
        c = (float2(id.xy) + 1.0) / float2(t.get_width(), t.get_height());
    }
    atomic_fetch_add_explicit(bad + 1, 1u, memory_order_relaxed);
}
'''
RUNNER = '''#import <Metal/Metal.h>
#include <stdio.h>
int main(int ac, char **av) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); NSError *e = nil;
    id<MTLLibrary> l = [d newLibraryWithURL:[NSURL fileURLWithPath:@(av[1])] error:&e]; if (!l) { puts(e.description.UTF8String); return 2; }
    id<MTLComputePipelineState> p = [d newComputePipelineStateWithFunction:[l newFunctionWithName:@"k"] error:&e]; if (!p) { puts(e.description.UTF8String); return 2; }
    enum { W = 256, H = 128 }; MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:W height:H mipmapped:NO]; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [d newTextureWithDescriptor:td]; static float px[H][W][4]; for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { px[y][x][0] = x; px[y][x][1] = y * W + x + 1; px[y][x][2] = y; px[y][x][3] = 1; }
    [t replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px bytesPerRow:W * 16];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge; id<MTLSamplerState> s = [d newSamplerStateWithDescriptor:sd];
    id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLBuffer> o = [d newBufferWithLength:8 options:MTLResourceStorageModeShared];
    for (int r = 0; r < 2; r++) { id<MTLCommandBuffer> c = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [c computeCommandEncoder]; [ce setComputePipelineState:p]; [ce setTexture:t atIndex:0]; [ce setSamplerState:s atIndex:0]; [ce setBuffer:o offset:0 atIndex:0];
        [ce dispatchThreads:MTLSizeMake(W - 2, H - 2, 1) threadsPerThreadgroup:MTLSizeMake(32, 4, 1)]; [ce endEncoding]; [c commit]; [c waitUntilCompleted]; }
    const uint32_t *v = o.contents; printf("%u %u\\n", v[0], v[1]); return 0;
} }
'''


class Rewrite(unittest.TestCase):
    def test_no_call_no_change(self):
        src = HEAD + '\nkernel void k() {}\n'
        self.assertEqual(gather_offsets(src), src)

    def test_call_becomes_four_gathers(self):
        out = gather_offsets(HEAD + 'float4 f(texture2d<float> t, sampler s, float2 c) { float4 a = spvGatherConstOffsets(t, s, _9, component::z, select(c, float2(0.0), isnan(c))); return a; }\n')
        self.assertNotIn('= spvGatherConstOffsets(', out)
        self.assertIn('dm_gather4(t.gather(s, select(c, float2(0.0), isnan(c)), int2(0), component::z).w, t.gather(s, select(c, float2(0.0), isnan(c)), int2(1, 0), component::z).w, '
                      't.gather(s, select(c, float2(0.0), isnan(c)), int2(0, 1), component::z).w, t.gather(s, select(c, float2(0.0), isnan(c)), int2(1), component::z).w)', out)

    def test_array_and_compare(self):
        out = gather_offsets(HEAD + 'float4 f(texture2d_array<float> t, depth2d<float> z, sampler s, float2 c) { float4 a = spvGatherConstOffsets(t, s, _9, component::x, c, uint(rint(1.0))); float4 b = spvGatherCompareConstOffsets(z, s, _9, c, 0.5); return a + b; }\n')
        self.assertIn('t.gather(s, c, uint(rint(1.0)), int2(1, 0), component::x).w', out)
        self.assertIn('z.gather_compare(s, c, 0.5, int2(0, 1)).w', out)

    def test_offsets_not_constant_refused(self):
        with self.assertRaises(ValueError):
            gather_offsets(HEAD + 'float4 f(texture2d<float> t, sampler s, float2 c, int2 o) { float4 a = spvGatherConstOffsets(t, s, o, component::z, c); return a; }\n')


@unittest.skipUnless(shutil.which('xcrun'), 'needs Metal')
class OnGpu(unittest.TestCase):
    def test_matches_reads_in_every_lane(self):
        with tempfile.TemporaryDirectory() as w:
            w = Path(w)
            (w / 'k.metal').write_text(gather_offsets(HEAD + KERNEL))
            subprocess.run(MAC_METAL + [str(w / 'k.metal'), '-o', str(w / 'k.air')], check=True, capture_output=True)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(w / 'k.air'), '-o', str(w / 'k.metallib')], check=True)
            (w / 'r.m').write_text(RUNNER)
            subprocess.run(['clang', '-fobjc-arc', '-framework', 'Metal', '-framework', 'Foundation', str(w / 'r.m'), '-o', str(w / 'r')], check=True, capture_output=True)
            out = subprocess.run([str(w / 'r'), str(w / 'k.metallib')], capture_output=True, text=True).stdout.split()
            self.assertEqual(out[0], '0', out)
            self.assertEqual(int(out[1]), 2 * 254 * 126)


if __name__ == '__main__':
    unittest.main()
