#!/usr/bin/env python3
"""Camelot's archive reader (dm/gfx/tools/cra.py) on synthetic data: the file table and the code that locates it, the scramble and its checksum, NARC members and the packing kinds.
The real archive's 8,671 files passing the game's own checksums is the check of the key and the variants; this keeps the layouts from drifting without the game's files."""
import io, lz4.block, os, struct, sys, unittest, zlib
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import cra


def adrp_add(at, reg, target):
    d = ((target & ~0xfff) - (at & ~0xfff)) >> 12
    return [0x90000000 | (d & 3) << 29 | (d >> 2 & 0x7ffff) << 5 | reg, 0x91000000 | (target & 0xfff) << 10 | reg << 5 | reg]


def mov_w(reg, imm): return 0x52800000 | imm << 5 | reg


def enc(i, s): return bytes((i - c) & 0x7f for c in s.encode()) + b'\0'


class Image:
    """An NRO with the code index_of looks for and a table of (prefix owner, prefix length, own text, suffix, size, offset)"""
    TABLE, SUMS, NAMES, SUFFIX = 0x10000, 0x18000, 0x19000, 0x19100

    def __init__(self, rows, sums=None):
        n = len(rows); d = bytearray(0x20000)
        d[0x10:0x14] = b'NRO0'
        code_end = 0x1000
        struct.pack_into('<I', d, 0x24, code_end)
        words = [0] * 8
        at = 0x100
        words = adrp_add(at, 10, self.TABLE) + [mov_w(12, 0x12), 0x78e1d948, 0x1b0c2028, mov_w(13, n * 2)]
        struct.pack_into(f'<{len(words)}I', d, at, *words)
        at = 0x200
        words = adrp_add(at, 9, self.SUMS) + [0xd28e6aa8, 0xf2aaaca8]
        struct.pack_into(f'<{len(words)}I', d, at, *words)
        at = 0x300
        words = [0x39c00d29, 0x37f801a9, 0x92401d29] + adrp_add(at + 12, 10, self.NAMES) + [0xf869794a]
        struct.pack_into(f'<{len(words)}I', d, at, *words)
        o = self.TABLE + 2 * n
        for i, (back, plen, text, sfx, size, off) in enumerate(rows):
            struct.pack_into('<h', d, self.TABLE + i * 2, o - (self.TABLE + i * 18 + 2 * n))
            rec = bytearray(13)
            struct.pack_into('<HBb', rec, 0, back, plen, sfx)
            struct.pack_into('<I', rec, 4, size ^ 0xff00ff00)
            rec[8:13] = ((off ^ 0xff00ff00ff) & 0xffffffffff).to_bytes(5, 'little')
            body = bytes(rec) + enc(i, text)
            d[o:o + len(body)] = body; o += len(body)
        struct.pack_into('<Q', d, self.NAMES, self.SUFFIX)
        d[self.SUFFIX:self.SUFFIX + 6] = enc(0, '.narc')
        for i, c in enumerate(sums or []): struct.pack_into('<I', d, self.SUMS + i * 4, c)
        self.bytes = bytes(d)


class Table(unittest.TestCase):
    def test_paths(self):
        rows = [(0, 0, '', -1, 0, 0), (0, 0, 'shader/font/$', 0, 0x20, 0x100), (1, 7, 'grass/$', -1, 0x10, 0x200)]
        idx = cra.index(Image(rows).bytes)
        self.assertIsInstance(idx, cra.Index)
        self.assertEqual((idx.table, idx.count, idx.sums, idx.names), (Image.TABLE, 3, Image.SUMS, Image.NAMES))
        fs = idx.files()
        self.assertEqual([e[1] for e in fs], ['shader/font/font.narc', 'shader/grass/grass'])   # the empty record is left out; '$' is the folder holding it
        self.assertEqual((fs[0][2], fs[0][3], fs[1][2], fs[1][3]), (0x20, 0x100, 0x10, 0x200))
        self.assertEqual({e[5] for e in fs}, {cra.LAYOUT})                      # read() and head() take each entry in its layout

    def test_no_table(self):
        d = bytearray(0x2000); d[0x10:0x14] = b'NRO0'; struct.pack_into('<I', d, 0x24, 0x1000)
        for nro in (bytes(d), bytes(0x100)):
            with self.assertRaises(cra.Error): cra.index(nro)                   # neither layout


