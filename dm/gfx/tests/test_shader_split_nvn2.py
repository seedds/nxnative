"""NVN2 stage metadata and BAHS descriptors must identify real programs."""

from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import shader_split as split


def control(stage, length=0x100):
    ctl = bytearray(0xD8)
    struct.pack_into("<II", ctl, 0, 0x98761234, 0xC9)
    struct.pack_into("<I", ctl, 0x30, len(ctl))
    struct.pack_into("<I", ctl, 0x44, length)
    ctl[0x56] = stage
    return ctl


def code(shader_type=1):
    data = bytearray(0x100)
    struct.pack_into("<I", data, 0, 0x020C0081 | (shader_type << 10))
    return data


class Nvn2StageTests(unittest.TestCase):
    def test_task_and_mesh_do_not_inherit_the_sph_stage(self):
        for shader_type in (1, 3):
            with self.subTest(shader_type=shader_type):
                self.assertEqual(split.nvn2_stage(control(6), code(shader_type)), "ts")
                self.assertEqual(split.nvn2_stage(control(7), code(shader_type)), "ms")

    def test_ordinary_graphics_and_headerless_compute(self):
        self.assertEqual(split.nvn2_stage(control(0), code(1)), "vs")
        self.assertEqual(split.nvn2_stage(control(1), code(5)), "ps")
        self.assertEqual(
            split.nvn2_stage(control(5), bytes.fromhex("19790000") + bytes(252)), "cs"
        )
        self.assertIsNone(split.nvn2_stage(bytes(0x48), b""))

    def test_bnsh_slot_does_not_hide_the_new_stages(self):
        for stage, expected in ((6, "ts"), (7, "ms")):
            data = bytearray(0x340)
            data[0x60:0x64] = b"grsc"
            struct.pack_into("<IQ", data, 0x7C, 1, 0x90)
            struct.pack_into("<Q", data, 0xA0, 0xC0)
            struct.pack_into("<Q", data, 0xC8, 0x100)
            struct.pack_into("<QQII", data, 0x108, 0x140, 0x240, 0x100, 0xD8)
            data[0x140:0x218] = control(stage)
            data[0x240:0x340] = code()
            self.assertEqual(
                split.bnsh_shaders(bytes(data), 0, "synthetic")[0][1], expected
            )


class BahsRecordTests(unittest.TestCase):
    def fixture(self):
        data = bytearray(0x400)
        data[:4] = b"BAHS"
        struct.pack_into("<II", data, 0x18, 0x100, 0x300)
        # One real variable-length binary record. Reflection data inside it
        # coincidentally points to another valid control and the same code.
        struct.pack_into("<II", data, 0x20, 0x208, 1)
        struct.pack_into("<II", data, 0x28, 0x200, 0)
        struct.pack_into("<IIIi", data, 0x38, 0, 0, 0xD8, 0x10)
        data[0x48:0x120] = control(0)
        struct.pack_into("<IIIi", data, 0x130, 0, 0, 0xD8, 0x20)
        data[0x150:0x228] = control(5)
        data[0x300:0x400] = code()
        return data

    def test_reflection_metadata_is_not_an_extra_shader(self):
        data = self.fixture()
        shaders = split.bahs_shaders(bytes(data), "synthetic")
        self.assertEqual(
            [(s[0], s[1]) for s in shaders], [("synthetic#BAHS@0x38", "vs")]
        )
        self.assertEqual(split.bahs_record_entries(data, {0x38, 0x130}), {0x38})

    def test_unknown_array_layout_retains_the_scan(self):
        data = self.fixture()
        struct.pack_into("<I", data, 0x20, 0)
        self.assertIsNone(split.bahs_record_entries(data, {0x38, 0x130}))
        self.assertEqual(len(split.bahs_shaders(bytes(data), "synthetic")), 2)

    def test_incomplete_record_walk_does_not_filter(self):
        data = self.fixture()
        struct.pack_into("<I", data, 0x28, 0x1F0)
        self.assertIsNone(split.bahs_record_entries(data, {0x38, 0x130}))

    def test_records_are_bounded_by_the_array(self):
        for size, count, record_size in (
            (0x208, 1000, 0x200),
            (0x208, 1, 0x210),
            (0x208, 1, 0),
            (0x10000, 1, 0x200),
        ):
            with self.subTest(size=size, count=count, record_size=record_size):
                data = self.fixture()
                struct.pack_into("<II", data, 0x20, size, count)
                struct.pack_into("<I", data, 0x28, record_size)
                self.assertIsNone(split.bahs_record_entries(data, {0x38, 0x130}))


if __name__ == "__main__":
    unittest.main()
