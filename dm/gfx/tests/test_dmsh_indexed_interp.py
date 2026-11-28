#!/usr/bin/env python3
"""Indexed fragment inputs preserve per-location interpolation qualifiers (Maxwell and Ampere).

When a fragment shader has an indexed attribute read (Maxwell IPA.IDX or Ampere 0x526),
Ryujinx.Graphics.Shader collapses user-defined fragment inputs into one 16-element vec4 array
at location 0. Previously that array carried only location 0's interpolation mode, so other
locations lost their flat or screen-linear qualifiers.

Now Declarations.TryDeclareSplitIndexedFragmentInputs emits per-location Input vec4 variables
with their SPH qualifiers and gathers them into a Private array at main entry whenever any used
location disagrees with location 0 (leaving uniform-mode shaders unchanged).

Run with DMSH_TEST_GPU=1 on a Mac to also compile and rasterize the translated Maxwell and Ampere
shaders on Metal and verify perspective, flat (exact provoking-vertex bit pattern), noperspective,
and dynamic indexed reads on the GPU.
"""
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

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_chain  # noqa: E402

XLATE = os.environ.get('DMSH_TEST_XLATE', str(TOOLS / 'dmsh_xlate_bin/dmsh_xlate'))
SPIRV_CROSS = str(TOOLS / 'spirv_cross_bin/spirv-cross')

RZ, PT = 255, 7
AMPERE_CTL = 0x000fea0000000000 << 64


def maxwell_pack(words):
    nop = 0x50b0000000070f00
    sched = 0x001f8000fc0007e0
    return b''.join(struct.pack('<4Q', sched, *(tuple(words[i:i + 3]) + (nop, nop))[:3])
                    for i in range(0, len(words), 3))


def maxwell_mov_c(rd, bank, word_offset):
    return 0x4c98000000000000 | (0xf << 39) | (bank << 34) | (word_offset << 20) | (PT << 16) | rd


def maxwell_ipa(rd, imm10, *, ra=RZ, rb=RZ, idx=False, op=0):
    return (0xe000000000000000 | (op << 54) | (int(idx) << 38) | (imm10 << 28)
            | (rb << 20) | (PT << 16) | (ra << 8) | rd)


def maxwell_mufu_rcp(rd, ra):
    return 0x5080000000000000 | (4 << 20) | (PT << 16) | (ra << 8) | rd


def maxwell_fragment(imap_bytes, *, indexed=True):
    """Maxwell pixel shader (0x50 SPH + code):
      R0 = IPA.MUL a[0x80], (1 / FragCoord.w)   (loc0.x, perspective-corrected when loc0 is perspective)
      R1 = IPA.PASS a[0x90]                     (loc1.x)
      R2 = IPA.PASS a[0xa0]                     (loc2.x)
      R3 = IPA.IDX a[R4] (R4 = c[3][0])         (dynamic indexed read when indexed=True, else direct loc0.x)
    """
    header = bytearray(0x50)
    struct.pack_into('<I', header, 0, 2 | (3 << 5) | (5 << 10))
    for loc, val in imap_bytes.items():
        header[0x18 + loc] = val
    struct.pack_into('<I', header, 18 * 4, 0xf)   # OmapTargets: RT0 RGBA
    body = [
        maxwell_ipa(10, 0x7c),                    # R10 = FragCoord.w (1/w)
        maxwell_mufu_rcp(10, 10),                 # R10 = w
        maxwell_ipa(0, 0x80, rb=10, op=1),        # R0 = loc0.x * FragCoord.w * w
        maxwell_ipa(1, 0x90),                     # R1 = loc1.x
        maxwell_ipa(2, 0xa0),                     # R2 = loc2.x
    ]
    if indexed:
        body += [
            maxwell_mov_c(4, 3, 0),               # R4 = c[3][0]
            maxwell_ipa(3, 0, ra=4, idx=True),    # R3 = a[R4] * FragCoord.w
        ]
    else:
        body += [maxwell_ipa(3, 0x80)]
    body += [0xe30000000007000f]                  # EXIT
    return bytes(header) + maxwell_pack(body)


def ampere_inst(op, rd=RZ, ra=RZ, fields=()):
    v = op | (PT << 12) | (rd << 16) | (ra << 24)
    for bit, width, val in fields:
        v |= (val & ((1 << width) - 1)) << bit
    v |= AMPERE_CTL
    return struct.pack('<QQ', v & ((1 << 64) - 1), v >> 64)


