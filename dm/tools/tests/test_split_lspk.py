"""shader_split: lspk_shaders reads a Larian LSPK package of version 13 (Divinity: Original Sin 2 Definitive Edition's Data/*.pak) through its file list at the end, inflates
each .bshd of part 0 (a 4-byte word, then a zlib stream) whether the package stores it as is, zlib or LZ4, and hands it to glslc_shaders. The DX11 .shd files and the entries
of other parts are left alone, a package of another version is refused with a red line, and lspk_tail tells such a package from other .pak files by its last four bytes."""
import os
import struct
import sys
import tempfile
import unittest
import zlib
from unittest import mock
from pathlib import Path

import lz4.block

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz   # shader_split with a profile
ss = fz.ss


def bshd(payload):   # a .bshd: a 4-byte word, then the zlib stream
    return struct.pack('<I', 0x08220080) + zlib.compress(payload)


def package(entries, version=13):
    """entries: (name, payload, how, part) with how 0 stored, 1 zlib, 2 LZ4 → the package's bytes: the entries' data, the file list (u32 count, an LZ4 block of 280-byte
    entries), then the 32-byte header, its size (40: it counts the size field and the magic) and 'LSPK'"""
    data, rows = bytearray(), bytearray()
    for name, payload, how, part in entries:
        stored = payload if how == 0 else zlib.compress(payload) if how == 1 else lz4.block.compress(payload, store_size=False)
        rows += struct.pack('<256sIIIIII', name.encode(), len(data), len(stored), len(payload) if how else 0, part, how, 0)
        data += stored
    flo = len(data); lst = struct.pack('<I', len(entries)) + lz4.block.compress(bytes(rows), store_size=False)
    head = struct.pack('<IIIHBB16s', version, flo, len(lst), 2, 2, 0, b'\0' * 16)
    return bytes(data + lst + head + struct.pack('<I', len(head) + 8) + b'LSPK')


class LspkPackage(unittest.TestCase):
    def setUp(self):
        self.seen = []
        patch = mock.patch.object(ss, 'glslc_shaders', lambda d, label, need=True, skip=(): self.seen.append((label, d)) or [])
        patch.start(); self.addCleanup(patch.stop)
        self.dir = tempfile.TemporaryDirectory(); self.addCleanup(self.dir.cleanup)

    def write(self, b):
        p = os.path.join(self.dir.name, 'Test.pak')
        with open(p, 'wb') as f: f.write(b)
        return p

    def test_bshd_of_part_0_in_each_storage(self):
        p = self.write(package([('Shaders/NX/A.bshd', bshd(b'stored'), 0, 0), ('Public/M/B_SK_DEF_NX.bshd', bshd(b'zlib'), 1, 0),
                                ('Public/M/C_DX11.shd', bshd(b'dx11'), 0, 0), ('Shaders/NX/D.bshd', bshd(b'part 1'), 0, 1),
                                ('Shaders/NX/E.bshd', bshd(b'lz4'), 2, 0)]))
        self.assertTrue(ss.lspk_tail(p))
        found = []; ss.lspk_shaders(p, 'Data/Test.pak', found)
        self.assertEqual(self.seen, [('/Data/Test.pak/Shaders/NX/A.bshd', b'stored'), ('/Data/Test.pak/Public/M/B_SK_DEF_NX.bshd', b'zlib'),
                                     ('/Data/Test.pak/Shaders/NX/E.bshd', b'lz4')])
        self.assertEqual(found, [])

    def test_other_version_is_refused(self):
        p = self.write(package([('Shaders/NX/A.bshd', bshd(b'x'), 0, 0)], version=14))
        with self.assertRaises(SystemExit) as e: ss.lspk_shaders(p, 'Data/Test.pak', [])
        self.assertIn('LSPK version 14', str(e.exception))
        self.assertEqual(self.seen, [])

    def test_tail_tells_packages_apart(self):
        self.assertFalse(ss.lspk_tail(self.write(b'PAK\x01' + b'\0' * 60)))   # a Heaps PAK (Dead Cells)
        self.assertFalse(ss.lspk_tail(self.write(b'LSPK')))                  # too short to hold a header


if __name__ == '__main__':
    unittest.main()
