"""NVN2 BNSH task and mesh shader extraction without game data."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


def ctl(stage, n):
    c = bytearray(0xe0)
    c[:4] = split.CTL_MAGIC; c[4:8] = b'\xc9\0\0\0'
    struct.pack_into('<II', c, 0x40, n, n)
    c[0x56] = stage
    return bytes(c)


def bnsh(slots):
    """Build one variation with shader records or raw words in its stage slots."""
    d = bytearray(0x2000)
    d[:4] = b'BNSH'; d[0x60:0x64] = b'grsc'
    struct.pack_into('<IQ', d, 0x7c, 1, 0x100)
    struct.pack_into('<Q', d, 0x100 + 0x10, 0x200)
    at = 0x400
    for si, rec in slots.items():
        if isinstance(rec, int):
            struct.pack_into('<Q', d, 0x200 + 8 + 8 * si, rec)
            continue
        stage, b = rec
        code = bytes([b]) * 0x100
        c = ctl(stage, len(code))
        sc, pc, pk = at, at + 0x20, at + 0x100
        d[pc:pc + len(c)] = c; d[pk:pk + len(code)] = code
        struct.pack_into('<QQII', d, sc + 8, pc, pk, len(code), len(c))
        struct.pack_into('<Q', d, 0x200 + 8 + 8 * si, sc)
        at += 0x300
    return bytes(d)


class Nvn2SlotTests(unittest.TestCase):
    def test_task_and_mesh_slots(self):
        found = split.bnsh_shaders(bnsh({4: (0, 1), 7: (6, 2), 8: (7, 3)}), 0, 'Shader/x.bfsha')
        self.assertEqual(sorted(st for _, st, _, _ in found), ['ms', 'ps', 'ts'])
        self.assertEqual({st: code[0] for _, st, _, code in found}, {'ps': 1, 'ts': 2, 'ms': 3})

    def test_other_words_left_alone(self):
        d = bnsh({4: (0, 1), 6: 0x10_0000, 8: 0x40})
        found = split.bnsh_shaders(d, 0, 'x')
        self.assertEqual([st for _, st, _, _ in found], ['ps'])

    def test_six_stages_as_before(self):
        found = split.bnsh_shaders(bnsh({0: (0, 4), 4: (0, 5)}), 0, 'x')
        self.assertEqual([st for _, st, _, _ in found], ['vs', 'ps'])


if __name__ == '__main__':
    unittest.main()
