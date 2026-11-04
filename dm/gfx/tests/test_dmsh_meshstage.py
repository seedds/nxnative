#!/usr/bin/env python3
"""NVN2 task and mesh shaders as Metal stage functions (dmsh_meshstage.py), on the shape of Mario Kart World's: the control section's facts, the SPH's outputs, the stage memory's layout
and the wrapper's entry points. When the Metal compiler is installed, the stages compile for macOS and iOS."""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_abi as abi
import dmsh_meshstage as ms

HEAD = """#include <metal_stdlib>
using namespace metal;
template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    thread T& operator [] (size_t pos) thread { return elements[pos]; }
    threadgroup T& operator [] (size_t pos) threadgroup { return elements[pos]; }
};
struct B { uint _m0[4]; };
struct spvDescriptorSetBuffer0 { constant B* cp_c3 [[id(0)]]; };
"""
KERNEL = HEAD + """
kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    threadgroup spvUnsafeArray<uint, %d> _23;
    if (gl_LocalInvocationID.x != 0u) return;
    _23[%d] = (*spvDescriptorSet0.cp_c3)._m0[0];
}
"""


def sph(pos=0xf0, generics=b''):
    s = bytearray(0x80); s[0x35] = pos; s[0x36:0x36 + len(generics)] = generics
    return bytes(s)


def control(stage, threads=32, payload=256, task=0, max_v=0, max_p=0, prim_at=0):
    c = bytearray(0x80); c[0x56] = stage; struct.pack_into('<2H', c, 0x58, threads, payload); c[0x5e] = task; struct.pack_into('<2H', c, 0x60, max_v, max_p)
    if prim_at: struct.pack_into('<I', c, 0x64, prim_at); c[0x68] = 0x80
    return bytes(c)


class Facts(unittest.TestCase):
    def test_stage_byte(self):
        self.assertEqual([ms.is_stage(control(s)) for s in (1, 5, 6, 7)], [None, None, 'ts', 'ms'])

    def test_outputs_follow_the_omap(self):   # 0x36: a nibble an attribute, low first. ddc034e8e009ebfc's 0x30: generic 1's x and y
        self.assertEqual(ms.sph_outputs(sph(0xf0, b'\x30')), [(None, 0), (None, 1), (None, 2), (None, 3), (1, 0), (1, 1)])
        self.assertEqual(ms.sph_outputs(sph(0x30, b'\x01')), [(None, 0), (None, 1), (0, 0)])

    def test_per_primitive_sph(self):   # after the instructions, at control +0x64 where +0x68 bit 7 is set; the position is not a primitive's
        area = sph(0xf0, b'\x30') + bytes(0x100) + sph(0x00, b'\x01')
        i = ms.info(control(7, 8, 1408, 1, 32, 16, 0x180), area)
        self.assertEqual((i['kind'], i['threads'], i['payload'], i['task'], i['max_v'], i['max_p'], i['primitive']), ('ms', 8, 1408, True, 32, 16, [(0, 0)]))
        self.assertEqual(ms.info(control(7, max_v=128, max_p=64), area)['primitive'], [])

    def test_task_memory(self):   # the payload's bytes hold the mesh count at +4, as the game's stores do
        lay, size = ms.memory(ms.info(control(6), bytes(0x80)))
        self.assertEqual(lay, [0, 0x10, 0x110, 0x210]); self.assertEqual(size, 0x210)

    def test_mesh_memory(self):   # 128 vertices: four blocks of 32. Six components a block, 0x80 bytes each; the primitive bytes follow, 3 a triangle after the count's word
        i = ms.info(control(7, payload=128, max_v=128, max_p=64), sph(0xf0, b'\x30'))
        lay, size = ms.memory(i)
        self.assertEqual(lay[2], 0x10 + 0x80); self.assertEqual(lay[3], lay[2] + 4 * 6 * 0x80); self.assertEqual(size, lay[3] + 4 + 3 * 64)

    def test_largest_of_the_game_fits_metal(self):   # c34e01565dac1124: 256 vertices, 192 primitives
        i = ms.info(control(7, max_v=256, max_p=192), sph(0xf0, b'\x00\x0f'))
        self.assertLessEqual(ms.memory(i)[1], ms.METAL_MAX_SHARED)

    def test_own_shared_memory(self):
        sts = struct.pack('<QQ', 0x7988, 0); other = struct.pack('<QQ', 0x7927, 0)
        self.assertTrue(ms.own_shared(other + sts)); self.assertFalse(ms.own_shared(other * 2))


