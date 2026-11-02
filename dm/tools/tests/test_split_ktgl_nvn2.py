"""shader_split on Pokémon Pokopia's KTGL (NVN2): the LINKDATA pair LD_<name>.BIN / LI_<name>.IDX beside Three Houses' DATA1.bin / DATA0.bin (linkdata_index), chunked zlib
entries with chunks larger than 0x10000 (kt_gz), and an NVN2 control section keyed without the header bytes KTGL rewrites at load time (key_ctl with Pokopia's shader_key_skip
0x8+0x28). No game files needed."""
import os
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '0400A3A01EE4C000'   # shader_split loads a profile at import: Pokopia's, for its shader_key_skip
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def kt_chunked(data, cs):
    """Koei Tecmo's chunked zlib: chunk size, count, inflated length, the stored length of each chunk, then the chunks at 0x80 alignment (u32 length + zlib stream)"""
    chunks = [data[o:o + cs] for o in range(0, len(data), cs)]
    stored = [struct.pack('<I', len(z)) + z for z in (zlib.compress(c) for c in chunks)]
    out = bytearray(struct.pack('<III', cs, len(chunks), len(data)) + b''.join(struct.pack('<I', len(s)) for s in stored))
    for s in stored:
        out += bytes(-len(out) % 0x80) + s
    return bytes(out)


class KtGz(unittest.TestCase):
    def test_chunk_sizes(self):
        data = bytes(range(256)) * 4096   # 1 MB
        for cs in (0x10000, 0x80000, 0x100000):
            self.assertEqual(ss.kt_gz(kt_chunked(data, cs)), data, hex(cs))

    def test_other_chunk_sizes_are_not_this_form(self):
        self.assertIsNone(ss.kt_gz(kt_chunked(bytes(0x30000), 0x18000)))


class LinkdataIndex(unittest.TestCase):
    def test_pairs(self):
        with tempfile.TemporaryDirectory() as d:
            for n in ('LD_C0_R.BIN', 'LI_C0_R.IDX', 'DATA0.bin', 'DATA1.bin', 'LD_X.BIN'): Path(d, n).write_bytes(b'')
            self.assertEqual(ss.linkdata_index(os.path.join(d, 'LD_C0_R.BIN')), os.path.join(d, 'LI_C0_R.IDX'))
            self.assertEqual(ss.linkdata_index(os.path.join(d, 'DATA1.bin')), os.path.join(d, 'DATA0.bin'))
            self.assertIsNone(ss.linkdata_index(os.path.join(d, 'LD_X.BIN')))   # no LI_X.IDX beside it
            self.assertIsNone(ss.linkdata_index(os.path.join(d, 'DATA0.bin')))


class Nvn2KeySkip(unittest.TestCase):
    def ctl(self, b8, h20):
        c = bytearray(0x80); c[:4] = ss.CTL_MAGIC; c[4:8] = b'\xc9\0\0\0'; c[8] = b8; c[0x20:0x28] = h20; c[0x30:0x34] = b'\xd8\0\0\0'; c[0x44] = 0x40
        return bytes(c)

    def test_rewritten_header_keys_the_same(self):
        self.assertEqual(ss.KEY_SKIP, (0x8, 0x28))
        packaged, loaded = self.ctl(0x0b, b'hash-one'), self.ctl(0x11, b'hash-two')
        self.assertTrue(ss.nvn2_ctl(packaged))
        self.assertEqual(ss.skey(packaged, b'code'), ss.skey(loaded, b'code'))
        self.assertNotEqual(ss.skey(packaged, b'code'), ss.skey(packaged, b'other'))
        self.assertTrue(ss.same_shader((packaged, b'code'), (loaded, b'code')))


if __name__ == '__main__':
    unittest.main()
