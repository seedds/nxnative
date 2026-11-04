"""Compute shared memory past Metal's 32 KB (dmsh_build.py tg_spill): a kernel whose threadgroup arrays exceed abi.TG_MAX takes them from the device buffer at abi.TG_SLOT, one slice per
workgroup at its index modulo abi.TG_GROUPS, with barriers that order device memory; a kernel within the limit is left as it is. The shape is Mario Kart World's DrivePassShaderDefault
compute shaders (spvUnsafeArray<uint, 12288>, 48 KB). When the Metal compiler is installed, the rewritten kernel is compiled too."""
import ast
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_abi as abi
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
selected = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name == 'tg_spill')
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) in ('TG_DECL', 'TG_BYTES') for t in n.targets))]
scope = {'re': re, 'abi': abi}
exec(compile(ast.Module(body=selected, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
tg_spill = scope['tg_spill']

ARRAY = """template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    thread T& operator [] (size_t pos) thread { return elements[pos]; }
    device T& operator [] (size_t pos) device { return elements[pos]; }
    threadgroup T& operator [] (size_t pos) threadgroup { return elements[pos]; }
};
"""
KERNEL = """#include <metal_stdlib>
using namespace metal;
""" + ARRAY + """
struct B { uint _m0[1]; };
kernel void main0(device B& out [[buffer(1)]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    threadgroup spvUnsafeArray<uint, %d> _39;
    threadgroup float4 _40[4];
    _39[gl_LocalInvocationID.x] = gl_WorkGroupID.x;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    _40[0] = float4(1.0);
    out._m0[0] = _39[(gl_LocalInvocationID.x + 1u) %% 64u] + uint(_40[0].x);
}
"""


class TgSpill(unittest.TestCase):
    def test_past_the_limit(self):
        out, n = tg_spill(KERNEL % 12288)
        self.assertEqual(n, 12288 * 4 + 4 * 16)
        self.assertIn(f'device uchar* dm_tg [[buffer({abi.TG_SLOT})]]', out)
        self.assertIn('uint3 dm_ntg [[threadgroups_per_grid]]', out)
        self.assertNotIn('dm_tgid', out)   # the kernel's own workgroup position is used
        self.assertIn(f'% {abi.TG_GROUPS}u) * {n}ul;', out)
        self.assertIn('device spvUnsafeArray<uint, 12288>& _39 = *(device spvUnsafeArray<uint, 12288>*)(dm_tg + dm_tgbase + 0);', out)
        self.assertIn(f'device float4 (&_40)[4] = *(device float4 (*)[4])(dm_tg + dm_tgbase + {12288 * 4});', out)
        self.assertIn('threadgroup_barrier(mem_flags::mem_device | mem_flags::mem_threadgroup);', out)
        self.assertIn('threadgroup T& operator [] (size_t pos) threadgroup', out)   # the template keeps its overloads
        self.assertNotRegex(out, r'^\s*threadgroup (spvUnsafeArray|float4) ', 'no threadgroup array is left')

    def test_within_the_limit(self):
        src = KERNEL % 4096
        self.assertEqual(tg_spill(src), (src, 0))

    def test_no_position_parameter(self):
        src = (KERNEL % 12288).replace(', uint3 gl_WorkGroupID [[threadgroup_position_in_grid]]', '').replace('= gl_WorkGroupID.x', '= 0u')
        out, n = tg_spill(src)
        self.assertTrue(n)
        self.assertIn('uint3 dm_tgid [[threadgroup_position_in_grid]]', out)
        self.assertIn('((dm_tgid.z * dm_ntg.y + dm_tgid.y) * dm_ntg.x + dm_tgid.x)', out)

    def test_unknown_element_type(self):
        with self.assertRaises(ValueError):
            tg_spill((KERNEL % 12288).replace('threadgroup float4 _40[4];', 'threadgroup S _40[4];'))

    @unittest.skipUnless(shutil.which('xcrun') and subprocess.run(['xcrun', '-sdk', 'macosx', '-f', 'metal'], capture_output=True).returncode == 0, 'no Metal compiler')
    def test_compiles(self):
        out, n = tg_spill(KERNEL % 12288)
        with tempfile.TemporaryDirectory() as d:
            src = Path(d, 'k.metal'); src.write_text(out)
            r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', str(src), '-o', str(Path(d, 'k.air'))], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-2000:])


if __name__ == '__main__':
    unittest.main()