class Stage(unittest.TestCase):
    def task(self):
        i = ms.info(control(6), bytes(0x80))
        return ms.to_stage(KERNEL % (ms.memory(i)[1] // 4, ms.memory(i)[0][2] // 4 + 1), i), i

    def mesh(self, task=False, primitive=False, vertices=128, primitives=64):
        area = sph(0xf0, b'\x30') + (sph(0, b'\x01') if primitive else b'')
        i = ms.info(control(7, payload=128, task=int(task), max_v=vertices, max_p=primitives, prim_at=0x80 if primitive else 0), area)
        return ms.to_stage(KERNEL % (ms.memory(i)[1] // 4, ms.memory(i)[0][3] // 4), i), i

    def test_task_launches_what_it_wrote(self):
        src, i = self.task(); out = ms.memory(i)[0][2] // 4
        self.assertIn('[[object]] void main0(', src)
        self.assertIn(f'set_threadgroups_per_grid(uint3(min(_23[{out + 1}], {ms.METAL_MAX_GRID}u), 1u, 1u))', src)
        self.assertIn(f'dm_payload.w[i] = _23[{out}u + i]', src)
        self.assertIn(f'[[buffer({abi.MESH_FIRST_SLOT})]]', src)

    def test_body_returns_to_the_wrapper(self):   # an early return must not skip handing the outputs over
        src, _ = self.task()
        self.assertIn('dm_stage_body(spvDescriptorSet0, gl_LocalInvocationID, _23);', src)
        self.assertNotIn('kernel void', src)

    def test_memory_starts_at_zero(self):   # the count byte relies on it (InstEmitAmpereMem.cs AStageOut)
        self.assertIn('_23[i] = 0u;', self.task()[0])

    def test_mesh_has_both_depth_conventions(self):
        src, _ = self.mesh()
        gl, z = src.split('[[mesh]] void main0_z(')
        self.assertIn('v.position.z = fma(v.position.z, 0.5, v.position.w * 0.5);', gl)
        self.assertNotIn('v.position.z =', z)

    def test_mesh_outputs_and_absent_components(self):   # generic 1 has x and y: z 0 and w 1, as a vertex output starts
        src, _ = self.mesh()
        self.assertIn('float4 a1 [[user(locn1)]];', src)
        self.assertRegex(src, r'v\.a1 = float4\(as_type<float>\(_23\[b \+ 128u\]\), as_type<float>\(_23\[b \+ 160u\]\), 0\.0, 1\.0\);')
        self.assertIn('metal::mesh<dm_vertex_t, void, 128, 64, metal::topology::triangle>', src)

    def test_mesh_count_and_indices(self):
        src, i = self.mesh(); prim = ms.memory(i)[0][3]
        self.assertIn(f'uint np = min(_23[{prim >> 2}] >> 24, 64u);', src)
        self.assertIn(f'uint at = {prim + 4}u + n;', src)

    def test_mesh_with_a_task_reads_its_number_from_the_payload(self):
        self.assertIn('_23[4] = dm_tg;', self.mesh(task=True)[0])

    def test_mesh_without_a_task_counts_from_the_draws_first(self):
        src = self.mesh()[0]
        self.assertIn('_23[0] = dm_first[0] + dm_tg;', src)
        self.assertNotIn('[[payload]]', src)

    def test_per_primitive_outputs(self):
        src = self.mesh(primitive=True)[0]
        self.assertIn('metal::mesh<dm_vertex_t, dm_prim_t,', src)
        self.assertRegex(src, r'p\.p0 = float4\(as_type<float>\(_23\[b \+ 192u\]\), 0\.0, 0\.0, 1\.0\);')   # after the six vertex components

    def test_past_metal_refused(self):
        with self.assertRaises(ValueError): self.mesh(vertices=300)

    def test_no_kernel_refused(self):
        with self.assertRaises(ValueError): ms.to_stage(HEAD, ms.info(control(6), bytes(0x80)))

    def test_parameter_without_a_name_refused(self):   # the build records a ValueError as this shader's failure (dmsh_build.py); anything else stops it
        i = ms.info(control(6), bytes(0x80))
        src = (KERNEL % (ms.memory(i)[1] // 4, ms.memory(i)[0][2] // 4 + 1)).replace('main0(', 'main0(uint (&a)[4], ')
        with self.assertRaises(ValueError): ms.to_stage(src, i)

    @unittest.skipUnless(shutil.which('xcrun') and subprocess.run(['xcrun', '-sdk', 'macosx', '-f', 'metal'], capture_output=True).returncode == 0, 'no Metal compiler')
    def test_compiles(self):
        cases = [self.task()[0], self.mesh()[0], self.mesh(task=True)[0], self.mesh(primitive=True)[0]]
        with tempfile.TemporaryDirectory() as d:
            for n, src in enumerate(cases):
                f = Path(d, f's{n}.metal'); f.write_text(src)
                for sdk, target in (('macosx', 'air64-apple-macos15.0'), ('iphoneos', 'air64-apple-ios18.0')):
                    r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-target', target, '-c', str(f), '-o', str(f.with_suffix('.air'))], capture_output=True, text=True)
                    self.assertEqual(r.returncode, 0, f'{sdk} case {n}: {r.stderr[-1500:]}')


class IntegerPassThrough(unittest.TestCase):
    """dmsh_build.py fs_uint: a pixel shader that passes a flat input through to an integer target needs the integer entry point: Mario Kart World's dcb0504c844f1ecc, whose R8Uint output
    comes from its mesh stage's per-primitive attribute. A smooth input is not integer data"""
    SRC = """struct main0_out
{
    float4 m_19 [[color(0)]];
};

struct main0_in
{
    float4 m_16 [[user(locn0)%s]];
};

fragment main0_out main0(main0_in in [[stage_in]])
{
    main0_out out = {};
    float _26 = in.m_16.x;
    out.m_19.x = %s;
    return out;
}
"""

    @classmethod
    def setUpClass(cls):
        import ast, re
        tools = Path(__file__).resolve().parents[1] / 'tools'
        tree = ast.parse((tools / 'dmsh_build.py').read_text())
        scope = {'re': re, 'collections': __import__('collections')}
        exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ('fs_uint', 'int_outputs')], type_ignores=[]), 'dmsh_build.py', 'exec'), scope)
        cls.fs_uint = staticmethod(scope['fs_uint'])

    def test_flat_input_through_a_local(self):
        src, mask = self.fs_uint(self.SRC % (', flat', '_26'))
        self.assertIn('fragment main0_out_u main0_u(', src); self.assertEqual(mask, 0)

    def test_flat_input_directly(self):
        self.assertIsNotNone(self.fs_uint(self.SRC % (', flat', 'in.m_16.x'))[0])

    # Kirby Star Allies' grass shader (c44819fbd37e33dc): normal and albedo reach their outputs as the bits of floats, its material word from integer locals, one a denormal literal after SPIRV-Cross
    GBUFFER = """struct main0_out
{
    float4 m_48 [[color(0)]];
    float4 m_50 [[color(1)]];
    float4 m_52 [[color(2)]];
    float4 m_54 [[color(3)]];
};

fragment main0_out main0(main0_in in [[stage_in]])
{
    main0_out out = {};
    float _10 = in.m_1.x;
    int _20 = 0;
    int _21 = 0;
    if (_10 > 0.0)
    {
        _20 = as_type<int>(_10);
        _21 = 64;
    }
    int _30 = _20;
    int _31 = _21;
    int _32 = _31 << 1;
    out.m_48.x = as_type<float>(_30);
    out.m_50.x = as_type<float>(_30);
    out.m_52.x = as_type<float>(_31);
    out.m_52.y = as_type<float>(_32);
    out.m_52.z = %s;
    out.m_54.x = 0.0;
    return out;
}
"""

    def test_material_word_beside_float_bits(self):
        src, mask = self.fs_uint(self.GBUFFER % '8.96831e-44')
        self.assertEqual(mask, 1 << 2)
        self.assertIn('uint4 m_52 [[color(2)]];\n    float4 m_54 [[color(3)]];\n};\n\nfragment main0_out_m main0_m(', src)

    def test_float_bits_are_no_integers(self):   # every output the bits of a float: nothing to tell an integer target by, so the mask stays on the old rule, every output written through integer bits
        src, mask = self.fs_uint("""struct main0_out
{
    float4 m_48 [[color(0)]];
    float4 m_50 [[color(1)]];
};

fragment main0_out main0(main0_in in [[stage_in]])
{
    main0_out out = {};
    float _10 = in.m_1.x;
    int _20 = as_type<int>(_10);
    out.m_48.x = as_type<float>(_20);
    out.m_50.x = _10;
    return out;
}
""")
        self.assertEqual(mask, 1)

    def test_smooth_input_gets_none(self):
        self.assertIsNone(self.fs_uint(self.SRC % ('', '_26'))[0])

    def test_computed_from_a_flat_input_gets_none(self):
        self.assertIsNone(self.fs_uint(self.SRC % (', flat', '_26 * 2.0'))[0])


if __name__ == '__main__':
    unittest.main()
