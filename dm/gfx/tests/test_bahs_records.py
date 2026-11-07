"""A validated BAHS binary table excludes scan matches inside unrelated data."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


def archive():
    data = bytearray(0xa00)
    data[:4] = b'BAHS'
    struct.pack_into('<II', data, 0x18, 0x200, 0x800)
    # Two binary records and one program spanning both records.
    struct.pack_into('<II', data, 0x20, 0x48, 2)
    for record, control, offset in ((0x28, 0x100, 0), (0x48, 0x200, 0x100)):
        struct.pack_into('<4I', data, record, 0x20, 3, 0, 0)
        entry = record + 0x10
        struct.pack_into('<IIIi', data, entry, offset, 0, 0xe0, control - entry)
        data[control:control + 8] = split.CTL_MAGIC + b'\xc9\0\0\0'
        struct.pack_into('<I', data, control + 0x44, 0x100)
        data[control + 0x56] = 5
    struct.pack_into('<6I', data, 0x68, 0x18, 1, 0x10, 0, 0, 0)
    # Words elsewhere happen to look like an entry, pairing different code and ctl.
    struct.pack_into('<IIIi', data, 0x300, 0, 0, 0xe0, 0x200 - 0x300)
    data[0x800:0xa00] = b'a' * 0x100 + b'b' * 0x100
    return data


class Records(unittest.TestCase):
    def test_only_verified_records_are_shaders(self):
        found = split.bahs_shaders(archive(), 'synthetic')
        self.assertEqual([x[0] for x in found], ['synthetic#BAHS@0x38', 'synthetic#BAHS@0x58'])
        self.assertEqual([x[3] for x in found], [b'a' * 0x100, b'b' * 0x100])

    def test_unrecognized_binary_table_keeps_scan_results(self):
        data = archive()
        struct.pack_into('<I', data, 0x20, 0x44)
        self.assertEqual(len(split.bahs_shaders(data, 'synthetic')), 3)

    def test_incomplete_program_table_does_not_filter(self):
        data = archive()
        struct.pack_into('<I', data, 0x70, 0x14)
        self.assertEqual(len(split.bahs_shaders(data, 'synthetic')), 3)

    def test_real_compute_record_is_kept(self):
        found = split.bahs_shaders(archive(), 'synthetic')
        self.assertEqual([x[1] for x in found], ['cs', 'cs'])


if __name__ == '__main__':
    unittest.main()
