"""MT Framework shader packages in synthetic ARC v7 and v9 archives."""
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


def headed_package(shaders, h=b'\xf2R\xa3c'):
    # Resident Evil HD Remaster: each record is binding data of its own length, a 0x20-byte header, the control section, the code area.
    package = bytearray(b'SPK\0' + h + bytes(24))
    for i, (_, ctl, code) in enumerate(shaders):
        package += bytes(8 + 4 * i) + struct.pack('<4s7I', h, len(ctl) + len(code), 0x10003, 4, 0, len(ctl), len(code), len(ctl)) + ctl + code
    return bytes(package + bytes(20))


def archive_v9(package):
    # Every member is a zlib stream, the short one too; entries are 0x54 bytes with a u32 before the offset, the first member at 0x8000.
    rows = [(b'misc', b'hello'), (b'sc\\NX_64\\root', package)]
    table, data = bytearray(struct.pack('<4sHH', b'ARC\0', 9, len(rows))), bytearray()
    for path, plain in rows:
        stored = zlib.compress(plain)
        table.extend(struct.pack('<64s5I', path, 0x02358e1a, len(stored), len(plain), 2, 0x8000 + len(data)))
        data.extend(stored)
    return bytes(table.ljust(0x8000, b'\0') + data)


class MTFrameworkV9Tests(unittest.TestCase):
    def test_v9_archive_and_headed_package(self):
        shaders = [('vs', *shader('vs', 0x100)), ('ps', *shader('ps', 0x180)), ('vs', *shader('vs', 0x80))]
        self.assertEqual([(p, len(m)) for p, m in split.mtarc_members(archive_v9(b'x'), 'fixture')][:1], [('misc', 5)])
        found = []
        split.mtarc_shaders(archive_v9(headed_package(shaders)), '/fixture.arc', found)
        self.assertEqual([(stage, ctl, code) for _, stage, ctl, code in found], shaders)

    def test_v9_equal_length_zlib_stored_and_empty_members(self):
        equal_plain = bytes(11)
        equal_stored = zlib.compress(equal_plain)
        self.assertEqual(len(equal_stored), len(equal_plain))
        stored_x = b'x\x9cnot-zlib!'
        rows = [
            (b'empty', b'', 0, 0),
            (b'equal_z', equal_stored, len(equal_plain), None),
            (b'stored_x', stored_x, len(stored_x), None),
        ]
        table, data = bytearray(struct.pack('<4sHH', b'ARC\0', 9, len(rows))), bytearray()
        for path, stored, plain_len, off in rows:
            entry_off = 0 if off == 0 else 0x8000 + len(data)
            table.extend(struct.pack('<64s5I', path, 1, len(stored), plain_len, 2, entry_off))
            data.extend(stored)
        arc = bytes(table.ljust(0x8000, b'\0') + data)
        self.assertEqual(
            list(split.mtarc_members(arc, 'fixture')),
            [('empty', b''), ('equal_z', equal_plain), ('stored_x', stored_x)],
        )

    def test_v9_directory_bounds_and_table_overlap(self):
        # Member offset inside the directory table.
        data = bytearray(archive_v9(b'SPK\0'))
        struct.pack_into('<I', data, 8 + 64 + 16, 8)
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] fixture: ARC v9'):
            list(split.mtarc_members(bytes(data), 'fixture'))
        # Member running past EOF.
        data = bytearray(archive_v9(b'SPK\0'))
        struct.pack_into('<I', data, 8 + 64 + 4, len(data))
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] fixture: ARC v9'):
            list(split.mtarc_members(bytes(data), 'fixture'))
        # Directory table running past EOF.
        truncated = struct.pack('<4sHH', b'ARC\0', 9, 4) + bytes(0x54)
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] fixture: ARC v9'):
            list(split.mtarc_members(truncated, 'fixture'))

    def test_arc_zlib_inflation_is_bounded_and_rejects_truncated(self):
        bomb = zlib.compress(bytes(8 << 20))
        cases = [
            ('v7_bomb', 7, bomb, 0x20),
            ('v9_bomb', 9, bomb, 0x20),
            ('v7_trunc', 7, zlib.compress(b'hello world' * 8)[:-4], len(b'hello world' * 8)),
            ('v9_trunc', 9, zlib.compress(b'hello world' * 8)[:-4], len(b'hello world' * 8)),
        ]
        for name, ver, stored, usz in cases:
            with self.subTest(name=name):
                if ver == 7:
                    off = 8 + 64 + 16
                    arc = struct.pack('<4sHH64s4I', b'ARC\0', 7, 1, b'item', 1, len(stored), usz, off) + stored
                else:
                    off = 8 + 64 + 20
                    arc = struct.pack('<4sHH64s5I', b'ARC\0', 9, 1, b'item', 1, len(stored), usz, 2, off) + stored
                with self.assertRaisesRegex(SystemExit, r'^\[RED\] fixture#item'):
                    list(split.mtarc_members(arc, 'fixture'))

    def test_headed_package_rejects_a_control_without_its_header(self):
        shaders = [('vs', *shader('vs', 0x100)), ('ps', *shader('ps', 0x100))]
        package = bytearray(headed_package(shaders))
        second = package.rfind(split.CTL_MAGIC)
        package[second - 0x20:second - 0x1c] = b'nope'
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] fixture SPK: control section at .* without a shader header'):
            split.mtspk_shaders(bytes(package), 'fixture')

    def test_headed_package_rejects_overlapping_headers_and_malformed_records(self):
        h = b'\xf2R\xa3c'
        ctl0, code0 = shader('vs', 0x100)
        ctl1, code1 = shader('ps', 0x100)
        # First header cannot overlap SPK +4..+8 (self-matching d[4:8] at c == 0x24): trailing bytes must not be accepted as a headed package.
        self_match = b'SPK\0' + struct.pack('<4s7I', h, len(ctl0) + len(code0), 0, 0, 0, len(ctl0), len(code0), len(ctl0)) + ctl0 + code0 + bytes(20)
        self.assertFalse(split.mtspk_headed(self_match, 0x24))
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] fixture SPK'):
            split.mtspk_shaders(self_match, 'fixture')
        # Second shader header overlapping the first shader's code area (gap < 0x20).
        hdr1 = struct.pack('<4s7I', h, len(ctl1) + len(code1), 0x10003, 4, 0, len(ctl1), len(code1), len(ctl1))
        code0_overlap = bytearray(code0)
        code0_overlap[-0x10:] = hdr1[:0x10]
        pkg_overlap = b'SPK\0' + h + bytes(24) + bytes(8) + struct.pack('<4s7I', h, len(ctl0) + len(code0), 0x10003, 4, 0, len(ctl0), len(code0), len(ctl0)) + ctl0 + bytes(code0_overlap) + hdr1[0x10:] + ctl1 + code1
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] fixture SPK: control section at .* without a shader header'):
            split.mtspk_shaders(pkg_overlap, 'fixture')
        # Short control section (< 0x708) rejected by mtspk_headed.
        short_ctl = bytearray(0x708)
        short_ctl[:4] = split.CTL_MAGIC
        struct.pack_into('<I', short_ctl, 0x20, 0x40)
        struct.pack_into('<I', short_ctl, 0x704, 0x100)
        short_pkg = b'SPK\0' + h + bytes(24) + struct.pack('<4s7I', h, 0x40 + 0x100, 0, 0, 0, 0x40, 0x100, 0x40) + bytes(short_ctl)
        self.assertFalse(split.mtspk_headed(short_pkg, 0x40))
        # Stray CTL_MAGIC inside a shader's code area rejected by the total control-magic count check.
        code_stray = bytearray(code0)
        code_stray[0x80:0x84] = split.CTL_MAGIC
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] fixture SPK: 2 control magics, 1 shaders read'):
            split.mtspk_shaders(headed_package([('vs', ctl0, bytes(code_stray))], h=h), 'fixture')
