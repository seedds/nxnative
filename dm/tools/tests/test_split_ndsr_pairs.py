"""shader_split: an NDSR pack's table is {offset, length | type} pairs; a shader's control pair need not start a 16-byte record when constant-data pairs come between shaders
(The Legend of Zelda: Ocarina of Time's agl_ndsr_vr.pck). No game files needed."""
import os
import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '040074A01BF12000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def ctl(code_len, stage=5, own=0xe0):
    """An NVN2 control section of own bytes: the magic, version 0xc9, its length at +0x30, the code length at +0x44, the stage byte at +0x56"""
    c = bytearray(own); c[:4] = ss.CTL_MAGIC; c[4:8] = b'\xc9\0\0\0'
    struct.pack_into('<I', c, 0x30, own); struct.pack_into('<I', c, 0x44, code_len); c[0x56] = stage
    return bytes(c)


def ndsr(entries):
    """entries: [('ctl', control bytes, code bytes) | ('data', bytes)] → an NDSR pack whose table lists them as {offset, length << 4 | type} pairs from +0x10"""
    words, blobs, off = [], b'', 0x10 + 8 * (sum(2 if e[0] == 'ctl' else 1 for e in entries) + 1)
    off = (off + 0xff) & ~0xff
    for e in entries:
        if e[0] == 'ctl':
            _, c, k = e
            words += [off, len(c) << 4 | 6]; blobs += c; off += len(c)
            words += [off, (len(k) // 16) << 8 | 7]; blobs += k; off += len(k)
        else:
            words += [off, len(e[1]) << 4 | 0xb]; blobs += e[1]; off += len(e[1])
    head = b'NDSR' + bytes.fromhex('00010400') + bytes(8) + struct.pack(f'<{len(words)}I', *words) + bytes(8)
    return head + bytes(off - len(blobs) - len(head)) + blobs


class Ndsr(unittest.TestCase):
    def test_pairs_between_shaders_do_not_hide_the_next_shader(self):   # Ocarina of Time's agl_ndsr_vr.pck: a constant-data pair between the first shader's pairs and the second's
        code1, code2 = bytes(range(256)) * 3, bytes(reversed(range(256))) * 2
        d = ndsr([('ctl', ctl(len(code1)), code1), ('data', b'\x01' * 4), ('ctl', ctl(len(code2)), code2)])
        got = ss.ndsr_shaders(d, '/pack')
        self.assertEqual([(st, k) for _, st, _, k in got], [('cs', code1), ('cs', code2)])

    def test_two_pairs_between_shaders(self):   # the pack's other shape: two data pairs, so the next control pair lands on a record start again
        code = bytes(0x300)
        d = ndsr([('ctl', ctl(len(code)), code), ('data', b'\x01' * 4), ('data', b'\x02' * 4), ('ctl', ctl(len(code)), code)])
        self.assertEqual(len(ss.ndsr_shaders(d, '/pack')), 2)

    def test_aligned_records_read_as_before(self):   # Bananza's pack: a shader's pairs form one 16-byte record
        code = bytes(0x300)
        got = ss.ndsr_shaders(ndsr([('ctl', ctl(len(code)), code), ('ctl', ctl(len(code)), code)]), '/pack')
        self.assertEqual(len(got), 2)

    def test_a_vertex_and_pixel_pair_still_pairs(self):   # the upscaler's graphics programs: a vertex record followed by its pixel record
        vs, ps = ctl(0x100, stage=0), ctl(0x100, stage=0)
        vcode = bytes.fromhex('00040000') + bytes(0xfc); pcode = bytes.fromhex('00140000') + bytes(0xfc)   # SPH word 0: shader type 1 (vertex), 5 (pixel) in bits 10..13
        d = ndsr([('ctl', vs, vcode), ('ctl', ps, pcode)])
        got = ss.ndsr_shaders(d, '/pack')
        self.assertEqual([st for _, st, _, _ in got], ['vs', 'ps'])
        self.assertIn(got[0][0], ss.BAHS_VAR)


if __name__ == '__main__':
    unittest.main()
