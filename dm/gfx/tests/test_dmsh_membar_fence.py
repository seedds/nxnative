#!/usr/bin/env python3
"""dmsh_build.py membar_fence: the device-scope threadgroup_barrier SPIRV-Cross prints for OpMemoryBarrier (MEMBAR) becomes a device-scope fence, a MEMBAR.SC + ERRBAR pair
becomes one, a real barrier (BAR.SYNC: threadgroup_barrier(mem_flags::mem_threadgroup)) stays, and loop_guard afterwards gives a loop whose only "barrier" was a MEMBAR no counter
of its own. With DMSH_TEST_METAL=1 the results also compile with the Metal compiler."""
from pathlib import Path
import ast
import os
import re
import shutil
import subprocess
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
selected = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name in ('membar_fence', 'loop_guard'))
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) in ('_MEMBAR', 'LOOP_CAP', '_LOOP_HEAD', '_BARRIER') for t in n.targets))]
scope = {'re': re}
exec(compile(ast.Module(body=selected, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
membar_fence, loop_guard, MEMBAR = scope['membar_fence'], scope['loop_guard'], scope['_MEMBAR']
FENCE = 'atomic_thread_fence(mem_flags::mem_device | mem_flags::mem_threadgroup | mem_flags::mem_texture, memory_order_seq_cst, thread_scope_device);'
BAR = 'threadgroup_barrier(mem_flags::mem_threadgroup);'

HEAD = """#include <metal_stdlib>
using namespace metal;
struct B { uint _m0[1024]; };
struct spvDescriptorSetBuffer0 { device B* cp_s0 [[id(0)]]; };
"""
# The shape of PRAGMATA's culling queue 12a52b683eec95c1: a persistent loop polling a done flag, with MEMBAR.SC.GPU; ERRBAR at the end of its body
QUEUE = HEAD + """
kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    uint _5 = 0u;
    do
    {
        uint _7 = atomic_fetch_or_explicit((device atomic_uint*)&(*spvDescriptorSet0.cp_s0)._m0[512], 0u, memory_order_relaxed);
        if (_7 != 0u)
        {
            break;
        }
        (*spvDescriptorSet0.cp_s0)._m0[gl_LocalInvocationID.x] = _5;
        _5++;
        threadgroup_barrier(mem_flags::mem_device | mem_flags::mem_threadgroup | mem_flags::mem_texture);
        threadgroup_barrier(mem_flags::mem_device | mem_flags::mem_threadgroup | mem_flags::mem_texture);
    } while (true);
}
"""
# A real barrier next to a MEMBAR, as in the 18 mixed kernels
MIXED = HEAD + """
kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    threadgroup uint _9[64];
    int _3 = 0;
    do
    {
        _9[gl_LocalInvocationID.x & 63u] = uint(_3);
        threadgroup_barrier(mem_flags::mem_threadgroup);
        (*spvDescriptorSet0.cp_s0)._m0[gl_LocalInvocationID.x] = _9[(gl_LocalInvocationID.x + 1u) & 63u];
        threadgroup_barrier(mem_flags::mem_device | mem_flags::mem_threadgroup | mem_flags::mem_texture);
        _3++;
    } while (_3 < 4);
}
"""


class MembarFence(unittest.TestCase):
    def test_pair_becomes_one_fence(self):
        out = membar_fence(QUEUE)
        self.assertNotIn(MEMBAR, out)
        self.assertEqual(out.count(FENCE), 1)

    def test_real_barrier_kept(self):
        out = membar_fence(MIXED)
        self.assertEqual(out.count(BAR), 1)
        self.assertEqual(out.count(FENCE), 1)
        self.assertNotIn(MEMBAR, out)

    def test_unchanged_without_membar(self):
        src = HEAD + 'kernel void main0() { }\n'
        self.assertIs(membar_fence(src), src)

    def test_loop_guard_after_it(self):
        # the queue's loop has no barrier left: the shared counter, no counter of its own
        out = loop_guard(membar_fence(QUEUE))
        self.assertNotIn('dm_lg1', out)
        self.assertIn('++dm_lg <', out)
        # a loop with a real barrier still gets its own
        self.assertIn('dm_lg1', loop_guard(membar_fence(MIXED)))

    @unittest.skipUnless(os.environ.get('DMSH_TEST_METAL') == '1' and shutil.which('xcrun'), 'DMSH_TEST_METAL=1 compiles with the Metal compiler')
    def test_metal_compiles(self):
        with tempfile.TemporaryDirectory() as d:
            for name, src in (('queue', QUEUE), ('mixed', MIXED)):
                p = Path(d) / (name + '.metal'); p.write_text(loop_guard(membar_fence(src)))
                r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', str(p), '-o', str(Path(d) / (name + '.air'))], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == '__main__':
    unittest.main()
