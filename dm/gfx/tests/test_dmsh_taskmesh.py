"""Native task/mesh ABI: packed attributes, payload, and entry ownership."""

from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import dmsh_taskmesh as tm


def program(
    stage=7,
    vertices=64,
    primitives=32,
    attrs=(0x70, 0x74, 0x78, 0x7C, 0x90, 0x98),
    primitive_attrs=None,
):
    ctl = bytearray(0xD8)
    ctl[0x56] = stage
    struct.pack_into("<3H", ctl, 0x58, 32, 0x100, 0)
    ctl[0x5F] = 3
    struct.pack_into("<2H", ctl, 0x60, vertices, primitives)
    bits = sum(1 << (400 + a // 4) for a in attrs)
    code = bits.to_bytes(0x80, "little")
    if primitive_attrs is not None:
        struct.pack_into("<2I", ctl, 0x64, 0x100, 0x80)
        gs_bits = 0x010C1081 | sum(1 << (400 + a // 4) for a in primitive_attrs)
        code += bytes(0x80) + gs_bits.to_bytes(0x80, "little")
    return ctl, code


def source(meta, include_input=True):
    arrays = "\n".join(
        f"    threadgroup spvUnsafeArray<uint, {n}> {name};"
        for name, n in zip(tm._REGIONS, meta["isbe_sizes"])
        if (include_input or name != "dm_isbe_input")
        and (meta["stage"] == "ms" or name != "dm_isbe_map")
    )
    return (
        """#include <metal_stdlib>
using namespace metal;
kernel void main0(constant Args& args [[buffer(0)]], uint lane [[thread_index_in_simdgroup]])
{
"""
        + arrays
        + """
    if (lane == 7u) return;
    dm_isbe_output[lane] = 1u;
}
"""
    )


class MetadataTests(unittest.TestCase):
    def test_payload_already_includes_its_header(self):
        md = tm.metadata(*program(stage=6))
        self.assertEqual((md["stage"], md["threads"], md["payload"]), ("ts", 32, 256))
        self.assertEqual(md["isbe_sizes"], [64, 64, 1])

    def test_sparse_output_mask_has_no_packed_holes(self):
        md = tm.metadata(*program())
        self.assertEqual(md["attrs"], [0x70, 0x74, 0x78, 0x7C, 0x90, 0x98])
        self.assertEqual(md["isbe_sizes"], [64, 384, 100])
        # Vertex 32 begins the second block of six 32-lane component lines.
        self.assertEqual(tm.skew_word(31, 5, 6), 191)
        self.assertEqual(tm.skew_word(32, 0, 6), 192)
        self.assertEqual(tm.skew_word(33, 5, 6), 353)

    def test_partial_last_vertex_group_still_allocates_full_lines(self):
        md = tm.metadata(*program(vertices=33))
        self.assertEqual(md["isbe_sizes"][1], 384)

    def test_secondary_sph_adds_primitive_lines_after_all_vertex_lines(self):
        md = tm.metadata(
            *program(vertices=33, primitives=33, primitive_attrs=(0x80, 0xBC))
        )
        self.assertEqual(md["primitive_attrs"], [0x80, 0xBC])
        self.assertEqual(md["primitive_base_words"], 384)
        self.assertEqual(md["isbe_sizes"][1], 512)

    def test_truncated_secondary_sph_cannot_silently_drop_primitive_outputs(self):
        ctl, code = program(primitive_attrs=(0x80,))
        with self.assertRaisesRegex(ValueError, "primitive GS SPH"):
            tm.metadata(ctl, code[:0x80])
        damaged = bytearray(code)
        struct.pack_into("<I", damaged, 0x100, 0x020C0481)
        with self.assertRaisesRegex(ValueError, "geometry passthrough"):
            tm.metadata(ctl, damaged)

    def test_same_location_cannot_mix_vertex_and_primitive_interpolation(self):
        with self.assertRaisesRegex(ValueError, "mixes vertex and primitive"):
            tm.metadata(*program(primitive_attrs=(0x94,)))

    def test_map_count_uses_backwards_byte_store_order(self):
        self.assertEqual(tm.map_count([0x12, 0x34, 0x56, 0x78]), 0x12345678)
        self.assertEqual(tm.map_count([0, 0, 0, 192]), 192)

    def test_unknown_outputs_and_bad_limits_are_explicit(self):
        for ctl, code in (
            program(attrs=(0x60, 0x70, 0x74, 0x78, 0x7C)),
            program(vertices=257),
            program(primitives=0),
        ):
            with self.subTest(ctl=ctl.hex(), code=code.hex()):
                with self.assertRaises(ValueError):
                    tm.metadata(ctl, code)


class WrapperTests(unittest.TestCase):
    def test_early_return_stays_inside_helper_before_export_barrier(self):
        md = tm.metadata(*program())
        result = tm.wrap(source(md), md)
        helper = result[
            result.index("static inline void dm_guest") : result.index(
                "struct dm_task_payload"
            )
        ]
        self.assertIn("if (lane == 7u) return;", helper)
        self.assertIn("threadgroup spvUnsafeArray<uint, 384>& dm_isbe_output", helper)
        self.assertNotIn(
            "threadgroup spvUnsafeArray<uint, 384> dm_isbe_output;", helper
        )
        self.assertNotIn("kernel void main0", result)
        self.assertEqual(result.count("dm_guest(args, dm_lane,"), 4)
        self.assertEqual(
            result.count("    threadgroup spvUnsafeArray<uint, 384> dm_isbe_output;"), 4
        )
        self.assertEqual(result.count("dm_isbe_map);\n    threadgroup_barrier"), 4)

    def test_mesh_entries_keep_depth_and_direct_dispatch_contracts(self):
        md = tm.metadata(*program())
        result = tm.wrap(source(md), md)
        for name in ("main0", "main0_z", "main0_direct", "main0_direct_z"):
            self.assertIn("[[mesh]] void " + name + "(", result)
        self.assertEqual(
            result.count("dm_v.position.z = (dm_v.position.z + dm_v.position.w)"), 2
        )
        self.assertEqual(
            result.count(f"constant uint& dm_first [[buffer({tm.FIRST_SLOT})]]"), 2
        )
        self.assertEqual(
            result.count("const object_data dm_task_payload& dm_payload [[payload]]"), 2
        )
        self.assertIn("float4 attr1 [[user(locn1)]];", result)
        self.assertNotIn("attr0 [[", result)
        self.assertIn(
            "dm_v.attr1[2] = as_type<float>(dm_isbe_output[(dm_vtx / 32u) * 192u + 160u",
            result,
        )

    def test_object_copies_complete_payload_and_emits_declared_child_count(self):
        md = tm.metadata(*program(stage=6))
        result = tm.wrap(source(md, include_input=False), md)
        self.assertIn("[[object]] void main0(", result)
        self.assertIn("struct dm_task_payload { uint data[64]; };", result)
        self.assertIn(
            "uint dm_group = dm_first + dm_tg.x + dm_dims.x * (dm_tg.y + dm_dims.y * dm_tg.z);",
            result,
        )
        self.assertIn("dm_payload.data[dm_i] = dm_isbe_output[dm_i];", result)
        self.assertIn("uint3(dm_isbe_output[1], 1u, 1u)", result)
        self.assertNotIn("[[mesh]]", result)

    def test_primitive_attributes_use_the_mesh_primitive_output_and_packed_suffix(self):
        md = tm.metadata(
            *program(vertices=33, primitives=33, primitive_attrs=(0x80, 0xBC))
        )
        result = tm.wrap(source(md), md)
        vertex = result[
            result.index("struct dm_vertex") : result.index("struct dm_primitive")
        ]
        primitive = result[
            result.index("struct dm_primitive") : result.index("[[mesh]]")
        ]
        self.assertNotIn("user(locn0)", vertex)
        self.assertIn("float4 attr0 [[user(locn0)]];", primitive)
        self.assertIn("float4 attr3 [[user(locn3)]];", primitive)
        self.assertIn(
            "dm_prim.attr3[3] = as_type<float>(dm_isbe_output[384u + (dm_p / 32u) * 64u + 32u + (dm_p % 32u)]);",
            result,
        )
        self.assertEqual(result.count("dm_mesh.set_primitive(dm_p, dm_prim);"), 4)

    def test_anonymous_regions_or_wrong_sizes_do_not_guess(self):
        md = tm.metadata(*program())
        for bad in (
            source(md).replace("dm_isbe_output", "_27"),
            source(md).replace("uint, 384", "uint, 385"),
        ):
            with self.assertRaises(ValueError):
                tm.wrap(bad, md)


if __name__ == "__main__":
    unittest.main()
