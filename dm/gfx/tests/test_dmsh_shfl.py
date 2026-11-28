"""SHFL's lane/offset is b[4:0], for Maxwell and Ampere and all four modes (PR #2044).

Synthetic shaders read b and c from shared memory, shuffle the lane ID, and store the result and predicate. The GPU check uses the production translator,
SPIRV-Cross and Metal flags, including segmented/clamped shuffles, high bits through bit 31 and an immediate lane. UP uses clamp zero: its separate
nonzero-clamp defect is outside this test. Run with DMSH_TEST_XLATE=<translator> DMSH_TEST_GPU=1 python3 dm/gfx/tests/test_dmsh_shfl.py.
"""
import array
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
import dmsh_chain
import dmsh_geometry


def program(arch, mode, immediate):
    """R9 = lane ID; R1 = b, R2 = c; SHFL R3|P0, R9, R1 (or 1), R2; store R3 and P0 at [lane * 16 + 8/12]."""
    if arch == 'sm86':
        def inst(hi, lo): return struct.pack('<QQ', lo, hi | 0x3f << 52)   # wait on all scoreboards
        body = [(0x000e260000002100, 0x0000000000097919),                    # S2R R9, SR_TID.X
                (0x000fe20000000800, 0x00000000ff017984),                    # LDS R1, [RZ]
                (0x000fe20000000800, 0x00000400ff027984),                    # LDS R2, [RZ + 4]
                (0x000fe20000000002, 0x7389 | 3 << 16 | 9 << 24 | 1 << 32 | mode << 58) if not immediate else
                (0x000fe20000000002, 0x7989 | 3 << 16 | 9 << 24 | 1 << 53 | mode << 58),
                (0x000fc200040000ff, 0x00000001ff047807),                    # SEL R4, RZ, 1, !P0
                (0x000fe8000000c800, 0x0000080309007388),                    # STS [R9 * 16 + 8], R3
                (0x000fe8000000c800, 0x00000c0409007388),                    # STS [R9 * 16 + 12], R4
                (0x000fea0003800000, 0x000000000000794d)]                    # EXIT
        return b''.join(inst(*i) for i in body)
    body = [0xf0c8000002170009,                                             # S2R R9, SR_TID.X (SR 0x21)
            0x3848000000000000 | 7 << 16 | 8 | 9 << 8 | 4 << 20,           # SHL R8, R9, 4
            0xef4c000000070000 | 1 | 255 << 8,                              # LDS R1, [RZ]
            0xef4c000000070000 | 2 | 255 << 8 | 4 << 20,                    # LDS R2, [RZ + 4]
            0xef10000000070000 | 3 | 9 << 8 | 1 << 20 | 2 << 39 | mode << 30 | int(immediate) << 28,
            0x0100000000070005 | 1 << 20,                                   # MOV32I R5, 1
            0x5ca0000000070004 | 5 << 8 | 255 << 20,                        # SEL R4, R5, RZ, P0
            0xef5c000000070003 | 8 << 8 | 8 << 20,                         # STS [R8 + 8], R3
            0xef5c000000070004 | 8 << 8 | 12 << 20,                        # STS [R8 + 12], R4
            0xe30000000007000f]                                             # EXIT
    return b''.join(struct.pack('<4Q', 0x001f8000fc0007e0, *(tuple(body[i:i + 3]) + (0x50b0000000070f00,) * 2)[:3])
                    for i in range(0, len(body), 3))


def harness(msl):
    """Seed the shader's two shared input words and copy each lane's two output words to buffer 30."""
    start = msl.index('kernel void main0(') + len('kernel void main0(')
    msl = msl[:start] + 'device uint* dm_io [[buffer(30)]], uint dm_lane [[thread_index_in_threadgroup]], uint3 dm_group [[threadgroup_position_in_grid]], ' + msl[start:]
    start = msl.index('{', msl.index('kernel void main0(')) + 1
    end = msl.rindex('}')
    body = msl[start:end]
    shared = re.search(r'threadgroup (?:spvUnsafeArray<uint, \d+> (\w+)|uint (\w+)\[\d+\]);', body)
    if not shared: raise ValueError('no shared array')
    name = shared.group(1) or shared.group(2)
    seed = f'\n    if (dm_lane == 0) {{ {name}[0] = dm_io[dm_group.x * 66]; {name}[1] = dm_io[dm_group.x * 66 + 1]; }}\n    threadgroup_barrier(mem_flags::mem_threadgroup);\n'
    dump = f'dm_io[dm_group.x * 66 + 2 + dm_lane * 2] = {name}[dm_lane * 4 + 2]; dm_io[dm_group.x * 66 + 3 + dm_lane * 2] = {name}[dm_lane * 4 + 3];'
    body = body[:shared.end()] + seed + body[shared.end():]
    return msl[:start] + re.sub(r'\breturn;', '{ ' + dump + ' return; }', body) + '\n    ' + dump + '\n' + msl[end:]