def ampere_fragment(imap_bytes, *, indexed=True):
    """Ampere (sm86) pixel shader (0x80 SPH + code):
      R0 = IPA a[0x80] * (1 / IPA a[0x7c])      (loc0.x)
      R1 = IPA a[0x90]                          (loc1.x)
      R2 = IPA a[0xa0]                          (loc2.x)
      R3 = IPA.IDX a[UR4 + 0] (UR4 = c[3][0])   (dynamic indexed read when indexed=True)
    """
    header = bytearray(0x80)
    struct.pack_into('<I', header, 0, 2 | (4 << 5) | (5 << 10))
    for loc, val in imap_bytes.items():
        header[0x18 + loc] = val
    struct.pack_into('<I', header, 18 * 4, 0xf)
    # R10 = IPA a[0x7c] (1/w), R10 = MUFU.RCP R10 (op 0x308, func 4 at 74..78), R0 = FMUL R0, R10 (op 0x220)
    code = [
        ampere_inst(0x326, rd=10, fields=((64, 8, 0x7c >> 2),)),
        ampere_inst(0x308, rd=10, fields=((32, 8, 10), (74, 4, 4))),
        ampere_inst(0x326, rd=0, fields=((64, 8, 0x80 >> 2),)),
        ampere_inst(0x220, rd=0, ra=0, fields=((32, 8, 10), (84, 3, 4))),
        ampere_inst(0x326, rd=1, fields=((64, 8, 0x90 >> 2),)),
        ampere_inst(0x326, rd=2, fields=((64, 8, 0xa0 >> 2),)),
    ]
    if indexed:
        code += [
            # ULDC UR4, c[3][0] (op 0xab9, rd=4, bank=3 at 54..58, offset=0 at 38..53, size=4 at 73..75)
            ampere_inst(0xab9, rd=4, fields=((38, 16, 0), (54, 5, 3), (73, 3, 4))),
            # IPA.IDX R3, a[UR4 + 0] (op 0x526, URa=4 at 24..29)
            ampere_inst(0x526, rd=3, ra=4, fields=((64, 8, 0),)),
        ]
    else:
        code += [ampere_inst(0x326, rd=3, fields=((64, 8, 0x80 >> 2),))]
    code += [ampere_inst(0x94d, fields=((87, 3, PT),))]
    return bytes(header) + b''.join(code)


def translate(work, code, *, arch=None):
    (work / 'ps.bin').write_bytes(code)
    job = dict(key='ps', stage='ps', code=str(work / 'ps.bin'), tex={}, img={}, attr={}, fout={}, debug=False)
    if arch:
        job['arch'] = arch
    (work / 'jobs.json').write_text(json.dumps([job]))
    r = subprocess.run([XLATE, str(work / 'jobs.json'), str(work)], capture_output=True, text=True)
    if r.returncode != 0:
        raise AssertionError(f'dmsh_xlate failed:\n{r.stdout}\n{r.stderr}')
    spv = work / 'ps.spv'
    res = json.loads((work / 'ps.res.json').read_text())
    if shutil.which('spirv-val'):
        v = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(spv)], capture_output=True, text=True)
        if v.returncode != 0:
            raise AssertionError(f'spirv-val failed:\n{v.stdout}\n{v.stderr}')
    c = subprocess.run([SPIRV_CROSS, *dmsh_chain.CROSS_MSL, str(spv)], capture_output=True, text=True)
    if c.returncode != 0:
        raise AssertionError(f'spirv-cross failed:\n{c.stdout}\n{c.stderr}')
    dis = subprocess.check_output(['spirv-dis', str(spv)], text=True) if shutil.which('spirv-dis') else ''
    return spv.read_bytes(), c.stdout, res, dis


def main0_in_qualifiers(msl):
    """Map locn number -> qualifier string ('', 'flat', 'center_no_perspective') from struct main0_in."""
    m = re.search(r'struct main0_in\s*\{(.*?)\};', msl, re.S)
    assert m, f'no struct main0_in in:\n{msl}'
    out = {}
    for loc, qual in re.findall(r'\[\[user\(locn(\d+)\)(?:,\s*([^\]]+))?\]\]', m.group(1)):
        out[int(loc)] = qual.strip()
    return out


