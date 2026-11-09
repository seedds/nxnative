#!/usr/bin/env python3
"""dmsh_build.py loop_guard: every loop ends after LOOP_CAP iterations; a loop with a barrier in its body counts on a counter of its own, so the cap ends it on the same iteration
for every thread of a threadgroup (the shared counter is per thread, and the barrier-free loops around it give the threads different counts). With DMSH_TEST_METAL=1 the guarded
kernels also compile with the Metal compiler."""
from pathlib import Path
import ast
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_meshstage as ms
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
selected = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name == 'loop_guard')
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) in ('LOOP_CAP', '_LOOP_HEAD', '_BARRIER') for t in n.targets))]
scope = {'re': re}
exec(compile(ast.Module(body=selected, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
loop_guard, CAP = scope['loop_guard'], scope['LOOP_CAP']

HEAD = """#include <metal_stdlib>
using namespace metal;
struct B { uint _m0[64]; };
struct spvDescriptorSetBuffer0 { device B* cp_s0 [[id(0)]]; };
"""
# The shape of PRAGMATA's cloth solver 6f364b25e417234a: a stride-256 copy loop, then do { batch loop { per-thread loop } barrier } while (--n != 0) with barriers
SOLVER = HEAD + """
kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    threadgroup uint _46[256];
    int _10 = int(gl_LocalInvocationID.x);
    uint _12 = (*spvDescriptorSet0.cp_s0)._m0[0];
    bool _14;
    do
    {
        _46[_10 & 255] = uint(_10);
        _10 += 256;
        _14 = uint(_10) < _12;
    } while (_14);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint _20 = (*spvDescriptorSet0.cp_s0)._m0[1];
    bool _30;
    do
    {
        _20 = _20 - 1u;
        for (int _22 = 0; _22 < int(_12); _22++)
        {
            int _24 = int(gl_LocalInvocationID.x);
            bool _26;
            do
            {
                _46[_24 & 255] += 1u;
                _24 += 256;
                _26 = uint(_24) < _12;
            } while (_26);
            threadgroup_barrier(mem_flags::mem_threadgroup);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        _30 = _20 != 0u;
    } while (_30);
    for (;;)
    {
        if (_46[0] != 0u)
        {
            break;
        }
    }
    (*spvDescriptorSet0.cp_s0)._m0[2] = _46[0];
}
"""
TWO_FUNCTIONS = HEAD + """
static inline __attribute__((always_inline))
uint _5(thread uint& _6)
{
    while (_6 > 3u)
    {
        _6 >>= 1u;
    }
    return _6;
}

kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    uint _8 = (*spvDescriptorSet0.cp_s0)._m0[0];
    while (_8 != 0u)
    {
        _8 = _8 - 1u;
        simdgroup_barrier(mem_flags::mem_none);
    }
    (*spvDescriptorSet0.cp_s0)._m0[1] = _5(_8);
}
"""
# A task shader as the translator leaves it for dmsh_meshstage.py: its stage memory first in the kernel, a loop with a barrier (Ocarina of Time's 520 task and mesh shaders all have
# the stage memory; the loop guard's declarations go in ahead of it)
TASK = HEAD.replace('struct B', """template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    thread T& operator [] (size_t pos) thread { return elements[pos]; }
    threadgroup T& operator [] (size_t pos) threadgroup { return elements[pos]; }
};
struct B""") + """
kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    threadgroup spvUnsafeArray<uint, %d> _23;
    uint _8 = (*spvDescriptorSet0.cp_s0)._m0[0];
    while (_8 != 0u)
    {
        _8 = _8 - 1u;
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    _23[1] = _8;
}
"""


def lines_with(src, text):
    return [l.strip() for l in src.split('\n') if text in l]


class LoopGuard(unittest.TestCase):
    def test_barrier_free_loops_share_the_function_counter(self):
        out = loop_guard(SOLVER)
        self.assertIn(f'}} while ((_14) && ++dm_lg < {CAP}u);', out)        # the copy loop
        self.assertIn(f'}} while ((_26) && ++dm_lg < {CAP}u);', out)        # the per-thread loop inside the batch loop
        self.assertIn(f'for (; ++dm_lg < {CAP}u;)', out)                    # the barrier-free for (;;) after them

    def test_a_barrier_loop_counts_on_its_own(self):
        out = loop_guard(SOLVER)
        self.assertIn(f'}} while ((_30) && ++dm_lg1 < {CAP}u);', out)       # the solver loop: barriers at two depths
        self.assertIn(f'for (int _22 = 0; (_22 < int(_12)) && ++dm_lg2 < {CAP}u; _22++)', out)   # the batch loop: a barrier after its per-thread loop
        self.assertEqual(lines_with(out, 'uint dm_lg'), ['uint dm_lg = 0u;', 'uint dm_lg1 = 0u;', 'uint dm_lg2 = 0u;'])

    def test_no_barrier_loop_still_has_one_counter(self):   # the MSL of a shader without barriers in loops is what it was (the Metal compile cache keeps its libraries)
        src = SOLVER.replace('            threadgroup_barrier(mem_flags::mem_threadgroup);\n', '').replace('        threadgroup_barrier(mem_flags::mem_threadgroup);\n        _30', '        _30')
        out = loop_guard(src)
        self.assertNotRegex(out, r'dm_lg\d')
        self.assertEqual(out.count('++dm_lg <'), 5)

    def test_counters_are_declared_in_their_function(self):
        out = loop_guard(TWO_FUNCTIONS)
        helper, kernel = out.split('kernel void main0')
        self.assertIn(f'while ((_6 > 3u) && ++dm_lg < {CAP}u)', helper)
        self.assertNotRegex(helper, r'dm_lg\d')
        self.assertIn(f'while ((_8 != 0u) && ++dm_lg1 < {CAP}u)', kernel)
        self.assertIn('uint dm_lg = 0u;\n    (void)dm_lg;\n    uint dm_lg1 = 0u;\n    (void)dm_lg1;', kernel)

    def test_the_barrier_loop_ends_on_the_same_iteration_for_every_thread(self):
        # Thread t runs the stride-256 loops ceil((n - t) / 256) times. With one shared counter a thread with fewer per-thread iterations reaches the cap later, so it was still
        # in the solver loop (and at its barrier) when the others had left it. Count the solver loop's exits per thread for a solver count of 0 (it wraps: only the cap ends it)
        def exits(solver, batch, n=300):
            seen = set()
            for t in (0, 43, 44, 255):
                counters = {'dm_lg': 0, solver: 0, batch: 0}
                def tick(name): counters[name] += 1; return counters[name] < CAP
                i = t
                while True:   # the copy loop
                    i += 256
                    if not (i < n and tick('dm_lg')): break
                k = 0
                while True:   # the solver loop: one batch (its per-thread loop, then the batch loop's guard), then its own guard
                    k += 1
                    j = t
                    while True:
                        j += 256
                        if not (j < n and tick('dm_lg')): break
                    tick(batch)
                    if not tick(solver): break
                seen.add(k)
            return seen
        out = loop_guard(SOLVER)
        solver = re.search(r'\} while \(\(_30\) && \+\+(dm_lg\d*) < ', out).group(1)
        batch = re.search(r'for \(int _22 = 0; .* && \+\+(dm_lg\d*) < ', out).group(1)
        self.assertGreater(len(exits('dm_lg', 'dm_lg')), 1)   # one shared counter: the threads leave the barrier loop at different iterations
        self.assertEqual(len(exits(solver, batch)), 1)

    def task_stage(self):
        c = bytearray(0x80); c[0x56] = 6; struct.pack_into('<2H', c, 0x58, 32, 256)   # control: a task shader, 32 threads, a 256-byte payload
        i = ms.info(bytes(c), bytes(0x80))
        return ms.to_stage(loop_guard(TASK % (ms.memory(i)[1] // 4)), i)

    def test_a_guarded_task_shader_still_wraps_as_a_stage(self):   # dmsh_meshstage.py _KERNEL reads the kernel's leading declarations a statement a line
        src = self.task_stage()
        self.assertIn('[[object]] void main0(', src)
        body = src[src.index('void dm_stage_body('):src.index('[[object]]')]
        self.assertIn('uint dm_lg1 = 0u;', body)
        self.assertIn(f'while ((_8 != 0u) && ++dm_lg1 < {CAP}u)', body)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_METAL') and shutil.which('xcrun'), 'set DMSH_TEST_METAL=1 to compile with the Metal compiler')
    def test_guarded_kernels_compile(self):
        with tempfile.TemporaryDirectory() as d:
            for name, src in (('solver', loop_guard(SOLVER)), ('two', loop_guard(TWO_FUNCTIONS)), ('task', self.task_stage())):
                p = Path(d) / f'{name}.metal'; p.write_text(src)
                r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', str(p), '-o', str(p.with_suffix('.air'))], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == '__main__':
    unittest.main()
