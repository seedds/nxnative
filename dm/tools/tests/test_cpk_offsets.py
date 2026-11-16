"""CRI CPK file offsets (dm/gfx/tools/cpk.py files / read): FileOffset counts from the smaller of ContentOffset and TocOffset, TocOffset counted as at most 0x800, so a
CPK whose TOC follows the header and one whose TOC is at the end of the file (LUNAR Remastered Collection) both give each file its own bytes. Synthetic bytes only."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import cpk  # noqa: E402

SIZE = {4: 4, 6: 8, 0xa: 4}   # u32, u64, string (an offset into the string pool)


def utf_table(name, cols, rows):
    """A CRI @UTF table: every column stored per row, (column name, type) in cols, one dict per row"""
    strings = bytearray(b'<NULL>\0')
    def string(s):
        o = len(strings); strings.extend(s.encode() + b'\0'); return o
    name_at = string(name)
    col_bytes = b''.join(bytes([0x50 | t]) + struct.pack('>I', string(c)) for c, t in cols)
    width = sum(SIZE[t] for _, t in cols)
    body = bytearray()
    for r in rows:
        for c, t in cols:
            body += struct.pack('>I', string(r[c])) if t == 0xa else struct.pack('>Q' if t == 6 else '>I', r[c])
    ro = 0x18 + len(col_bytes); so = ro + len(body)
    head = struct.pack('>HHIIIHHI', 1, ro, so, so + len(strings), name_at, len(cols), width, len(rows))
    t = head + col_bytes + bytes(body) + bytes(strings)
    return b'@UTF' + struct.pack('>I', len(t)) + t


def chunk(magic, table):
    return magic + b'\xff\0\0\0' + struct.pack('<I', len(table)) + bytes(4) + table


def cpk_file(files, toc_at_end):
    """A CPK holding files (name → bytes), stored; its TOC right after the header, or after the file data. Files are 0x800-aligned from ContentOffset"""
    content = 0x1800 if toc_at_end else 0x2000
    data, at = bytearray(), {}
    for nm, b in files.items():
        at[nm] = content + len(data); data += b + bytes(-len(b) % 0x800)
    toc = content + len(data) if toc_at_end else 0x800
    base = min(content, toc, 0x800)
    rows = [{'DirName': 'd', 'FileName': nm, 'FileSize': len(b), 'ExtractSize': len(b), 'FileOffset': at[nm] - base} for nm, b in files.items()]
    toc_chunk = chunk(b'TOC ', utf_table('CpkTocInfo', [('DirName', 0xa), ('FileName', 0xa), ('FileSize', 4), ('ExtractSize', 4), ('FileOffset', 6)], rows))
    head = chunk(b'CPK ', utf_table('CpkHeader', [('ContentOffset', 6), ('TocOffset', 6)], [{'ContentOffset': content, 'TocOffset': toc}]))
    out = bytearray(head.ljust(0x800, b'\0'))
    if not toc_at_end: out += toc_chunk
    out = out.ljust(content, b'\0') + data
    if toc_at_end: out += toc_chunk
    return bytes(out)


FILES = {'a.bnsh': b'BNSH\0\0\0\0' + bytes(range(56)), 'b.dat': b'second file ' * 300, 'c.usm': b'CRID' + bytes(100)}


class CpkOffsets(unittest.TestCase):
    def read_all(self, data):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / 'data.cpk'; p.write_bytes(data)
            return {e[0]: cpk.read(str(p), e) for e in cpk.files(str(p))}

    def test_toc_after_the_header(self):   # Bayonetta 2, Unicorn Overlord: TocOffset 0x800, as before
        self.assertEqual(self.read_all(cpk_file(FILES, toc_at_end=False)), {f'd/{k}': v for k, v in FILES.items()})

    def test_toc_at_the_end(self):   # LUNAR Remastered Collection: min(TocOffset, ContentOffset) was ContentOffset, 0x1000 too far in here
        self.assertEqual(self.read_all(cpk_file(FILES, toc_at_end=True)), {f'd/{k}': v for k, v in FILES.items()})


if __name__ == '__main__':
    unittest.main()
