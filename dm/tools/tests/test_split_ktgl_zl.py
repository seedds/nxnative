"""shader_split: Team Ninja's .ZL_ files (Marvel Ultimate Alliance 3's STAGE/*/*.pktf.ZL_: a u32 inflated length, then chunks of a u32 stored length and one zlib stream, ended by
a zero length) are inflated before the scan (ktgl_zl), and other forms are left alone. No game files needed."""
import os
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '0100199023B2E000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def zl(data, size=0x4000, length=None):
    out = struct.pack('<I', len(data) if length is None else length)
    for o in range(0, len(data), size):
        z = zlib.compress(data[o:o + size]); out += struct.pack('<I', len(z)) + z
    return out + bytes(4)


class KtglZl(unittest.TestCase):
    def test_chunks_are_joined(self):
        data = bytes(range(256)) * 200   # 51,200 bytes: three full chunks and a short one
        self.assertEqual(ss.ktgl_zl(zl(data)), data)

    def test_other_forms_are_left_alone(self):
        data = b'PKTF' + bytes(60)
        self.assertIsNone(ss.ktgl_zl(bytes(8)))                     # an empty file (two of the game's motion files): nothing to scan
        self.assertIsNone(ss.ktgl_zl(data))                         # a stored file
        self.assertIsNone(ss.ktgl_zl(zl(data, length=len(data) + 1)))   # the inflated length disagrees
        self.assertIsNone(ss.ktgl_zl(zl(data)[:-4]))                # no zero length at the end
        self.assertIsNone(ss.ktgl_zl(zl(data) + b'more'))           # bytes after the zero length
        self.assertIsNone(ss.ktgl_zl(zl(data)[4:]))                 # gust_gz's form, without the length word

    def test_scan_reads_the_inflated_file(self):
        inner = b'PKTF ' + b'\x91\x68\x86\x19' + bytes(64)
        seen = []
        real = ss.glslc_shaders
        ss.glslc_shaders = lambda d, label, need=True, skip=(): seen.append(bytes(d)) or []
        try:
            with tempfile.TemporaryDirectory() as t:
                p = os.path.join(t, 'ST010.pktf.ZL_')
                with open(p, 'wb') as f: f.write(zl(inner))
                ss.scan_file_in((p, 'STAGE/ST010/ST010.pktf.ZL_'))
        finally: ss.glslc_shaders = real
        self.assertEqual(seen, [inner])


if __name__ == '__main__':
    unittest.main()
