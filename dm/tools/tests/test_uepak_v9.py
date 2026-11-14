"""Version 9 paks (Unreal Engine 4.25, PakFile_Version_FrozenIndex: Monster Jam Steel Titans 2): version 8's per-file index, entries and named compression methods, with the frozen index
flag (u8) between the SHA-1 and the names, so the magic is 0xcd bytes from the end. The game's indexes are encrypted with the key in main. A synthetic pak and a synthetic main; no game files or keys."""
from pathlib import Path
import os
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import uepak
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010029B0118E8000'   # shader_split loads a profile at import; other tests expect no default
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def fstr(s):
    b = s.encode() + b'\0'
    return struct.pack('<i', len(b)) + b


def entry(off, size, raw, meth, blocks, bsz):
    """FPakEntry as the index and the data header write it (block bounds from the entry's start)"""
    b = struct.pack('<qqqI', off, size, raw, meth) + bytes(20)
    if meth: b += struct.pack('<i', len(blocks)) + b''.join(struct.pack('<qq', s, e) for s, e in blocks)
    return b + struct.pack('<BI', 0, bsz)


def aes_encrypt(key, data):
    import ctypes
    cc = ctypes.CDLL('/usr/lib/libSystem.B.dylib').CCCrypt
    out = ctypes.create_string_buffer(len(data)); moved = ctypes.c_size_t()
    assert cc(0, 0, 2, key, len(key), None, data, len(data), out, len(data), ctypes.byref(moved)) == 0
    return out.raw[:moved.value]


def pak(path, files, key=None, frozen=0):
    """files: [(name, data, compress)] → a version 9 pak whose footer names Zlib as method 1, its index encrypted with key when one is given"""
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
    if key: index = aes_encrypt(key, index + bytes(-len(index) % 16))
    names = b'Zlib'.ljust(32, b'\0') + bytes(32 * 4)
    foot = bytes(16) + bytes([bool(key)]) + struct.pack('<IIQQ', uepak.PAK_MAGIC, 9, len(body), len(index)) + bytes(20) + bytes([frozen]) + names
    path.write_bytes(body + index + foot)


SHADER = b'LSLG' + bytes(range(256)) * 8
FILES = [('Game/Content/M_Test.uexp', SHADER, True), ('Engine/Config/a.ini', b'[x]\n', False)]


class Version9(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def test_footer(self):
        p = self.dir / 'pakchunk0-Switch.pak'; pak(p, FILES)
        with open(p, 'rb') as f:
            self.assertEqual(uepak.magic_back(f), (0xcd, 9))
            enc, _, _, methods, ver = uepak.footer(f)
        self.assertEqual((enc, ver, methods[0], methods[1]), (0, 9, 'Zlib', ''))

    def test_frozen_index_refused(self):
        p = self.dir / 'pakchunk0-Switch.pak'; pak(p, FILES, frozen=1)
        with open(p, 'rb') as f, self.assertRaisesRegex(ValueError, 'frozen'): uepak.footer(f)
        self.assertEqual(uepak.unreal_paks(self.tmp.name), [])

    def test_inline_and_read(self):
        p = self.dir / 'pakchunk0-Switch.pak'; pak(p, FILES)
        self.assertTrue(uepak.inline_shaders(str(p)))
        self.assertEqual(uepak.unreal_paks(self.tmp.name, inline=True), [str(p)])
        k = uepak.Pak(str(p))
        self.assertEqual(k.entry('Game/Content/M_Test.uexp')['method'], 'Zlib')
        self.assertEqual(k.read('Game/Content/M_Test.uexp'), SHADER)
        self.assertEqual(k.read('Engine/Config/a.ini'), b'[x]\n')

    def test_shader_archive_pak_not_inline(self):
        """Monster Jam's paks hold shader code archives: such a pak goes to the archive decoder (uepak.job takes unreal_paks inline=False), not to ue_legacy_jobs"""
        p = self.dir / 'pakchunk0-Switch.pak'
        pak(p, FILES + [('Game/Content/ShaderArchive-Game-GLSL_SWITCH.ushaderbytecode', b'x', False)])
        self.assertFalse(uepak.inline_shaders(str(p)))
        self.assertEqual(uepak.unreal_paks(self.tmp.name, inline=False), [str(p)])
        self.assertEqual(uepak.unreal_paks(self.tmp.name, inline=True), [])


@unittest.skipUnless(sys.platform == 'darwin', 'CommonCrypto')
class EncryptedIndex(unittest.TestCase):
    """The index encrypted with main's key, as in Monster Jam Steel Titans 2: uepak.Keys finds it and ue_legacy_jobs carries it to the worker, as for version 4 and 8"""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.key = bytes(range(7, 39))
        self.p = Path(self.tmp.name) / 'pakchunk0-Switch.pak'; pak(self.p, FILES, self.key)
        ro = bytearray(os.urandom(0x4000)); ro[0x100:0x120] = self.key   # the key stored whole in main's read-only data
        self.image = (bytes(0x1000) + bytes(ro), 0x1000, 0x5000)
        ss._PAKS.pop(str(self.p), None); self.addCleanup(ss._PAKS.pop, str(self.p), None)

    def test_key_from_main(self):
        with open(self.p, 'rb') as f: self.assertEqual(uepak.footer(f)[0], 1)
        with patch.object(uepak, 'nso_image', return_value=self.image):
            self.assertEqual(uepak.Keys(['main'])(str(self.p)), self.key)
        self.assertEqual(uepak.Pak(str(self.p), self.key).read('Game/Content/M_Test.uexp'), SHADER)

    def test_jobs_carry_the_key(self):
        with patch.object(uepak, 'nso_image', return_value=self.image):
            jobs, taken = ss.ue_legacy_jobs([(str(self.p), 'pakchunk0-Switch.pak')], mains=['main'])
        self.assertEqual(taken, {str(self.p)})
        self.assertTrue(jobs)
        for path, _, (tag, _, key) in jobs: self.assertEqual((path, tag, key), (str(self.p), ss.UE_LEGACY, self.key))
        self.assertEqual(sorted(n for j in jobs for n in j[2][1]), sorted(n for n, _, _ in FILES))


if __name__ == '__main__':
    unittest.main()
