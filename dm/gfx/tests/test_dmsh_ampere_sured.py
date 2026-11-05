"""Ampere SURED through the actual translator and native Metal image atomics.

The encoding is independently checked with NVIDIA nvdisasm 12.9.88 (SM86):
60000c000400759f / 000fe2000010e000 is
SURED.D.2D.ADD.STRONG.GPU.IGN [R4], R0, 0x0, 0xc.
Bit probes establish coordinates 24..32, source 32..40, handle 40..59,
bounds 59..61, dimensions 61..64, BA 72, size 73..76 and operation 87..90.
Set DMSH_TEST_XLATE to the rebuilt translator; DMSH_TEST_GPU=1 also executes
native textures, checking contention, signed operands and out-of-range writes.
"""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_atomic  # noqa: E402
import dmsh_geometry  # noqa: E402

EXIT = (0x000000000000794d, 0x000fea0003800000)
SURED = 0x000fe2000010e000 << 64 | 0x60000c000400759f


def field(word, lo, hi, value):
    return word & ~(((1 << (hi - lo)) - 1) << lo) | value << lo


def mov(reg, value):
    # IMAD.MOV.U32 Rd, RZ, RZ, imm32 (same as MKW's MOV R0, 1 preceding SURED).
    return struct.pack('<QQ', (value & 0xffffffff) << 32 | 0xff007424 | reg << 16, 0x000fc800078e00ff)


def code(x=0, y=0, value=1, operation=0, size=0, dim=3, byte_address=0, bounds=0):
    word = field(SURED, 87, 90, operation)
    word = field(word, 73, 76, size)
    word = field(word, 61, 64, dim)
    word = field(word, 59, 61, bounds)
    word = field(word, 72, 73, int(byte_address))
    return mov(0, value) + mov(4, x) + mov(5, y) + mov(6, 0) + word.to_bytes(16, 'little') + struct.pack('<QQ', *EXIT)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'needs the rebuilt offline translator')
class Compiler(unittest.TestCase):
    def translate(self, work, shader, expect=True):
        (work / 'k.bin').write_bytes(shader)
        jobs = [dict(key='k', stage='cs', arch='sm86', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={},
                     local=[32, 1, 1], nvn2_cs=[0, 0, 3, 0x40, 0], debug=True)]
        (work / 'jobs.json').write_text(json.dumps(jobs))
        result = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), str(work)], capture_output=True, text=True)
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0 if expect else 1, output)
        spv = work / 'k.spv'
        self.assertEqual(spv.exists(), expect, output)
        if not expect:
            return output
        dmsh_geometry.flatten_sets(spv)
        if shutil.which('spirv-val'):
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(spv)], check=True)
        args = dmsh_atomic.cross_args(['--msl', '--msl-version', '20300', '--msl-argument-buffers', '--msl-texture-buffer-native'], str(spv))
        result = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), *args, str(spv)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return dmsh_atomic.check_msl(result.stdout)

    def test_instruction_preserves_image_atomic_side_effect(self):
        with tempfile.TemporaryDirectory() as temp:
            msl = self.translate(Path(temp), code(x=3, y=2, value=5))
            self.assertIn('.atomic_fetch_add', msl)
            self.assertIn('cp_i_tcb_4A_r32ui', msl)
            self.assertIn('int2(3, 2)', msl)
            self.assertIn('5u', msl)

    def test_native_operators_and_signed_type(self):
        for operation, name in ((0, 'add'), (1, 'min'), (2, 'max'), (5, 'and'), (6, 'or'), (7, 'xor')):
            with self.subTest(operation=operation), tempfile.TemporaryDirectory() as temp:
                msl = self.translate(Path(temp), code(value=-5, operation=operation, size=1))
                self.assertIn('.atomic_fetch_' + name, msl)
                self.assertIn('texture2d<int, access::read_write>', msl)

    def test_unsupported_semantics_rejected(self):
        for kwargs, reason in ((dict(bounds=2), 'bounds mode'), (dict(operation=3), 'with a limit'),
                               (dict(operation=4), 'with a limit'), (dict(size=2), '32-bit integer'),
                               (dict(dim=6), 'dimension')):
            with self.subTest(**kwargs), tempfile.TemporaryDirectory() as temp:
                self.assertIn(reason, self.translate(Path(temp), code(**kwargs), expect=False))

    @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') == '1' and shutil.which('xcrun'), 'set DMSH_TEST_GPU=1 on a Mac')
    def test_real_native_texture_reductions(self):
        cases = [
            # description, shader parameters, initial value, changed texel (-1 means none), result
            ('contended add', dict(value=5), 3, 0, 3 + 257 * 5),
            ('unsigned minimum', dict(value=5, operation=1), 100, 0, 5),
            ('signed minimum', dict(value=-5, operation=1, size=1), 3, 0, -5),
            ('signed maximum', dict(value=-2, operation=2, size=1), -3, 0, -2),
            ('and', dict(value=0xa5, operation=5), 0xf0, 0, 0xa0),
            ('or', dict(value=0xa5, operation=6), 0xf0, 0, 0xf5),
            ('xor', dict(value=0xa5, operation=7), 0xf0, 0, 0x55),
            ('byte address', dict(x=4, y=2, byte_address=True, value=2), 3, 17, 517),
            ('buffer', dict(dim=1, x=7, value=2), 3, 7, 517),
            ('negative x', dict(x=-1), 3, -1, 3),
            ('right edge', dict(x=8), 3, -1, 3),
            ('negative y', dict(y=-1), 3, -1, 3),
            ('bottom edge', dict(y=4), 3, -1, 3),
        ]
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            (work / 'run.m').write_text(RUNNER)
            subprocess.run(['clang', '-fobjc-arc', '-O1', str(work / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(work / 'run')], check=True)
            for index, (name, kwargs, initial, changed, expected) in enumerate(cases):
                with self.subTest(name=name):
                    case = work / str(index); case.mkdir()
                    (case / 'k.metal').write_text(self.translate(case, code(**kwargs)))
                    subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-std=metal3.1', '-c', str(case / 'k.metal'), '-o', str(case / 'k.air')], check=True)
                    subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(case / 'k.air'), '-o', str(case / 'k.metallib')], check=True)
                    result = subprocess.run([str(work / 'run'), str(case / 'k.metallib'), str(initial), str(changed), str(expected),
                                             str(kwargs.get('size', 0)), str(kwargs.get('dim', 3))], capture_output=True, text=True)
                    if result.returncode == 2:
                        self.skipTest(result.stdout.strip())
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


