"""Loop abort (dmsh_rewrite.py loop_abort, on top of loop_guard): in a compute kernel's entry function the first thread at loop_guard's cap raises the dispatch's abort word
at compute buffer DMSH_ABORT_SLOT (one for each submission), and every capped loop reads it every 1,024 iterations and ends once it is raised. Each loop form gets the check, a loop with a barrier
(in its body or a helper it calls) only raises the word, helpers and other stages keep loop_guard's cap alone. With DMSH_TEST_GPU=1, on this Mac's GPU: a dispatch of many
workgroups spinning on a flag nobody writes, and PRAGMATA's shape (several 128-workgroup dispatches in one submission), end far sooner than with every thread running to
its own cap, and the next submission, with its own word, runs its loops in full.
The cap is set small for the test, and the spin has a bound of its own besides, so no kernel here can run unbounded, with or without the rewrite."""
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_abi as abi   # noqa: E402
import dmsh_rewrite   # noqa: E402
from dmsh_rewrite import LOOP_CAP, loop_abort, loop_guard   # noqa: E402

GO, CAP = 'dm_lg_go(dm_lg, dm_abort)', 'dm_lg_cap(dm_lg, dm_abort)'

# The loop forms of spirv-cross's MSL, a helper with a loop and a helper with a barrier (a loop calling it only raises the word)
FORMS = '''#include <metal_stdlib>
using namespace metal;
struct S
{
    uint n;
};

static inline uint helper(uint v)
{
    uint r = 0u;
    for (uint i = 0u; i < v; i++)
    {
        r += i;
    }
    return r;
}

static inline __attribute__((always_inline)) void sync_all(threadgroup uint& t)
{
    t = 0u;
    threadgroup_barrier(mem_flags::mem_threadgroup);
}

kernel void main0(device uint* out [[buffer(0)]], constant S& s [[buffer(1)]])
{
    threadgroup uint t;
    uint a = 0u;
    do
    {
        a++;
    } while (a < s.n);
    uint b = 0u;
    while (b < s.n)
    {
        b++;
    }
    for (;;)
    {
        break;
    }
    for (uint i = 0u; i < s.n; i++)
    {
        out[i] = helper(i);
    }
    for (uint j = 0u; j < s.n; j++)
    {
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    uint c = 0u;
    while (c < s.n)
    {
        sync_all(t);
        c++;
    }
    out[0] = a + b;
}
'''

# A spin on a flag nobody writes (q[0]), as PRAGMATA's persistent queue kernels wait on their counter, with a bound of its own (s.bound) besides the cap.
# q[1] counts the threads that left, q[2] keeps the most iterations a thread made and q[3] the fewest
SPIN = '''#include <metal_stdlib>
using namespace metal;
struct S
{
    uint bound;
};

kernel void main0(device atomic_uint* q [[buffer(0)]], constant S& s [[buffer(1)]])
{
    uint i = 0u;
    uint v = 0u;
    do
    {
        v = atomic_load_explicit(&q[0], memory_order_relaxed);
        atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);
        i++;
    } while ((v == 0u) && (i < s.bound));
    atomic_fetch_add_explicit(&q[1], 1u, memory_order_relaxed);
    atomic_fetch_max_explicit(&q[2], i, memory_order_relaxed);
    atomic_fetch_min_explicit(&q[3], i, memory_order_relaxed);
}
'''
TEST_CAP = 1 << 14     # loop_guard's cap for the GPU case (the shipped one is 2^18)
# (workgroups of 32 threads, runaway dispatches in the submission): many workgroups in one dispatch, far more than the GPU runs at once, so they start in waves; and
# PRAGMATA's shape, its queue kernels' 128 workgroups, several dispatches committed behind each other
SHAPES = ((16384, 1), (128, 8))
HEALTHY = 5000         # iterations the next submission's loop runs: past 1,024, so a word left raised would cut it short
TRIALS = 5

