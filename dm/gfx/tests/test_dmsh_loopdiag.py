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

    def test_graphics_stage_has_no_buffer(self):
        frag = self.guarded.replace('kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])',
                                    'fragment void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])').replace('int(gl_LocalInvocationID.x)', '0').replace(
                                    '        threadgroup_barrier(mem_flags::mem_threadgroup);\n', '').replace('threadgroup uint _46[32];', 'uint _46[32] = {};')
        out, sites = ld.transform(frag, KEY, ld.stage_of(frag))
        self.assertEqual(ld.stage_of(frag), 'fragment')
        self.assertNotIn('[[buffer(28)]]', out)
        self.assertIn('device atomic_uint* dm_hg = nullptr;', out)
        if os.environ.get('DMSH_TEST_METAL'):
            rc, err = metal(out, 'frag'); self.assertEqual(rc, 0, err)

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


if __name__ == '__main__':
    unittest.main()