RUNNER = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
int main(int argc, char **argv) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *error = nil;
    if (!dev) { puts("no Metal device"); return 2; }
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
    id<MTLFunction> fn = [lib newFunctionWithName:@"main0"];
    id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:fn error:&error];
    if (!ps) { puts(error.localizedDescription.UTF8String); return 1; }
    uint32_t initial = (uint32_t)strtoll(argv[2], NULL, 10), expected = (uint32_t)strtoll(argv[4], NULL, 10);
    int changed = atoi(argv[3]); bool buffer = atoi(argv[6]) == 1;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = buffer ? MTLTextureTypeTextureBuffer : MTLTextureType2D;
    d.pixelFormat = atoi(argv[5]) ? MTLPixelFormatR32Sint : MTLPixelFormatR32Uint;
    d.width = 8; d.height = buffer ? 1 : 4; d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageShaderAtomic;
    id<MTLBuffer> backing = buffer ? [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared] : nil;
    id<MTLTexture> tex = buffer ? [backing newTextureWithDescriptor:d offset:0 bytesPerRow:32] : [dev newTextureWithDescriptor:d];
    if (!tex) { puts("texture allocation failed"); return 1; }
    uint32_t data[1024]; for (int i = 0; i < 1024; i++) data[i] = initial;
    if (buffer) memcpy(backing.contents, data, sizeof data);
    else [tex replaceRegion:MTLRegionMake2D(0, 0, 8, 4) mipmapLevel:0 withBytes:data bytesPerRow:32];
    id<MTLArgumentEncoder> args = [fn newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> ab = [dev newBufferWithLength:args.encodedLength options:MTLResourceStorageModeShared];
    [args setArgumentBuffer:ab offset:0]; [args setTexture:tex atIndex:0];
    id<MTLCommandQueue> queue = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:ps]; [enc setBuffer:ab offset:0 atIndex:0];
    [enc useResource:tex usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [enc dispatchThreads:MTLSizeMake(257, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { puts(cb.error.localizedDescription.UTF8String); return 1; }
    if (buffer) memcpy(data, backing.contents, sizeof data);
    else [tex getBytes:data bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 4) mipmapLevel:0];
    unsigned bad = 0, n = buffer ? 1024 : 32;
    for (unsigned i = 0; i < n; i++) bad += data[i] != ((int)i == changed ? expected : initial);
    printf("257 reductions, %u incorrect texels or sentinels\n", bad); return bad != 0;
} }
'''


if __name__ == '__main__':
    unittest.main()