# run <metallib> <groups> <dispatches> <healthy> → "A <GPU ms> <threads out> <most> <fewest> <word>  B <threads out> <most> <fewest>". Submission A: <dispatches>
# dispatches spinning on the flag until the cap (their own bound 2^22 is never reached), sharing the submission's zeroed word at DMSH_ABORT_SLOT, as nvn_mtl.m dispatch
# binds it. Submission B, the next command buffer with a word of its own: one workgroup whose loop ends by itself after <healthy> iterations
RUNNER = r"""#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
int main(int argc, char **argv) { @autoreleasepool {
    if (argc < 5) return 2;
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); if (!d) return 2;
    NSError *e = nil; id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e]; if (!lib) { fprintf(stderr, "%s\n", e.description.UTF8String); return 1; }
    id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"main0"] error:&e]; if (!ps) { fprintf(stderr, "%s\n", e.description.UTF8String); return 1; }
    uint32_t groups = (uint32_t)strtoul(argv[2], NULL, 0), runs = (uint32_t)strtoul(argv[3], NULL, 0), bounds[2] = {1u << 22, (uint32_t)strtoul(argv[4], NULL, 0)};
    id<MTLCommandQueue> cq = [d newCommandQueue]; id<MTLBuffer> q[2], s[2], w[2]; id<MTLCommandBuffer> cb[2];
    for (int k = 0; k < 2; k++) {
        q[k] = [d newBufferWithLength:16 options:MTLResourceStorageModeShared]; s[k] = [d newBufferWithLength:4 options:MTLResourceStorageModeShared]; w[k] = [d newBufferWithLength:16 options:MTLResourceStorageModeShared];
        memset(q[k].contents, 0, 16); ((uint32_t *)q[k].contents)[3] = ~0u; *(uint32_t *)s[k].contents = bounds[k]; memset(w[k].contents, 0, 16);
        cb[k] = [cq commandBuffer];
        for (uint32_t r = 0; r < (k ? 1 : runs); r++) {
            id<MTLComputeCommandEncoder> ce = [cb[k] computeCommandEncoder];
            [ce setComputePipelineState:ps]; [ce setBuffer:q[k] offset:0 atIndex:0]; [ce setBuffer:s[k] offset:0 atIndex:1]; [ce setBuffer:w[k] offset:0 atIndex:ABORT_SLOT];
            [ce dispatchThreadgroups:MTLSizeMake(k ? 1 : groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)]; [ce endEncoding];
        }
    }
    [cb[0] commit]; [cb[1] commit]; [cb[1] waitUntilCompleted];
    if (cb[0].status != MTLCommandBufferStatusCompleted || cb[1].status != MTLCommandBufferStatusCompleted) return 1;
    uint32_t *a = q[0].contents, *b = q[1].contents;
    printf("A %.2f %u %u %u %u  B %u %u %u\n", (cb[0].GPUEndTime - cb[0].GPUStartTime) * 1e3, a[1], a[2], a[3], *(uint32_t *)w[0].contents, b[1], b[2], b[3]);
    return 0;
} }
""".replace('ABORT_SLOT', str(abi.ABORT_SLOT))


def guarded(src, cap=LOOP_CAP):   # as dmsh_build.py makes a compute kernel: loop_guard on every stage, loop_abort in the compute branch
    old = dmsh_rewrite.LOOP_CAP
    dmsh_rewrite.LOOP_CAP = cap
    try: return loop_guard(src)
    finally: dmsh_rewrite.LOOP_CAP = old


