"""An NVN2 control section's own lengths are not a BAHS table slot: bahs_shaders scans in steps of 4 bytes, and (n, 0, n, n) at +0x38 reads as an entry when +n lands on a control section."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split

REGION, RSZ = 0x1000, 0x800
SLOT_A, SLOT_B = 0x40, 0x60
CTL_A = 0x100
LEN_A, LEN_B = 0x300, 0x100
CTL_B = CTL_A + 0x38 + LEN_A   # where the lengths of A, read as an entry, point


def control(d, at, length, stage, group):
    d[at:at + 8] = split.CTL_MAGIC + b'\xc9\0\0\0'
    struct.pack_into('<IIII', d, at + 0x38, length, 0, length, length)   # the instructions fill the code area
    d[at + 0x56] = stage
    struct.pack_into('<3I', d, at + 0x58, *group)


def bahs():
    d = bytearray(REGION + RSZ)
    d[:4] = b'BAHS'
    struct.pack_into('<II', d, 0x18, RSZ, REGION)
    control(d, CTL_A, LEN_A, 5, (16, 16, 1))
    control(d, CTL_B, LEN_B, 5, (8, 8, 1))
    struct.pack_into('<IIIi', d, SLOT_A, 0, 0, 0xe0, CTL_A - SLOT_A)
    struct.pack_into('<IIIi', d, SLOT_B, 0x400, 0, 0xe0, CTL_B - SLOT_B)
    d[REGION:REGION + 4] = bytes.fromhex('19790000')           # A's code
    d[REGION + LEN_A:REGION + LEN_A + 4] = bytes.fromhex('8104 0c02')   # what the lengths of A would point at: a vertex shader's SPH
    d[REGION + 0x400:REGION + 0x404] = bytes.fromhex('19790100')   # B's code
    return bytes(d)


class ControlLengths(unittest.TestCase):
    def test_the_layout_is_the_false_match(self):
        """Without the rule these bytes pass every other check of the scan: an in-range NVN2 control section, whose code area fits and is a multiple of 0x100 away"""
        d = bahs()
        a = CTL_A + 0x38
        off, z, csz, rel = struct.unpack_from('<IIIi', d, a)
        self.assertEqual((off, z, csz, rel), (LEN_A, 0, LEN_A, LEN_A))
        self.assertEqual(a + rel, CTL_B)
        self.assertTrue(split.nvn2_ctl(d[a + rel:a + rel + csz]))

    def test_only_the_table_slots_are_taken(self):
        out = split.bahs_shaders(bahs(), 'x')
        self.assertEqual(sorted(w for w, *_ in out), [f'x#BAHS@{SLOT_A:#x}', f'x#BAHS@{SLOT_B:#x}'])

    def test_each_control_keeps_its_own_code(self):
        by = {w: code[:4].hex() for w, _, _, code in split.bahs_shaders(bahs(), 'x')}
        self.assertEqual(by[f'x#BAHS@{SLOT_A:#x}'], '19790000')
        self.assertEqual(by[f'x#BAHS@{SLOT_B:#x}'], '19790100')


if __name__ == '__main__':
    unittest.main()
