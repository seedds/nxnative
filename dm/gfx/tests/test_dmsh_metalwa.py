#!/usr/bin/env python3
"""dmsh_metalwa.py: the Metal compiler workarounds after KosmicKrisp, which a profile's shader_metalwa row turns on (dmsh_chain.metalwa). KK3 turns every ballot / vote of a non-helper function into simd_or of the lane bit and brings the lane id
to that function (a kernel builtin, or a parameter its callers pass on); KK9 puts the lone breaks of functions with subgroup operations behind a run-time condition; KK10 and KK18
(options) rewrite selects and device fences; KK6 (option) adds `coherent` to argument-buffer pointers but never to volatile ones (KK15). A source without the patterns is unchanged,
and a second run changes nothing. PRAGMATA's 965043fe ballot waterfall (t_kk3_waterfall.metal, its r9 MSL) gets all four ballots rewritten. With DMSH_TEST_METAL=1 the
results also compile with the Metal compiler, and t_kk3_waterfall.m runs the waterfall on the GPU (bounded): the rewritten kernel must converge with the right item count on
every lane pattern, as -O0 does; whether the unrewritten -O1 kernel spins (the macOS 26 bug) is printed, not asserted."""
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
# 12a52b68's shape: a warp-aggregated atomic under a data-driven branch, with the ballot in the kernel and in a helper that has no lane id yet
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
    def test_ballot(self):
        out, st = W.apply(QUEUE, {3})
        body = out.split('kernel void main0', 1)[1]
        self.assertNotIn('spvSubgroupBallot(', body)
        self.assertIn('dm_mwa_ballot(true, gl_SubgroupInvocationID)', body)
        self.assertEqual(st[3], 2)
        # KosmicKrisp's shift form, not a select feeding the reduction (the KK10 shape)
        self.assertIn('simd_or(uint(value) << lane)', out)
        self.assertNotIn('value ? (1u << lane)', out)
        # votes are not rewritten without KK3v
        self.assertRegex(body, r'\bsimd_any\(_5 > 1u\)')
        self.assertRegex(body, r'\bsimd_all\(_9 == 0u\)')
        # the helper and its kernel get the lane id: a reference parameter, passed on; the kernel's builtin
        self.assertIn('uint _7(thread const uint& _1, thread uint& gl_SubgroupInvocationID)', out)
        self.assertIn('_7(_3, gl_SubgroupInvocationID)', out)
        self.assertIn('uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]])', out)
        self.assertIn(f'[[function_constant({W.FC_BASE})]]', out)
        self.assertIn('dm_mwa3 = is_function_constant_defined(dm_mwa3_fc) ? dm_mwa3_fc : true', out)
        self.assertIn('inline uint4 spvSubgroupBallot(bool value)', out)

    def test_votes_opt_in(self):
        out, st = W.apply(QUEUE, {3, 30})
        self.assertEqual(st[30], 2)
        self.assertIn('dm_mwa_any(_5 > 1u, gl_SubgroupInvocationID)', out)
        self.assertIn('simd_or(uint(!value) << lane) == 0u', out)
        self.assertIn('dm_mwa30 = is_function_constant_defined(dm_mwa30_fc) ? dm_mwa30_fc : false', out)

    def test_break_after_subgroup_op_only(self):
        out, st = W.apply(SWITCH, {9})
        # the loop break after the ballot, not the two switch breaks, not the loop without a subgroup op
        self.assertEqual(st[9], 1)
        self.assertEqual(out.count('if (!dm_mwa9 || (ulong)simd_active_threads_mask()) break;'), 1)
        self.assertIs(W.apply(PLAIN, {9})[0], PLAIN)
        # through a call: QUEUE's loop calls simd_all (a subgroup op) before its break
        out, st = W.apply(QUEUE, {9}); self.assertEqual(st[9], 1)

    def test_fragment_entry_and_vertex(self):
        out, st = W.apply(FRAG, {3})
        sig = [l for l in out.split('\n') if l.startswith('fragment')][0]
        self.assertIn('uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]])', sig)
        vert = HEAD + 'vertex float4 main0(uint v [[vertex_id]])\n{\n    uint m = spvSubgroupBallot(v > 1u).x;\n    return float4(float(m));\n}\n'
        self.assertIs(W.apply(vert, {3})[0], vert)

    def test_lane_only_from_parameters(self):
        out, st = W.apply(LATE, {3})
        f1 = out[out.index('uint f1'):].split('\n')[0]
        self.assertIn('thread uint& gl_SubgroupInvocationID', f1)

    def test_select_and_fence_options(self):
        out, st = W.apply(QUEUE, {10, 18})
        self.assertEqual((st[10], st[18]), (1, 1))
        self.assertIn('== dm_mwa_true())', out)
        self.assertIn('if (dm_mwa18 && (ulong)simd_active_threads_mask() == 0ul) atomic_thread_fence(', out)
        self.assertIn(f'[[function_constant({W.FC_BASE + W.SLOT[10]})]]', out)

    def test_coherent_not_on_volatile(self):
        out, st = W.apply(QUEUE, {6})
        self.assertIn('coherent device B* cp_s0 [[id(0)]];', out)
        self.assertIn('volatile device B* cp_s1 [[id(1)]];', out)
        self.assertNotIn('coherent volatile', out)

    def test_idempotent_and_unchanged(self):
        once = W.apply(QUEUE, {3, 9})[0]
        self.assertIs(W.apply(once, {3, 9})[0], once)
        src = HEAD + 'kernel void main0() { }\n'
        self.assertIs(W.apply(src, {3, 9})[0], src)

    def test_static_gate(self):
        out = W.apply(QUEUE, {3}, 'static')[0]
        self.assertIn('constant bool dm_mwa3 = true;', out)
        self.assertNotIn('function_constant', out)

    def test_host_defaults(self):
        d = W.host_defaults(26)
        self.assertTrue(d[W.FC_BASE] and d[W.FC_BASE + 1])
        self.assertFalse(d[W.FC_BASE + 5] or d[W.FC_BASE + 3] or d[W.FC_BASE + 4])
        self.assertFalse(any(W.host_defaults(27).values()))

    def test_abi_header(self):
        h = (TOOLS.parent / 'nvn' / 'dmsh_abi.h').read_text()
        self.assertEqual(int(re.search(r'#define DMSH_MWA_FC (\d+)', h).group(1)), W.FC_BASE)

    def test_chain_files(self):
        self.assertIn("'tools/dmsh_metalwa.py'", (TOOLS / 'dmsh_chain.py').read_text())

    @unittest.skipUnless(os.environ.get('DMSH_TEST_METAL') == '1' and shutil.which('xcrun'), 'DMSH_TEST_METAL=1 compiles with the Metal compiler')
    def test_metal_compiles(self):
        with tempfile.TemporaryDirectory() as d:
            for name, (src, en) in {'kk3_9': (QUEUE, {3, 9}), 'all': (QUEUE, {3, 30, 9, 10, 18}), 'kk6': (QUEUE, {6}), 'static': (QUEUE, {3, 9}), 'frag': (FRAG, {3}), 'switch': (SWITCH, {3, 9})}.items():
                p = Path(d) / (name + '.metal'); p.write_text(W.apply(src, en, 'static' if name == 'static' else 'fc')[0])
                r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', str(p), '-o', str(Path(d) / (name + '.air'))], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, name + ': ' + r.stderr)


