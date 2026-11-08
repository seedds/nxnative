#!/usr/bin/env python3
"""Raw global memory at the address itself (GlobalToStorage.TryGenerateRawDirectOp, the job's raw_direct, a profile's shader_raw_direct): through the offline translator (dmsh_xlate, arch sm86), a compute shader's loads
through an address it cannot trace call a helper that takes the address's two words and nothing else (no pool table, no remembered entry) and reads through it once; an LDG.E.128 is one 128-bit
load with the other three words passed back. On this Mac's GPU the translated helpers read Metal buffers through their own GPU addresses, which the host hands the game as NVN addresses
(nvn_mtl.m mtl_gva_on): runs of addresses that stay in, leave and come back to buffers, one lane after another.
Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import ast
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_dmsh_ampere_mem import EXIT, Translated, inst   # noqa: E402
from test_dmsh_ampere_alu import STORE, waiting, s2r_tid   # noqa: E402

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
scope = {}
exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.Assign) and any(getattr(t, 'id', None) == 'MAC_METAL' for t in n.targets)], type_ignores=[]),
             str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
MAC_METAL = scope['MAC_METAL']

LDG = (0x000ea200081e890f, 0x0000000405067981)   # LDG.E R6, [R5.64 + UR4]: R5 the thread index, an address the translator traces to no storage buffer
HELPER = re.compile(r'(?m)^int (_\d+)\(thread const int& _\d+, thread const int& _\d+\)\n\{\n')
LOAD = r'\*\(reinterpret_cast<const device {}\*>\(as_type<ulong>\(uint2\('

# Three buffers stand for pools; each lane reads a run of addresses in them through their Metal GPU addresses, as the host hands them out: a word walk in A, B, back to A, C, B, A
RUNNER = '''#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
enum { POOLS = 3, WORDS = 1024, LANES = 64, STEPS = 16 };
static uint32_t value(int p, uint64_t off) { return (uint32_t)(p + 1) << 24 | (uint32_t)(off / 4); }
int main(int ac, char **av) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); NSError *e = nil;
    id<MTLLibrary> l = [d newLibraryWithURL:[NSURL fileURLWithPath:@(av[1])] error:&e]; if (!l) { puts(e.description.UTF8String); return 2; }
    id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:[l newFunctionWithName:@"k"] error:&e]; if (!ps) { puts(e.description.UTF8String); return 2; }
    id<MTLBuffer> pool[POOLS];
    for (int p = 0; p < POOLS; p++) { pool[p] = [d newBufferWithLength:WORDS * 4 options:MTLResourceStorageModeShared]; uint32_t *w = pool[p].contents; for (int i = 0; i < WORDS; i++) w[i] = value(p, i * 4); }
    static const int SEQ[STEPS][2] = {{0, 0}, {0, 4}, {0, 8}, {1, 0}, {1, 4}, {0, 12}, {2, 0}, {2, 4}, {2, 8}, {1, 8}, {0, 16}, {0, 20}, {2, 64}, {1, 512}, {0, 4000}, {2, 4064}};
    id<MTLBuffer> addr = [d newBufferWithLength:LANES * STEPS * 8 options:MTLResourceStorageModeShared], out = [d newBufferWithLength:LANES * STEPS * 4 options:MTLResourceStorageModeShared];
    uint64_t *a = addr.contents; int want[LANES * STEPS];
    for (int lane = 0; lane < LANES; lane++) for (int s = 0; s < STEPS; s++) {
        int p = SEQ[s][0]; uint64_t off = (SEQ[s][1] + (uint64_t)lane * 16) % (WORDS * 4 - 16);
        a[lane * STEPS + s] = pool[p].gpuAddress + off; want[lane * STEPS + s] = (int)value(p, off);
    }
    id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLCommandBuffer> c = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [c computeCommandEncoder];
    [ce setComputePipelineState:ps]; [ce setBuffer:addr offset:0 atIndex:1]; [ce setBuffer:out offset:0 atIndex:2];
    for (int p = 0; p < POOLS; p++) [ce useResource:pool[p] usage:MTLResourceUsageRead];
    [ce dispatchThreads:MTLSizeMake(LANES, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)]; [ce endEncoding]; [c commit]; [c waitUntilCompleted];
    const int *got = out.contents; int bad = 0;
    for (int i = 0; i < LANES * STEPS; i++) if (got[i] != want[i]) { if (bad++ < 8) printf("lane %d step %d: address 0x%llx read 0x%x, want 0x%x\\n", i / STEPS, i % STEPS, a[i], got[i], want[i]); }
    printf("bad %d of %d\\n", bad, LANES * STEPS); return 0;
} }
'''


def run_on_gpu(case, kernel):
    with tempfile.TemporaryDirectory() as w:
        w = Path(w)
        (w / 'k.metal').write_text(kernel)
        r = subprocess.run(MAC_METAL + [str(w / 'k.metal'), '-o', str(w / 'k.air')], capture_output=True, text=True)
        case.assertEqual(r.returncode, 0, r.stderr[-3000:])
        subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(w / 'k.air'), '-o', str(w / 'k.metallib')], check=True)
        (w / 'r.m').write_text(RUNNER)
        subprocess.run(['clang', '-fobjc-arc', '-framework', 'Metal', '-framework', 'Foundation', str(w / 'r.m'), '-o', str(w / 'r')], check=True, capture_output=True)
        out = subprocess.run([str(w / 'r'), str(w / 'k.metallib')], capture_output=True, text=True)
        case.assertIn('bad 0 of 1024', out.stdout, out.stdout + out.stderr)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class RawDirect(Translated):
    def msl(self):
        code = (s2r_tid(9), s2r_tid(5), LDG, STORE, (LDG[0], LDG[1] | 4 << 40), STORE)   # two loads, the second 4 bytes on, each kept by a store to shared memory
        return self.translate(b''.join(inst(*waiting(i)) for i in code) + inst(*EXIT), shared=0x1000, raw_direct=True)

    def test_the_helper_reads_at_the_address(self):
        msl = self.msl()
        m = HELPER.search(msl)
        self.assertTrue(m, msl[:3000])
        helper = msl[m.end():msl.index('\n}\n', m.end())]
        self.assertEqual(len(re.findall(LOAD.format('uint'), helper)), 1, helper)
        self.assertNotIn('>> uint(21)', helper)   # no 2 MiB chunk hint, no table
        main = msl[msl.index('kernel void main0'):]
        self.assertEqual(len(re.findall(m.group(1) + r'\(', main)), 2)
        self.assertNotIn('spvDescriptorSet', main)   # nothing bound for it: no pool table

    @unittest.skipUnless(shutil.which('xcrun') and sys.platform == 'darwin', 'needs Metal')
    def test_reads_on_gpu(self):
        msl = self.msl()
        m = HELPER.search(msl)
        kernel = (msl[:msl.index('kernel void main0')] +
                  'kernel void k(const device uint2* addr [[buffer(1)]], device int* out [[buffer(2)]], uint id [[thread_position_in_grid]])\n'
                  '{\n    for (int s = 0; s < 16; s++)\n    {\n        int lo = int(addr[id * 16 + s].x), hi = int(addr[id * 16 + s].y);\n'
                  f'        out[id * 16 + s] = {m.group(1)}(lo, hi);\n    }}\n}}\n')
        run_on_gpu(self, kernel)


LDG128 = ((LDG[0] & ~(7 << 9)) | 6 << 9, (LDG[1] & ~(0xff << 16)) | 12 << 16)   # LDG.E.128 R12, [R5.64 + UR4]: size bits 73..75 = 6 (B128), R12..R15
def sts(rb, off): return (STORE[0], (STORE[1] & ~(0xffffffff << 32)) | rb << 32 | off << 40)   # STS.U32 [R9 * 16 + off], Rb
VEC_HELPER = re.compile(r'(?m)^int (_\d+)\(thread const int& _\d+, thread const int& _\d+, thread spvUnsafeArray<uint, 3>& (_\d+)\)\n\{\n')


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class RawDirectVector(Translated):
    """An LDG.128 through an address the translator ties to no storage buffer: one 128-bit load at the address (Operation.GlobalVectorCount), not four, the other words passed back in
    gmem_vec; on the GPU each of the four words is the buffer's"""
    def msl(self):
        code = (s2r_tid(9), s2r_tid(5), LDG128, sts(12, 0), sts(13, 4), sts(14, 8), sts(15, 12))
        return self.translate(b''.join(inst(*waiting(i)) for i in code) + inst(*EXIT), shared=0x1000, raw_direct=True)

    def test_one_vector_load_for_the_four_words(self):
        msl = self.msl()
        m = VEC_HELPER.search(msl)
        self.assertTrue(m, msl[:3000])
        helper = msl[m.end():msl.index('\n}\n', m.end())]
        self.assertEqual(len(re.findall(LOAD.format('uint4'), helper)), 1, helper)
        self.assertNotRegex(helper, LOAD.format('uint'))   # no word loads besides it
        main = msl[msl.index('kernel void main0'):]
        self.assertEqual(len(re.findall(m.group(1) + r'\(', main)), 1)
        scalar = HELPER.search(msl)
        self.assertFalse(scalar and re.search(scalar.group(1) + r'\(', main), 'a word of the 128-bit load went through the scalar helper')
        for k in range(3): self.assertRegex(main, re.escape(m.group(2)) + rf'\[{k}\]')

    @unittest.skipUnless(shutil.which('xcrun') and sys.platform == 'darwin', 'needs Metal')
    def test_reads_on_gpu(self):
        msl = self.msl()
        m = VEC_HELPER.search(msl)
        # each step's word 0 when words 1..3 are the next three of its buffer (value() counts words); the runner's offsets keep all four in one buffer
        kernel = (msl[:msl.index('kernel void main0')] +
                  'kernel void k(const device uint2* addr [[buffer(1)]], device int* out [[buffer(2)]], uint id [[thread_position_in_grid]])\n'
                  '{\n    spvUnsafeArray<uint, 3> vec;\n    for (int s = 0; s < 16; s++)\n    {\n        int lo = int(addr[id * 16 + s].x), hi = int(addr[id * 16 + s].y);\n'
                  f'        int w = {m.group(1)}(lo, hi, vec);\n'
                  '        out[id * 16 + s] = vec[0] == uint(w) + 1u && vec[1] == uint(w) + 2u && vec[2] == uint(w) + 3u ? w : int(0xdead);\n    }\n}\n')
        run_on_gpu(self, kernel)


if __name__ == '__main__':
    unittest.main()