RUNNER = r'''#import <Metal/Metal.h>
int main(int argc, char **argv) { @autoreleasepool {
  id<MTLDevice> d = MTLCreateSystemDefaultDevice(); if (!d) return 2;
  NSError *e = nil;
  id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];
  id<MTLFunction> f = [lib newFunctionWithName:@"main0"];
  id<MTLComputePipelineState> ps = f ? [d newComputePipelineStateWithFunction:f error:&e] : nil;
  if (!ps || ps.threadExecutionWidth != 32) { fprintf(stderr, "pipeline: %s\n", e.description.UTF8String); return 1; }
  NSData *in = [NSData dataWithContentsOfFile:@(argv[2])];
  id<MTLBuffer> b = [d newBufferWithBytes:in.bytes length:in.length options:MTLResourceStorageModeShared];
  id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
  [ce setComputePipelineState:ps]; [ce setBuffer:b offset:0 atIndex:30];
  [ce dispatchThreadgroups:MTLSizeMake(in.length / (66 * 4), 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
  [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
  if (cb.error) { fprintf(stderr, "%s\n", cb.error.description.UTF8String); return 1; }
  return [[NSData dataWithBytes:b.contents length:in.length] writeToFile:@(argv[2]) atomically:YES] ? 0 : 1;
} }
'''


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
@unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') and sys.platform == 'darwin' and shutil.which('xcrun'), 'set DMSH_TEST_GPU=1 on a Mac with Xcode')
class ShuffleOnGpu(unittest.TestCase):
    def test_lane_bits_and_predicate(self):
        with tempfile.TemporaryDirectory(prefix='shfl_') as tmp:
            work = Path(tmp)
            (work / 'run.m').write_text(RUNNER)
            subprocess.run(['clang', '-fobjc-arc', '-O1', str(work / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(work / 'run')], check=True, capture_output=True)
            for arch in ('sm50', 'sm86'):
                for mode in range(4):
                    for immediate in (False, True):
                        with self.subTest(arch=arch, mode=('IDX', 'UP', 'DOWN', 'BFLY')[mode], immediate=immediate):
                            self.check_shader(work, arch, mode, immediate)

    def check_shader(self, work, arch, mode, immediate):
        (work / 'k.bin').write_bytes(program(arch, mode, immediate))
        job = dict(key='k', stage='cs', arch=arch, code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=True, reduced=True, shared=512)
        if arch == 'sm86': job['nvn2_cs'] = [0, 0, 3, 0x40, 0]
        (work / 'jobs.json').write_text(json.dumps([job]))
        (work / 'k.spv').unlink(missing_ok=True)
        r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), str(work)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue((work / 'k.spv').exists(), r.stdout + r.stderr)
        dmsh_geometry.flatten_sets(work / 'k.spv')
        r = subprocess.run([dmsh_chain.SPIRV_CROSS, *dmsh_chain.CROSS_MSL, str(work / 'k.spv')], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        (work / 'k.metal').write_text(harness(r.stdout))
        subprocess.run([*dmsh_chain.MAC_METAL, str(work / 'k.metal'), '-o', str(work / 'k.air')], check=True, capture_output=True)
        subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(work / 'k.air'), '-o', str(work / 'k.metallib')], check=True, capture_output=True)
        masks = (0, 0x1800) if mode == 1 else (31, 7, 0x1807)
        cases = [(0, c) for c in masks] if immediate else [(high | low, c) for c in masks
                 for high in (0, 32, 0x80000000, 0x7fffffe0, 0xffffffe0, 0xdaa06cc0) for low in range(32)]
        words = array.array('I', [v for b, c in cases for v in (b, c, *([0xcdcdcdcd] * 64))])
        (work / 'io.bin').write_bytes(words.tobytes())
        r = subprocess.run([str(work / 'run'), str(work / 'k.metallib'), str(work / 'io.bin')], capture_output=True, text=True, timeout=60)
        if r.returncode == 2: self.skipTest('no Metal device')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        words = array.array('I'); words.frombytes((work / 'io.bin').read_bytes())
        for i, (b, c) in enumerate(cases):
            offset = 1 if immediate else b % 32
            for lane in range(32):
                lo = lane & ((c >> 8) & 31)
                hi = lo | ((c & 31) & ~((c >> 8) & 31))
                src = ((offset & ~((c >> 8) & 31)) | lo, lane - offset, lane + offset, lane ^ offset)[mode]
                valid = src >= lo if mode == 1 else src <= hi
                expected = [src if valid else lane, int(valid)]
                got = list(words[i * 66 + 2 + lane * 2:i * 66 + 4 + lane * 2])
                self.assertEqual(got, expected, f'lane={lane} b={b:#x} c={c:#x}')


if __name__ == '__main__': unittest.main()
