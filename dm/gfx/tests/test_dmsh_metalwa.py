#!/usr/bin/env python3
"""dmsh_metalwa.py: automatic Metal GPU compiler workarounds after KosmicKrisp.
KK3 rewrites spvSubgroupBallot as an always-inline helper using simd_or(uint(value) << lane) behind dm_mwa3_fc (DMSH_MWA_FC, default false; opt-in via DM_NVN_METALWA=1),
threading the lane ID by value (uint, never thread uint&, avoiding Apple's #2825 out-of-line scalarization bug). KK9 guards loop breaks in subgroup functions unconditionally.
With DMSH_TEST_METAL=1, compiles and runs t_metalwa.metal's bounded kernels on the GPU."""
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
import dmsh_metalwa as W   # noqa: E402

HEAD = """#pragma clang diagnostic ignored "-Wunused-variable"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct B { uint _m0[1024]; };
struct spvDescriptorSetBuffer0 { device B* cp_s0 [[id(0)]]; volatile device B* cp_s1 [[id(1)]]; };

inline uint4 spvSubgroupBallot(bool value)
{
    simd_vote vote = simd_ballot(value);
    return uint4(as_type<uint2>((simd_vote::vote_t)vote), 0, 0);
}

"""
QUEUE = HEAD + """static inline __attribute__((always_inline))
uint _7(thread const uint& _1)
{
    return spvSubgroupBallot(_1 != 0u).x;
}

kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    uint _3 = (*spvDescriptorSet0.cp_s0)._m0[gl_LocalInvocationID.x];
    if (_3 != 0u)
    {
        uint _5 = spvSubgroupBallot(true).x;
        if (simd_any(_5 > 1u))
        {
            atomic_fetch_add_explicit((device atomic_uint*)&(*spvDescriptorSet0.cp_s0)._m0[512], popcount(_5), memory_order_relaxed);
        }
    }
    uint _9 = _7(_3);
    for (int _11 = 0; _11 < 4; _11++)
    {
        if (simd_all(_9 == 0u))
        {
            break;
        }
        (*spvDescriptorSet0.cp_s0)._m0[_11] = select(_9, 0u, (_3 & 1u) != 0u || (_3 & 4u) != 0u);
        atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);
    }
}
"""
PLAIN = HEAD + """kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    for (int _1 = 0; _1 < 4; _1++)
    {
        if ((*spvDescriptorSet0.cp_s0)._m0[_1] == 0u)
        {
            break;
        }
    }
}
"""
FRAG = HEAD + """struct main0_out { float4 c [[color(0)]]; };
fragment main0_out main0(float4 p [[position]])
{
    main0_out o;
    uint _1 = spvSubgroupBallot(p.x > 1.0).x;
    o.c = float4(float(_1));
    return o;
}
"""
EARLY_FRAG = HEAD + """struct main0_out { float4 c [[color(0)]]; };
[[ early_fragment_tests ]] fragment main0_out main0(float4 p [[position]])
{
    main0_out o;
    uint _1 = spvSubgroupBallot(p.x > 1.0).x;
    o.c = float4(float(_1));
    return o;
}
"""
SWITCH = HEAD + """kernel void main0(device uint* o [[buffer(0)]], uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]])
{
    for (uint i = 0u; i < 4u; i++)
    {
        uint m = spvSubgroupBallot(o[i] != 0u).x;
        switch (o[i])
        {
            case 0u:
            {
                o[i] = m;
                break;
            }
            default:
            {
                break;
            }
        }
        if (m == 0u)
        {
            break;
        }
    }
    for (uint i = 0u; i < 64u; i++)
    {
        if (o[i] == 3u)
        {
            break;
        }
    }
}
"""
COMPACT_LOOPS = HEAD + """kernel void main0(device uint* o [[buffer(0)]], uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]])
{
    uint m = spvSubgroupBallot(o[0] != 0u).x;
    { uint dm_lg_b1 = 0u; for (uint i = 0u; i < 4u; i++)
    {
        if (o[i] == m) break;
    } }
    do {
        if (o[1] == m)
        {
            break;
        }
    } while (o[2] != 0u);
    switch (o[3]) {
        case 0u: break;
    }
}
"""
LATE = HEAD + """uint f1(uint x)
{
    uint r = spvSubgroupBallot(x != 0u).x;
    uint gl_SubgroupInvocationID = 3u;
    return r + gl_SubgroupInvocationID;
}

kernel void main0(device uint* o [[buffer(0)]], uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]])
{
    o[0] = f1(o[1]);
}
"""


