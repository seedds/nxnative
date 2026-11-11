"""shader_split.snvn_records: the program records of Retro Studios' SNVN chunks. A record is u32 code length, u32 control length, a byte, u32 stream length and a stream that
inflates to the code areas and then the control sections: zlib in Tropical Freeze and Metroid Prime Remastered, zstd (the program's frame and an empty one) in Metroid Prime 4: Beyond.
A stage's 12-byte entries (+0x7e8) follow its section, and the next section starts on 16 bytes, so a control length need not be a multiple of 0x40: such records were once skipped
(28,479 of Metroid Prime Remastered's shaders). Synthetic chunks, no game files needed."""
import os
import struct
import sys
import unittest
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010012101468C000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def section(code_len, entries):   # an SDK 12 / 19 control section (0x880 bytes) with its entries after it
    c = bytearray(0x880)
    c[:4] = ss.CTL_MAGIC
    struct.pack_into('<I', c, 0x20, 0x880); struct.pack_into('<I', c, 0x704, code_len)
    struct.pack_into('<III', c, 0x7e8, 0x880, 12 * entries, entries)
    return bytes(c) + bytes([1, 0, 0x20] + [0] * 9) * entries


def program(stages):   # [(code, entries)] → (code areas, control sections with the 16-byte step between them)
    code, ctl = b''.join(c for c, _ in stages), b''
    for i, (c, n) in enumerate(stages):
        ctl += section(len(c), n)
        if i + 1 < len(stages): ctl += bytes(-len(ctl) % 16)
    return code, ctl


def chunk(records, pack):   # a chunk's data: a table the reader does not rely on, then the records
    d = bytes(16)
    for code, ctl in records:
        z = pack(code + ctl)
        d += struct.pack('<IIBI', len(code), len(ctl), 0, len(z)) + z + bytes(44)
    return d


def zstd_stream(data):   # the program's frame, then an empty frame, as the game's files have them
    z = ss.zstd_mod()
    return z.compress(data) + z.compress(b'')


CODE_A, CODE_B = bytes(range(256)) * 2, b'\x5a' * 0x300


class SnvnRecords(unittest.TestCase):
    def read(self, stages_list, pack):
        recs = [program(s) for s in stages_list]
        d = chunk(recs, pack)
        return ss.snvn_records(d, 0, len(d), 'test', '<'), recs

    def check(self, stages_list, pack):
        out, _ = self.read(stages_list, pack)
        self.assertEqual(len(out), len(stages_list))
        for (_, got), want in zip(out, stages_list):
            self.assertEqual([code for _, code in got], [c for c, _ in want])
            self.assertEqual([len(ctl) for ctl, _ in got], [0x880 + 12 * n for _, n in want])

    def test_zlib_plain(self):   # two sections back to back: control length 0x1100
        self.check([[(CODE_A, 0), (CODE_B, 0)]], zlib.compress)

    def test_zlib_lengths_not_multiples_of_0x40(self):   # 0x88c, 0x1110 (an entry after the first stage), 0x1118 (two after the second), 0x11d0
        self.check([[(CODE_A, 1)], [(CODE_A, 1), (CODE_B, 0)], [(CODE_A, 0), (CODE_B, 2)], [(CODE_A, 1), (CODE_B, 16)]], zlib.compress)

    def test_zstd(self):
        self.check([[(CODE_A, 0), (CODE_B, 2)], [(CODE_A, 1), (CODE_B, 0)], [(CODE_B, 0)]], zstd_stream)


if __name__ == '__main__':
    unittest.main()
