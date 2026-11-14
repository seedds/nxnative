#!/usr/bin/env python3
"""dmsh_rewrite.py membar_fence: the device-scope threadgroup_barrier SPIRV-Cross prints for OpMemoryBarrier (MEMBAR) becomes a device-scope fence, a MEMBAR.SC + ERRBAR pair
becomes one, and a real barrier (BAR.SYNC: threadgroup_barrier(mem_flags::mem_threadgroup)) stays. With DMSH_TEST_METAL=1 the result also compiles with the Metal compiler.
Adapted from uncraftablexd's test in the PRAGMATA bring-up (PR #1407)."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_rewrite
membar_fence, MEMBAR = dmsh_rewrite.membar_fence, dmsh_rewrite._MEMBAR
FENCE = 'atomic_thread_fence(mem_flags::mem_device | mem_flags::mem_threadgroup | mem_flags::mem_texture, memory_order_seq_cst, thread_scope_device);'
BAR = 'threadgroup_barrier(mem_flags::mem_threadgroup);'

# The shape of PRAGMATA's culling queue 12a52b683eec95c1: a persistent loop polling a done flag, MEMBAR.SC.GPU; ERRBAR at the end of its body, no BAR.SYNC
QUEUE = """#include <metal_stdlib>
using namespace metal;
struct B { uint _m0[1024]; };
struct spvDescriptorSetBuffer0 { device B* cp_s0 [[id(0)]]; };

kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    do
    {
        uint _7 = atomic_fetch_or_explicit((device atomic_uint*)&(*spvDescriptorSet0.cp_s0)._m0[512], 0u, memory_order_relaxed);
        if (_7 != 0u)
        {
            break;
        }
        (*spvDescriptorSet0.cp_s0)._m0[gl_LocalInvocationID.x] = _7;
        MEMBAR_LINE
        MEMBAR_LINE
    } while (true);
    BAR_LINE
}
""".replace('MEMBAR_LINE', MEMBAR).replace('BAR_LINE', BAR)


class MembarFence(unittest.TestCase):
    def test_a_membar_becomes_a_device_fence(self):
        out = membar_fence(QUEUE)
        self.assertNotIn(MEMBAR, out)
        self.assertEqual(out.count(FENCE), 1)   # MEMBAR.SC and ERRBAR's pair: one fence
        self.assertIn(BAR, out)   # BAR.SYNC stays a barrier

    def test_a_shader_without_one_is_unchanged(self):
        src = QUEUE.replace(MEMBAR, '')
        self.assertEqual(membar_fence(src), src)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_METAL') and shutil.which('xcrun'), 'set DMSH_TEST_METAL=1 to compile with the Metal compiler')
    def test_the_fence_compiles(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'q.metal'; p.write_text(membar_fence(QUEUE))
            r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', str(p), '-o', str(Path(d) / 'q.air')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == '__main__':
    unittest.main()