class LoopAbort(unittest.TestCase):
    def test_every_capped_loop_form_reads_the_word(self):
        msl = loop_abort(guarded(FORMS))
        self.assertIn(f'    }} while ((a < s.n) && {GO});', msl)
        self.assertIn(f'    while ((b < s.n) && {GO})', msl)
        self.assertIn(f'    for (; {GO};)', msl)
        self.assertIn(f'    for (uint i = 0u; (i < s.n) && {GO}; i++)', msl)
        self.assertIn(f'device atomic_uint* dm_abort [[buffer({abi.ABORT_SLOT})]])\n{{\n    uint dm_lg = 0u;\n', msl)   # loop_guard's counter stays alone on its line
        self.assertIn(f'if (++n < {LOOP_CAP}u) return true;', msl)   # the same cap as loop_guard's
        self.assertEqual(msl.count('bool dm_lg_go('), 1)

    def test_a_loop_with_a_barrier_only_raises_the_word(self):
        msl = loop_abort(guarded(FORMS))
        self.assertIn(f'    for (uint j = 0u; (j < s.n) && {CAP}; j++)\n', msl)   # threadgroup_barrier in its body
        self.assertIn(f'    while ((c < s.n) && {CAP})\n', msl)                   # through the helper sync_all
        self.assertIn(f'    for (uint i = 0u; (i < v) && ++dm_lg < {LOOP_CAP}u; i++)\n', msl)   # a helper's loop: loop_guard's cap alone

    def test_unchanged_where_it_does_not_apply(self):
        self.assertEqual(loop_abort(FORMS), FORMS)   # no loop_guard cap to extend
        src = guarded('kernel void main0(device uint* out [[buffer(0)]])\n{\n    out[0] = 1u;\n}\n')
        self.assertEqual(loop_abort(src), src)
        frag = guarded(FORMS.replace('kernel void main0', 'fragment void main0'))
        self.assertEqual(loop_abort(frag), frag)
        once = loop_abort(guarded(FORMS))
        self.assertEqual(loop_abort(once), once)

    def test_the_slot_must_be_free(self):
        with self.assertRaises(ValueError):
            loop_abort(guarded(FORMS.replace('constant S& s [[buffer(1)]]', f'constant S& s [[buffer({abi.ABORT_SLOT})]]')))

    def test_the_host_binds_a_word_for_every_submission(self):
        host = (TOOLS.parent / 'nvn/nvn_mtl.m').read_text()
        self.assertIn('enc_c_args(ce, on ? aw : 0, DMSH_ABORT_SLOT);', host)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') == '1' and shutil.which('xcrun') and shutil.which('clang'), 'set DMSH_TEST_GPU=1 (runs on the GPU)')
    def test_gpu_the_first_runaway_ends_the_dispatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            def build(name, src):
                (root / f'{name}.metal').write_text(src)
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O2', str(root / f'{name}.metal'), '-o', str(root / f'{name}.air')], check=True, capture_output=True)
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(root / f'{name}.air'), '-o', str(root / f'{name}.metallib')], check=True, capture_output=True)
                return str(root / f'{name}.metallib')
            plain = guarded(SPIN, TEST_CAP)   # loop_guard alone: what the kernel is without the change
            libs = {'with': build('with', loop_abort(plain)), 'without': build('without', plain)}
            (root / 'run.m').write_text(RUNNER)
            subprocess.run(['clang', '-fobjc-arc', '-O1', str(root / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(root / 'run')], check=True, capture_output=True)
            for groups, runs in SHAPES:
                ms, n = {'with': [], 'without': []}, groups * 32 * runs
                for _ in range(TRIALS):
                    for arm, lib in libs.items():
                        r = subprocess.run([str(root / 'run'), lib, str(groups), str(runs), str(HEALTHY)], capture_output=True, text=True, timeout=120)
                        if r.returncode == 2: self.skipTest('no Metal device')
                        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                        m = re.fullmatch(r'A ([\d.]+) (\d+) (\d+) (\d+) (\d+)  B (\d+) (\d+) (\d+)\s*', r.stdout)
                        assert m, r.stdout
                        t, out, most, fewest, word, b_out, b_most, b_fewest = float(m.group(1)), *map(int, m.groups()[1:])
                        ms[arm].append(t)
                        self.assertEqual((out, most), (n, TEST_CAP), r.stdout)   # every thread left, none past the cap
                        self.assertEqual((b_out, b_most, b_fewest), (32, HEALTHY, HEALTHY), r.stdout)   # the next submission runs its loops in full
                        if arm == 'with': self.assertEqual(word, 1, r.stdout); self.assertLess(fewest, TEST_CAP, r.stdout)   # raised, and threads left before their cap
                        else: self.assertEqual((word, fewest), (0, TEST_CAP), r.stdout)   # nothing reads or writes the word: every thread runs to its cap
                w, wo = statistics.median(ms['with']), statistics.median(ms['without'])
                print(f'\n{runs} dispatch(es) of {groups} x 32 threads, cap {TEST_CAP}: with {w:.1f} ms, without {wo:.1f} ms (GPU time, medians of {TRIALS}; '
                      f'with {", ".join(f"{x:.1f}" for x in ms["with"])}; without {", ".join(f"{x:.1f}" for x in ms["without"])})')
                self.assertLess(w * 4, wo, f'{groups} x {runs}: with {ms["with"]}, without {ms["without"]}')


if __name__ == '__main__':
    unittest.main()
