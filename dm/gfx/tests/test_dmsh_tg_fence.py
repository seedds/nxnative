"""Warp-synchronous shared memory (dmsh_rewrite.py tg_fence): Ampere compilers drop the barrier in small scans whose lanes run in lockstep (Unreal's GPU radix sort downsweep,
Orbitals' d032ce5090303e01: STS, then LDS of the neighbouring lane's word, then STS). For one thread such a load and its own earlier store do not alias, so Metal may move the
load above the store. tg_fence puts a simdgroup-scope memory fence (not an execution barrier) after each threadgroup store. When the Metal compiler is installed, the result
is compiled as the build compiles it (dmsh_build.py MAC_METAL), and a kernel whose array spills to device memory (tg_spill, after tg_fence) gets fences that order device memory as well."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_abi as abi, dmsh_rewrite
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
# The downsweep's shape: each lane stores its word, reads its neighbour's, adds and stores again, with no barrier
SCAN = """#include <metal_stdlib>
using namespace metal;
""" + ARRAY + """
struct B { uint _m0[1]; };
kernel void main0(device B& out [[buffer(1)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    threadgroup spvUnsafeArray<uint, %d> _39;
    uint _10 = gl_LocalInvocationID.x;
    _39[_10] = out._m0[_10];
    uint _11 = _39[_10 - 1u] + _39[_10];
    _39[_10] = _11;
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


def lines_after_stores(src):
    ls = src.split('\n')
    return [(ls[i].strip(), ls[i + 1].strip()) for i in range(len(ls) - 1) if re.match(r'\s*_39\[[^\]]*\]\s*=(?!=)', ls[i])]


class TgFence(unittest.TestCase):
    def test_a_fence_follows_each_threadgroup_store(self):
        out = tg_fence(SCAN % 64)
        pairs = lines_after_stores(out)
        self.assertEqual(len(pairs), 2)
        for _, nxt in pairs:
            self.assertEqual(nxt, FENCE)
        self.assertEqual(out.count(FENCE), 2)   # not after the loads, nor after the device store

    def test_compound_and_member_stores_get_a_fence_too(self):   # SPIRV-Cross writes a read-modify-write of one element as a compound assignment
        src = SCAN.replace('    _39[_10] = _11;\n', '    _39[_10] += _11;\n    _39[_10 ^ 1u] <<= 1u;\n    _39[_10 + 2u].x = _11;\n    bool _12 = _39[_10] >= _11;\n    bool _13 = _39[_10] <= _11;\n') % 64
        out = tg_fence(src).split('\n')
        for store in ('_39[_10] += _11;', '_39[_10 ^ 1u] <<= 1u;', '_39[_10 + 2u].x = _11;'):
            k = next(i for i, ln in enumerate(out) if ln.strip() == store)
            self.assertEqual(out[k + 1].strip(), FENCE, store)
        for compare in ('bool _12', 'bool _13'):   # comparisons are no stores
            k = next(i for i, ln in enumerate(out) if ln.strip().startswith(compare))
            self.assertNotEqual(out[k + 1].strip(), FENCE, compare)

    def test_it_is_a_simdgroup_memory_fence_not_a_barrier(self):
        self.assertIn('thread_scope_simdgroup', FENCE)
        self.assertNotIn('barrier', FENCE)
        self.assertNotIn('threadgroup_barrier', tg_fence(SCAN % 64))

    def test_a_kernel_without_threadgroup_memory_is_left_as_it_is(self):
        self.assertEqual(tg_fence(PLAIN), PLAIN)

    def test_spilled_arrays_get_device_fences(self):   # tg_spill runs after tg_fence: an array past abi.TG_MAX moves to device memory, and so must the fence's scope
        n = abi.TG_MAX // 4 + 1024
        src, spill = tg_spill(tg_fence(SCAN % n))
        self.assertGreater(spill, abi.TG_MAX)
        self.assertEqual(src.count('mem_flags::mem_device | mem_flags::mem_threadgroup, memory_order_seq_cst, thread_scope_simdgroup'), 2)

    @unittest.skipUnless(shutil.which('xcrun') and subprocess.run(['xcrun', '-f', 'metal'], capture_output=True).returncode == 0, 'no Metal compiler')
    def test_compiles(self):
        for src in (tg_fence(SCAN % 64), tg_spill(tg_fence(SCAN % (abi.TG_MAX // 4 + 1024)))[0]):
            with tempfile.TemporaryDirectory() as d:
                p = Path(d) / 'k.metal'
                p.write_text(src)
                r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', str(p), '-o', str(Path(d) / 'k.air')], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr[-2000:])


if __name__ == '__main__':
    unittest.main()
