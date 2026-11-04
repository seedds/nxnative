"""MT Framework shader packages in synthetic ARC v7 archives."""
from pathlib import Path
import struct
import sys
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


def shader(stage, length):
    control, code = bytearray(0x880), bytearray(length)
    control[:4] = split.CTL_MAGIC
    struct.pack_into('<I', control, 0x20, len(control))
    struct.pack_into('<I', control, 0x704, len(code))
    code[:4] = split.GFX_MAGIC
    struct.pack_into('<I', code, 0x30, (split.STAGES.index(stage) + 1) << 10 | 1)
    return bytes(control), bytes(code)


def archive(path_size, package):
    # A stored ordinary file, then a compressed shader package. Padding is outside both members.
    rows = [(b'misc', b'hello', b'hello'), (b'sc\\NX_64\\root', zlib.compress(package), package)]
    table = bytearray(struct.pack('<4sHH', b'ARC\0', 7, len(rows)))
    data = bytearray()
    for path, stored, plain in rows:
        off = 8 + len(rows) * (path_size + 16) + len(data)
        table.extend(struct.pack(f'<{path_size}s4I', path, 1, len(stored), len(plain) | (1 << 29), off))
        data.extend(stored)
    return table + data + bytes(16)


class MTFrameworkTests(unittest.TestCase):
    def test_arc_and_shader_layouts(self):
        shaders = [('vs', *shader('vs', 0x100)), ('ps', *shader('ps', 0x180))]
        for path_size in (128, 64):
            for control_first in (False, True):
                with self.subTest(path_size=path_size, control_first=control_first):
                    package = b'SPK\0' + bytes(28)
                    for _, ctl, code in shaders:
                        package += ctl + code if control_first else code + ctl
                    found = []
                    split.mtarc_shaders(archive(path_size, package), '/fixture.arc', found)
                    self.assertEqual([(stage, ctl, code) for _, stage, ctl, code in found], shaders)
                    self.assertTrue(all('#sc/NX_64/root#MTSPK@' in tag for tag, *_ in found))

    def test_control_first_requires_adjacent_complete_records(self):
        ctl, code = shader('ps', 0x100)
        for tail in (ctl[:32], ctl + code[:-1], b'gap' + ctl + code, b'trailing'):
            with self.subTest(tail=len(tail)), self.assertRaisesRegex(SystemExit, r'^\[RED\] fixture SPK'):
                split.mtspk_shaders(b'SPK\0' + ctl + code + tail, 'fixture')

    def test_incomplete_code_first_package_is_not_shifted_one_shader(self):
        ctl, code = shader('vs', 0x100)
        _, next_code = shader('ps', 0x100)
        # The second control is missing. Treating the first vertex shader as a table would pair its control with the pixel shader.
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] fixture SPK'):
            split.mtspk_shaders(b'SPK\0' + code + ctl + next_code, 'fixture')

    def test_empty_member_has_no_data_range(self):
        # The established wide reader yields an empty entry even if its unused offset is zero.
        for width in (128, 64):
            row = struct.Struct(f'<{width}s4I')
            entries = row.pack(b'empty', 1, 0, 0, 0) + row.pack(b'next', 1, 1, 1, 8 + 2 * row.size)
            data = struct.pack('<4sHH', b'ARC\0', 7, 2) + entries + b'A' + bytes(160)
            self.assertEqual(list(split.mtarc_members(data, 'fixture')), [('empty', b''), ('next', b'A')])

    def test_directory_cannot_read_member_from_table(self):
        data = archive(64, b'SPK\0')
        struct.pack_into('<I', data, 8 + 64 + 12, 8)
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] fixture'):
            list(split.mtarc_members(data, 'fixture'))