class Scramble(unittest.TestCase):
    def test_stem(self):
        self.assertEqual(cra.stem('shader/font/font.narc'), int.from_bytes(b'font', 'big'))   # 4 at most
        self.assertEqual(cra.stem('a/bc_d.x'), int.from_bytes(b'bc', 'big'))                  # up to the first '_' or '.'
        self.assertEqual(cra.stem('a/grasses'), int.from_bytes(b'gras', 'big'))
        self.assertEqual(cra.stem('objshake.rx'), int.from_bytes(b'objs', 'big'))

    def test_block_and_checksum(self):
        raw = bytes(range(48))
        for path in ('a/one', 'a/two', 'a/three', 'b/four', 'b/five', 'b/six', 'c/seven', 'c/eight', 'd/nine', 'd/ten'):
            plain, total = cra.unscramble(path, raw)
            self.assertEqual(len(plain), 48)
            self.assertNotEqual(plain, raw)
            self.assertEqual(plain[:16], cra.unscramble(path, raw[:16])[0])     # blocks do not depend on each other
            sum_of = sum(struct.unpack_from('<Q', cra.unscramble(path, raw[i:i + 16])[0], 0 if cra.key(path) & 1 else 8)[0] for i in range(0, 48, 16)) & cra.M64
            self.assertEqual(total, sum_of)                                     # the sum is of the xored word, wherever the variant writes it
        self.assertEqual(sorted({cra.key(p) & 7 for p in map(str, range(200))}), list(range(8)))   # all 8 variants occur

    def test_read_checks(self):
        path = 'x/file'; raw = bytes((i * 7) & 0xff for i in range(32))
        plain, total = cra.unscramble(path, raw)
        good = (0, path, 25, 16, ((total >> 32) ^ total) & 0xffffffff, cra.LAYOUT)
        f = io.BytesIO(bytes(16) + raw)
        self.assertEqual(cra.read(f, good), plain[:25])
        self.assertEqual(cra.head(f, good), cra.unscramble(path, raw[:16])[0])
        with self.assertRaises(cra.Error): cra.read(f, good[:4] + (good[4] ^ 1, cra.LAYOUT))


def rev8(b): return bytes(int(f'{x:08b}'[::-1], 2) for x in b)


class Image2021:
    """An NRO with the code index_2021 looks for and a table of (how far back the prefix's record is, prefix length, own text, size, offset, checksum); the first record is the empty one"""
    TABLE = 0x10000

    def __init__(self, rows):
        n = len(rows) + 1; d = bytearray(0x20000)
        d[0x10:0x14] = b'NRO0'
        struct.pack_into('<I', d, 0x24, 0x1000)
        at = 0x100
        words = adrp_add(at, 10, self.TABLE) + [0xb8a07800 | 9 << 16 | 10 << 5 | 8, 0x52800000 | 0x2eca << 5 | 12, 0x72a00000 | 0xce46 << 5 | 12]
        struct.pack_into(f'<{len(words)}I', d, at, *words)
        o = self.TABLE + 4 * n
        struct.pack_into('<i', d, self.TABLE, self.TABLE - o); o += 0x12
        for i, (back, plen, text, size, off, want) in enumerate(rows, 1):
            struct.pack_into('<i', d, self.TABLE + i * 4, self.TABLE - o)
            rec = bytearray(0x11)
            struct.pack_into('<HB', rec, 0, back, plen)
            struct.pack_into('<II', rec, 4, size ^ 0xce462eca, want)
            rec[0xc:0x11] = (off ^ 0x46f636762a).to_bytes(5, 'little')
            body = bytes(rec) + rev8(bytes(c ^ ((i & 15) + 1) for c in text.encode())) + b'\0'
            d[o:o + len(body)] = body; o += len(body)
        self.bytes = bytes(d)


def scramble_2021(path, plain):   # what the archive holds for a file: its bits reversed in each byte, then xored with the 16 key bytes over each block
    last = path.rsplit('/', 1)[-1].split('.')[0]
    k = int.from_bytes(rev8((last * 8)[:8].encode()), 'big')
    ks = (k ^ 0x4adfc980eae9731b).to_bytes(8, 'little') + (k ^ 0xfbccb4fac6ff003e).to_bytes(8, 'little')
    return bytes(a ^ ks[i & 15] for i, a in enumerate(rev8(plain)))


class Layout2021(unittest.TestCase):
    def test_paths(self):
        rows = [(0, 0, 'shader/grass.narc', 0x20, 0x100, 5), (1, 7, 'font/font.bin', 0x10, 0x4000000200, 6), (1, 12, 'x', 3, 0x300, 7)]
        idx = cra.index(Image2021(rows).bytes)
        self.assertIsInstance(idx, cra.Index2021)                               # index takes the layout the NRO's code reads
        self.assertEqual((idx.table, idx.count), (Image2021.TABLE, 4))
        fs = idx.files()                                                        # the empty first record is left out; a prefix can run through two records
        self.assertEqual([e[1] for e in fs], ['shader/grass.narc', 'shader/font/font.bin', 'shader/font/x'])
        self.assertEqual([e[2:] for e in fs], [(0x20, 0x100, 5, cra.LAYOUT_2021), (0x10, 0x4000000200, 6, cra.LAYOUT_2021), (3, 0x300, 7, cra.LAYOUT_2021)])

    def test_keys(self):
        self.assertEqual(cra.keys_2021('a/ab.c'), cra.keys_2021('b/abababab'))  # a short name is read again from its start, at its end or before a '.'
        self.assertEqual(cra.keys_2021('abcdefgh'), cra.keys_2021('x/abcdefghij.narc'))   # 8 characters at most, of the last element
        self.assertNotEqual(cra.keys_2021('a/ab'), cra.keys_2021('ab/a'))
        with self.assertRaises(cra.Error): cra.keys_2021('a/')

    def test_read_checks(self):
        for path, n in (('shader/grass.narc', 37), ('a/b', 16), ('x/abcdefghij', 5)):
            plain = bytes((i * 7 + 1) & 0xff for i in range(n))
            raw = scramble_2021(path, plain)
            self.assertNotEqual(raw, plain)
            good = (1, path, n, 16, zlib.crc32(plain) ^ 0x12345678, cra.LAYOUT_2021)
            f = io.BytesIO(bytes(16) + raw + bytes(16))
            self.assertEqual(cra.read(f, good), plain)                          # the size is not rounded up to a block
            self.assertEqual(cra.head(f, good), plain[:16])
            with self.assertRaises(cra.Error): cra.read(f, good[:4] + (good[4] ^ 1, cra.LAYOUT_2021))


