"""Version 11 encoded pak entries and single-block decompression, using synthetic paks only."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

REPO = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(REPO / 'dm/gfx/tools'), str(Path(__file__).parent)]
import uepak
from test_uepak_key import aes_encrypt


def fstr(s):
    b = s.encode() + b'\0'
    return struct.pack('<i', len(b)) + b


def v8_entry(off, size, raw, meth, blocks, bsz):
    b = struct.pack('<qqqI', off, size, raw, meth) + bytes(20)
    if meth: b += struct.pack('<i', len(blocks)) + b''.join(struct.pack('<qq', s, e) for s, e in blocks)
    return b + struct.pack('<BI', 0, bsz)


def pak11(path, entries, *, key=None):
    """Build a version 11 pak with encoded entries:
    entries = [(name, raw_bytes, method_idx, bsz_bits, explicit_bsz, block_chunks, encrypted)].
    block_chunks is a list of compressed block byte strings (empty for stored entries)."""
    data, enc_entries, dirs = bytearray(), bytearray(), []
    for name, raw, meth, bsz_bits, explicit_bsz, chunks, encrypted in entries:
        at = len(data)
        nblk = len(chunks)
        head = 53 + (4 + 16 * nblk if meth else 0)
        body = bytearray()
        for chunk in chunks if meth else [raw]:
            if encrypted:
                padded = chunk + bytes(-len(chunk) % 16)
                body.extend(aes_encrypt(key, padded))
            else:
                body.extend(chunk)
        stored_total = sum(len(c) for c in chunks) if meth else len(raw)
        data.extend(bytes(head) + body)
        v = (bsz_bits & 0x3f) | (nblk << 6) | (int(encrypted) << 22) | (meth << 23) | (1 << 29) | (1 << 30) | (1 << 31)
        dirs.append((name, len(enc_entries)))
        enc_entries.extend(struct.pack('<I', v))
        if (bsz_bits & 0x3f) == 0x3f:
            enc_entries.extend(struct.pack('<I', explicit_bsz))
        enc_entries.extend(struct.pack('<II', at, len(raw)))
        if meth:
            enc_entries.extend(struct.pack('<I', stored_total))
        if not (nblk == 1 and not encrypted):
            for chunk in chunks:
                enc_entries.extend(struct.pack('<I', len(chunk)))
    ioff = len(data)
    dirindex = struct.pack('<i', 1) + fstr('/') + struct.pack('<i', len(dirs)) + b''.join(fstr(n) + struct.pack('<i', o) for n, o in dirs)
    index = fstr('../../../') + struct.pack('<i', len(entries)) + struct.pack('<Q', 0) + struct.pack('<i', 0) + struct.pack('<i', 1)
    doff = ioff + len(index) + 8 + 8 + 20 + 4 + len(enc_entries) + 4
    index += struct.pack('<QQ', doff, len(dirindex)) + bytes(20) + struct.pack('<i', len(enc_entries)) + bytes(enc_entries) + struct.pack('<i', 0)
    methods = b''.join(m.ljust(32, b'\0') for m in (b'Zlib', b'', b'', b'', b''))
    foot = bytes(16) + b'\0' + struct.pack('<IIQQ', uepak.PAK_MAGIC, 11, ioff, len(index)) + bytes(20) + methods
    path.write_bytes(bytes(data) + index + dirindex + foot)


class Version11(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.p = Path(self.tmp.name) / 'pakchunk0-Switch.pak'

    def test_single_block_truncated_6bit_block_size(self):
        """UE 4.26 writes (raw >> 11) & 0x3f without the 0x3f escape on 1-block files (e.g. 0x3800 for ~15 KB BaseCompat.ini)."""
        compat = (b'[AppCompat]\nDevice=Switch\n' * 650)[:0x3b82]
        small = b'[Small]\na=1\n'
        escaped = (b'escaped-block-' * 1200)[:0x4123]
        pak11(self.p, [
            ('Engine/Config/BaseCompat.ini', compat, 1, len(compat) >> 11, 0, [zlib.compress(compat)], False),
            ('Engine/Config/Base.ini', small, 1, 0, 0, [zlib.compress(small)], False),
            ('Game/Escaped.uexp', escaped, 1, 0x3f, 0x40000, [zlib.compress(escaped)], False),
        ])
        pk = uepak.Pak(str(self.p)); self.addCleanup(pk.f.close)
        for name, expected in (
            ('Engine/Config/BaseCompat.ini', compat),
            ('Engine/Config/Base.ini', small),
            ('Game/Escaped.uexp', escaped),
        ):
            self.assertEqual(pk.entry(name)['block_size'], len(expected))
            self.assertEqual(pk.read(name), expected)

    @unittest.skipUnless(sys.platform == 'darwin', 'CommonCrypto')
    def test_encrypted_single_block_truncated_6bit_block_size(self):
        key = bytes(range(1, 33))
        raw = (b'encrypted-ue426-material-' * 700)[:0x3c11]
        pak11(self.p, [('Game/M_World.uexp', raw, 1, len(raw) >> 11, 0, [zlib.compress(raw)], True)], key=key)
        pk = uepak.Pak(str(self.p), key=key); self.addCleanup(pk.f.close)
        self.assertEqual(pk.entry('Game/M_World.uexp')['block_size'], len(raw))
        self.assertEqual(pk.read('Game/M_World.uexp'), raw)

    def test_multiblock_entries_keep_their_block_size(self):
        raw = (b'0123456789abcdef' * 5000)[:0x11234]
        std_chunks = [zlib.compress(raw[:0x10000]), zlib.compress(raw[0x10000:])]
        custom_bsz = 0x1234
        custom_raw = raw[:0x2500]
        custom_chunks = [zlib.compress(custom_raw[i:i + custom_bsz]) for i in range(0, len(custom_raw), custom_bsz)]
        pak11(self.p, [
            ('Game/MultiStd.uexp', raw, 1, 0x10000 >> 11, 0, std_chunks, False),
            ('Game/MultiCustom.uexp', custom_raw, 1, 0x3f, custom_bsz, custom_chunks, False),
        ])
        pk = uepak.Pak(str(self.p)); self.addCleanup(pk.f.close)
        self.assertEqual(pk.entry('Game/MultiStd.uexp')['block_size'], 0x10000)
        self.assertEqual(pk.read('Game/MultiStd.uexp'), raw)
        self.assertEqual(pk.entry('Game/MultiCustom.uexp')['block_size'], custom_bsz)
        self.assertEqual(pk.read('Game/MultiCustom.uexp'), custom_raw)

    def test_legacy_single_block_with_small_block_size(self):
        raw = (b'legacy-single-block-' * 800)[:0x3b82]
        comp = zlib.compress(raw)
        head = len(v8_entry(0, 0, 0, 1, [(0, 0)], 0))
        ent = v8_entry(0, len(comp), len(raw), 1, [(head, head + len(comp))], 0x3800)
        body = ent + comp
        index = fstr('../../../') + struct.pack('<i', 1) + fstr('Game/Test.uexp') + ent
        names = b'Zlib'.ljust(32, b'\0') + bytes(32 * 4)
        foot = bytes(16) + b'\0' + struct.pack('<IIQQ', uepak.PAK_MAGIC, 8, len(body), len(index)) + bytes(20) + names
        self.p.write_bytes(body + index + foot)
        pk = uepak.Pak(str(self.p)); self.addCleanup(pk.f.close)
        self.assertEqual(pk.read('Game/Test.uexp'), raw)

    def test_damaged_block_coverage_raises_value_error(self):
        raw = b'abcd' * 0x1000
        pak11(self.p, [
            ('Game/NoBlocks.uexp', raw, 1, 0x10000 >> 11, 0, [], False),
            ('Game/ZeroBsz.uexp', raw, 1, 0, 0, [zlib.compress(b'a'), zlib.compress(b'b')], False),
            ('Game/ShortBlocks.uexp', raw, 1, 0x800 >> 11, 0, [zlib.compress(raw[:0x800]), zlib.compress(raw[0x800:0x1000])], False),
        ])
        pk = uepak.Pak(str(self.p)); self.addCleanup(pk.f.close)
        with self.assertRaisesRegex(ValueError, 'decoded 0x0 bytes, not 0x4000'):
            pk.read('Game/NoBlocks.uexp')
        with self.assertRaisesRegex(ValueError, 'non-positive block size'):
            pk.read('Game/ZeroBsz.uexp')
        with self.assertRaisesRegex(ValueError, 'decoded 0x1000 bytes, not 0x4000'):
            pk.read('Game/ShortBlocks.uexp')


if __name__ == '__main__':
    unittest.main()
