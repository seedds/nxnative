"""forge.py on a synthetic Scimitar .forge file: the header, a table with two entries and their name records, an entry's blocks (a stored chunk and one through a decoder), the control
sections each followed by its code area in a decoded block, and damaged files."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import forge


def block(chunks, typ=6):   # a compressed block: chunks [(raw size, stored bytes)], each after its u32 checksum
    head = forge.BLOCK + struct.pack('<HBHHH', 1, typ, 0x8000, 0x8000, len(chunks)) + b''.join(struct.pack('<HH', raw, len(s)) for raw, s in chunks)
    return head + b''.join(b'\xcc' * 4 + s for _, s in chunks)


def entry(name, blocks):   # a FILEDATA entry: its 0x1b8-byte header (its name, padding), one 8-byte item, then its blocks
    return b'FILEDATA' + name.encode() + bytes(forge.ENTRY_HEAD - 8 - len(name)) + struct.pack('<II', 1, 0) + b''.join(blocks)


def forge_file(ents, version=31):   # ents: [(name, entry bytes)]; header, file data header and one table, then the entries
    n = len(ents); hdr = 0x20; tab = hdr + 0x28; idx = tab + 0x30; names = idx + 16 * n; data = names + forge.NAME_REC * n
    out = bytearray(forge.MAGIC + struct.pack('<Iq', version, hdr))
    out += bytes(hdr - len(out)) + struct.pack('<IIqqIIq', n, 1, 0, -1, 5000, 1, tab)
    out += struct.pack('<IIqqIIqq', n, 1, idx, -1, 0, 4999, names, data)
    offs = []; p = data
    for _, e in ents: offs.append(p); p += len(e)
    for (nm, e), o in zip(ents, offs): out += struct.pack('<qII', o, 0x1000 + len(offs), len(e) - forge.ENTRY_HEAD)   # the index's size leaves the header out
    for nm, e in ents:
        rec = struct.pack('<IQ', len(e), 0x1234) + bytes(forge.NAME_AT - 12) + nm.encode()
        out += rec + bytes(forge.NAME_REC - len(rec))
    for _, e in ents: out += e
    return bytes(out)


def shader(code_len=0x60):   # a control section (0x880 bytes, its code length at +0x704) and its code area
    code = struct.pack('<I', 0x12345678) + bytes(code_len - 4)
    ctl = bytearray(forge.CTL_MAGIC + bytes(0x87c)); struct.pack_into('<I', ctl, 0x20, 0x880); struct.pack_into('<I', ctl, 0x704, len(code))
    return bytes(ctl), code


class ForgeTests(unittest.TestCase):
    def test_entries(self):
        title = entry('TitleScreen', [block([(3, b'abc')])])
        d = forge_file([('GlobalMetaFile', entry('GlobalMetaFile', [])), ('TitleScreen', title)])
        self.assertTrue(forge.is_forge(d))
        es = forge.entries(d)
        self.assertEqual([n for _, _, n in es], ['GlobalMetaFile', 'TitleScreen'])
        self.assertEqual(d[es[1][0]:es[1][0] + es[1][1]], title)   # the size read includes the header
        self.assertEqual(forge.read(d, *es[1][:2], None), [b'abc'])

    def test_blocks_stored_and_decoded(self):   # a chunk whose stored size is its raw size is taken as it is; the others go to the decoder with their raw size
        e = entry('a', [block([(2, b'hi')]), block([(5, b'\x8c\x0aXY'), (3, b'end')], typ=4)])
        seen = []
        def decode(comp, n): seen.append((comp, n)); return b'hello'[:n]
        self.assertEqual(forge.read(e, 0, len(e), decode), [b'hi', b'helloend'])
        self.assertEqual(seen, [(b'\x8c\x0aXY', 5)])
        self.assertEqual(forge.read(entry('tiny', []), 0, forge.ENTRY_HEAD + 8, decode), [])

    def test_nvn_shaders(self):
        ctl, code = shader(); ctl2, code2 = shader(0x80)
        b = b'material' + ctl + code + b'\0' * 7 + ctl2 + code2 + b'tail'
        found, bad = forge.nvn_shaders(b)
        self.assertEqual([(c, x, y) for c, x, y in found], [(8, ctl, code), (8 + len(ctl) + len(code) + 7, ctl2, code2)])
        self.assertEqual(bad, [])
        self.assertEqual(forge.nvn_shaders(forge.CTL_MAGIC + bytes(0x800)), ([], []))   # the magic in other data: no control section's length
        found, bad = forge.nvn_shaders(ctl + b'no code here' + bytes(0x60))
        self.assertEqual(found, []); self.assertEqual(len(bad), 1)

    def test_damaged(self):
        e = entry('t', [block([(3, b'abc')])])
        d = forge_file([('t', e)])
        with self.assertRaises(ValueError): forge.entries(b'scimitaR\0' + d[9:])   # not a forge
        with self.assertRaises(ValueError): forge.entries(d[:len(d) - len(e) - 1])   # the entry runs past the end
        bad = bytearray(d); struct.pack_into('<I', bad, 0x20, 2)   # the header counts two entries
        with self.assertRaises(ValueError): forge.entries(bytes(bad))
        with self.assertRaises(ValueError): forge.read(entry('t', [block([(3, b'abc')], typ=9)]), 0, 0x200, None)   # an unknown block type
        cut = entry('t', [block([(3, b'abc')])])[:-2]
        with self.assertRaises(ValueError): forge.read(cut, 0, len(cut), None)   # a chunk runs past the entry
        with self.assertRaises(ValueError): forge.read(entry('t', [block([(5, b'abc')])]), 0, 0x200, lambda c, n: b'ab')   # decodes to 2 bytes, not 5


if __name__ == '__main__':
    unittest.main()
