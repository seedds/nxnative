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


def entry(off, size, raw, meth, blocks, bsz, mw=4):
    """FPakEntry as the index and the data header write it (version 8: block bounds from the entry's start)"""
    b = struct.pack('<qqq', off, size, raw) + meth.to_bytes(mw, 'little') + bytes(20)
    if meth: b += struct.pack('<i', len(blocks)) + b''.join(struct.pack('<qq', s, e) for s, e in blocks)
    return b + struct.pack('<BI', 0, bsz)


def pak(path, files, names=5, mw=4):
    """files: [(name, data, compress)] → a version 8 pak whose footer names Zlib as method 1; names=4, mw=1: Minecraft Dungeons' (UE 4.22: four names, a one-byte method field)"""
    body, index = b'', b''
    for name, data, comp in files:
        off = len(body)
        if comp:
            z = zlib.compress(data)
            head = len(entry(0, 0, 0, 1, [(0, 0)], 0, mw))
            e = entry(off, len(z), len(data), 1, [(head, head + len(z))], 0x10000, mw)
            body += e + z
        else:
            e = entry(off, len(data), len(data), 0, [], 0, mw)
            body += e + data
        index += fstr(name) + e
    index = fstr('../../../') + struct.pack('<i', len(files)) + index
    names = b'Zlib'.ljust(32, b'\0') + bytes(32 * (names - 1))
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


class Version8FourNames(unittest.TestCase):
    """Minecraft Dungeons (UE 4.22): the footer's magic is 0xac from the end (four names), and an entry's compression method is one byte, so its header is 50 bytes, not 53"""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.p = Path(self.tmp.name) / 'pakchunk0-Switch.pak'
        self.shader = b'LSLG' + bytes(range(256)) * 8
        pak(self.p, [('AT/Content/M_Test.uexp', self.shader, True), ('Engine/Config/a.ini', b'[x]\n', False)], names=4, mw=1)

    def test_footer(self):
        with open(self.p, 'rb') as f:
            self.assertEqual(uepak.magic_back(f), (0xac, 8)); enc, _, _, methods, ver = uepak.footer(f)
        self.assertEqual((enc, ver, methods[0]), (0, 8, 'Zlib'))

    def test_read(self):
        self.assertTrue(uepak.inline_shaders(str(self.p)))
        k = uepak.Pak(str(self.p))
        self.assertEqual(sorted(k.files), ['AT/Content/M_Test.uexp', 'Engine/Config/a.ini'])
        self.assertEqual(k.entry('AT/Content/M_Test.uexp')['method'], 'Zlib')
        self.assertEqual(k.read('AT/Content/M_Test.uexp'), self.shader)
        self.assertEqual(k.read('Engine/Config/a.ini'), b'[x]\n')

    def test_pak_with_shader_archives_is_not_inline(self):
        pak(self.p, [('Dungeons/Content/ShaderArchive-Dungeons-GLSL_SWITCH.ushaderbytecode', b'x' * 64, False), ('Dungeons/Content/a.uexp', b'y', False)], names=4, mw=1)
        self.assertFalse(uepak.inline_shaders(str(self.p)))
        self.assertEqual(uepak.unreal_paks(self.tmp.name, inline=True), [])
        self.assertEqual(uepak.unreal_paks(self.tmp.name, inline=False), [str(self.p)])


def v1_archive(shaders, freq=(0, 3)):
    """A version 1 shader code archive (UE 4.22): version, count, per shader SHA-1 + offset + size + raw size + frequency, then the zlib code"""
    ents, code = b'', b''
    for i, rec in enumerate(shaders):
        z = zlib.compress(rec)
        ents += bytes([i]) * 20 + struct.pack('<QIIB', len(code), len(z), len(rec), freq[i % len(freq)]); code += z
    return struct.pack('<II', 1, len(shaders)) + ents + code


class Version1Archive(unittest.TestCase):
    RECS = [b'LSLG' + bytes(range(i, i + 120)) for i in range(5)]

    def test_tables(self):
        b = v1_archive(self.RECS); a = uepak.archive(b)
        self.assertEqual((a['version'], len(a['shaders']), a['code']), (1, 5, 8 + 37 * 5))
        self.assertEqual([x[3] for x in a['shaders']], [0, 3, 0, 3, 0])
        self.assertEqual(a['shaders'][1][0], a['shaders'][0][1])   # the code is contiguous

    def test_zlib_decode_and_relayout(self):
        b = v1_archive(self.RECS); a = uepak.archive(b)
        raws = uepak.zlib_shaders(b, a)
        self.assertEqual(raws, self.RECS)
        d = uepak.decoded_archive(b, raws); a2 = uepak.archive(d)
        self.assertTrue(all(size == raw for _, size, raw, _ in a2['shaders']))
        self.assertEqual([d[a2['code'] + o:a2['code'] + o + sz] for o, sz, _, _ in a2['shaders']], self.RECS)
        self.assertEqual(d[8:28], b[8:28])   # the SHA-1s stay

    def test_not_lslg_and_damage(self):
        b = v1_archive([b'XXXX' + bytes(100)]); a = uepak.archive(b)
        with self.assertRaisesRegex(ValueError, 'not LSLG'): uepak.zlib_shaders(b, a)
        b = v1_archive(self.RECS); a = uepak.archive(b)
        bad = bytearray(b); bad[a['code'] + 4] ^= 0xff
        self.assertIsNone(uepak.zlib_shaders(bytes(bad), a))
        with self.assertRaises(ValueError): uepak.archive(struct.pack('<II', 1, 10 ** 9) + bytes(100))
        with self.assertRaises(ValueError): uepak.archive(struct.pack('<I', 7) + bytes(100))


if __name__ == '__main__':
    unittest.main()
