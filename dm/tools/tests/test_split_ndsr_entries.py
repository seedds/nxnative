"""shader_split: an NDSR pack is a list of 8-byte entries (ndsr_shaders). A 4-byte data entry between two programs puts the next program on an odd entry, which a walk of two
entries at a time stepped over. Synthetic bytes only."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'dm/gfx/tools'))
import shader_split as split


def ctl(code_len):
    c = bytearray(0xe0)
    c[:4] = split.CTL_MAGIC; c[4:8] = b'\xc9\0\0\0'
    struct.pack_into('<II', c, 0x40, code_len, code_len)
    c[0x56] = 5   # compute
    struct.pack_into('<3I', c, 0x58, 32, 1, 1)
    return c


def pack(start, data_after=(0,)):
    """3 compute programs from +start, each a control entry then a code entry; 4 bytes of data (type 9) follow the programs whose index is in data_after"""
    d = bytearray(0x800)
    d[:4] = b'NDSR'; struct.pack_into('<I', d, 4, 0x10100)
    words, ctl_at, code_at = [], 0x200, 0x400
    for i in range(3):
        code = bytes([0x19, 0x79, i]) + bytes(0xfd)
        d[code_at:code_at + len(code)] = code
        d[ctl_at:ctl_at + 0xe0] = ctl(len(code))
        words += [ctl_at, 0xe0 << 4 | 6, code_at, len(code) << 4 | 7]
        if i in data_after: words += [0x1f0, 4 << 4 | 9]
        ctl_at += 0xe0; code_at += 0x100
    struct.pack_into(f'<{len(words)}I', d, start, *words)
    return bytes(d)


class NdsrEntries(unittest.TestCase):
    def codes(self, d):
        found = split.ndsr_shaders(d, 'Lib/agl/x.pck')
        self.assertEqual({st for _, st, _, _ in found}, {'cs'})
        return [code[2] for _, _, _, code in found]

    def test_a_program_on_an_odd_entry(self):
        for start in (0x10, 0x14):
            with self.subTest(start=start):
                self.assertEqual(self.codes(pack(start)), [0, 1, 2])

    def test_two_data_entries_in_a_row(self):
        for start in (0x10, 0x14):
            with self.subTest(start=start):
                self.assertEqual(self.codes(pack(start, data_after=(0, 1))), [0, 1, 2])

    def test_no_data(self):
        self.assertEqual(self.codes(pack(0x14, data_after=())), [0, 1, 2])


if __name__ == '__main__':
    unittest.main()