WATERFALL = (Path(__file__).resolve().parent / 't_kk3_waterfall.metal').read_text()


class Waterfall(unittest.TestCase):
    """965043fe128ef98f loop 1: `uint _570 = spvSubgroupBallot(true).x;` and two more under `if (!_606)` / `if (!_652)`, the loop's exit ballot after them"""
    def test_all_ballots_rewritten(self):
        out, st = W.apply(WATERFALL, {3, 9}, 'static')
        body = out.split('kernel void wf(', 1)[1]
        self.assertEqual(st[3], 4)
        self.assertEqual(st[9], 0)   # a do-while without a break
        self.assertNotIn('spvSubgroupBallot(', body)
        for v in ('true', '_706'): self.assertIn(f'dm_mwa_ballot({v}, gl_SubgroupInvocationID)', body)
        self.assertIn('constant bool dm_mwa3 = true;', out)
        # the kernel has the lane builtin already, and _77 takes it as a parameter: no signature changes
        self.assertEqual(out.count('[[thread_index_in_simdgroup]]'), 1)
        self.assertIn('_77(_642, _644, _646, _648, gl_SubgroupInvocationID)', body)
        self.assertIs(W.apply(out, {3, 9}, 'static')[0], out)

    def test_profile_row(self):
        import dmsh_chain

        class G:
            rows = [('shader_metalwa', ['kk3 kk9 965043fe128ef98f']), ('shader_metalwa', ['927F6B99'])]
        self.assertEqual(dmsh_chain.metalwa(G), (frozenset({3, 9}), frozenset({'965043fe128ef98f', '927f6b99'})))
        G.rows = [('shader_metalwa', ['kk3'])]
        self.assertEqual(dmsh_chain.metalwa(G), (frozenset({3}), frozenset()))
        G.rows = []
        self.assertEqual(dmsh_chain.metalwa(G), (frozenset(), frozenset()))
        self.assertEqual(dmsh_chain.metalwa(None), (frozenset(), frozenset()))
        for bad in (['965043fe128ef98f'], ['kk3 zz'], ['kk4']):
            G.rows = [('shader_metalwa', bad)]
            with self.assertRaises(SystemExit): dmsh_chain.metalwa(G)
        G.rows = [('shader_metalwa', ['kk3 965043fe'])]
        self.assertIn(b'shader_metalwa\tkk3 965043fe\n', dmsh_chain.profile_rows(G))

    @unittest.skipUnless(os.environ.get('DMSH_TEST_METAL') == '1' and shutil.which('xcrun'), 'DMSH_TEST_METAL=1 runs the waterfall on the GPU')
    def test_gpu(self):
        here = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as d:
            run = Path(d) / 't_kk3_waterfall'
            subprocess.run(['clang', '-O1', '-fobjc-arc', '-o', str(run), str(here / 't_kk3_waterfall.m'), '-framework', 'Metal', '-framework', 'Foundation'], check=True)
            res = {}
            for name, src, opt in (('O1', WATERFALL, '-O1'), ('O0', WATERFALL, '-O0'), ('O1+kk3', W.apply(WATERFALL, {3, 9}, 'static')[0], '-O1')):
                m, air, lib = (Path(d) / (name + x) for x in ('.metal', '.air', '.metallib'))
                m.write_text(src)
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', opt, '-fpreserve-invariance', str(m), '-o', str(air)], check=True)
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(air), '-o', str(lib)], check=True)
                r = subprocess.run([str(run), str(lib)], capture_output=True, text=True, timeout=60)
                self.assertEqual(r.returncode, 0, r.stderr)
                res[name] = [tuple(map(int, re.search(r'capped (\d+) bad (\d+)', l).groups())) for l in r.stdout.splitlines()]
                self.assertEqual(len(res[name]), 7, r.stdout)
            print(f"\nt_kk3_waterfall: -O1 as built {'spins (KK3 present)' if any(c for c, _ in res['O1']) else 'converges (KK3 absent)'}; -O0 and KK3 converge", file=sys.stderr)
            self.assertEqual(res['O0'], [(0, 0)] * 7)
            self.assertEqual(res['O1+kk3'], [(0, 0)] * 7)


if __name__ == '__main__':
    unittest.main()
