"""Version 8 paks (Unreal Engine 4.22 to 4.25, DRAGON BALL Z: KAKAROT): version 4's per-file index with the compression method as an index into the footer's names, version 11's footer.
A synthetic pak with one Zlib file and one stored file; no game files."""
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


if __name__ == '__main__':
    unittest.main()
