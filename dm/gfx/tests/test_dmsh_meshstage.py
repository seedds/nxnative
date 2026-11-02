#!/usr/bin/env python3
"""NVN2 task and mesh shaders as Metal object and mesh functions (dm/gfx/tools/dmsh_meshstage.py): the outputs an SPH enables, the shared-memory layout the translator
writes them in, the per-primitive SPH after the instructions, and the stage functions wrapped around a translated kernel (its shape as spirv-cross writes it). With
DMSH_TEST_METAL=1 the wrapped sources are compiled for macOS and iOS."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_meshstage as ms

KERNEL = """#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    thread T& operator [] (size_t pos) thread { return elements[pos]; }
    threadgroup T& operator [] (size_t pos) threadgroup { return elements[pos]; }
};

kernel void main0(uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    threadgroup spvUnsafeArray<uint, %d> _18;
    _18[%du + gl_LocalInvocationID.x] = _18[0u] + gl_LocalInvocationID.x;
}
"""


def sph(position=0xf, generics=(0x3,)):
    s = bytearray(0x80); s[0x35] = position << 4
    for n, nib in enumerate(generics): s[0x36 + n // 2] |= nib << (4 * (n % 2))
    return bytes(s)


class Layout(unittest.TestCase):
    def test_sph_outputs(self):   # the position's xyzw, then generic 0's x and y
        self.assertEqual(ms.sph_outputs(sph()), [(None, 0), (None, 1), (None, 2), (None, 3), (0, 0), (0, 1)])

    def test_task_layout(self):   # the payload after the outputs' start, then the words the kernel's array needs
        self.assertEqual(ms.layout('ts', 0, [], 0x30), ([0, 0x40, 0x900, 0x930], (0x930 + 0xf) // 4 + 4))

    def test_outputs_after_a_large_payload(self):
        self.assertEqual(ms.layout('ts', 0, [], 0x1000)[0], [0, 0x40, 0x1040, 0x2040])

    def test_mesh_layout(self):   # 64 vertices and 32 primitives: two blocks of 32, each 6 vertex and 1 primitive components of 0x80 bytes, then the primitive bytes
        lay, words = ms.layout('ms', 64, ms.sph_outputs(sph()), 0x30, 32, [(1, 0)])
        self.assertEqual(lay, [0, 0x40, 0x900, 0x900 + 2 * 7 * 0x80])
        self.assertEqual(words, (lay[3] + 4 + 3 * 32 + 0xf) // 4 + 4)

    def test_primitive_sph(self):   # control +0x64: its offset in the code area, when +0x68 bit 7 is set
        ctl = bytearray(0x70); ctl[0x64:0x68] = (0x100).to_bytes(4, 'little'); code = bytes(0x100) + sph(0, (0x1,))
        self.assertIsNone(ms.prim_sph(bytes(ctl), code))
        ctl[0x68] = 0x80
        self.assertEqual(ms.sph_outputs(ms.prim_sph(bytes(ctl), code)), [(0, 0)])


class Stage(unittest.TestCase):
    def mesh_info(self, task=True, pcomps=((1, 0),)):   # per-primitive generic 1: locations are shared with the vertex outputs, as in GLSL
        comps = ms.sph_outputs(sph())
        lay, words = ms.layout('ms', 64, comps, 0x30, 32, list(pcomps))
        return dict(threads=32, payload=0x30, mesh_layout=lay, max_v=64, max_p=32, comps=comps, pcomps=list(pcomps), task=task), words

    def kernel(self, words, out_word):
        return KERNEL % (words, out_word)

    def test_task(self):
        lay, words = ms.layout('ts', 0, [], 0x30)
        src = ms.to_stage(self.kernel(words, lay[2] // 4), 'ts', dict(threads=32, payload=0x30, mesh_layout=lay))
        self.assertIn('[[object]] void main0(', src)
        self.assertIn('dm_mgp.set_threadgroups_per_grid(uint3(min(_18[%d], 1024u), 1u, 1u));' % (lay[2] // 4 + 1), src)   # the launch count at payload +4
        self.assertIn('_18[0] = dm_first[0] + dm_tg;', src)
        self.compile(src)

    def test_mesh(self):
        info, words = self.mesh_info()
        src = ms.to_stage(self.kernel(words, info['mesh_layout'][2] // 4), 'ms', info)
        self.assertIn('using dm_mesh_t = metal::mesh<dm_vertex_t, dm_prim_t, 64, 32, metal::topology::triangle>;', src)
        self.assertIn('float4 a0 [[user(locn0)]];', src)
        self.assertIn('[[mesh]] void main0(', src); self.assertIn('[[mesh]] void main0_z(', src)
        gl, z = src.split('[[mesh]] void main0_z(')
        self.assertIn('o.position.z = fma(o.position.z, 0.5, o.position.w * 0.5);', gl)   # NVN's GL depth in main0 only
        self.assertNotIn('fma(o.position.z', z)
        self.assertIn('const object_data dm_payload_t& dm_pl [[payload]]', z)
        self.assertIn('dm_out.set_primitive(p, q);', z)
        self.compile(src)

    def test_mesh_without_task(self):   # it counts its groups from the draw's first itself, and reads no payload
        info, words = self.mesh_info(task=False, pcomps=())
        src = ms.to_stage(self.kernel(words, info['mesh_layout'][2] // 4), 'ms', info)
        self.assertNotIn('[[payload]]', src)
        self.assertIn('_18[0] = dm_first[0] + dm_tg;', src)
        self.assertIn('metal::mesh<dm_vertex_t, void, 64, 32', src)
        self.compile(src)

    def test_own_shared_memory_refused(self):   # it would lie at 0 with the stage memory
        info, words = self.mesh_info()
        with self.assertRaises(ValueError): ms.to_stage(self.kernel(words, 576), 'ms', dict(info, own_shared=True))

    def compile(self, src):
        if os.environ.get('DMSH_TEST_METAL') != '1': return
        with tempfile.TemporaryDirectory(prefix='meshstage_') as d:
            path = Path(d) / 'k.metal'; path.write_text(src)
            for sdk in ('macosx', 'iphoneos'):
                r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-fmodules-cache-path=' + d + '/modules', str(path), '-o', str(path.with_suffix('.' + sdk + '.air'))],
                                   capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr[-3000:])


if __name__ == '__main__':
    unittest.main()
