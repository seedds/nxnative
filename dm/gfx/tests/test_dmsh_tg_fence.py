"""Warp-synchronous shared memory: Ampere compilers drop the barrier in small scans whose lanes run in lockstep (Unreal's GPU radix sort downsweep, Orbitals' d032ce5090303e01:
STS, then LDS of the neighbouring lane's word, then STS). For one thread such a load and its own earlier store do not alias, so Metal may move the load above the store.
The translator (SharedMemoryFence.cs) calls dmsh_tg_fence() before each shared memory load that some path reaches from a store with no barrier between, and dmsh_rewrite.py
tg_fence makes the helper's body a simdgroup-scope memory fence (not an execution barrier). The rewrite is tested on its own; the placement needs DMSH_TEST_XLATE (the translator)
like the other translator tests. When the Metal compiler is installed, the result is compiled as the build compiles it (dmsh_chain.py MAC_METAL), and a kernel whose array
spills to device memory (tg_spill, after tg_fence) gets a fence that orders device memory as well."""
from pathlib import Path
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import dmsh_abi as abi, dmsh_rewrite
from dmsh_chain import MAC_METAL   # noqa: E402  (the build's Mac compile)
import test_dmsh_control_flow as cf   # noqa: E402  (the translator harness; its module, so its own tests do not run here)
PT, WORK, EXIT, bits, bra = cf.PT, cf.WORK, cf.EXIT, cf.bits, cf.bra
tg_fence, tg_spill, FENCE = dmsh_rewrite.tg_fence, dmsh_rewrite.tg_spill, dmsh_rewrite.TG_FENCE

ARRAY = """template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    thread T& operator [] (size_t pos) thread { return elements[pos]; }
    device T& operator [] (size_t pos) device { return elements[pos]; }
    threadgroup T& operator [] (size_t pos) threadgroup { return elements[pos]; }
};
"""
# The downsweep's shape as the translator writes it: each lane stores its word, a fence, reads its neighbour's, adds and stores again (SPIRV-Cross's own output for the helper,
# whose body membar_fence has already made a device fence by the time tg_fence runs)
SCAN = """#include <metal_stdlib>
using namespace metal;
""" + ARRAY + """
struct B { uint _m0[1]; };

static inline __attribute__((always_inline))
void dmsh_tg_fence()
{
    threadgroup_barrier(mem_flags::mem_device | mem_flags::mem_threadgroup | mem_flags::mem_texture);
}

kernel void main0(device B& out [[buffer(1)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    threadgroup spvUnsafeArray<uint, %d> _39;
    uint _10 = gl_LocalInvocationID.x;
    _39[_10] = out._m0[_10];
    dmsh_tg_fence();
    uint _11 = _39[_10 - 1u] + _39[_10];
    _39[_10] = _11;
    dmsh_tg_fence();
    out._m0[_10] = _39[_10];
}
"""
PLAIN = """#include <metal_stdlib>
using namespace metal;
struct B { uint _m0[1]; };
kernel void main0(device B& out [[buffer(1)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    uint _10 = gl_LocalInvocationID.x;
    out._m0[_10] = _10;
}
"""


def helper_body(src):
    m = dmsh_rewrite.TG_FENCE_FN.search(src)
    return src[m.end():dmsh_rewrite.block_end(src, m.end() - 1)].strip()


