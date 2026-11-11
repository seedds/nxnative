#!/usr/bin/env python3
"""dmsh_metalwa.py: the Metal compiler workarounds after KosmicKrisp, which a profile's shader_metalwa row turns on (dmsh_chain.metalwa). KK3 turns every ballot of a
non-helper function into simd_or of the lane bit, behind the function constant DMSH_MWA_FC (default on; the host turns it off on macOS 27), and brings the lane id to that
function (a kernel builtin, or a parameter its callers pass on); KK9 puts the lone loop breaks after a subgroup operation behind a run-time condition. A source without the
patterns is unchanged, and a second run changes nothing. With DMSH_TEST_METAL=1 the results also compile with the Metal compiler, and t_metalwa.m runs t_metalwa.metal's
kernels on the GPU (bounded): rewritten, every check must pass; whether the kernels as built still fail (the macOS 26 bugs) is printed, not asserted."""
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
# A warp-aggregated atomic under a data-driven branch, with the ballot in the kernel and in a helper that has no lane id yet, and a loop that breaks after a vote
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
        self.assertEqual(st, {3: 2})
        # KosmicKrisp's shift form, not a select feeding the reduction
        self.assertIn('simd_or(uint(value) << lane)', out)
        # votes are left as they are
        self.assertRegex(body, r'\bsimd_any\(_5 > 1u\)')
        self.assertRegex(body, r'\bsimd_all\(_9 == 0u\)')
        # the helper and its kernel get the lane id: a reference parameter, passed on; the kernel's builtin
        self.assertIn('uint _7(thread const uint& _1, thread uint& gl_SubgroupInvocationID)', out)
        self.assertIn('_7(_3, gl_SubgroupInvocationID)', out)
        self.assertIn('uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]])', out)
        # SPIRV-Cross's helper stays, for the gate's other side
        self.assertIn('inline uint4 spvSubgroupBallot(bool value)', out)
        self.assertNotIn('break;   // KK_WORKAROUND_9', out)

    def test_gate(self):
        out = W.apply(QUEUE, {3, 9})[0]
        self.assertIn(f'constant bool dm_mwa3_fc [[function_constant({W.FC_KK3})]];', out)
        self.assertIn('constant bool dm_mwa3 = is_function_constant_defined(dm_mwa3_fc) ? dm_mwa3_fc : true;', out)
        self.assertIn('if (dm_mwa3) return uint4(simd_or(', out)
        self.assertEqual(out.count('function_constant('), 1)   # KK9 has no gate
        st = W.apply(QUEUE, {3}, 'static')[0]
        self.assertIn('constant bool dm_mwa3 = true;', st)
        self.assertNotIn('function_constant', st)
        # KK9 alone declares no constant (no function the host would have to specialize)
        self.assertNotIn('function_constant', W.apply(QUEUE, {9})[0])

    def test_loop_breaks_of_subgroup_functions(self):
        out, st = W.apply(SWITCH, {9})
        # both loops' breaks (the function uses subgroup operations), not the two switch breaks
        self.assertEqual(st[9], 2)
        self.assertEqual(out.count('if ((ulong)simd_active_threads_mask()) break;   // KK_WORKAROUND_9'), 2)
        self.assertEqual(out.count('                break;\n'), 2)
        self.assertIs(W.apply(PLAIN, {9})[0], PLAIN)
        # through a call: QUEUE's loop calls simd_all (a subgroup op) before its break
        out, st = W.apply(QUEUE, {9}); self.assertEqual(st[9], 1)

    def test_fragment_entry_and_vertex(self):
        out, st = W.apply(FRAG, {3})
        sig = [l for l in out.split('\n') if l.startswith('fragment')][0]
        self.assertIn('uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]])', sig)
        vert = HEAD + 'vertex float4 main0(uint v [[vertex_id]])\n{\n    uint m = spvSubgroupBallot(v > 1u).x;\n    return float4(float(m));\n}\n'
        self.assertIs(W.apply(vert, {3})[0], vert)

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
        self.assertIn('static __attribute__((noinline)) uint g(uint x, thread uint& gl_SubgroupInvocationID)', out)
        self.assertIn('o[0] = g(g(o[1], gl_SubgroupInvocationID) + 1u, gl_SubgroupInvocationID);', out)

    def test_refuses_what_it_cannot_rewrite(self):
        dup = HEAD + 'uint f(uint x)\n{\n    return spvSubgroupBallot(x != 0u).x;\n}\n\nuint f(int x)\n{\n    return 1u;\n}\n\nkernel void main0(device uint* o [[buffer(0)]])\n{\n    o[0] = f(o[1]);\n}\n'
        with self.assertRaisesRegex(ValueError, 'two functions'): W.apply(dup, {3})
        multiline = HEAD + 'kernel void main0(device uint* o [[buffer(0)]],\n                  uint i [[thread_position_in_grid]])\n{\n    o[i] = spvSubgroupBallot(true).x;\n}\n'
        with self.assertRaisesRegex(ValueError, 'outside the functions'): W.apply(multiline, {3})
        local = HEAD + 'kernel void main0(device uint* o [[buffer(0)]])\n{\n    uint gl_SubgroupInvocationID = o[2];\n    o[0] = spvSubgroupBallot(true).x;\n}\n'
        with self.assertRaisesRegex(ValueError, 'in its body'): W.apply(local, {3})

    def test_shared_body_and_side_unit(self):
        """a pixel shader with extra entry points calls main0's body (dmsh_side.shared_body); rewritten, the body takes the lane from main0, and the side libraries'
        parser (dmsh_side.unit) still reads it"""
        import dmsh_side
        out, st = W.apply(dmsh_side.shared_body(FRAG), {3})
        self.assertIn('main0_out dm_body_main0(float4 p, thread uint& gl_SubgroupInvocationID)', out)
        self.assertIn('dm_body_main0(p, gl_SubgroupInvocationID)', out)
        self.assertIsNotNone(dmsh_side.unit(out)[0])

    def test_lane_only_from_parameters(self):
        out, st = W.apply(LATE, {3})
        f1 = out[out.index('uint f1'):].split('\n')[0]
        self.assertIn('thread uint& gl_SubgroupInvocationID', f1)

    def test_idempotent_and_unchanged(self):
        once = W.apply(QUEUE, {3, 9})[0]
        self.assertIs(W.apply(once, {3, 9})[0], once)
        src = HEAD + 'kernel void main0() { }\n'
        self.assertIs(W.apply(src, {3, 9})[0], src)
        self.assertIs(W.apply(QUEUE, set())[0], QUEUE)

    def test_abi_header(self):
        h = (TOOLS.parent / 'nvn' / 'dmsh_abi.h').read_text()
        self.assertEqual(int(re.search(r'#define DMSH_MWA_FC (\d+)', h).group(1)), W.FC_KK3)

    def test_host_specializes_through_fc_values(self):
        """every set of function constant values the host makes for a game's shader comes from fc_values, which turns KK3 off from macOS 27: a set made elsewhere
        would leave the rewrite on there, and a prewarm descriptor that differs from shader_load's would miss its pipeline"""
        m = (TOOLS.parent / 'nvn' / 'nvn_mtl.m').read_text()
        fc = m[m.index('static MTLFunctionConstantValues *fc_values(void) {'):]
        fc = fc[:fc.index('\n}\n')]
        self.assertEqual(m.count('[MTLFunctionConstantValues new]'), fc.count('[MTLFunctionConstantValues new]'))
        self.assertIn('atIndex:DMSH_MWA_FC', fc)
        self.assertIn('isOperatingSystemAtLeastVersion:(NSOperatingSystemVersion){27, 0, 0}', m)
        self.assertIn('getenv("DM_NVN_METALWA")', m)

    def test_chain_files(self):
        self.assertIn("'tools/dmsh_metalwa.py'", (TOOLS / 'dmsh_chain.py').read_text())

    def test_profile_row(self):
        import dmsh_chain

        class G:
            rows = [('shader_metalwa', ['kk3 kk9 0123456789abcdef']), ('shader_metalwa', ['FEDC1234'])]
        self.assertEqual(dmsh_chain.metalwa(G), (frozenset({3, 9}), frozenset({'0123456789abcdef', 'fedc1234'})))
        G.rows = [('shader_metalwa', ['kk3'])]
        self.assertEqual(dmsh_chain.metalwa(G), (frozenset({3}), frozenset()))
        G.rows = []
        self.assertEqual(dmsh_chain.metalwa(G), (frozenset(), frozenset()))
        self.assertEqual(dmsh_chain.metalwa(None), (frozenset(), frozenset()))
        for bad in (['0123456789abcdef'], ['kk3 zz'], ['kk4'], ['kk3 012']):
            G.rows = [('shader_metalwa', bad)]
            with self.assertRaises(SystemExit): dmsh_chain.metalwa(G)
        G.rows = [('shader_metalwa', ['kk3 01234567'])]
        self.assertIn(b'shader_metalwa\tkk3 01234567\n', dmsh_chain.profile_rows(G))

    @unittest.skipUnless(os.environ.get('DMSH_TEST_METAL') == '1' and shutil.which('xcrun'), 'DMSH_TEST_METAL=1 compiles with the Metal compiler')
    def test_metal_compiles(self):
        import dmsh_side
        with tempfile.TemporaryDirectory() as d:
            for name, (src, en, gate) in {'shared': (dmsh_side.shared_body(FRAG), {3, 9}, 'fc'), 'kk3_9': (QUEUE, {3, 9}, 'fc'), 'static': (QUEUE, {3, 9}, 'static'), 'frag': (FRAG, {3}, 'fc'), 'switch': (SWITCH, {3, 9}, 'fc')}.items():
                p = Path(d) / (name + '.metal'); p.write_text(W.apply(src, en, gate)[0])
                r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', str(p), '-o', str(Path(d) / (name + '.air'))], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, name + ': ' + r.stderr)