VERTEX_MSL = r'''#include <metal_stdlib>
using namespace metal;

struct vs_out {
    float4 pos [[position]];
    float4 loc0 [[user(locn0)]];
    float4 loc1 [[user(locn1)]];
    float4 loc2 [[user(locn2)]];
    float4 loc3 [[user(locn3)]];
    float4 loc4 [[user(locn4)]];
    float4 loc5 [[user(locn5)]];
    float4 loc6 [[user(locn6)]];
    float4 loc7 [[user(locn7)]];
    float4 loc8 [[user(locn8)]];
    float4 loc9 [[user(locn9)]];
    float4 loc10 [[user(locn10)]];
    float4 loc11 [[user(locn11)]];
    float4 loc12 [[user(locn12)]];
    float4 loc13 [[user(locn13)]];
    float4 loc14 [[user(locn14)]];
    float4 loc15 [[user(locn15)]];
};

vertex vs_out test_vs(uint vid [[vertex_id]]) {
    // Full-cover triangle with varying clip-space w so perspective and screen-linear diverge.
    const float2 ndc[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    const float ws[3] = { 1.0, 4.0, 2.0 };
    const float grad[3] = { 0.0, 1.0, 0.0 };
    const uint flat_bits[3] = { 0x00000007u, 0x12345678u, 0x7f7fffffu };
    float w = ws[vid];
    vs_out o = {};
    o.pos = float4(ndc[vid] * w, 0.0, w);
    o.loc0 = float4(grad[vid], 0.0, 0.0, 1.0);
    o.loc1 = float4(as_type<float>(flat_bits[vid]), 0.0, 0.0, 1.0);
    o.loc2 = float4(grad[vid], 0.0, 0.0, 1.0);
    return o;
}
'''

