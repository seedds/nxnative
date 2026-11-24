"""Compute containment: helper propagation, barrier exclusion, attributable hits and legitimate cumulative budgets. GPU inputs are bounded even without containment."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_abi as abi   # noqa: E402
import dmsh_rewrite as rewrite   # noqa: E402

HEADER = '#include <metal_stdlib>\nusing namespace metal;\n'
ARGS = 'struct Args { device uint* buf [[id(0)]]; };\n'
SPIN = HEADER + ARGS + '''
static inline uint wait_flag(device uint* q)
{
    uint i = 0u;
    do
    {
        if (atomic_load_explicit((device atomic_uint*)q, memory_order_relaxed)) break;
        atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);
        i++;
    } while (i < 65536u);
    return i;
}
static inline uint wrapper(device uint* q)
{
    return wait_flag(q);
}
kernel void main0(constant Args& a [[buffer(0)]])
{
    uint i = wrapper(a.buf);
    atomic_fetch_add_explicit((device atomic_uint*)&a.buf[1], 1u, memory_order_relaxed);
    atomic_fetch_max_explicit((device atomic_uint*)&a.buf[2], i, memory_order_relaxed);
    atomic_fetch_min_explicit((device atomic_uint*)&a.buf[3], i, memory_order_relaxed);
}
'''
HEALTHY = HEADER + ARGS + '''
kernel void main0(constant Args& a [[buffer(0)]], uint tid [[thread_index_in_threadgroup]])
{
    if (tid != 0u) return;
    uint bound = a.buf[0] == 2u ? 512u : 256u;
    uint sum = 0u;
    for (uint i = 0u; i < bound; i++)
    {
        for (uint j = 0u; j < bound; j++)
        {
            sum++;
        }
    }
    a.buf[4] = sum;
}
'''
BARRIER = HEADER + ARGS + '''
static inline void arrive()
{
    threadgroup_barrier(mem_flags::mem_threadgroup);
}
kernel void main0(constant Args& a [[buffer(0)]], uint tid [[thread_index_in_threadgroup]])
{
    uint count = 0u;
    for (uint i = 0u; i < 300u + tid; i++)
    {
        atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);
    }
    for (uint i = 0u; i < 2048u; i++)
    {
        arrive();
        count++;
    }
    a.buf[8u + tid] = count;
}
'''


class ComputeLoops(unittest.TestCase):
    def test_helpers_and_loop_free_wrappers_carry_state(self):
        out, sites = rewrite.compute_loops(SPIN)
        self.assertEqual([(s['site'], s['function'], s['barrier']) for s in sites], [(1, 'wait_flag', False)])
        self.assertIn('wrapper(device uint* q, device dm_loop_state& dm_loop)', out)
        self.assertIn('wait_flag(q, dm_loop)', out)
        self.assertIn('wrapper(a.buf, dm_loop)', out)
        self.assertEqual(out.count('uint dm_lg = 0u;'), 1)

    def test_all_loop_forms_and_nested_calls_in_conditions(self):
        src = SPIN.replace('uint i = wrapper(a.buf);', '''uint i = 0u;
    while (wrapper(a.buf) != 0u)
    {
        i++;
    }
    for (;;)
    {
        break;
    }''')
        out, sites = rewrite.compute_loops(src)
        self.assertEqual(len(sites), 3)
        self.assertIn('while ((wrapper(a.buf, dm_loop) != 0u) && dm_lg_go', out)
        self.assertIn('for (;(true) && dm_lg_go', out)
        self.assertIn('} while ((i < 65536u) && dm_lg_go', out)

    def test_barriers_through_helpers_disable_only_abort_reads(self):
        src = SPIN.replace('uint i = 0u;', 'uint i = 0u;\n    threadgroup_barrier(mem_flags::mem_threadgroup);', 1)
        src = src.replace('uint i = wrapper(a.buf);', 'uint i = 0u;\n    while (i < 8u)\n    {\n        i += wrapper(a.buf);\n    }')
        out, sites = rewrite.compute_loops(src)
        self.assertFalse(sites[0]['barrier'])   # barrier outside wait_flag's loop
        self.assertTrue(sites[1]['barrier'])    # main0's loop calls it through wrapper
        self.assertIn('while ((i < 8u) && dm_lg_cap(dm_lg_b2', out)
        self.assertIn('{ uint dm_lg_b2 = 0u; while', out)

    def test_barrier_budgets_reset_at_each_loop_entry(self):
        src = BARRIER.replace('for (uint i = 0u; i < 2048u; i++)', 'do').replace('    a.buf[8u + tid] = count;', '    while (count < 2048u);\n    a.buf[8u + tid] = count;')
        out, sites = rewrite.compute_loops(src)
        self.assertEqual([s['barrier'] for s in sites], [False, True])
        self.assertIn('{ uint dm_lg_b2 = 0u; do', out)
        self.assertIn('while ((count < 2048u) && dm_lg_cap(dm_lg_b2', out)
        self.assertIn('/* dm_loop_site 2: main0 */); }', out)

    def test_barrier_calls_in_loop_headers_get_private_budgets(self):
        src = BARRIER.replace('i < 2048u; i++', '(arrive(), i < 2048u); i++').replace('        arrive();', '')
        out, sites = rewrite.compute_loops(src)
        self.assertEqual([s['barrier'] for s in sites], [False, True])
        self.assertIn('dm_lg_cap(dm_lg_b2', out)

    def test_other_stages_and_loop_free_compute_are_unchanged(self):
        for src in (HEALTHY.replace('kernel void', 'fragment void'), HEADER + 'kernel void main0()\n{\n}\n'):
            self.assertEqual(rewrite.compute_loops(src), (src, []))

    def test_reserved_slot_and_unknown_loop_fail(self):
        with self.assertRaisesRegex(ValueError, 'taken'):
            rewrite.compute_loops(SPIN.replace('buffer(0)', f'buffer({abi.LOOP_SLOT})'))
        with self.assertRaisesRegex(ValueError, 'unknown loop'):
            rewrite.compute_loops(HEALTHY.replace('for (uint i = 0u; i < bound; i++)', 'for (uint i : bound)'))

    def test_compact_matrix_helper_loops_and_comments(self):
        src = SPIN.replace('uint i = 0u;', 'uint i = 0u;\n    // for (;;) is a comment\n    for (uint j=0u; j<2u; j++) { i += j; }', 1)
        out, sites = rewrite.compute_loops(src)
        self.assertEqual(len(sites), 2)
        self.assertIn('for (uint j=0u;(j<2u) && dm_lg_go', out)
        self.assertIn('// for (;;) is a comment', out)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') == '1' and shutil.which('xcrun'), 'set DMSH_TEST_GPU=1')
    def test_gpu_host_attribution_and_legitimate_budgets(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            def build(name, src, cap):
                with patch.object(rewrite, 'LOOP_CAP', cap): out, _ = rewrite.compute_loops(src)
                source, air, lib = (root / f'{name}.{ext}' for ext in ('metal', 'air', 'metallib'))
                source.write_text(out)
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-std=metal3.2', '-c', '-O1', f'-fmodules-cache-path={root / "modules"}', str(source), '-o', str(air)], check=True, capture_output=True)
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(air), '-o', str(lib)], check=True, capture_output=True)
                return lib
            spin, healthy, barrier = build('spin', SPIN, 1 << 14), build('healthy', HEALTHY, rewrite.LOOP_CAP), build('barrier', BARRIER, 2048)
            subprocess.run(['make', '-C', str(TOOLS.parent / 'tests'), 'GAME=040041E0212AE000', 't_compute_loops'], check=True, capture_output=True)
            for thread in ('0', '1'):
                env = dict(os.environ, DM_NVN_ENC_THREAD=thread, DM_NVN_DISPATCH_BARRIER='full', MTL_DEBUG_LAYER='1')
                run = subprocess.run([str(TOOLS.parent / 'tests/t_compute_loops'), str(spin), str(healthy), str(barrier)], env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                log = run.stdout + run.stderr
                self.assertEqual(log.count('[RED] loop cap hit:'), 3, log)
                self.assertIn('kernel 1111111111111111, loop site 1', log)
                self.assertIn('kernel 2222222222222222, loop site 1', log)
                self.assertIn('kernel 3333333333333333, loop site 2', log)
                print(log)


if __name__ == '__main__': unittest.main()