REPRO = (Path(__file__).resolve().parent / 't_metalwa.metal').read_text()


class Repro(unittest.TestCase):
    """t_metalwa.metal: a ballot in a branch (kk3_branch), a ballot waterfall (kk3_waterfall), a ballot before a loop's only break (kk9_break)"""
    def test_rewritten(self):
        out, st = W.apply(REPRO, {3, 9})
        self.assertEqual(st, {3: 4, 9: 1})   # the waterfall is a do-while without a break
        for k in ('kk3_branch', 'kk3_waterfall', 'kk9_break'):
            body = out.split(f'kernel void {k}(', 1)[1].split('\n}\n', 1)[0]
            self.assertNotIn('spvSubgroupBallot(', body)
            self.assertIn('dm_mwa_ballot(', body)
        # the kernels have the lane builtin already: no signature changes
        self.assertEqual(out.count('[[thread_index_in_simdgroup]]'), REPRO.count('[[thread_index_in_simdgroup]]'))
        self.assertIs(W.apply(out, {3, 9})[0], out)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_METAL') == '1' and shutil.which('xcrun'), 'DMSH_TEST_METAL=1 runs the kernels on the GPU')
    def test_gpu(self):
        here = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as d:
            run = Path(d) / 't_metalwa'
            subprocess.run(['clang', '-O1', '-fobjc-arc', '-o', str(run), str(here / 't_metalwa.m'), '-framework', 'Metal', '-framework', 'Foundation'], check=True)
            res = {}
            for name, src, opt in (('as built', REPRO, '-O1'), ('-O0', REPRO, '-O0'), ('KK3 + KK9', W.apply(REPRO, {3, 9})[0], '-O1'), ('KK3 + KK9 static', W.apply(REPRO, {3, 9}, 'static')[0], '-O1')):
                m, air, lib = (Path(d) / (str(len(res)) + x) for x in ('.metal', '.air', '.metallib'))
                m.write_text(src)
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', opt, '-fpreserve-invariance', str(m), '-o', str(air)], check=True)
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(air), '-o', str(lib)], check=True)
                r = subprocess.run([str(run), str(lib)], capture_output=True, text=True, timeout=60)
                self.assertEqual(r.returncode, 0, r.stderr)
                res[name] = {l.split(': bad ')[0]: int(l.split(': bad ')[1].split()[0]) for l in r.stdout.splitlines()}
                self.assertEqual(len(res[name]), 9, r.stdout)
            bad = lambda n: sorted(k for k, v in res[n].items() if v)
            print(f"\nt_metalwa on this OS: as built, failing {bad('as built') or 'none'}; at -O0, failing {bad('-O0') or 'none'}", file=sys.stderr)
            self.assertEqual(bad('KK3 + KK9'), [])
            self.assertEqual(bad('KK3 + KK9 static'), [])


if __name__ == '__main__':
    unittest.main()
