#!/usr/bin/env python3
"""Raw global memory loads through one pointer at constant offsets resolve its pool once (GlobalToStorage.ResolveRawRuns): through the offline translator (dmsh_xlate, arch sm86), a compute
shader's three loads at +0, +4 and +12 through an address it cannot trace (the thread index, as Ocarina of Time's colour-pass mesh shaders read a structure through a pointer they loaded)
call the range helper once, before the loads, for [base, base + 16) (it tries the entry it chose last first), and each load calls the resolved helper with that entry and its delta instead of the per-access lookup; and on this
Mac's GPU the two translated helpers read what the pool table names: a run inside a pool takes its entry (also above 4 GiB, through the hint), a run across two adjacent pools, one that
leaves a pool for a gap, one in a gap and one at address 0 take entry 0 and still read each word through the full lookup.
Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
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
from test_dmsh_raw_last import LDG, MAC_METAL   # noqa: E402

LDG_R5 = (LDG[0], (LDG[1] & ~(0x3f << 32)) | 0x3f << 32)   # LDG.E R6, [R5]: the uniform register is URZ, so the address is R5 (the thread index) alone, a 32-bit register
OFFSETS = (0, 4, 12)
RESOLVE = re.compile(r'(?m)^int (_\d+)\((?:thread const int& _\d+, ){4}const device (_\d+)& \w+, thread uint& _\d+\)\n\{\n')
RESOLVED = re.compile(r'(?m)^int (_\d+)\((?:thread const int& _\d+, ){5}const device _\d+& \w+, thread uint& _\d+\)\n\{\n')

# The pool table as in test_dmsh_raw_last (nvn_mtl.m gmem_map, dmsh_abi.h DMSH_GMEM_MAP_*): pools A and B adjacent (entries 1 and 2), C past a gap and above 4 GiB (entry 3) with a hint
# for its 2 MiB chunk. Each lane resolves [base, base + 16) and loads +0, +4 and +12: inside A, across A's end into B, inside C, across C's end into the gap, in the gap, at address 0
RUNNER = '''#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
enum { POOLS = 3, WORDS = 1024, LANES = 64, CASES = 6, OUTS = 4 };
static const uint64_t GUEST[POOLS] = {0x10000000ull, 0x10001000ull, 0x200040000ull};
static const int OFF[3] = {0, 4, 12};
static uint32_t value(int p, uint64_t off) { return (uint32_t)(p + 1) << 24 | (uint32_t)(off / 4); }
static int expect(uint64_t a) { for (int p = 0; p < POOLS; p++) if (a >= GUEST[p] && a < GUEST[p] + WORDS * 4) return (int)value(p, a - GUEST[p]); return 0; }
static int entry(uint64_t s, uint64_t e) { for (int p = 0; p < POOLS; p++) if (s >= GUEST[p] && e <= GUEST[p] + WORDS * 4) return p + 1; return 0; }
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
    id<MTLBuffer> base = [d newBufferWithLength:LANES * CASES * 8 options:MTLResourceStorageModeShared], out = [d newBufferWithLength:LANES * CASES * OUTS * 4 options:MTLResourceStorageModeShared];
    uint64_t *b = base.contents; int want[LANES * CASES * OUTS];
    for (int lane = 0; lane < LANES; lane++) {
        uint64_t in = (uint64_t)(lane * 8) % (WORDS * 4 - 64);
        uint64_t cs[CASES] = {GUEST[0] + in, GUEST[1] - 8, GUEST[2] + in, GUEST[2] + WORDS * 4 - 8, 0x10002000ull + in, 0};
        for (int c = 0; c < CASES; c++) {
            int i = lane * CASES + c; b[i] = cs[c]; want[i * OUTS] = entry(cs[c], cs[c] + 16);
            for (int k = 0; k < 3; k++) want[i * OUTS + 1 + k] = expect(cs[c] + OFF[k]);
        }
    }
    id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLCommandBuffer> c = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [c computeCommandEncoder];
    [ce setComputePipelineState:ps]; [ce setBuffer:map offset:0 atIndex:0]; [ce setBuffer:base offset:0 atIndex:1]; [ce setBuffer:out offset:0 atIndex:2];
    for (int p = 0; p < POOLS; p++) [ce useResource:pool[p] usage:MTLResourceUsageRead];
    [ce dispatchThreads:MTLSizeMake(LANES, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)]; [ce endEncoding]; [c commit]; [c waitUntilCompleted];
    const int *got = out.contents; int bad = 0;
    for (int i = 0; i < LANES * CASES * OUTS; i++) if (got[i] != want[i]) { if (bad++ < 8) printf("lane %d case %d out %d: base 0x%llx got 0x%x, want 0x%x\\n", i / OUTS / CASES, i / OUTS % CASES, i % OUTS, b[i / OUTS], got[i], want[i]); }
    printf("bad %d of %d\\n", bad, LANES * CASES * OUTS); return 0;
} }
'''


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class RawRun(Translated):
    def msl(self):
        code = (s2r_tid(9), s2r_tid(5)) + sum((((LDG_R5[0], LDG_R5[1] | o << 40), STORE) for o in OFFSETS), ())   # each load kept by a store to shared memory
        return self.translate(b''.join(inst(*waiting(i)) for i in code) + inst(*EXIT), shared=0x1000)

    def test_one_resolve_before_the_runs_loads(self):
        msl = self.msl()
        r, h = RESOLVE.search(msl), RESOLVED.search(msl)
        self.assertTrue(r and h, msl[:4000])
        main = msl[msl.index('kernel void main0'):]
        calls = re.findall(r'(?m)^ *int (_\d+) = ' + r.group(1) + r'\(([^;]*)\);$', main)
        self.assertEqual(len(calls), 1, main[:3000])
        entry = calls[0][0]
        loads = [m.start() for m in re.finditer(h.group(1) + r'\(', main)]
        self.assertEqual(len(loads), len(OFFSETS), main[:3000])
        self.assertLess(main.index(r.group(1) + '('), loads[0])   # the resolve comes before the first load
        self.assertEqual(len(re.findall(r'(?m)^int _\d+\(thread const int& _\d+, thread const int& _\d+, const device', msl)), 0)   # no per-access helper is left
        names = [entry] + re.findall(r'(?m)^ *int (_\d+) = ' + entry + ';$', main)   # the entry, or a copy of it
        self.assertEqual(len(re.findall(r'(?m)^ *int _\d+ = (?:' + '|'.join(names) + r') \* 8;$', main)), 1, main[:2000])   # the entry's delta, read once

    @unittest.skipUnless(shutil.which('xcrun') and sys.platform == 'darwin', 'needs Metal')
    def test_reads_on_gpu(self):
        msl = self.msl()
        r, h = RESOLVE.search(msl), RESOLVED.search(msl)
        n = 6
        body = ''.join(f'        {{ uint a = b.x + {o}u; int alo = int(a), ahi = hi + (a < b.x ? 1 : 0); out[(i * {n} + s) * 4 + {k + 1}] = {h.group(1)}(alo, ahi, e, dlo, dhi, map, last); }}\n'
                       for k, o in enumerate(OFFSETS))
        kernel = (msl[:msl.index('kernel void main0')] +
                  f'kernel void k(const device {r.group(2)}& map [[buffer(0)]], const device uint2* base [[buffer(1)]], device int* out [[buffer(2)]], uint i [[thread_position_in_grid]])\n'
                  '{\n    uint last = 0u, chosen = 0u;\n'
                  f'    for (int s = 0; s < {n}; s++)\n    {{\n        uint2 b = base[i * {n} + s]; int lo = int(b.x), hi = int(b.y);\n'
                  '        uint en = b.x + 16u; int elo = int(en), ehi = hi + (en < b.x ? 1 : 0);\n'
                  f'        int e = {r.group(1)}(lo, hi, elo, ehi, map, chosen);\n'
                  '        int dlo = int(map._m0[e * 8 + 4]), dhi = int(map._m0[e * 8 + 5]);\n'
                  f'        out[(i * {n} + s) * 4] = e;\n' + body + '    }\n}\n')
        with tempfile.TemporaryDirectory() as w:
            w = Path(w)
            (w / 'k.metal').write_text(kernel)
            c = subprocess.run(MAC_METAL + [str(w / 'k.metal'), '-o', str(w / 'k.air')], capture_output=True, text=True)
            self.assertEqual(c.returncode, 0, c.stderr[-3000:])
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(w / 'k.air'), '-o', str(w / 'k.metallib')], check=True)
            (w / 'r.m').write_text(RUNNER)
            subprocess.run(['clang', '-fobjc-arc', '-framework', 'Metal', '-framework', 'Foundation', str(w / 'r.m'), '-o', str(w / 'r')], check=True, capture_output=True)
            out = subprocess.run([str(w / 'r'), str(w / 'k.metallib')], capture_output=True, text=True)
            self.assertIn('bad 0 of 1536', out.stdout, out.stdout + out.stderr)


if __name__ == '__main__':
    unittest.main()
