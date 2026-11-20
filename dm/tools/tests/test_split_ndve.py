"""shader_split: nn::ndve's movie enhancement file wraps an NDSR pack and then holds its networks, each control section with the code area right behind it. Entries of the pack are 8 bytes,
so data entries can shift the next shader by 8. No game files needed: synthetic control sections as nvn2_ctl reads them, the compute stage byte set."""
import os
import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '0100199023B2E000'   # shader_split loads a profile at import; other tests expect no default
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def ctl(code_len, own=0xe0):
    c = bytearray(own)
    c[:4] = ss.CTL_MAGIC; c[4:8] = b'\xc9\0\0\0'
    struct.pack_into('<I', c, 0x30, own); struct.pack_into('<I', c, 0x44, code_len)
    c[0x56] = 5   # compute
    return bytes(c)


def ndsr():
    # Two shaders with data entries between them, which put the second control entry on the other 16-byte phase: the file's own layout, offsets from the pack's start
    table = 0x14
    c1, k1, c2, k2 = 0x200, 0x300, 0x400, 0x500
    d = bytearray(0x600)
    d[:4] = b'NDSR'
    entries = [(0x600, 0xcb), (0x100, 0x100000c),         # data
               (c1, 0xe06), (k1, 0x1007),                 # a shader
               (0x60c, 0x49),                             # one data entry: the next shader is 8 bytes off
               (c2, 0xe06), (k2, 0x1007)]
    o = table
    for a, b in entries:
        struct.pack_into('<II', d, o, a, b); o += 8
    d[c1:c1 + 0xe0] = ctl(0x100); d[k1:k1 + 4] = b'code'
    d[c2:c2 + 0xe0] = ctl(0x100); d[k2:k2 + 4] = b'cod2'
    return bytes(d)


class Ndve(unittest.TestCase):
    def test_pack_and_networks(self):
        pack = ndsr()
        net = ctl(0x100, own=0x770) + b'net1'.ljust(0x100, b'\0') + bytes(0x10) + ctl(0x80, own=0xf18) + b'net2'.ljust(0x80, b'\0')
        d = b'NDVE' + struct.pack('<IIIII', 0x0ff00005, 0, len(pack), len(net), 2) + struct.pack('<IfIf', 0x1000000, 4 / 3, 0x1000004, 1.0) + pack + net
        out = ss.ndve_shaders(d, '/x')
        self.assertEqual([st for _, st, _, _ in out], ['cs'] * 4)
        self.assertEqual([code[:4] for _, _, _, code in out], [b'code', b'cod2', b'net1', b'net2'])
        self.assertEqual([len(c) for _, _, c, _ in out], [0xe0, 0xe0, 0x770, 0xf18])
        found = []
        ss.scan_blob(d, '/x', found)
        self.assertEqual(len(found), 4)

    def test_short_file(self):
        self.assertEqual(ss.ndve_shaders(b'NDVE', '/x'), [])

    def test_ndsr_terminator(self):   # the walk ends at a zero entry pair
        d = bytearray(ndsr()); struct.pack_into('<4I', d, 0x14 + 8 * 2, 0, 0, 0, 0)
        out = ss.ndsr_shaders(bytes(d), '/x')
        self.assertEqual(out, [])


if __name__ == '__main__':
    unittest.main()