class Rewrite(unittest.TestCase):
    def test_the_helper_becomes_a_simdgroup_fence(self):
        out = tg_fence(SCAN % 64)
        self.assertEqual(helper_body(out), FENCE)
        self.assertEqual(out.count('dmsh_tg_fence();'), 2)   # the calls stay where the translator put them

    def test_it_is_a_simdgroup_memory_fence_not_a_barrier(self):
        self.assertIn('thread_scope_simdgroup', FENCE)
        self.assertNotIn('barrier', FENCE)
        self.assertNotIn('threadgroup_barrier', tg_fence(SCAN % 64))

    def test_a_shader_without_the_helper_is_left_as_it_is(self):
        self.assertEqual(tg_fence(PLAIN), PLAIN)

    def test_spilled_arrays_get_device_fences(self):   # tg_spill runs after tg_fence: an array past abi.TG_MAX moves to device memory, and so must the fence's scope
        n = abi.TG_MAX // 4 + 1024
        src, spill = tg_spill(tg_fence(SCAN % n))
        self.assertGreater(spill, abi.TG_MAX)
        self.assertEqual(helper_body(src), 'atomic_thread_fence(mem_flags::mem_device | mem_flags::mem_threadgroup, memory_order_seq_cst, thread_scope_simdgroup);')

    @unittest.skipUnless(shutil.which('xcrun') and subprocess.run(['xcrun', '-sdk', 'macosx', '-f', 'metal'], capture_output=True).returncode == 0, 'no Metal compiler')
    def test_compiles(self):
        for src in (tg_fence(SCAN % 64), tg_spill(tg_fence(SCAN % (abi.TG_MAX // 4 + 1024)))[0]):
            with tempfile.TemporaryDirectory() as d:
                p = Path(d) / 'k.metal'
                p.write_text(src)
                r = subprocess.run(MAC_METAL + [str(p), '-o', str(Path(d) / 'k.air')], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr[-2000:])


def mov(rd, value): return (0x000fc60000000f00, value << 32 | rd << 16 | 0x7802)   # MOV Rd, imm32


STS = (0x000fe80000000800, 0x0000000302027388)   # STS.U32 [R2], R3
LDS = (0x000fe20000000800, 0x00000000030c7984)   # LDS R12, [R3]: SUATOM (WORK) then keeps R12
BAR = bits((0, 12, 0xb1d), (12, 3, PT))          # BAR.SYNC 0
SETUP = [mov(2, 8), mov(3, 4)]


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class Placement(unittest.TestCase):
    def fences(self, program):   # the kernel's dmsh_tg_fence() calls, and its body from the store of shared memory on
        msl = cf.ControlFlow.translate(self, SETUP + program)
        body = msl[msl.index('kernel void main0'):]
        return body.count('dmsh_tg_fence();'), body

    def test_the_downsweep_gets_a_fence_before_each_load(self):   # STS / LDS / STS / LDS, Orbitals' shape: both loads read what the lanes stored just before
        n, body = self.fences([STS, LDS, WORK, STS, LDS, WORK, EXIT])
        self.assertEqual(n, 2)
        for at in [m.start() for m in re.finditer(r'dmsh_tg_fence\(\);', body)]:
            self.assertRegex(body[at:].split('\n')[1], r'_\d+\[')   # the next statement is the load

    def test_a_barrier_between_store_and_load_needs_no_fence(self):
        self.assertEqual(self.fences([STS, BAR, LDS, WORK, EXIT])[0], 0)

    def test_a_run_of_stores_takes_one_fence_before_the_load(self):
        self.assertEqual(self.fences([STS, STS, STS, LDS, WORK, EXIT])[0], 1)

    def test_stores_at_the_end_take_none(self):   # nothing reads after the kernel; a task or mesh shader's wrapper has a barrier after the body
        n, body = self.fences([LDS, WORK, STS, STS, EXIT])
        self.assertEqual(n, 0)
        self.assertNotIn('dmsh_tg_fence', body)

    def test_a_store_at_the_bottom_of_a_loop_reaches_the_load_at_its_top(self):   # top: LDS; work; STS; BRA.P0 top: the next iteration's load follows the store
        n, _ = self.fences([LDS, WORK, STS, bra(5, 2, pred=0), EXIT])
        self.assertEqual(n, 1)

    def test_a_barrier_inside_the_loop_covers_the_back_edge(self):   # top: LDS; work; STS; BAR; BRA.P0 top
        self.assertEqual(self.fences([LDS, WORK, STS, BAR, bra(6, 2, pred=0), EXIT])[0], 0)

    def test_a_store_on_one_path_reaches_the_load_after_the_join(self):   # BRA.P0 past the store; STS; join: LDS
        self.assertEqual(self.fences([bra(2, 4, pred=0), STS, WORK, LDS, WORK, EXIT])[0], 1)

    def test_a_chain_of_hmmas_takes_one_fence_before_its_first(self):
        # LDSM; STS; HMMA; HMMA: the first HMMA reads A from shared memory where the LDSM read it (dmsh_rewrite.hmma_msl, FlagADirect), so it follows the store; the second
        # stores only to its own scratch, so nothing is pending for the next. Each HMMA of Ocarina of Time's movie networks took a fence when a call counted as a store
        import test_dmsh_hmma as h
        t = h.TranslatorFlags
        code = b''.join(h.inst(*h.waiting(i)) for i in (t.S2R, t.IMAD, t.LDSM, t.STS, t.HMMA0, t.HMMA1, t.STS, t.STS1)) + h.inst(*t.EXIT)
        with tempfile.TemporaryDirectory(prefix='tg_fence_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(code)
            job = [dict(key='k', stage='cs', arch='sm86', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=True, nvn2_cs=[0, 0, 3, 0x40, 0], shared=0x1000)]
            (work / 'jobs.json').write_text(json.dumps(job))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            import dmsh_geometry
            dmsh_geometry.flatten_sets(work / 'k.spv')
            r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
        body = r.stdout[r.stdout.index('kernel void main0'):]
        self.assertEqual(body.count('dmsh_hmma16816_f16('), 2)
        self.assertEqual(body.count('dmsh_tg_fence();'), 1)
        self.assertLess(body.index('dmsh_tg_fence();'), body.index('dmsh_hmma16816_f16('))


if __name__ == '__main__':
    unittest.main()
