#!/usr/bin/env python3
"""NVN2 task and mesh shaders as Metal stage functions (dmsh_meshstage.py), on the shape of Mario Kart World's: the control section's facts, the SPH's outputs, the stage memory's layout
and the wrapper's entry points. When the Metal compiler is installed, the stages compile for macOS and iOS."""
from pathlib import Path
import os
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


def control(stage, threads=32, payload=256, task=0, max_v=0, max_p=0, prim_at=0, verts=0):
    c = bytearray(0x80); c[0x56] = stage; struct.pack_into('<2H', c, 0x58, threads, payload); c[0x5e] = task; c[0x5f] = verts; struct.pack_into('<2H', c, 0x60, max_v, max_p)
    if prim_at: struct.pack_into('<I', c, 0x64, prim_at); c[0x68] = 0x80
    return bytes(c)


class Facts(unittest.TestCase):
    def test_stage_byte(self):
        self.assertEqual([ms.is_stage(control(s)) for s in (1, 5, 6, 7)], [None, None, 'ts', 'ms'])

    def test_outputs_follow_the_omap(self):   # 0x36: a nibble an attribute, low first. ddc034e8e009ebfc's 0x30: generic 1's x and y; 0x35 bit 3: point size ahead of position
        self.assertEqual(ms.sph_outputs(sph(0xf0, b'\x30')), [(None, 0), (None, 1), (None, 2), (None, 3), (1, 0), (1, 1)])
        self.assertEqual(ms.sph_outputs(sph(0x30, b'\x01')), [(None, 0), (None, 1), (0, 0)])
        self.assertEqual(ms.sph_outputs(sph(0xf8)), [('psiz', 0), (None, 0), (None, 1), (None, 2), (None, 3)])

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
        self.assertEqual(lay[2], 0x10 + 0x80); self.assertEqual(lay[3], lay[2] + 4 * 6 * 0x80); self.assertEqual(size, lay[3] + 4 + 3 * 2 * 32)   # the bytes of two primitive blocks: the vertices' four blocks do not count
        # With per-primitive attributes (Ocarina of Time's ee4ef51a4fddc28e: 128 vertices, 64 primitives, 4 primitive components): two primitive blocks follow the four vertex blocks
        ip = ms.info(control(7, payload=128, max_v=128, max_p=64, prim_at=0x80), sph(0xf0, b'\x30') + sph(0, b'\x0f'))
        self.assertEqual(ms.memory(ip)[0][3], lay[2] + (4 * 6 + 2 * 4) * 0x80)

    def test_mesh_memory_126_primitives(self):   # lanes 126 and 127 of the fourth primitive block write too (a count of 0 wraps count - 1); the vertices' two blocks come first
        i = ms.info(control(7, max_v=64, max_p=126), sph(0xf0, b'\x30'))
        lay, size = ms.memory(i)
        self.assertEqual(lay[3], lay[2] + 2 * 6 * 0x80); self.assertEqual(size, lay[3] + 4 + 3 * 4 * 32)

    def test_point_mesh_memory(self):   # a point has one index byte: one block of 32 (Ocarina of Time's 3a6340a04bf09797: point size and position, 32 vertices, 32 points)
        i = ms.info(control(7, max_v=32, max_p=32, verts=1), sph(0xf8))
        lay, size = ms.memory(i)
        self.assertEqual(lay[3], lay[2] + 5 * 0x80); self.assertEqual(size, lay[3] + 4 + 32)

    def test_primitive_system_value_has_room(self):
        # PRAGMATA 5aab38bb5ba67ef7 (and 493 more): 128 vertices of a position, 126 primitives, a per-primitive SPH enabling only the primitive array id (0x35 bit 0).
        # Its code writes vertex v at (v / 32) * 0x200 + 0x80 * c and primitive p's id at 0x800 + (p / 32) * 0x80 + 4 * (p % 32) from the outputs: the id must not
        # reach the primitive bytes (before, the count byte was 0x893, inside primitive 0's id, so every workgroup drew nothing)
        area = sph(0xf0) + sph(0x01)
        i = ms.info(control(7, payload=128, max_v=128, max_p=126, prim_at=0x80), area)
        self.assertEqual((i['primitive'], i['primitive_system']), ([], 1))
        lay, size = ms.memory(i)
        self.assertEqual(lay[2], 0x90); self.assertEqual(lay[3], 0x90 + 0x800 + 4 * 0x80)
        last_id = lay[2] + 0x800 + (125 // 32) * 0x80 + 4 * (125 % 32)
        self.assertLess(last_id + 4, lay[3] + 1)
        src = ms.to_stage(KERNEL % (size // 4, lay[3] // 4), i)
        self.assertIn('uint b = 36u + (n / 32u) * 128u + n % 32u;', src)   # the vertex blocks keep their 0x200 bytes
        self.assertIn(f'uint np = min(_23[{lay[3] // 4}] >> 24, 126u);', src)
        self.assertIn(f'uint at = {lay[3] + 4}u + n;', src)

    def test_without_per_primitive_components_unchanged(self):   # the same shader without a per-primitive SPH keeps the layout it had
        i = ms.info(control(7, payload=128, max_v=128, max_p=126), sph(0xf0))
        lay, size = ms.memory(i); self.assertEqual((lay[:4], size), ([0, 0x10, 0x90, 0x890], 0x890 + 4 + 3 * 128))   # index bytes for whole primitive blocks (d365496d); [4..6]: the vertex records (test_mesh_vertex_records)

    def test_system_values_of_an_sph(self):
        self.assertEqual(ms.sph_system(sph(0xf1)), [8]); self.assertEqual(ms.sph_system(sph(0xf0)), [])

    def test_mesh_vertex_records(self):   # PRAGMATA 6adf14e9a4100082's shape: 128 vertices of 6 components. Its non-skew attribute stores (mode 0xb8) name vertex records from
        # the end of its own SoA area, 24 bytes a vertex: [4..6] tell the translator where (AosToSoa)
        lay, _ = ms.memory(ms.info(control(7, payload=128, max_v=128, max_p=64), sph(0xf0, b'\x30')))
        self.assertEqual(lay[4:], [lay[3] - lay[2], 6, 128 * 24])
        lay, _ = ms.memory(ms.info(control(7, payload=128, max_v=128, max_p=128), sph(0xf0)))   # 4fcffc73d65dc1ed's shape: 4 components
        self.assertEqual(lay[4:], [lay[3] - lay[2], 4, 128 * 16])
        self.assertEqual(len(ms.memory(ms.info(control(6), bytes(0x80)))[0]), 4)   # a task's mode 0xb8 is its payload: no records

    def test_vertex_record_to_soa_word(self):   # InstEmitAmpereMem.cs AosToSoa's arithmetic: v = w * ceil(2^18 / n) >> 18 is w / n for every record word a Metal mesh can have
        for n in range(1, 33):
            m = ((1 << 18) + n - 1) // n
            for v in range(256):
                for c in range(n):
                    w = v * n + c; vv = (w * m) >> 18
                    self.assertEqual((vv, w - vv * n), (v, c))
        n = 6; m = ((1 << 18) + n - 1) // n; rel = 37 * 24 + 5 * 4; w = rel >> 2; v = (w * m) >> 18   # vertex 37, component 5: block 1, lane 5
        self.assertEqual((v >> 5) * n * 0x80 + (v & 31) * 4 + (w - v * n) * 0x80, 0x300 + 5 * 4 + 5 * 0x80)

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

    def mesh(self, task=False, primitive=False, vertices=128, primitives=64, pos=0xf0, verts=0, reads=None):
        area = sph(pos, b'\x30') + (sph(0, b'\x01') if primitive else b'')
        i = ms.info(control(7, payload=128, task=int(task), max_v=vertices, max_p=primitives, prim_at=0x80 if primitive else 0, verts=verts), area)
        return ms.to_stage(KERNEL % (ms.memory(i)[1] // 4, ms.memory(i)[0][3] // 4), i, reads), i

    def test_task_launches_what_it_wrote(self):
        src, i = self.task(); out = ms.memory(i)[0][2] // 4
        self.assertIn('[[object]] void main0(', src)
        self.assertIn(f'set_threadgroups_per_grid(uint3(min(_23[{out + 1}], {ms.METAL_MAX_GRID}u), 1u, 1u))', src)
        self.assertIn(f'dm_payload.w[i] = _23[{out}u + i]', src)
        self.assertIn(f'[[buffer({abi.MESH_DRAW_SLOT})]]', src)
        self.assertIn('_23[0] = dm_draw[1] + dm_tg;', src)   # the draw's {count, first}: its first task + this threadgroup

    def test_body_returns_to_the_wrapper(self):   # an early return must not skip handing the outputs over
        src, _ = self.task()
        self.assertIn('dm_stage_body(spvDescriptorSet0, gl_LocalInvocationID, _23);', src)
        self.assertNotIn('kernel void', src)

    def test_memory_starts_at_zero(self):   # the count byte relies on it (InstEmitAmpereMem.cs AStageOut)
        self.assertIn('_23[i] = 0u;', self.task()[0])

    def test_loop_guard_counter_before_the_stage_memory(self):   # dmsh_build.py loop_guard runs first: its counter comes before the array and stays in the body
        i = ms.info(control(6), bytes(0x80))
        src = (KERNEL % (ms.memory(i)[1] // 4, ms.memory(i)[0][2] // 4 + 1)).replace('{\n    threadgroup', '{\n    uint dm_lg = 0u; (void)dm_lg;\n    threadgroup', 1)
        out = ms.to_stage(src, i)
        body = out[out.index('void dm_stage_body('):]
        self.assertIn('uint dm_lg = 0u; (void)dm_lg;', body[:body.index('\n}')])
        self.assertIn('[[object]] void main0(', out)

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
        src = self.mesh(task=True)[0]
        self.assertIn('_23[4] = dm_tg;', src)
        self.assertNotIn('main0_o', src)

    def test_mesh_without_a_task_counts_from_its_blocks_first(self):   # Metal's mesh grid holds at most METAL_MAX_GRID threadgroups: an object function launches the draw in blocks
        src = self.mesh()[0]
        self.assertEqual(ms.METAL_MAX_GRID, abi.MESH_GRID)
        self.assertIn('_23[0] = dm_block.first + dm_tg;', src)
        self.assertIn('const object_data dm_block_t& dm_block [[payload]]', src)
        self.assertIn('[[object]] void main0_o(object_data dm_block_t& dm_block [[payload]]', src)
        self.assertIn(f'[[buffer({abi.MESH_DRAW_SLOT})]]', src)
        self.assertIn(f'uint b = dm_otg * {ms.METAL_MAX_GRID}u;', src)
        self.assertIn('dm_block.first = dm_draw[1] + b;', src)
        self.assertIn(f'dm_grid.set_threadgroups_per_grid(uint3(min(dm_draw[0] - b, {ms.METAL_MAX_GRID}u), 1u, 1u));', src)

    def test_per_primitive_outputs(self):   # four blocks of 6 vertex components (36 + 4 * 192 = 804 words), then the per-primitive blocks of 1 component (stride 32)
        src = self.mesh(primitive=True)[0]
        self.assertIn('metal::mesh<dm_vertex_t, dm_prim_t,', src)
        self.assertIn('uint b = 36u + (n / 32u) * 192u + n % 32u;', src)
        self.assertIn('uint b = 804u + (n / 32u) * 32u + n % 32u;', src)
        self.assertRegex(src, r'p\.p0 = float4\(as_type<float>\(_23\[b \+ 0u\]\), 0\.0, 0\.0, 1\.0\);')

    def test_point_mesh(self):   # Ocarina of Time's 2582bd7ea6490d33 / 28f82c129deea140: point size at +0 ahead of position, topology point (control +0x5f = 1)
        src = self.mesh(vertices=32, primitives=32, pos=0xf8, verts=1)[0]
        self.assertIn('float point_size [[point_size]];', src)
        self.assertIn('metal::mesh<dm_vertex_t, void, 32, 32, metal::topology::point>', src)
        self.assertIn('v.point_size = as_type<float>(_23[b + 0u]);', src)
        self.assertIn('v.position = float4(as_type<float>(_23[b + 32u]), as_type<float>(_23[b + 64u]), as_type<float>(_23[b + 96u]), as_type<float>(_23[b + 128u]));', src)
        self.assertIn('for (uint n = gl_LocalInvocationID.x; n < np * 1u; n += 32u)', src)

    def test_pad_unwritten_pixel_inputs(self):   # Ocarina of Time's foliage point mesh 3a6340a04bf09797 writes no generic output, its pixel shader 2a7c48d595feaaeb reads locn5
        src = self.mesh(primitive=True, reads={0: 1, 1: 1, 5: 0xf})[0]   # 1 is a vertex output and 0 a primitive output: only 5 is padded, as (0,0,0,1)
        self.assertIn('    float4 dm_pad5 [[user(locn5)]];\n};', src)
        self.assertIn('        v.dm_pad5 = float4(0.0, 0.0, 0.0, 1.0);\n', src)
        self.assertEqual(src.count('[[user(locn1)]]'), 1); self.assertEqual(src.count('[[user(locn0)]]'), 1)
        self.assertNotIn('dm_pad', self.mesh(primitive=True)[0])

    def test_outputs_as_wide_as_read(self):   # Ocarina of Time's 626fbf48041cb82b: 160 vertices, its float4s came to 35,840 bytes, past a Metal mesh's 32 KB
        src = self.mesh(primitive=True, reads={0: 1, 1: 1, 5: 0x7})[0]
        self.assertIn('    float2 a1 [[user(locn1)]];', src)   # x and y written, x read: what it writes
        self.assertIn('v.a1 = float2(as_type<float>(_23[b + 128u]), as_type<float>(_23[b + 160u]));', src)
        self.assertIn('    float dm_pad5 [[user(locn5)]];', src)   # no w read: Metal gives the missing y, z, w 0, the hardware's default for x, y and z
        self.assertIn('v.dm_pad5 = 0.0;', src)
        self.assertIn('    float p0 [[user(locn0)]];', src)
        self.assertIn('    float4 a1 [[user(locn1)]];', self.mesh(primitive=True, reads={1: 0xf})[0])   # w read and not written: it must hold 1
        self.assertIn('v.a1 = float4(as_type<float>(_23[b + 128u]), as_type<float>(_23[b + 160u]), 0.0, 1.0);', self.mesh(primitive=True, reads={1: 0xf})[0])
        full = self.mesh(primitive=True)[0]   # partners not known: every output a float4, as the hardware starts it
        self.assertIn('    float4 a1 [[user(locn1)]];', full); self.assertIn('    float4 p0 [[user(locn0)]];', full)

    def test_pixel_reads(self):   # c5eb985e769e317f's input map: locn0 x (linear), 3 xyzw, 4 xyz
        s = bytearray(0x80); s[0x18] = 0x03; s[0x18 + 3] = 0xaa; s[0x18 + 4] = 0x2a
        self.assertEqual(ms.pixel_reads(bytes(s)), {0: 1, 3: 0xf, 4: 0x7})

    def test_per_primitive_attribute_after_the_system_values(self):   # a primitive id (0x35 bit 0) and generic 0's x: p0 is the block's second component
        area = sph(0xf0, b'\x30') + sph(0x01, b'\x01')
        i = ms.info(control(7, payload=128, max_v=128, max_p=64, prim_at=0x80), area)
        src = ms.to_stage(KERNEL % (ms.memory(i)[1] // 4, ms.memory(i)[0][3] // 4), i)
        self.assertIn('uint b = 804u + (n / 32u) * 64u + n % 32u;', src)
        self.assertRegex(src, r'p\.p0 = float4\(as_type<float>\(_23\[b \+ 32u\]\), 0\.0, 0\.0, 1\.0\);')

    def local_word(self):   # a private variable spirv-cross initializes ahead of the stage memory: the last entry of a raw global memory lookup (GlobalToStorage.TryGenerateRawStorageOp)
        i = ms.info(control(7, payload=128, max_v=128, max_p=64), sph(0xf0, b'\x30'))
        src = (KERNEL % (ms.memory(i)[1] // 4, ms.memory(i)[0][3] // 4)).replace('{\n    threadgroup', '{\n    uint _9 = 0u;\n    threadgroup', 1)
        src = src.replace('kernel void', 'static inline __attribute__((always_inline))\nvoid _5(thread uint& _10) { _10 = _10 + 1u; }\n\nkernel void', 1).replace('    if (gl_', '    _5(_9);\n    if (gl_', 1)
        return ms.to_stage(src, i)

    def test_thread_variables_stay_in_the_body(self):
        src = self.local_word()
        body, entries = src.split('[[mesh]] void main0(')
        self.assertRegex(body, r'void dm_stage_body\([^)]*\)\n\{\n    uint _9 = 0u;\n    _5\(_9\);')
        self.assertNotIn('_9', entries)

    def test_primitive_id_handed_to_metal(self):
        # PRAGMATA 5aab38bb5ba67ef7: its per-primitive SPH enables only the primitive id (0x35 bit 0), which its code writes as (cluster << 7) | p and its fragment shader
        # 6c475973 reads as gl_PrimitiveID (the visibility buffer's cluster bits). The primitive gets it as its [[primitive_id]], from the first primitive block's first component
        # (36 + 4 * 4 * 32 words: the four vertex blocks of a position)
        area = sph(0xf0) + sph(0x01)
        i = ms.info(control(7, payload=128, max_v=128, max_p=126, prim_at=0x80), area)
        self.assertEqual(i['primitive_builtins'], [8])
        src = ms.to_stage(KERNEL % (ms.memory(i)[1] // 4, ms.memory(i)[0][3] // 4), i)
        self.assertIn('struct dm_prim_t {\n    uint dm_pid [[primitive_id]];\n};', src)
        self.assertIn('metal::mesh<dm_vertex_t, dm_prim_t, 128, 126,', src)
        self.assertEqual(src.count('uint b = 548u + (n / 32u) * 32u + n % 32u;'), 2)   # both depth conventions
        self.assertEqual(src.count('p.dm_pid = _23[b + 0u];'), 2)
        self.assertEqual(src.count('dm_out.set_primitive(n, p);'), 2)

    def test_primitive_id_beside_an_attribute(self):   # the id first, generic 0's x after it in the block
        area = sph(0xf0, b'\x30') + sph(0x01, b'\x01')
        i = ms.info(control(7, payload=128, max_v=128, max_p=64, prim_at=0x80), area)
        src = ms.to_stage(KERNEL % (ms.memory(i)[1] // 4, ms.memory(i)[0][3] // 4), i)
        self.assertIn('    uint dm_pid [[primitive_id]];\n    float4 p0 [[user(locn0)]];\n', src)
        self.assertIn('p.dm_pid = _23[b + 0u];', src)
        self.assertRegex(src, r'p\.p0 = float4\(as_type<float>\(_23\[b \+ 32u\]\), 0\.0, 0\.0, 1\.0\);')

    def test_layer_and_viewport_keep_room_only(self):   # 0x35 bits 1 and 2: room in the block, no Metal output (no game's mesh shader writes them)
        i = ms.info(control(7, payload=128, max_v=128, max_p=64, prim_at=0x80), sph(0xf0) + sph(0x07))
        self.assertEqual((i['primitive_system'], i['primitive_builtins']), (3, [8, 9, 10]))
        src = ms.to_stage(KERNEL % (ms.memory(i)[1] // 4, ms.memory(i)[0][3] // 4), i)
        self.assertNotIn('render_target_array_index', src); self.assertNotIn('viewport_array_index', src)
        self.assertIn('uint b = 548u + (n / 32u) * 96u + n % 32u;', src)

    def test_no_primitive_id_without_its_bit(self):   # a mesh shader without per-primitive components hands Metal no primitive: its fragment reads Metal's own index
        src = self.mesh()[0]
        self.assertNotIn('dm_prim_t', src); self.assertNotIn('[[primitive_id]]', src)

    def test_past_metal_refused(self):
        with self.assertRaises(ValueError): self.mesh(vertices=300)

    def test_no_kernel_refused(self):
        with self.assertRaises(ValueError): ms.to_stage(HEAD, ms.info(control(6), bytes(0x80)))

    def test_parameter_without_a_name_refused(self):   # the build records a ValueError as this shader's failure (dmsh_build.py); anything else stops it
        i = ms.info(control(6), bytes(0x80))
        src = (KERNEL % (ms.memory(i)[1] // 4, ms.memory(i)[0][2] // 4 + 1)).replace('main0(', 'main0(uint (&a)[4], ')
        with self.assertRaises(ValueError): ms.to_stage(src, i)

    def test_position_is_invariant(self):   # two mesh shaders of one geometry must agree: a pre-pass and a colour pass testing depth for equality
        self.assertIn('float4 position [[position, invariant]];', self.mesh()[0])

    @unittest.skipUnless(shutil.which('xcrun') and subprocess.run(['xcrun', '-sdk', 'macosx', '-f', 'metal'], capture_output=True).returncode == 0, 'no Metal compiler')
    def test_compiles(self):
        i = ms.info(control(7, payload=128, max_v=128, max_p=126, prim_at=0x80), sph(0xf0) + sph(0x01, b'\x01'))   # the primitive id and an attribute
        cases = [self.task()[0], self.mesh()[0], self.mesh(task=True)[0], self.mesh(primitive=True)[0], self.mesh(vertices=32, primitives=32, pos=0xf8, verts=1, reads={5: 0x7})[0], self.mesh(primitive=True, reads={0: 1, 1: 0xf, 5: 0xf})[0], self.local_word(), ms.to_stage(KERNEL % (ms.memory(i)[1] // 4, ms.memory(i)[0][3] // 4), i)]
        with tempfile.TemporaryDirectory() as d:
            for n, src in enumerate(cases):
                f = Path(d, f's{n}.metal'); f.write_text(src)
                for sdk, target in (('macosx', 'air64-apple-macos15.0'), ('iphoneos', 'air64-apple-ios18.0')):
                    r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-target', target, '-c', str(f), '-o', str(f.with_suffix('.air'))], capture_output=True, text=True)
                    self.assertEqual(r.returncode, 0, f'{sdk} case {n}: {r.stderr[-1500:]}')


# The fragment stage reads the mesh's [[primitive_id]]: a mesh function from to_stage whose body writes two full-screen triangles with the ids 1000 + 77 and 1000 + 99 (the
# second drawn last), and a fragment function that returns its [[primitive_id]] into an R32Uint target. Metal's own index would read 1
GPU_KERNEL = HEAD + """
kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    threadgroup spvUnsafeArray<uint, %(words)d> _23;
    if (gl_LocalInvocationID.x != 0u) return;
    float4 pos[3] = { float4(-1.0, -1.0, 0.5, 1.0), float4(3.0, -1.0, 0.5, 1.0), float4(-1.0, 3.0, 0.5, 1.0) };
    for (uint v = 0u; v < 3u; v++) for (uint c = 0u; c < 4u; c++) _23[%(out)du + c * 32u + v] = as_type<uint>(pos[v][c]);
    _23[%(pid)du] = 1077u; _23[%(pid)du + 1u] = 1099u;
    _23[%(count)du] = 2u << 24;
    _23[%(count)du + 1u] = 0x00020100u; _23[%(count)du + 2u] = 0x00000201u;
}
fragment uint fs(uint id [[primitive_id]]) { return id; }
"""
GPU_HOST = r"""
#import <Metal/Metal.h>
#include <stdio.h>
int main(int argc, char **argv) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); NSError *e = nil;
    id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e]; if (!lib) { printf("library: %s\n", e.description.UTF8String); return 1; }
    MTLMeshRenderPipelineDescriptor *m = [MTLMeshRenderPipelineDescriptor new]; m.meshFunction = [lib newFunctionWithName:@"main0_z"]; m.fragmentFunction = [lib newFunctionWithName:@"fs"];
    m.colorAttachments[0].pixelFormat = MTLPixelFormatR32Uint;
    id<MTLRenderPipelineState> ps = [d newRenderPipelineStateWithMeshDescriptor:m options:0 reflection:nil error:&e]; if (!ps) { printf("pipeline: %s\n", e.description.UTF8String); return 1; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared; id<MTLTexture> t = [d newTextureWithDescriptor:td];
    id<MTLBuffer> b = [d newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [[d newCommandQueue] commandBuffer]; MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
    rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp]; [en setRenderPipelineState:ps];
    for (int k = 0; k < 31; k++) [en setMeshBuffer:b offset:0 atIndex:k];
    [en drawMeshThreadgroups:MTLSizeMake(1, 1, 1) threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1) threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
    [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
    uint32_t px[16]; [t getBytes:px bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
    for (int i = 0; i < 16; i++) printf("%u ", px[i]); printf("\n"); return 0; } }
"""


@unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') == '1' and shutil.which('xcrun'), 'set DMSH_TEST_GPU=1 (draws on the GPU)')
class PrimitiveIdOnTheGpu(unittest.TestCase):
    def test_fragment_reads_the_mesh_shaders_id(self):
        i = ms.info(control(7, payload=128, max_v=32, max_p=2, prim_at=0x80), sph(0xf0) + sph(0x01))
        # The guest's addresses: one block of 32 vertices of the 4 position components, then the primitive block, whose component 0 is the id (a word per primitive)
        lay, size = ms.memory(i); out = lay[2] // 4; pid = out + 4 * ms.BLOCK
        src = ms.to_stage(GPU_KERNEL % {'words': size // 4, 'out': out, 'pid': pid, 'count': lay[3] // 4}, i)
        with tempfile.TemporaryDirectory() as d:
            f = Path(d, 'm.metal'); f.write_text(src); lib = Path(d, 'm.metallib'); host = Path(d, 'h.m'); host.write_text(GPU_HOST); exe = Path(d, 'h')
            r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', str(f), '-o', str(lib)], capture_output=True, text=True); self.assertEqual(r.returncode, 0, r.stderr[-1500:])
            r = subprocess.run(['clang', '-fobjc-arc', '-framework', 'Metal', '-framework', 'Foundation', str(host), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-1500:])
            r = subprocess.run([str(exe), str(lib)], capture_output=True, text=True); self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertEqual(r.stdout.split(), ['1099'] * 16)


class IntegerPassThrough(unittest.TestCase):
    """dmsh_entry.py fs_uint: a pixel shader that passes a flat input through to an integer target needs the integer entry point: Mario Kart World's dcb0504c844f1ecc, whose R8Uint output
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
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools')); import dmsh_entry
        cls.fs_uint = staticmethod(dmsh_entry.fs_uint)

    def test_flat_input_through_a_local(self):
        self.assertIn('fragment main0_out_u main0_u(', self.fs_uint(self.SRC % (', flat', '_26')))

    def test_flat_input_directly(self):
        self.assertIsNotNone(self.fs_uint(self.SRC % (', flat', 'in.m_16.x')))

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

    def test_material_word_beside_float_bits(self):   # its G-buffer's colour 2 is RGBA8Uint between float targets: the pipeline's formats say so, main0_u switches each colour
        src = self.fs_uint(self.GBUFFER % '8.96831e-44')
        for n, nm in enumerate(('m_48', 'm_50', 'm_52', 'm_54')):
            self.assertIn(f'    float4 {nm} [[color({n}), function_constant(dm_uf{n})]];\n    uint4 {nm}_ui [[color({n}), function_constant(dm_ui{n})]];\n', src)
            self.assertIn(f'    if (dm_ui{n}) r.{nm}_ui = as_type<uint4>(out.{nm}); else r.{nm} = out.{nm};\n', src)

    def test_smooth_input_gets_none(self):
        self.assertIsNone(self.fs_uint(self.SRC % ('', '_26')))

    def test_computed_from_a_flat_input_gets_none(self):
        self.assertIsNone(self.fs_uint(self.SRC % (', flat', '_26 * 2.0')))


if __name__ == '__main__':
    unittest.main()
