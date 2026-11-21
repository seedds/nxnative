"""shader_split.py's SARC reader (sarc_members): each member's name read from the name table up to its NUL, the names and data the reader gave before it found the NUL in
place, and in time that does not grow with the archive's size for each member (docs/reference/shaders.md, "SARC archives with many members")."""
import os
from pathlib import Path
import struct
import sys
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
_had = os.environ.get('DM_GAME'); os.environ.setdefault('DM_GAME', '010015100B514000')   # shader_split loads a profile at import; these tests use no game's data, so any
# profile does (tests.yml sets one for the CI); without DM_GAME, the CI's profile, set for the import only (other tests expect no default game)
import shader_split as split
if _had is None: del os.environ['DM_GAME']


def sarc(members, tail=b''):
    """A SARC: header (0x14 bytes), SFAT (0xc + 0x10 per node), SFNT (8 bytes, then the names, each NUL-terminated and padded to 4), the data. members: [(name, data)], a name
    None for a node without one, bytes for a name as it is (no NUL added: it runs on into what follows); tail: bytes after the names, before the data"""
    names, nodes, data = b'', [], b''
    for nm, blob in members:
        attr = 0
        if nm is not None:
            attr = 1 << 24 | len(names) // 4
            b = nm if isinstance(nm, bytes) else nm.encode() + b'\0'
            names += b + b'\0' * (-len(b) % 4)
        nodes.append((0, attr, len(data), len(data) + len(blob))); data += blob
    sfat = b'SFAT' + struct.pack('<HHI', 0xc, len(members), 0x65) + b''.join(struct.pack('<IIII', *n) for n in nodes)
    sfnt = b'SFNT' + struct.pack('<HH', 8, 0) + names + tail
    doff = 0x14 + len(sfat) + len(sfnt)
    return b'SARC' + struct.pack('<HHIIHH', 0x14, 0xfeff, doff + len(data), doff, 0x100, 0) + sfat + sfnt + data


def members_before(d):   # the reader as it was: each name split off the rest of the archive at every NUL (the reference for names and data)
    hl, bom, size, doff = struct.unpack_from('<HHII', d, 4)
    n = struct.unpack_from('<H', d, hl + 6)[0]
    names = hl + 12 + 16 * n + 8
    for i in range(n):
        h, attr, s, e = struct.unpack_from('<IIII', d, hl + 12 + 16 * i)
        nm = d[names + (attr & 0xffffff) * 4:].split(b'\0')[0].decode('utf-8', 'replace') if attr >> 24 else f'#{i}'
        yield nm, d[doff + s:doff + e]


class SarcMembersTests(unittest.TestCase):
    def test_names_and_data_as_before(self):
        cases = {
            'named': sarc([('Shader/a.bnsh', b'BNSH\0\0\0\0' + bytes(8)), ('b.bfsha', b'\1\2\3'), ('c', b'')]),
            'unnamed node': sarc([(None, b'x'), ('d', b'yy')]),
            'not UTF-8': sarc([(b'\xff\xfename\0', b'z')]),
            'name into the data': sarc([('e', b'1'), (b'runs', b'abc\0def')]),   # no NUL in the name table after it: the name runs into the data, up to its first NUL
            'name to the end': sarc([(b'last', b'')]),   # nothing after it: the name runs to the end of the archive
        }
        d = bytearray(sarc([('g', b'3')])); struct.pack_into('<I', d, 0x14 + 12 + 4, 1 << 24 | 0xfffff)   # a name offset beyond the archive: an empty name
        cases['offset past the end'] = bytes(d)
        for what, d in cases.items():
            with self.subTest(what):
                self.assertEqual(list(split.sarc_members(d)), list(members_before(d)))
        self.assertEqual([n for n, _ in split.sarc_members(cases['named'])], ['Shader/a.bnsh', 'b.bfsha', 'c'])
        self.assertEqual([n for n, _ in split.sarc_members(cases['unnamed node'])], ['#0', 'd'])

    def test_time_does_not_grow_with_the_archive_per_member(self):   # 1,000 members in 1 MB of NULs: splitting the rest at every NUL took seconds; reading in place, milliseconds
        d = sarc([(f'm{i}', bytes(1024)) for i in range(1000)])
        t = time.perf_counter(); got = list(split.sarc_members(d)); t = time.perf_counter() - t
        self.assertEqual((len(got), got[999][0], got[999][1]), (1000, 'm999', bytes(1024)))
        self.assertLess(t, 1.0)


if __name__ == '__main__':
    unittest.main()
