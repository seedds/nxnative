"""Version 8 and version 9 paks (Unreal Engine 4.22 to 4.25: DRAGON BALL Z: KAKAROT is version 8, DRAGON QUEST TREASURES is version 9 with a frozen FMemoryImage index).
Synthetic paks with one Zlib file and one stored file; no game files."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import uepak


def fstr(s):
    b = s.encode() + b'\0'
    return struct.pack('<i', len(b)) + b


def entry(off, size, raw, meth, blocks, bsz):
    """FPakEntry as the index and the data header write it (version 8: block bounds from the entry's start)"""
    b = struct.pack('<qqqI', off, size, raw, meth) + bytes(20)
    if meth: b += struct.pack('<i', len(blocks)) + b''.join(struct.pack('<qq', s, e) for s, e in blocks)
    return b + struct.pack('<BI', 0, bsz)


def pak(path, files):
    """files: [(name, data, compress)] → a version 8 pak whose footer names Zlib as method 1"""
    body, index = b'', b''
    for name, data, comp in files:
        off = len(body)
        if comp:
            z = zlib.compress(data)
            head = len(entry(0, 0, 0, 1, [(0, 0)], 0))
            e = entry(off, len(z), len(data), 1, [(head, head + len(z))], 0x10000)
            body += e + z
        else:
            e = entry(off, len(data), len(data), 0, [], 0)
            body += e + data
        index += fstr(name) + e
    index = fstr('../../../') + struct.pack('<i', len(files)) + index
    names = b'Zlib'.ljust(32, b'\0') + bytes(32 * 4)
    foot = bytes(16) + b'\0' + struct.pack('<IIQQ', uepak.PAK_MAGIC, 8, len(body), len(index)) + bytes(20) + names
    path.write_bytes(body + index + foot)


def pak9(path, files):
    """files: [(dir, name, data, compress)] → a version 9 frozen-index pak whose footer names Zlib as method 1"""
    body = bytearray()
    idx = bytearray(0x58)
    def put_arr(at, elem_bytes, n):
        off = len(idx)
        idx.extend(elem_bytes)
        struct.pack_into('<qII', idx, at, ((off - at) << 1) | 1, n, n)
        return off
    def put_str(at, s):
        u = (s + '\0').encode('utf-16-le')
        put_arr(at, u, len(s) + 1)
    put_str(0, '../../../')
    e_at = put_arr(0x10, bytes(0x50 * len(files)), len(files))
    d_at = put_arr(0x20, bytes(0x50 * len(files)), len(files))
    for i, (dn, fn, data, comp) in enumerate(files):
        off = len(body)
        if comp:
            z = zlib.compress(data)
            head = len(entry(0, 0, 0, 1, [(0, 0)], 0))
            body += entry(off, len(z), len(data), 1, [(head, head + len(z))], 0x10000) + z
            eoff = e_at + i * 0x50
            struct.pack_into('<qqq', idx, eoff, off, len(z), len(data))
            put_arr(eoff + 0x30, struct.pack('<qq', head, head + len(z)), 1)
            struct.pack_into('<IIB', idx, eoff + 0x40, 0x10000, 1, 0)
        else:
            body += entry(off, len(data), len(data), 0, [], 0) + data
            struct.pack_into('<qqq', idx, e_at + i * 0x50, off, len(data), len(data))
        doff = d_at + i * 0x50
        put_str(doff, dn)
        foff = put_arr(doff + 0x10, bytes(0x20), 1)
        put_str(foff, fn)
        struct.pack_into('<i', idx, foff + 0x10, i)
    names = b'Zlib'.ljust(32, b'\0') + bytes(32 * 4)
    foot = bytes(16) + b'\0' + struct.pack('<IIQQ', uepak.PAK_MAGIC, 9, len(body), len(idx)) + bytes(20) + b'\x01' + names
    path.write_bytes(bytes(body) + bytes(idx) + foot)


class Version8(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.p = Path(self.tmp.name) / 'pakchunk0-Switch.pak'
        self.shader = b'LSLG' + bytes(range(256)) * 8
        pak(self.p, [('AT/Content/M_Test.uexp', self.shader, True), ('Engine/Config/a.ini', b'[x]\n', False)])

    def test_footer(self):
        with open(self.p, 'rb') as f: enc, _, _, methods, ver = uepak.footer(f)
        self.assertEqual((enc, ver, methods[0]), (0, 8, 'Zlib'))

    def test_inline_and_read(self):
        self.assertTrue(uepak.inline_shaders(str(self.p)))
        self.assertEqual(uepak.unreal_paks(self.tmp.name, inline=True), [str(self.p)])
        k = uepak.Pak(str(self.p))
        self.assertEqual(k.entry('AT/Content/M_Test.uexp')['method'], 'Zlib')
        self.assertEqual(k.read('AT/Content/M_Test.uexp'), self.shader)
        self.assertEqual(k.read('Engine/Config/a.ini'), b'[x]\n')


class Version9(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.p = Path(self.tmp.name) / 'pakchunk0-Switch.pak'
        self.shader = b'LSLG' + bytes(range(256)) * 8
        pak9(self.p, [('DarwinGame/Content/', 'M_Test.uexp', self.shader, True), ('Engine/Config/', 'a.ini', b'[x]\n', False)])

    def test_footer_and_read(self):
        with open(self.p, 'rb') as f: enc, _, _, methods, ver = uepak.footer(f)
        self.assertEqual((enc, ver, methods[0]), (0, 9, 'Zlib'))
        self.assertTrue(uepak.inline_shaders(str(self.p)))
        self.assertEqual(uepak.unreal_paks(self.tmp.name, inline=True), [str(self.p)])
        k = uepak.Pak(str(self.p))
        self.assertEqual(k.entry('DarwinGame/Content/M_Test.uexp')['method'], 'Zlib')
        self.assertEqual(k.read('DarwinGame/Content/M_Test.uexp'), self.shader)
        self.assertEqual(k.read('Engine/Config/a.ini'), b'[x]\n')


if __name__ == '__main__':
    unittest.main()
