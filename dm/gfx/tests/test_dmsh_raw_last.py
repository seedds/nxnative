#!/usr/bin/env python3
"""Raw global memory lookups that start from the entry they chose last (GlobalToStorage.TryGenerateRawStorageOp): through the offline translator (dmsh_xlate, arch sm86), a compute shader's two loads
through an address it cannot trace share one zero-initialized word, the helper tries that entry before the hint and the search and records a new choice; and on this Mac's GPU the translated helper reads
what the pool table names on a run of addresses that hit, leave and come back to pools: adjacent pools, a gap, an address above 4 GiB, a hinted chunk, one lookup after another in each lane.
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
HELPER = re.compile(r'(?m)^int (_\d+)\(thread const int& _\d+, thread const int& _\d+, const device (_\d+)& \w+, thread uint& (_\d+)\)\n\{\n')

# The pool table as the host writes it (nvn_mtl.m gmem_map, dmsh_abi.h DMSH_GMEM_MAP_*): 8,192 entries of 8 words {start lo, start hi, end lo, end hi, delta lo, delta hi, hint, 0}, sorted by start,
# entry 0 empty, unused entries starting at ~0. Pools A and B are adjacent, C lies past a gap and above 4 GiB, and its 2 MiB chunk has a hint naming it (the only pool there; the gap after it is in
# the chunk too, so the hint's entry does not hold that address)
RUNNER = '''#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
enum { POOLS = 3, WORDS = 1024, LANES = 64, STEPS = 16 };
static const uint64_t GUEST[POOLS] = {0x10000000ull, 0x10001000ull, 0x200040000ull};
static uint32_t value(int p, uint64_t off) { return (uint32_t)(p + 1) << 24 | (uint32_t)(off / 4); }
static int expect(uint64_t a, int64_t *pool) { for (int p = 0; p < POOLS; p++) if (a >= GUEST[p] && a < GUEST[p] + WORDS * 4) { *pool = p; return (int)value(p, a - GUEST[p]); } *pool = -1; return 0; }
int main(int ac, char **av) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); NSError *e = nil;
    id<MTLLibrary> l = [d newLibraryWithURL:[NSURL fileURLWithPath:@(av[1])] error:&e]; if (!l) { puts(e.description.UTF8String); return 2; }
    id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:[l newFunctionWithName:@"k"] error:&e]; if (!ps) { puts(e.description.UTF8String); return 2; }
    id<MTLBuffer> pool[POOLS], map = [d newBufferWithLength:8192 * 32 options:MTLResourceStorageModeShared];
    uint32_t *t = map.contents; memset(t, 0, 8192 * 32); for (int k = 1; k < 8192; k++) memset(t + k * 8, 0xff, 8);
    for (int p = 0; p < POOLS; p++) {
        pool[p] = [d newBufferWithLength:WORDS * 4 options:MTLResourceStorageModeShared]; uint32_t *w = pool[p].contents; for (int i = 0; i < WORDS; i++) w[i] = value(p, i * 4);
        uint64_t s = GUEST[p], end = s + WORDS * 4, delta = pool[p].gpuAddress - s, *ent = (uint64_t *)(t + (p + 1) * 8); ent[0] = s; ent[1] = end; ent[2] = delta;
    }
    uint32_t chunk = (uint32_t)(GUEST[2] >> 32) << 11 | (uint32_t)GUEST[2] >> 21; t[(chunk & 8191) * 8 + 6] = 3 | chunk << 13;   // C's chunk: entry 3
    // each lane: a word walk in A, B right after A, back to A, a gap, C above 4 GiB (through the hint), the gap after C in its chunk, C again, B, entry 0's range, A
    static const int64_t SEQ[STEPS][2] = {{0, 0}, {0, 4}, {0, 8}, {1, 0}, {1, 4}, {0, 12}, {-1, 0x10002000}, {2, 0}, {2, 4}, {-1, 0x200041000}, {2, 8}, {1, 8}, {-1, 0}, {-1, 0xfff}, {0, 16}, {0, 20}};
    id<MTLBuffer> addr = [d newBufferWithLength:LANES * STEPS * 8 options:MTLResourceStorageModeShared], out = [d newBufferWithLength:LANES * STEPS * 4 options:MTLResourceStorageModeShared];
    uint64_t *a = addr.contents; int want[LANES * STEPS];
    for (int lane = 0; lane < LANES; lane++) for (int s = 0; s < STEPS; s++) {
        uint64_t x = SEQ[s][0] < 0 ? (uint64_t)SEQ[s][1] : GUEST[SEQ[s][0]] + SEQ[s][1] + (uint64_t)(lane * 8) % (WORDS * 4 - 64); int64_t p;
        a[lane * STEPS + s] = x; want[lane * STEPS + s] = expect(x, &p);
    }
    id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLCommandBuffer> c = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [c computeCommandEncoder];
    [ce setComputePipelineState:ps]; [ce setBuffer:map offset:0 atIndex:0]; [ce setBuffer:addr offset:0 atIndex:1]; [ce setBuffer:out offset:0 atIndex:2];
    for (int p = 0; p < POOLS; p++) [ce useResource:pool[p] usage:MTLResourceUsageRead];
    [ce dispatchThreads:MTLSizeMake(LANES, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)]; [ce endEncoding]; [c commit]; [c waitUntilCompleted];
    const int *got = out.contents; int bad = 0;
    for (int i = 0; i < LANES * STEPS; i++) if (got[i] != want[i]) { if (bad++ < 8) printf("lane %d step %d: address 0x%llx read 0x%x, want 0x%x\\n", i / STEPS, i % STEPS, a[i], got[i], want[i]); }
    printf("bad %d of %d\\n", bad, LANES * STEPS); return 0;
} }
'''


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class RawLast(Translated):
    def msl(self):
        code = (s2r_tid(9), s2r_tid(5), LDG, STORE, (LDG[0], LDG[1] | 4 << 40), STORE)   # two loads, the second 4 bytes on, each kept by a store to shared memory
        return self.translate(b''.join(inst(*waiting(i)) for i in code) + inst(*EXIT), shared=0x1000)

    def test_two_loads_share_one_zeroed_word(self):
        msl = self.msl()
        m = HELPER.search(msl)
        self.assertTrue(m, msl[:3000])
        name, last = m.group(1), m.group(3)
        main = msl[msl.index('kernel void main0'):]
        word = re.search(r'(?m)^    uint (_\d+) = 0u;$', main)
        self.assertTrue(word, main[:600])
        calls = re.findall(name + r'\([^;]*, (_\d+)\);', main)
        self.assertEqual(calls, [word.group(1)] * 2)
        helper = msl[m.end():msl.index('\n}\n', m.end())]
        self.assertLess(helper.index(last), helper.index('>> uint(21)'))   # the last entry first, then the 2 MiB chunk's hint
        self.assertRegex(helper, re.escape(last) + r' = uint\(_\d+\);')   # a hint or search records its choice

    @unittest.skipUnless(shutil.which('xcrun') and sys.platform == 'darwin', 'needs Metal')
    def test_reads_on_gpu(self):
        msl = self.msl()
        m = HELPER.search(msl)
        kernel = (msl[:msl.index('kernel void main0')] +
                  f'kernel void k(const device {m.group(2)}& map [[buffer(0)]], const device uint2* addr [[buffer(1)]], device int* out [[buffer(2)]], uint id [[thread_position_in_grid]])\n'
                  '{\n    uint last = 0u;\n    for (int s = 0; s < 16; s++)\n    {\n        int lo = int(addr[id * 16 + s].x), hi = int(addr[id * 16 + s].y);\n'
                  f'        out[id * 16 + s] = {m.group(1)}(lo, hi, map, last);\n    }}\n}}\n')
        with tempfile.TemporaryDirectory() as w:
            w = Path(w)
            (w / 'k.metal').write_text(kernel)
            r = subprocess.run(MAC_METAL + [str(w / 'k.metal'), '-o', str(w / 'k.air')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-3000:])
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(w / 'k.air'), '-o', str(w / 'k.metallib')], check=True)
            (w / 'r.m').write_text(RUNNER)
            subprocess.run(['clang', '-fobjc-arc', '-framework', 'Metal', '-framework', 'Foundation', str(w / 'r.m'), '-o', str(w / 'r')], check=True, capture_output=True)
            out = subprocess.run([str(w / 'r'), str(w / 'k.metallib')], capture_output=True, text=True)
            self.assertIn('bad 0 of 1024', out.stdout, out.stdout + out.stderr)


LDG128 = ((LDG[0] & ~(7 << 9)) | 6 << 9, (LDG[1] & ~(0xff << 16)) | 12 << 16)   # LDG.E.128 R12, [R5.64 + UR4]: size bits 73..75 = 6 (B128), R12..R15
def sts(rb, off): return (STORE[0], (STORE[1] & ~(0xffffffff << 32)) | rb << 32 | off << 40)   # STS.U32 [R9 * 16 + off], Rb
VEC_HELPER = re.compile(r'(?m)^int (_\d+)\(thread const int& _\d+, thread const int& _\d+, const device (_\d+)& \w+, thread uint& (_\d+), thread spvUnsafeArray<uint, 3>& (_\d+)\)\n\{\n')


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class RawVector(Translated):
    """An LDG.128 through an address the translator ties to no storage buffer: one pool table lookup and one 128-bit load (Operation.GlobalVectorCount, GlobalToStorage
    GenerateRawVectorLoad), not four, the other words passed back in gmem_vec; on the GPU each of the four words is the pool's"""
    def msl(self):
        code = (s2r_tid(9), s2r_tid(5), LDG128, sts(12, 0), sts(13, 4), sts(14, 8), sts(15, 12))
        return self.translate(b''.join(inst(*waiting(i)) for i in code) + inst(*EXIT), shared=0x1000)

    def test_one_vector_load_for_the_four_words(self):
        msl = self.msl()
        m = VEC_HELPER.search(msl)
        self.assertTrue(m, msl[:3000])
        helper = msl[m.end():msl.index('\n}\n', m.end())]
        self.assertEqual(len(re.findall(r'uint4 _\d+ = \*\(reinterpret_cast<const device uint4\*>', helper)), 1, helper[-1500:])
        self.assertNotRegex(helper, r'reinterpret_cast<const device uint\*>')   # no word loads besides it
        main = msl[msl.index('kernel void main0'):]
        self.assertEqual(len(re.findall(m.group(1) + r'\(', main)), 1)
        scalar = HELPER.search(msl)
        self.assertFalse(scalar and re.search(scalar.group(1) + r'\(', main), 'a word of the 128-bit load went through the scalar helper')
        for k in range(3): self.assertRegex(main, re.escape(m.group(4)) + rf'\[{k}\]')

    def test_a_miss_zeroes_the_other_words(self):   # gmem_vec has no initial value and the caller reads its words after the call
        msl = self.msl()
        m = VEC_HELPER.search(msl)
        helper = msl[m.end():msl.index('\n}\n', m.end())]
        miss = helper[helper.rindex('return'):]
        self.assertTrue(miss.startswith('return 0;'), miss)
        before = helper[:helper.rindex('return')]
        before = before[max(before.rindex('}'), before.rindex('{')):]   # the miss path: the straight run of lines that ends in its return
        for k in range(3): self.assertIn(f'{m.group(4)}[{k}] = 0u;', before, helper[-600:])

    def test_a_64_bit_miss_zeroes_its_other_word(self):
        ldg64 = ((LDG[0] & ~(7 << 9)) | 5 << 9, (LDG[1] & ~(0xff << 16)) | 12 << 16)   # LDG.E.64 R12, [R5.64 + UR4]: size bits = 5 (B64), R12..R13
        code = (s2r_tid(9), s2r_tid(5), ldg64, sts(12, 0), sts(13, 4))
        msl = self.translate(b''.join(inst(*waiting(i)) for i in code) + inst(*EXIT), shared=0x1000)
        m = VEC_HELPER.search(msl)
        self.assertTrue(m, msl[:3000])
        helper = msl[m.end():msl.index('\n}\n', m.end())]
        self.assertIn('uint2 ', helper)
        before = helper[:helper.rindex('return')]
        before = before[max(before.rindex('}'), before.rindex('{')):]
        self.assertTrue(helper[helper.rindex('return'):].startswith('return 0;'))
        self.assertIn(f'{m.group(4)}[0] = 0u;', before, helper[-600:])
        self.assertNotIn(f'{m.group(4)}[1] = 0u;', before)

    @unittest.skipUnless(shutil.which('xcrun') and sys.platform == 'darwin', 'needs Metal')
    def test_reads_on_gpu(self):
        msl = self.msl()
        m = VEC_HELPER.search(msl)
        # each step's word 0 when words 1..3 are the next three of its pool (value() counts words), 0 where it reads no pool; the runner's addresses keep all four in one pool
        kernel = (msl[:msl.index('kernel void main0')] +
                  f'kernel void k(const device {m.group(2)}& map [[buffer(0)]], const device uint2* addr [[buffer(1)]], device int* out [[buffer(2)]], uint id [[thread_position_in_grid]])\n'
                  '{\n    uint last = 0u;\n    spvUnsafeArray<uint, 3> vec;\n    for (int s = 0; s < 16; s++)\n    {\n        int lo = int(addr[id * 16 + s].x), hi = int(addr[id * 16 + s].y);\n'
                  f'        vec[0] = vec[1] = vec[2] = 0xdeadu; int w = {m.group(1)}(lo, hi, map, last, vec);\n'   # a miss must not leave them as they were
                  '        out[id * 16 + s] = (w == 0 && vec[0] == 0u && vec[1] == 0u && vec[2] == 0u) || (vec[0] == uint(w) + 1u && vec[1] == uint(w) + 2u && vec[2] == uint(w) + 3u) ? w : int(0xdead);\n    }\n}\n')
        with tempfile.TemporaryDirectory() as w:
            w = Path(w)
            (w / 'k.metal').write_text(kernel)
            r = subprocess.run(MAC_METAL + [str(w / 'k.metal'), '-o', str(w / 'k.air')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-3000:])
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(w / 'k.air'), '-o', str(w / 'k.metallib')], check=True)
            (w / 'r.m').write_text(RUNNER)
            subprocess.run(['clang', '-fobjc-arc', '-framework', 'Metal', '-framework', 'Foundation', str(w / 'r.m'), '-o', str(w / 'r')], check=True, capture_output=True)
            out = subprocess.run([str(w / 'r'), str(w / 'k.metallib')], capture_output=True, text=True)
            self.assertIn('bad 0 of 1024', out.stdout, out.stdout + out.stderr)



