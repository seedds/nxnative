#!/usr/bin/env python3
"""dmsh_loopdiag.py (diagnostic loop guard for side libraries): every loop_guard site reports itself through the debug buffer at compute index 28; a helper that loops
gets the caller's buffer and cap; loops with a barrier (own counter) never leave on the abort word. With DMSH_TEST_METAL=1 the rewritten shaders also compile, and
DMSH_TEST_LIB=<b-<chain> dir> DMSH_TEST_KEYS=<k,k,...> rewrites and compiles those library shaders too."""
from pathlib import Path
import ast
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_loopdiag as ld  # noqa: E402

tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
selected = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name == 'loop_guard')
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) in ('LOOP_CAP', '_LOOP_HEAD', '_BARRIER') for t in n.targets))]
scope = {'re': re}
exec(compile(ast.Module(body=selected, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
loop_guard = scope['loop_guard']

KEY = '12a52b683eec95c1'
# The persistent-queue shape (12a52b68): a helper with a CAS spin, called inside the outer work loop; a loop with a barrier; SPIRV-Cross's one-line helper header
QUEUE = """#include <metal_stdlib>
#include <metal_atomic>

using namespace metal;
struct B { uint _m0[1024]; };
struct spvDescriptorSetBuffer0 { device B* cp_s1 [[id(0)]]; };

static inline __attribute__((always_inline))
uint _56(thread const int& _1, device B& cp_s1)
{
    uint _5;
    do
    {
        _5 = 0u;
    } while (!atomic_compare_exchange_weak_explicit((device atomic_uint*)&cp_s1._m0[_1 & 511], &_5, 7u, memory_order_relaxed, memory_order_relaxed) && _5 == 0u);
    return _5;
}

static inline __attribute__((always_inline)) uint _58(thread const int& _2, device B& cp_s1)
{
    return _56(_2, cp_s1) + 1u;
}

kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    threadgroup uint _46[32];
    int _10 = int(gl_LocalInvocationID.x);
    bool _14;
    do
    {
        uint _20 = atomic_load_explicit((device atomic_uint*)&(*spvDescriptorSet0.cp_s1)._m0[512], memory_order_relaxed);
        uint _21 = _58(_10, (*spvDescriptorSet0.cp_s1));
        _14 = _20 == 0u && _21 != 0u;
    } while (_14);
    uint _30 = (*spvDescriptorSet0.cp_s1)._m0[513];
    do
    {
        _46[_10 & 31] += 1u;
        threadgroup_barrier(mem_flags::mem_threadgroup);
        _30 = _30 - 1u;
    } while (_30 != 0u);
    (*spvDescriptorSet0.cp_s1)._m0[_10] = _46[_10 & 31];
}
"""


def metal(src, name):
    d = tempfile.mkdtemp()
    try:
        p = Path(d) / (name + '.metal'); p.write_text(src)
        r = subprocess.run(ld.MAC_METAL + ['-w', str(p), '-o', str(p.with_suffix('.air'))], capture_output=True, text=True)
        return r.returncode, r.stderr
    finally:
        shutil.rmtree(d)


class LoopDiag(unittest.TestCase):
    def setUp(self):
        self.guarded = loop_guard(QUEUE)
        self.out, self.sites = ld.transform(self.guarded, KEY, ld.stage_of(self.guarded))

    def test_every_site_reports(self):
        n = len(re.findall(r'\+\+dm_lg\d* < \d+u', self.guarded))
        self.assertEqual(n, 3)
        self.assertEqual(len(self.sites), n)
        self.assertNotIn('++dm_lg', self.out)
        self.assertEqual([s[0] for s in self.sites], [1, 2, 3])
        for s, _, _, line in self.sites:
            self.assertIn(f', {s}u, dm_hg, dm_hgcap)', self.out.split('\n')[line - 1])

    def test_barrier_loop_never_aborts(self):
        own = [s for s in self.sites if s[1] != 'dm_lg']
        self.assertEqual(len(own), 1)
        self.assertIn(f'dm_hgb({own[0][1]}, {own[0][0]}u,', self.out)
        shared = [s for s in self.sites if s[1] == 'dm_lg']
        for s in shared: self.assertIn(f'dm_hgc(dm_lg, {s[0]}u,', self.out)

    def test_helpers_get_the_buffer(self):
        self.assertIn('uint _56(thread const int& _1, device B& cp_s1, device atomic_uint* dm_hg, uint dm_hgcap)', self.out)
        # _58 has no loop but calls _56: it takes and passes the buffer too
        self.assertIn('_58(thread const int& _2, device B& cp_s1, device atomic_uint* dm_hg, uint dm_hgcap)', self.out)
        self.assertIn('return _56(_2, cp_s1, dm_hg, dm_hgcap) + 1u;', self.out)
        self.assertIn('_58(_10, (*spvDescriptorSet0.cp_s1), dm_hg, dm_hgcap)', self.out)
        self.assertIn('always_inline)) uint _58(', self.out)   # the attribute itself is not taken for a call
        self.assertIn('device atomic_uint* dm_hg [[buffer(28)]])', self.out)
        self.assertEqual({s[2] for s in self.sites}, {'_56', 'main0'})

    def test_key_in_records(self):
        self.assertIn('#define DM_HG_KEY_HI 0x12a52b68u', self.out)
        self.assertIn('#define DM_HG_KEY_LO 0x3eec95c1u', self.out)

    def test_no_loops_unchanged(self):
        self.assertEqual(ld.transform('kernel void main0()\n{\n}\n', KEY)[0], 'kernel void main0()\n{\n}\n')

    def _graphics(self, kind):
        return self.guarded.replace('kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])',
                                    kind + ' void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])').replace('int(gl_LocalInvocationID.x)', '0').replace(
                                    '        threadgroup_barrier(mem_flags::mem_threadgroup);\n', '').replace('threadgroup uint _46[32];', 'uint _46[32] = {};')

    def test_vertex_stage_has_no_buffer(self):
        vert = self._graphics('vertex')
        out, sites = ld.transform(vert, KEY, ld.stage_of(vert))
        self.assertEqual(ld.stage_of(vert), 'vertex')
        self.assertNotIn('[[buffer(28)]]', out)
        self.assertIn('device atomic_uint* dm_hg = nullptr;', out)

    def test_fragment_stage_has_buffer_30(self):   # the host binds the debug buffer at fragment index 30 with DM_NVN_LOOPCAP (nvn_mtl.m encode_args_)
        frag = self._graphics('fragment')
        out, sites = ld.transform(frag, KEY, ld.stage_of(frag))
        self.assertEqual(ld.stage_of(frag), 'fragment')
        self.assertNotIn('[[buffer(28)]]', out)
        self.assertIn('device atomic_uint* dm_hg [[buffer(30)]]', out)
        self.assertNotIn('device atomic_uint* dm_hg = nullptr;', out)
        if os.environ.get('DMSH_TEST_METAL'):
            rc, err = metal(out, 'frag'); self.assertEqual(rc, 0, err)

    def test_gmem_miss_reported(self):
        src = QUEUE.replace("static inline __attribute__((always_inline)) uint _58(", """static inline __attribute__((always_inline))
int _83(thread const int& _7, thread const int& _8, device B& cp_s2)
{
    int _9 = int(0u + 2048u);
    if (cp_s2._m0[_9 & 1023] < uint(_8))
    {
        uint _10 = *(reinterpret_cast<device uint*>(as_type<ulong>(uint2(uint(_7), uint(_8)))));
        return int(_10);
    }
    return 0;
}

static inline __attribute__((always_inline)) uint _58(""").replace("return _56(_2, cp_s1) + 1u;", "return _56(_2, cp_s1) + uint(_83(_2, _2, cp_s1));")
        out, sites = ld.transform(loop_guard(src), KEY, 'compute')
        self.assertIn('    dm_hg_miss(uint(_7), uint(_8), dm_hg);\n    return dm_hg_poison(dm_hg);', out)   # DM_NVN_GMEM_POISON: word 9, 0 when unset
        self.assertIn('hg == nullptr ? 0 : int(atomic_load_explicit(&hg[9], memory_order_relaxed))', out)
        self.assertEqual(out.count('return dm_hg_poison(dm_hg);'), 2)   # the miss, and the hit path's low-address guard (dm_hg_lowaddr); the hit's own return is untouched
        self.assertIn('int _83(thread const int& _7, thread const int& _8, device B& cp_s2, device atomic_uint* dm_hg, uint dm_hgcap)', out)
        self.assertIn('_83(_2, _2, cp_s1, dm_hg, dm_hgcap)', out)
        if os.environ.get('DMSH_TEST_METAL'):
            rc, err = metal(out, 'gmem'); self.assertEqual(rc, 0, err)

    def test_empty_key_list_refused(self):
        r = subprocess.run([sys.executable, str(TOOLS / 'dmsh_loopdiag.py'), '/nonexistent', tempfile.mkdtemp(), '/dev/null'], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('no keys', r.stderr)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_METAL'), 'DMSH_TEST_METAL=1 compiles with the Metal compiler')
    def test_compiles(self):
        rc, err = metal(self.guarded, 'before'); self.assertEqual(rc, 0, err)
        rc, err = metal(self.out, 'after'); self.assertEqual(rc, 0, err)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_LIB') and os.environ.get('DMSH_TEST_KEYS'), 'DMSH_TEST_LIB + DMSH_TEST_KEYS: library shaders')
    def test_library_shaders(self):
        for k in os.environ['DMSH_TEST_KEYS'].split(','):
            src = (Path(os.environ['DMSH_TEST_LIB']) / f'{k}_b.metal').read_text()
            out, sites = ld.transform(src, k, ld.stage_of(src))
            self.assertEqual(len(sites), len(re.findall(r'\+\+dm_lg\d* < \d+u', src)), k)
            rc, err = metal(out, k); self.assertEqual(rc, 0, f'{k}: {err[-1500:]}')


class LowAddr(unittest.TestCase):
    def test_found_entry_load_is_guarded(self):   # hangguard hg25: a pool-table helper's load of a resolved address below 4 GB reads as a miss
        src = """#include <metal_stdlib>
using namespace metal;
struct B { uint _m0[1]; };
static inline __attribute__((always_inline))
int _83(thread const int& _7, thread const int& _8, device B& cp_s2)
{
    int _9 = int(0u + 2048u);
    if (cp_s2._m0[_9 & 1023] < uint(_8))
    {
        uint _10 = *(reinterpret_cast<device uint*>(as_type<ulong>(uint2(uint(_7), uint(_8)))));
        return int(_10);
    }
    return 0;
}
kernel void main0(device B& cp_s2 [[buffer(1)]])
{
    int a = 1, b = 2;
    cp_s2._m0[0] = uint(_83(a, b, cp_s2));
}
"""
        out, sites = ld.transform(src, KEY, 'compute')
        self.assertIn('if (dm_hg_lowaddr(uint(_7), uint(_8), dm_hg)) return dm_hg_poison(dm_hg);', out)
        self.assertIn('dm_hg_miss(uint(_7), uint(_8), dm_hg);', out)
        self.assertLess(out.index('dm_hg_lowaddr(uint(_7)'), out.index('uint _10 = *(reinterpret_cast'))
        if os.environ.get('DMSH_TEST_METAL'):
            rc, err = metal(out, 'low'); self.assertEqual(rc, 0, err)

# The automatic guard (dmsh_loopdiag.auto, dmsh_build.py LOOPGUARD=auto): a count read from a storage buffer (data-sized) and a constant count (not), as SPIRV-Cross writes them
DATA = """#include <metal_stdlib>
#include <metal_atomic>

using namespace metal;
struct B { uint _m0[1024]; };
struct spvDescriptorSetBuffer0 { device B* cp_s0 [[id(0)]]; };

kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    uint _10 = (*spvDescriptorSet0.cp_s0)._m0[0];
    uint _12 = 0u;
    uint _20 = 0u;
    bool _22;
    do
    {
        _12 += (*spvDescriptorSet0.cp_s0)._m0[_20 & 1023u];
        uint _21 = _20 + 1u;
        _22 = _21 < _10;
        _20 = _21;
    } while (_22);
    int _30 = 0;
    bool _32;
    do
    {
        _12 += uint(_30);
        int _31 = _30 + 1;
        _32 = _31 < 8;
        _30 = _31;
    } while (_32);
    (*spvDescriptorSet0.cp_s0)._m0[1] = _12;
}
"""
CONST = DATA.replace('uint _10 = (*spvDescriptorSet0.cp_s0)._m0[0];', 'uint _10 = 16u;')


class AutoGuard(unittest.TestCase):
    def setUp(self):
        self.guarded = loop_guard(DATA)
        self.out, self.table = ld.auto(self.guarded, KEY, 'compute')

    def test_only_the_data_sized_loop(self):
        self.assertEqual(len(re.findall(r'\+\+dm_lg < 262144u', self.guarded)), 2)
        self.assertEqual(len(self.table), 1)
        self.assertIn('} while ((_22) && dm_hgcv(dm_lg, 1u, as_type<uint>(_10), dm_hg, dm_hgcap));', self.out)   # the bound variable is the record's value
        self.assertIn('} while ((_32) && ++dm_lg < 262144u);', self.out)   # the constant loop keeps loop_guard's test
        t = self.table[0]
        self.assertEqual((t['site'], t['counter'], t['fn'], t['why'], t['value']), (1, 'dm_lg', 'main0', 'data', '_10'))
        self.assertIn('dm_hgcv(dm_lg, 1u,', self.out.split('\n')[t['line'] - 1])

    def test_off_is_loop_guard(self):   # no DM_NVN_LOOPCAP: a zeroed buffer, so the cap is loop_guard's and nothing records
        self.assertEqual(ld.LOOP_CAP, scope['LOOP_CAP'])
        self.assertIn('device atomic_uint* dm_hg_buf [[buffer(28)]])', self.out)
        self.assertIn(f'atomic_load_explicit(&dm_hg[{ld.AUTO_CAP_WORD}], memory_order_relaxed); if (dm_hgcap == 0u) dm_hgcap = atomic_load_explicit(&dm_hg[1], memory_order_relaxed);'
                      f' if (dm_hgcap == 0u) {{ dm_hg = nullptr; dm_hgcap = {scope["LOOP_CAP"]}u; }}', self.out)
        body = self.out[self.out.index('bool dm_hgcv('):]
        body = body[:body.index('\n}\n')]
        self.assertIn('if ((c & 1023u) != 0u || hg == nullptr) return true;', body)   # off: c < cap is the whole test, as ++c < cap
        self.assertIn('if (c == cap) dm_hg_rec(site, v, hg);', body)
        self.assertIn('if (hg == nullptr) return;', ld.PRELUDE_CORE)
        self.assertNotIn('dm_hg_miss', self.out)   # the automatic mode leaves the pool-table lookups alone

    def test_record_holds_the_value(self):
        self.assertIn('atomic_store_explicit(&hg[16u + 4u * n + 3u], v, memory_order_relaxed);', self.out)

    def test_no_data_sized_loop_unchanged(self):   # byte for byte: a library rebuilt with the guard keeps every other shader
        g = loop_guard(CONST)
        self.assertEqual(ld.auto(g, KEY, 'compute'), (g, []))
        self.assertEqual(ld.auto(QUEUE, KEY, 'compute'), (QUEUE, []))   # no loop_guard sites at all

    def test_stages(self):
        for st in ('vertex', 'fragment', 'mesh'):   # not guarded: no binding the host can give them safely (dmsh_loopdiag.py AUTO_STAGES)
            self.assertEqual(ld.auto(self.guarded, KEY, st), (self.guarded, []), st)
        task = self.guarded.replace('kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])',
                                    '[[object]] void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]], constant uint* dm_first [[buffer(29)]])')
        out, table = ld.auto(task, KEY, ld.stage_of(task))
        self.assertEqual(ld.stage_of(task), 'task')
        self.assertEqual(len(table), 1)
        self.assertIn('constant uint* dm_first [[buffer(29)]], device atomic_uint* dm_hg_buf [[buffer(28)]])', out)

    def test_helper_loop(self):   # a data-sized loop in a helper (the task shaders' dm_stage_body): the helper takes the buffer and cap, its caller passes them
        helper = self.guarded.replace('kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])\n{',
            'static inline __attribute__((always_inline)) void dm_stage_body(constant spvDescriptorSetBuffer0& spvDescriptorSet0)\n{') + \
            'kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])\n{\n    uint dm_lg = 0u; (void)dm_lg;\n    dm_stage_body(spvDescriptorSet0);\n}\n'
        out, table = ld.auto(helper, KEY, 'compute')
        self.assertEqual([t['fn'] for t in table], ['dm_stage_body'])
        self.assertIn('void dm_stage_body(constant spvDescriptorSetBuffer0& spvDescriptorSet0, device atomic_uint* dm_hg, uint dm_hgcap)', out)
        self.assertIn('    dm_stage_body(spvDescriptorSet0, dm_hg, dm_hgcap);', out)
        if os.environ.get('DMSH_TEST_METAL'):
            rc, err = metal(out, 'helper'); self.assertEqual(rc, 0, err)

    def test_value_out_of_scope_is_zero(self):   # a bound declared inside the loop body is not in scope at its `} while`
        src = self.guarded.replace('    uint _10 = (*spvDescriptorSet0.cp_s0)._m0[0];\n', '').replace('        uint _21 = _20 + 1u;\n',
                                                                                                   '        uint _21 = _20 + 1u;\n        uint _10 = (*spvDescriptorSet0.cp_s0)._m0[0];\n')
        out, table = ld.auto(src, KEY, 'compute')
        self.assertEqual(len(table), 1)
        self.assertIn('dm_hgcv(dm_lg, 1u, 0u, dm_hg, dm_hgcap)', out)

    def test_over_budget_takes_every_loop(self):
        found = ld.auto_sites(self.guarded, budget=0.0)
        self.assertEqual(len(found), 2)
        self.assertEqual({v['why'] for v in found.values()}, {'budget'})

    def test_build_switch(self):
        import dmsh_chain
        class G:
            rows = [('shader_border_emu', ['0'])]
            def get(self, k, d=None): return dict((a, b[0]) for a, b in self.rows).get(k, d)
        from unittest import mock
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop('DMSH_LOOPGUARD', None)
            self.assertEqual(dmsh_chain.loopguard(G()), 'off')
            self.assertEqual(dmsh_chain.profile_rows(G()), b'shader_border_emu\t0\n')   # unchanged without the switch: no shader's cache key moves
            os.environ['DMSH_LOOPGUARD'] = 'auto'
            self.assertEqual(dmsh_chain.loopguard(G()), 'auto')
            self.assertEqual(dmsh_chain.profile_rows(G()), b'shader_border_emu\t0\nshader_loopguard\tauto\n')
            os.environ['DMSH_LOOPGUARD'] = 'on'
            with self.assertRaises(SystemExit): dmsh_chain.loopguard(G())

    @unittest.skipUnless(os.environ.get('DMSH_TEST_METAL'), 'DMSH_TEST_METAL=1 compiles with the Metal compiler')
    def test_compiles(self):
        rc, err = metal(self.out, 'auto'); self.assertEqual(rc, 0, err)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_LIB') and os.environ.get('DMSH_TEST_KEYS'), 'DMSH_TEST_LIB + DMSH_TEST_KEYS: library shaders')
    def test_library_shaders(self):
        for k in os.environ['DMSH_TEST_KEYS'].split(','):
            src = (Path(os.environ['DMSH_TEST_LIB']) / f'{k}_b.metal').read_text()
            out, table = ld.auto(src, k, ld.stage_of(src))
            if not table: self.assertEqual(out, src, k); continue
            rc, err = metal(out, k); self.assertEqual(rc, 0, f'{k}: {err[-1500:]}')


if __name__ == '__main__':
    unittest.main()
