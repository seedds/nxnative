"""shader_split: nvn2_embedded reads NVN2 shaders a game keeps whole in a code module, each code area right before or right after its control section. The Duskbloods' main
holds RAD's Bink 2 vertex / pixel pair with the code before the section (the opening movie played black without them) and 12 compute shaders with the code after it, behind a
record of the lengths and zeros. The denser candidate is the code: the zeros before a compute section once made 12 shaders that read "Ampere instruction 000 at 0x0". No game files."""
import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz   # shader_split with a profile
ss = fz.ss

CODE = 0x200


def ctl(stage):
    """An NVN2 control section (0xe0 bytes, its own length at +0x30): +0x38 instruction length, +0x40 / +0x44 c1 start and end (the code area's length), +0x56 the stage byte (5: compute)"""
    c = bytearray(0xe0); c[:4] = ss.CTL_MAGIC; c[4:8] = b'\xc9\0\0\0'
    struct.pack_into('<I', c, 0x30, 0xe0); struct.pack_into('<4I', c, 0x38, CODE, 0, CODE, CODE); c[0x56] = stage
    return bytes(c)


def code(seed):   # compute code: non-zero 16-byte instructions (an S2R-like word), told apart by seed
    return b''.join(struct.pack('<IIII', 0x7919 + seed, i, 0x2500, 0x000e2800) for i in range(CODE // 16))


class Embedded(unittest.TestCase):
    def test_code_before_and_after(self):
        before, after = code(1), code(2)
        record = struct.pack('<IIII', 0xe0, CODE, 0xe0, 0) + bytes(CODE - 16)   # the lengths, then zeros: what lies before a section whose code follows it
        d = b'\0' * 0x40 + before + ctl(5) + record + ctl(5) + after + b'\0' * 0x40
        got = ss.nvn2_embedded(d, 'main')
        self.assertEqual([(st, k) for _, st, _, k in got], [('cs', before), ('cs', after)])
        self.assertEqual(len({ss.skey(c, k) for _, _, c, k in got}), 2)

    def test_stray_magic_adds_nothing(self):
        d = b'\0' * 0x400 + ss.CTL_MAGIC + b'\xc9\0\0\0' + b'\0' * 0x100   # a control section's first words with nothing behind them: no code either side
        self.assertEqual(ss.nvn2_embedded(d, 'main'), [])


if __name__ == '__main__':
    unittest.main()