class MetalWa(unittest.TestCase):
    def test_ballot_and_by_value_lane(self):
        out, st = W.apply(QUEUE, {3})
        body = out.split('kernel void main0', 1)[1]
        self.assertNotIn('spvSubgroupBallot(', body)
        self.assertIn('dm_mwa_ballot(true, gl_SubgroupInvocationID)', body)
        self.assertEqual(st, {3: 2})
        self.assertIn('simd_or(uint(value) << lane)', out)
        self.assertRegex(body, r'\bsimd_any\(_5 > 1u\)')
        self.assertRegex(body, r'\bsimd_all\(_9 == 0u\)')
        # Helper gets lane ID by value (uint, never thread uint& — avoids #2825 out-of-line scalarization bug)
        self.assertIn('uint _7(thread const uint& _1, uint gl_SubgroupInvocationID)', out)
        self.assertNotIn('thread uint& gl_SubgroupInvocationID', out)
        self.assertIn('_7(_3, gl_SubgroupInvocationID)', out)
        self.assertIn('uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]])', out)
        self.assertIn('inline uint4 spvSubgroupBallot(bool value)', out)
        self.assertNotIn('break;   // KK_WORKAROUND_9', out)

    def test_gate(self):
        out = W.apply(QUEUE, {3, 9})[0]
        self.assertIn(f'constant bool dm_mwa3_fc [[function_constant({W.FC_KK3})]];', out)
        self.assertIn('static inline __attribute__((always_inline)) uint4 dm_mwa_ballot(bool value, uint lane)', out)
        self.assertIn('if (is_function_constant_defined(dm_mwa3_fc) && dm_mwa3_fc) return uint4(simd_or(', out)
        self.assertNotIn('constant bool dm_mwa3 =', out)
        self.assertEqual(out.count('function_constant('), 1)
        st = W.apply(QUEUE, {3}, 'static')[0]
        self.assertIn('static inline __attribute__((always_inline)) uint4 dm_mwa_ballot(bool value, uint lane)', st)
        self.assertIn('return uint4(simd_or(uint(value) << lane), 0u, 0u, 0u);', st)
        self.assertNotIn('function_constant', st)
        self.assertNotIn('function_constant', W.apply(QUEUE, {9})[0])

    def test_loop_breaks_of_subgroup_functions(self):
        out, st = W.apply(SWITCH, {9})
        self.assertEqual(st[9], 2)
        self.assertEqual(out.count('if ((ulong)simd_active_threads_mask()) break;   // KK_WORKAROUND_9'), 2)
        self.assertEqual(out.count('                break;\n'), 2)
        self.assertIs(W.apply(PLAIN, {9})[0], PLAIN)
        out, st = W.apply(QUEUE, {9}); self.assertEqual(st[9], 1)

    def test_compact_loops_and_do_while(self):
        out, st = W.apply(COMPACT_LOOPS, {9})
        self.assertEqual(st[9], 2)
        self.assertIn('if (o[i] == m) { if ((ulong)simd_active_threads_mask()) break; }', out)
        self.assertIn('if ((ulong)simd_active_threads_mask()) break;   // KK_WORKAROUND_9', out)
        self.assertIn('case 0u: break;', out)

    def test_fragment_early_tests_and_vertex_skip(self):
        for src in (FRAG, EARLY_FRAG):
            out, st = W.apply(src, {3})
            self.assertEqual(st[3], 1)
            sig = [l for l in out.split('\n') if 'fragment main0_out main0(' in l][0]
            self.assertIn('uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]])', sig)
        for vert in (
            HEAD + 'vertex float4 main0(uint v [[vertex_id]])\n{\n    uint m = spvSubgroupBallot(v > 1u).x;\n    return float4(float(m));\n}\n',
            HEAD + '[[ patch(quad, 4) ]] vertex float4 main0(uint v [[vertex_id]])\n{\n    uint m = spvSubgroupBallot(v > 1u).x;\n    return float4(float(m));\n}\n',
        ):
            self.assertIs(W.apply(vert, {3})[0], vert)

    def test_existing_custom_lane_param_reused(self):
        src = HEAD + 'kernel void main0(device uint* o [[buffer(0)]], uint my_lane [[thread_index_in_simdgroup]])\n{\n    o[0] = spvSubgroupBallot(o[1] != 0u).x + my_lane;\n}\n'
        out, st = W.apply(src, {3})
        self.assertEqual(st[3], 1)
        self.assertEqual(out.count('[[thread_index_in_simdgroup]]'), 1)
        self.assertIn('dm_mwa_ballot(o[1] != 0u, my_lane)', out)

    def test_attribute_signatures_and_nested_calls(self):
        src = HEAD + """static __attribute__((noinline)) uint g(uint x)
{
    return spvSubgroupBallot(x != 0u).x;
}

kernel void main0(device uint* o [[buffer(0)]])
{
    o[0] = g(g(o[1]) + 1u);
}
"""
        out, st = W.apply(src, {3})
        self.assertEqual([f[3] for f in W.functions(src) if not W._helper(f[3])], ['g', 'main0'])
        self.assertIn('static __attribute__((noinline)) uint g(uint x, uint gl_SubgroupInvocationID)', out)
        self.assertIn('o[0] = g(g(o[1], gl_SubgroupInvocationID) + 1u, gl_SubgroupInvocationID);', out)

    def test_refuses_what_it_cannot_rewrite(self):
        dup = HEAD + 'uint f(uint x)\n{\n    return spvSubgroupBallot(x != 0u).x;\n}\n\nuint f(int x)\n{\n    return 1u;\n}\n\nkernel void main0(device uint* o [[buffer(0)]])\n{\n    o[0] = f(o[1]);\n}\n'
        with self.assertRaisesRegex(ValueError, 'two functions'): W.apply(dup, {3})
        multiline = HEAD + 'kernel void main0(device uint* o [[buffer(0)]],\n                  uint i [[thread_position_in_grid]])\n{\n    o[i] = spvSubgroupBallot(true).x;\n}\n'
        with self.assertRaisesRegex(ValueError, 'outside the functions'): W.apply(multiline, {3})
        local = HEAD + 'kernel void main0(device uint* o [[buffer(0)]])\n{\n    uint gl_SubgroupInvocationID = o[2];\n    o[0] = spvSubgroupBallot(true).x;\n}\n'
        with self.assertRaisesRegex(ValueError, 'in its body'): W.apply(local, {3})

    def test_pipeline_ordering_with_side_and_compute_loops(self):
        """dmsh_metalwa runs before shared_body/unit/fs_uint and before compute_loops: downstream passes preserve dm_mwa3_fc and the by-value lane parameter"""
        import dmsh_entry, dmsh_rewrite, dmsh_side
        ps = W.apply(FRAG, {3, 9})[0]
        fu = dmsh_entry.fs_uint(ps)
        self.assertIsNotNone(fu)
        self.assertIn('uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]]', fu)
        sh = dmsh_side.shared_body(ps)
        self.assertIn('main0_out dm_body_main0(float4 p, uint gl_SubgroupInvocationID)', sh)
        self.assertIsNotNone(dmsh_side.unit(sh + fu)[0])
        cs = W.apply(QUEUE, {3, 9})[0]
        capped, sites = dmsh_rewrite.compute_loops(cs)
        self.assertEqual(len(sites), 1)
        self.assertIn('if ((ulong)simd_active_threads_mask()) break;   // KK_WORKAROUND_9', capped)
        self.assertIn('uint _7(thread const uint& _1, uint gl_SubgroupInvocationID)', capped)

    def test_lane_only_from_parameters(self):
        out, st = W.apply(LATE, {3})
        f1 = out[out.index('uint f1'):].split('\n')[0]
        self.assertIn('uint gl_SubgroupInvocationID', f1)

    def test_idempotent_and_unchanged(self):
        once = W.apply(QUEUE, {3, 9})[0]
        self.assertIs(W.apply(once, {3, 9})[0], once)
        src = HEAD + 'kernel void main0() { }\n'
        self.assertIs(W.apply(src, {3, 9})[0], src)
        self.assertIs(W.apply(QUEUE, set())[0], QUEUE)

    def test_abi_and_chain_and_host_wiring(self):
        h = (TOOLS.parent / 'nvn' / 'dmsh_abi.h').read_text()
        self.assertEqual(int(re.search(r'#define DMSH_MWA_FC (\d+)', h).group(1)), W.FC_KK3)
        self.assertIn("'tools/dmsh_metalwa.py'", (TOOLS / 'dmsh_chain.py').read_text())
        m = (TOOLS.parent / 'nvn' / 'nvn_mtl.m').read_text()
        self.assertIn('atIndex:DMSH_MWA_FC', m)
        self.assertIn('getenv("DM_NVN_METALWA")', m)
        mwa_sec = m[m.index('static int mwa_on('):]
        mwa_sec = mwa_sec[:mwa_sec.index('\n}\n')]
        self.assertIn('strcmp(e, "0") != 0 : 0', mwa_sec)
        # Every host specialization path wires fc_set_mwa, and compute archive lookup recognizes dm_mwa3_fc
        for fn_head in ('static id<MTLFunction> fn_load(', 'static FnOff *fn_off(', 'static id<MTLFunction> side_spec(', 'static MTL4FunctionDescriptor *pre_fn(', 'static id<MTLFunction> bf_fn(NvShader *fs, const int *fx, uint8_t fbmask, const uint8_t *lmap) {'):
            sec = m[m.index(fn_head):]
            sec = sec[:sec.index('\n}\n')]
            self.assertIn('fc_set_mwa(', sec, fn_head)
        fd_sec = m[m.index('static int fd_host_fc('):]
        fd_sec = fd_sec[:fd_sec.index('\n}\n')]
        self.assertIn('@"dm_mwa3_fc"', fd_sec)


