"""Legacy paks (version 4 and version 8, UE 4.22) and the shader code archives before version 2, on synthetic files; no game files or keys."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import uepak


def fstr(s): return struct.pack('<i', len(s) + 1) + s.encode() + b'\0'


def pak(version, files):
    """A legacy pak of files [(name, bytes, zlib)]: each file's data behind an FPakEntry (53 bytes, 50 in version 8), the index, then the footer. Version 8 names its methods
    in the footer (Zlib first) and keeps the method in one byte; version 4 stores block offsets from the start of the file, later ones from the entry"""
    head0 = 50 if version == 8 else 53
    data, idx = bytearray(), fstr('../../../Game/') + struct.pack('<i', len(files))
    for name, raw, z in files:
        off, stored = len(data), zlib.compress(raw) if z else raw
        head = head0 + (4 + 16 if z else 0)
        data += bytes(head) + stored
        idx += fstr(name) + struct.pack('<qqq', off, len(stored), len(raw)) + struct.pack('<B' if version == 8 else '<i', int(z)) + bytes(20)
        if z: s = head if version >= 5 else off + head; idx += struct.pack('<iqq', 1, s, s + len(stored))
        idx += struct.pack('<BI', 0, len(raw) if z else 0)
    foot = bytes(1) + struct.pack('<IIQQ', uepak.PAK_MAGIC, version, len(data), len(idx)) + bytes(20)
    if version == 8: foot += b''.join(m.ljust(32, b'\0') for m in (b'Zlib', b'', b'', b''))
    return bytes(data + idx + foot)


def archive_v1(shaders, versioned):
    """Shader code archive: UE 4.22's version 1 (u32 1, u32 count, 37-byte entries with a u64 offset and the frequency) or the layout before it (u32 count, 32-byte entries)"""
    stored = [zlib.compress(s) if z else s for s, z in shaders]
    ents, o = b'', 0
    for (s, _), t in zip(shaders, stored):
        ents += bytes(20) + (struct.pack('<QIIB', o, len(t), len(s), 5) if versioned else struct.pack('<III', o, len(t), len(s))); o += len(t)
    return (struct.pack('<II', 1, len(shaders)) if versioned else struct.pack('<I', len(shaders))) + ents + b''.join(stored)


class LegacyPak(unittest.TestCase):
    def open(self, b):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        p = Path(tmp.name) / 'pakchunk0-Switch.pak'; p.write_bytes(b)
        k = uepak.Pak(p); self.addCleanup(k.f.close); return k

    def test_version_8(self):   # Bloodstained: Ritual of the Night 1.60
        files = [('Content/a.uasset', b'plain bytes' * 9, False), ('Content/ShaderArchive-Game-GLSL_SWITCH.ushaderbytecode', bytes(range(256)) * 40, True)]
        k = self.open(pak(8, files))
        self.assertEqual(k.version, 8)
        for name, raw, z in files:
            self.assertEqual(k.entry(name)['method'], 'Zlib' if z else '')
            self.assertEqual(k.read(name), raw)

    def test_version_4(self):   # Bloodstained: Ritual of the Night 1.0, DRAGON QUEST XI S
        files = [('Content/a.uasset', b'plain bytes' * 9, False), ('Content/b.uasset', b'zlib bytes' * 300, True)]
        k = self.open(pak(4, files))
        for name, raw, _ in files: self.assertEqual(k.read(name), raw)

    def test_footer_where_the_version_has_none(self):
        b = bytearray(pak(4, [('a', b'x', False)])); struct.pack_into('<I', b, len(b) - 0x2c + 4, 8)   # a version 8 number in a version 4 footer
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        p = Path(tmp.name) / 'bad.pak'; p.write_bytes(b)
        with open(p, 'rb') as f, self.assertRaises(ValueError): uepak.footer(f)


class ArchiveV1(unittest.TestCase):
    def test_both_layouts(self):
        shaders = [(b'LSLG' + bytes(60), False), (b'LSLG' + b'\x11' * 400, True)]
        for versioned in (True, False):
            b = archive_v1(shaders, versioned)
            ents = uepak.archive_v1(b)
            self.assertEqual([r for _, _, r in ents], [len(s) for s, _ in shaders])
            got = [b[o:o + n] for o, n, _ in ents]
            self.assertEqual([zlib.decompress(g) if z else g for g, (_, z) in zip(got, shaders)], [s for s, _ in shaders])

    def test_one_shader_before_version_1(self):   # a count of 1 reads as version 1 first: its table does not fit, so the older layout is taken
        b = archive_v1([(b'LSLG' + bytes(60), False)], False)
        self.assertEqual(uepak.archive_v1(b), [(36, 64, 64)])

    def test_not_an_archive(self):
        self.assertIsNone(uepak.archive_v1(b'\x01\x00\x00\x00' + bytes(4)))
        self.assertIsNone(uepak.archive_v1(bytes(range(64))))


if __name__ == '__main__':
    unittest.main()