def narc(members):
    fat = b'BTAF' + struct.pack('<II', 12 + 8 * len(members), len(members)); data = b''
    for m in members:
        fat += struct.pack('<II', len(data), len(data) + len(m)); data += m + bytes(-len(m) & 15)
    fnt = b'BTNF' + struct.pack('<I', 8)
    img = b'GMIF' + struct.pack('<I', 8 + len(data)) + data
    return b'NARC\xfe\xff\0\0' + struct.pack('<IHH', 16 + len(fat) + len(fnt) + len(img), 16, 3) + fat + fnt + img


class Members(unittest.TestCase):
    def test_narc(self):
        self.assertEqual(cra.narc_members(narc([b'abc', b'', b'0123456789abcdefg'])), [b'abc', b'', b'0123456789abcdefg'])
        with self.assertRaises(cra.Error): cra.narc_members(b'SARC' + bytes(32))
        with self.assertRaises(cra.Error): cra.narc_members(narc([b'abc'])[:40])

    def test_kinds(self):
        text = b'BNSH\0\0\0\0 shaders shaders shaders shaders ' * 40
        lz4d = b'\x94\0\0\0' + struct.pack('<I', len(text)) + lz4.block.compress(text, store_size=False)
        self.assertEqual(cra.undo(lz4d), text)
        z = cra.zstd_mod().compress(text)
        self.assertEqual(cra.undo(b'Z\0\0\0' + struct.pack('<I', len(text)) + z), text)
        c = zlib.compressobj(wbits=-15); raw = c.compress(text) + c.flush()
        self.assertEqual(cra.undo(b'\x30' + struct.pack('>I', len(text)) + bytes(11) + raw), text)
        self.assertEqual(cra.undo(b'\x70' + struct.pack('>I', 3) + bytes(11) + b'abc'), b'abc')
        chunks = [lz4d, b'Z\0\0\0' + struct.pack('<I', len(text)) + z]
        body = b''.join(x + bytes(-len(x) & 15) for x in chunks)
        head = b'MMMM' + struct.pack('<II', 2 * len(text), 2) + b''.join(struct.pack('<I', len(x)) for x in chunks)
        self.assertEqual(cra.undo(head + bytes(-len(head) & 15) + body), text + text)
        self.assertEqual(cra.undo(b'FRES plain'), b'FRES plain')          # anything else is the member itself
        self.assertEqual(cra.undo(b'\x10not zlib, sixteen bytes or more'), b'\x10not zlib, sixteen bytes or more')

    def test_lz(self):
        # A flag byte holds 8 items from its high bit after negation (1 a match); a distance 0 ends. 'abcd', then 3 more at distance 4 (length in the low nibble), then the end
        flags = -0b00001100 & 0xff
        s = bytes([flags]) + b'abcd' + bytes([0x43, 0x00]) + bytes([0x00, 0x00])
        self.assertEqual(cra.lz_undo(b'\x50\0\0\0' + struct.pack('>I', 7) + s, 8), b'abcdabc')
        self.assertEqual(cra.undo(b'\x50\0\0\0' + struct.pack('>I', 7) + s), b'abcdabc')
        with self.assertRaises(cra.Error): cra.lz_undo(bytes([0x80]) + bytes([0x93, 0x00]), 0)   # before the start of the output
        # Length forms: low nibble 0 is the next byte + 16, low nibble 1 two bytes + 0x110
        s = bytes([flags]) + b'abcd' + bytes([0x40, 0x00, 0x05]) + bytes([0, 0])
        self.assertEqual(cra.lz_undo(s, 0), (b'abcd' * 7)[:4 + 21])
        s = bytes([flags]) + b'abcd' + bytes([0x41, 0x00, 0x05, 0x01]) + bytes([0, 0])
        self.assertEqual(len(cra.lz_undo(s, 0)), 4 + 5 + 0x11 + 0x100 + 0xff)
        # A flag byte 0 is 8 literals
        self.assertEqual(cra.lz_undo(bytes([0]) + b'12345678' + bytes([0x80]) + bytes([0, 0]), 0), b'12345678')


if __name__ == '__main__': unittest.main()