@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class RawVectorPair(Translated):
    """Two LDG.128 in one block (lunks on PR #1813): the first's words 1..3 unused, the second's word 0 unused and its word 2 used. The first load's follower scan took the
    second's word 2 as its own (same word count, word > 0), so it read the first load's gmem_vec and the second load's memory was never read. Each LDG's words now carry
    their own group (Operation.GlobalVectorGroup): the first is one vector call, the second's word a scalar lookup of its own address"""
    def msl(self):
        a = (LDG128[0] & ~0xf, LDG128[1])   # predicate PT (64..67 = 0, stored as 7 - P): no branch around the load, so both loads are in one block
        b = (a[0], (a[1] & ~(0xff << 16) & ~(0xffffff << 40)) | 16 << 16 | 0x40 << 40)   # LDG.E.128 R16, [R5.64 + UR4 + 0x40]
        code = (s2r_tid(9), s2r_tid(5), a, b, sts(12, 0), sts(18, 4))
        return self.translate(b''.join(inst(*waiting(i)) for i in code) + inst(*EXIT), shared=0x1000)

    def test_a_word_of_the_next_load_is_not_read_from_this_ones_vector(self):
        msl = self.msl()
        vec, scalar = VEC_HELPER.search(msl), HELPER.search(msl)
        self.assertTrue(vec, msl[:3000])
        main = msl[msl.index('kernel void main0'):]
        self.assertEqual(len(re.findall(vec.group(1) + r'\(', main)), 1, main[:3000])
        self.assertNotRegex(main, re.escape(vec.group(4)) + r'\[\d\]', 'a word was read from the vector call\'s gmem_vec')
        self.assertTrue(scalar and len(re.findall(scalar.group(1) + r'\(', main)) == 1, 'the second load\'s word 2 has no lookup of its own:\n' + main[:3000])

if __name__ == '__main__':
    unittest.main()