REPRO = (Path(__file__).resolve().parent / 't_metalwa.metal').read_text()


def _offline_metal_ok():
    if not shutil.which('xcrun'): return False
    r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '--version'], capture_output=True)
    return r.returncode == 0


class Repro(unittest.TestCase):
    """t_metalwa.metal: a ballot in a branch (kk3_branch), a ballot waterfall (kk3_waterfall), a ballot before a loop's only break (kk9_break)"""
    def test_rewritten(self):
        out, st = W.apply(REPRO, {3, 9})
        self.assertEqual(st, {3: 4, 9: 1})
        for k in ('kk3_branch', 'kk3_waterfall', 'kk9_break'):
            body = out.split(f'kernel void {k}(', 1)[1].split('\n}\n', 1)[0]
            self.assertNotIn('spvSubgroupBallot(', body)
            self.assertIn('dm_mwa_ballot(', body)
        self.assertEqual(out.count('[[thread_index_in_simdgroup]]'), REPRO.count('[[thread_index_in_simdgroup]]'))
        self.assertIs(W.apply(out, {3, 9})[0], out)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_METAL') == '1' and shutil.which('clang'), 'DMSH_TEST_METAL=1 runs the kernels on the GPU')
    def test_gpu(self):
        here = Path(__file__).resolve().parent
        offline = _offline_metal_ok()
        with tempfile.TemporaryDirectory() as d:
            run = Path(d) / 't_metalwa'
            subprocess.run(['clang', '-O1', '-fobjc-arc', '-o', str(run), str(here / 't_metalwa.m'), '-framework', 'Metal', '-framework', 'Foundation'], check=True)
            cases = [('as built', REPRO, '-O1', 'default'), ('KK3 + KK9', W.apply(REPRO, {3, 9})[0], '-O1', '1'),
                     ('KK3 + KK9 default-off', W.apply(REPRO, {3, 9})[0], '-O1', 'default'),
                     ('KK3 + KK9 static', W.apply(REPRO, {3, 9}, 'static')[0], '-O1', 'default'),
                     ('KK9 only', W.apply(REPRO, {9})[0], '-O1', 'default')]
            if offline: cases.insert(1, ('-O0', REPRO, '-O0', 'default'))
            res = {}
            for name, src, opt, fc in cases:
                m = Path(d) / (str(len(res)) + '.metal'); m.write_text(src)
                target = m
                if offline:
                    air, lib = m.with_suffix('.air'), m.with_suffix('.metallib')
                    subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', opt, '-fpreserve-invariance', str(m), '-o', str(air)], check=True)
                    subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(air), '-o', str(lib)], check=True)
                    target = lib
                    if name == 'KK3 + KK9':   # dm_mwa_ballot must stay inlined in AIR
                        ll = m.with_suffix('.ll')
                        subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-S', '-emit-llvm', opt, '-fpreserve-invariance', str(m), '-o', str(ll)], check=True)
                        self.assertNotRegex(ll.read_text(), r'^define .*dm_mwa_ballot')
                r = subprocess.run([str(run), str(target), fc], capture_output=True, text=True, timeout=60)
                self.assertEqual(r.returncode, 0, r.stderr)
                res[name] = {l.split(': bad ')[0]: int(l.split(': bad ')[1].split()[0]) for l in r.stdout.splitlines()}
                self.assertEqual(len(res[name]), 9, r.stdout)
            bad = lambda n: sorted(k for k, v in res[n].items() if v)
            print(f"\nt_metalwa on this OS ({'offline' if offline else 'online'}): as built failing {bad('as built') or 'none'}", file=sys.stderr)
            self.assertEqual(bad('KK3 + KK9'), [])
            self.assertEqual(bad('KK3 + KK9 static'), [])
            self.assertNotIn('kk9_break', bad('KK3 + KK9 default-off'))
            self.assertNotIn('kk9_break', bad('KK9 only'))


if __name__ == '__main__':
    unittest.main()