GPU_RUNNER = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int render_once(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLRenderPipelineState> pso,
                       id<MTLFunction> fs, uint32_t idx_byte_offset, float out_rgba[4]) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                  width:16 height:16 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];

    id<MTLArgumentEncoder> ae = [fs newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> args = [dev newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
    id<MTLBuffer> cb = [dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    memset(cb.contents, 0, cb.length);
    ((uint32_t *)cb.contents)[0] = idx_byte_offset;
    [ae setArgumentBuffer:args offset:0];
    [ae setBuffer:cb offset:0 atIndex:0];

    MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.colorAttachments[0].texture = tex;
    rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    rpd.colorAttachments[0].clearColor = MTLClearColorMake(-1, -1, -1, -1);

    id<MTLCommandBuffer> cmd = [q commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rpd];
    [enc setRenderPipelineState:pso];
    [enc setFragmentBuffer:args offset:0 atIndex:0];
    [enc useResource:cb usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    if (cmd.error) { fprintf(stderr, "cmd error: %s\n", cmd.error.description.UTF8String); return 1; }

    float pixels[16 * 16 * 4];
    [tex getBytes:pixels bytesPerRow:16 * 16 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0];
    // Sample interior pixel (x=8, y=8) where barycentric coordinates are strictly interior.
    memcpy(out_rgba, &pixels[(8 * 16 + 8) * 4], 4 * sizeof(float));
    return 0;
}

int main(int argc, const char **argv) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) return 77;
    NSError *err = nil;
    id<MTLLibrary> vlib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&err];
    id<MTLLibrary> flib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[2])] error:&err];
    id<MTLFunction> vs = [vlib newFunctionWithName:@"test_vs"];
    id<MTLFunction> fs = [flib newFunctionWithName:@"main0"];
    if (!vs || !fs) { fprintf(stderr, "missing functions: %s\n", err.description.UTF8String); return 1; }

    MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = vs;
    desc.fragmentFunction = fs;
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:desc error:&err];
    if (!pso) { fprintf(stderr, "pso: %s\n", err.description.UTF8String); return 1; }

    id<MTLCommandQueue> q = [dev newCommandQueue];
    float p0[4], p1[4];
    if (render_once(dev, q, pso, fs, 0x80u, p0)) return 1;
    if (render_once(dev, q, pso, fs, 0xa0u, p1)) return 1;

    uint32_t flat_bits = 0;
    memcpy(&flat_bits, &p0[1], sizeof(flat_bits));
    if (flat_bits != 0x00000007u) {
        fprintf(stderr, "flat bits mismatch: got 0x%08x, want 0x00000007\n", flat_bits);
        return 1;
    }
    // At pixel (8.5, 8.5) on 16x16, ndc = (0.0625, -0.0625):
    // screen-linear barycentrics of (-1,-1), (3,-1), (-1,3) give u_screen = (0.0625+1)/4 = 0.265625,
    // v_screen = (-0.0625+1)/4 = 0.234375, t_screen = 0.5.
    // With w = (1, 4, 2): inv_w = 0.5/1 + 0.265625/4 + 0.234375/2 = 0.68359375,
    // loc0_persp = (0.265625 / 4) / inv_w = 0.097142857, loc2_linear = 0.265625.
    float expect_linear = 0.265625f;
    float inv_w = 0.5f / 1.0f + 0.265625f / 4.0f + 0.234375f / 2.0f;
    float expect_persp = (0.265625f / 4.0f) / inv_w;
    if (fabsf(p0[0] - expect_persp) > 0.01f) {
        fprintf(stderr, "perspective mismatch: got %g, want %g\n", p0[0], expect_persp);
        return 1;
    }
    if (fabsf(p0[2] - expect_linear) > 0.01f) {
        fprintf(stderr, "noperspective mismatch: got %g, want %g\n", p0[2], expect_linear);
        return 1;
    }
    if (fabsf(p0[0] - p0[2]) < 0.1f) {
        fprintf(stderr, "perspective and noperspective did not diverge: %g vs %g\n", p0[0], p0[2]);
        return 1;
    }
    // Dynamic indexed IPA.IDX reads attr * FragCoord.w = attr * inv_w.
    if (fabsf(p0[3] - expect_persp * inv_w) > 0.01f) {
        fprintf(stderr, "indexed loc0 mismatch: got %g, want %g\n", p0[3], expect_persp * inv_w);
        return 1;
    }
    if (fabsf(p1[3] - expect_linear * inv_w) > 0.01f) {
        fprintf(stderr, "indexed loc2 mismatch: got %g, want %g\n", p1[3], expect_linear * inv_w);
        return 1;
    }
    return 0;
} }
'''


@unittest.skipUnless(os.path.isfile(XLATE) and os.path.isfile(SPIRV_CROSS), 'dmsh_xlate and spirv-cross required')
class IndexedFragmentInterpolation(unittest.TestCase):
    def test_maxwell_mixed_flat_smooth_noperspective(self):
        with tempfile.TemporaryDirectory() as d:
            _, msl, res, dis = translate(Path(d), maxwell_fragment({0: 0xaa, 1: 0x01, 2: 0xff}))
        q = main0_in_qualifiers(msl)
        self.assertEqual(len(q), 16)
        self.assertEqual(q[0], '')
        self.assertEqual(q[1], 'flat')
        self.assertEqual(q[2], 'center_no_perspective')
        for loc in range(3, 16):
            self.assertEqual(q[loc], '')
        if dis:
            self.assertIn('OpVariable %_ptr_Private__arr_v4float_uint_16 Private', dis)
        self.assertFalse(any('mixed component' in n for n in res['notes']))

    def test_maxwell_flat_loc0_does_not_flatten_other_locations(self):
        with tempfile.TemporaryDirectory() as d:
            _, msl, _, _ = translate(Path(d), maxwell_fragment({0: 0x01, 1: 0xaa, 2: 0xff}))
        q = main0_in_qualifiers(msl)
        self.assertEqual(q[0], 'flat')
        self.assertEqual(q[1], '')
        self.assertEqual(q[2], 'center_no_perspective')
        for loc in range(3, 16):
            self.assertEqual(q[loc], '')

    def test_maxwell_unused_loc0_preserves_flat_loc1(self):
        with tempfile.TemporaryDirectory() as d:
            _, msl, _, _ = translate(Path(d), maxwell_fragment({0: 0x00, 1: 0x55, 2: 0xaa}))
        q = main0_in_qualifiers(msl)
        self.assertEqual(q[0], '')
        self.assertEqual(q[1], 'flat')
        self.assertEqual(q[2], '')

    def test_maxwell_uniform_modes_keep_stock_input_array(self):
        for mode_byte, expected in ((0xaa, ''), (0x55, 'flat'), (0xff, 'center_no_perspective')):
            with self.subTest(mode=hex(mode_byte)), tempfile.TemporaryDirectory() as d:
                _, msl, _, dis = translate(Path(d), maxwell_fragment({0: mode_byte, 1: mode_byte, 2: mode_byte}))
            q = main0_in_qualifiers(msl)
            self.assertEqual(len(q), 16)
            for loc in range(16):
                self.assertEqual(q[loc], expected)
            if dis:
                self.assertNotIn('OpVariable %_ptr_Private__arr_v4float_uint_16 Private', dis)

    def test_maxwell_mixed_component_location_logs_and_fixes_other_locations(self):
        # loc1 has x=Constant(1), y=Perspective(2) -> 0x09; loc2 is ScreenLinear -> 0xff.
        with tempfile.TemporaryDirectory() as d:
            _, msl, res, _ = translate(Path(d), maxwell_fragment({0: 0xaa, 1: 0x09, 2: 0xff}))
        q = main0_in_qualifiers(msl)
        self.assertEqual(q[0], '')
        self.assertEqual(q[1], 'flat')
        self.assertEqual(q[2], 'center_no_perspective')
        self.assertTrue(any('location 1 has mixed component interpolation modes' in n for n in res['notes']), res['notes'])

    def test_maxwell_nonindexed_is_unchanged(self):
        with tempfile.TemporaryDirectory() as d:
            _, msl, _, dis = translate(Path(d), maxwell_fragment({0: 0xaa, 1: 0x01, 2: 0xff}, indexed=False))
        q = main0_in_qualifiers(msl)
        self.assertEqual(q, {0: '', 1: 'flat', 2: 'center_no_perspective'})
        if dis:
            self.assertNotIn('Private', dis)

    def test_ampere_mixed_and_uniform_indexed_inputs(self):
        with tempfile.TemporaryDirectory() as d:
            _, msl, _, dis = translate(Path(d), ampere_fragment({0: 0xaa, 1: 0x01, 2: 0xff}), arch='sm86')
        q = main0_in_qualifiers(msl)
        self.assertEqual(len(q), 16)
        self.assertEqual(q[0], '')
        self.assertEqual(q[1], 'flat')
        self.assertEqual(q[2], 'center_no_perspective')
        if dis:
            self.assertIn('OpVariable %_ptr_Private__arr_v4float_uint_16 Private', dis)

        with tempfile.TemporaryDirectory() as d:
            _, msl, _, dis = translate(Path(d), ampere_fragment({0: 0xaa, 1: 0xaa, 2: 0xaa}), arch='sm86')
        if dis:
            self.assertNotIn('OpVariable %_ptr_Private__arr_v4float_uint_16 Private', dis)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') == '1' and shutil.which('xcrun'),
                         'set DMSH_TEST_GPU=1 on a Mac')
    def test_gpu_rasterization_maxwell_and_ampere(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            vs_src = root / 'vs.metal'
            vs_air = root / 'vs.air'
            vs_lib = root / 'vs.metallib'
            vs_src.write_text(VERTEX_MSL)
            subprocess.run([*dmsh_chain.MAC_METAL, str(vs_src), '-o', str(vs_air)], check=True)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(vs_air), '-o', str(vs_lib)], check=True)

            runner_src = root / 'runner.m'
            runner_bin = root / 'runner'
            runner_src.write_text(GPU_RUNNER)
            subprocess.run(['clang', '-fobjc-arc', '-framework', 'Foundation', '-framework', 'Metal',
                            str(runner_src), '-o', str(runner_bin)], check=True)

            for name, code, arch in (
                ('maxwell', maxwell_fragment({0: 0xaa, 1: 0x01, 2: 0xff}), None),
                ('ampere', ampere_fragment({0: 0xaa, 1: 0x01, 2: 0xff}), 'sm86'),
            ):
                with self.subTest(arch=name):
                    sub = root / name
                    sub.mkdir()
                    _, msl, _, _ = translate(sub, code, arch=arch)
                    fs_src = sub / 'fs.metal'
                    fs_air = sub / 'fs.air'
                    fs_lib = sub / 'fs.metallib'
                    fs_src.write_text(msl)
                    subprocess.run([*dmsh_chain.MAC_METAL, str(fs_src), '-o', str(fs_air)], check=True)
                    subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(fs_air), '-o', str(fs_lib)], check=True)
                    r = subprocess.run([str(runner_bin), str(vs_lib), str(fs_lib)], capture_output=True, text=True)
                    if r.returncode == 77:
                        self.skipTest('no Metal GPU available')
                    self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
