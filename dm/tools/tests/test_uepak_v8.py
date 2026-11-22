"""Ordinary and short version 8 paks, and version 1 shader archives, with synthetic data only."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import uepak
import shader_split as ss


def fstr(s):
    b = s.encode() + b'\0'
    return struct.pack('<i', len(b)) + b


def entry(off, size, raw, meth, blocks, bsz, short=False):
    """FPakEntry as the index and the data header write it (version 8: block bounds from the entry's start)"""
    b = struct.pack('<qqqB' if short else '<qqqI', off, size, raw, meth) + bytes(20)
    if meth: b += struct.pack('<i', len(blocks)) + b''.join(struct.pack('<qq', s, e) for s, e in blocks)
    return b + struct.pack('<BI', 0, bsz)


def pak(path, files, *, short=False, fifth_slot=False, codec=None):
    """files: [(name, data, compress)] → a version 8 pak whose footer names codec (default Zlib) as method 1 or 5"""
    body, index = b'', b''
    method = 5 if fifth_slot else 1
    cname, compress = (codec[0].encode(), codec[1]) if codec else (b'Zlib', zlib.compress)
    for name, data, comp in files:
        off = len(body)
        if comp:
            z = compress(data)
            head = len(entry(0, 0, 0, method, [(0, 0)], 0, short))
            e = entry(off, len(z), len(data), method, [(head, head + len(z))], 0x10000, short)
            body += e + z
        else:
            e = entry(off, len(data), len(data), 0, [], 0, short)
            body += e + data
        index += fstr(name) + e
    index = fstr('../../../') + struct.pack('<i', len(files)) + index
    names = bytes(32 * 4) + cname.ljust(28, b'\0') if fifth_slot else cname.ljust(32, b'\0') + bytes(32 * (3 if short else 4))
    foot = bytes(16) + b'\0' + struct.pack('<IIQQ', uepak.PAK_MAGIC, 8, len(body), len(index)) + bytes(20) + names
    path.write_bytes(body + index + foot)


def v1_archive(records):
    """(SHA-1, encoded bytes, raw size, shader frequency) in table order."""
    entries, code = bytearray(), bytearray()
    for sha, data, raw, freq in records:
        entries.extend(sha + struct.pack('<QIIB', len(code), len(data), raw, freq))
        code.extend(data)
    return struct.pack('<II', 1, len(records)) + entries + code


class Version8(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.p = Path(self.tmp.name) / 'pakchunk0-Switch.pak'
        self.shader = b'LSLG' + bytes(range(256)) * 8
        pak(self.p, [('AT/Content/M_Test.uexp', self.shader, True), ('Engine/Config/a.ini', b'[x]\n', False)])

    def test_zstd_pak_blocks_do_not_use_the_game_decoder(self):
        zstd = uepak.zstd_mod()
        def stream_compress(data):
            c = zstd.ZstdCompressor()
            return c.compress(data) + c.flush()
        for label, compress in (('framed', zstd.compress), ('no-content-size', stream_compress)):
            with self.subTest(label=label):
                pak(self.p, [('Game/Material.uexp', self.shader, True)], codec=('Zstd', compress))
                pk = uepak.Pak(str(self.p))
                with pk.f:
                    def wrong_decoder(*args): self.fail('Zstd must not go to the game decoder')
                    self.assertEqual(pk.read('Game/Material.uexp', decode=wrong_decoder), self.shader)

    def test_zstd_blocks_reject_wrong_sizes_and_incomplete_frames(self):
        zstd = uepak.zstd_mod()
        data = zstd.compress(self.shader)
        self.assertEqual(uepak.zstd_block(zstd.compress(b''), 0), b'')
        for block, size in [
            (data, len(self.shader) - 1),
            (data, len(self.shader) + 1),
            (data, -1),
            (data, -2),
            (data[:-1], len(self.shader)),
            (data + b'extra', len(self.shader)),
            (zstd.compress(b''), 1),
        ]:
            with self.subTest(size=size, stored=len(block)), self.assertRaises(zstd.ZstdError):
                uepak.zstd_block(block, size)
        pak(self.p, [('Game/Bad.uexp', self.shader, True)], codec=('Zstd', lambda d: zstd.compress(d)[:-1]))
        pk = uepak.Pak(str(self.p))
        with pk.f, self.assertRaisesRegex(ValueError, 'block 0 does not decode'):
            pk.read('Game/Bad.uexp')

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

    def test_ordinary_v8_shortened_fifth_slot_and_inline_archive(self):
        name = 'Content/Test.ushaderbytecode'
        pak(self.p, [(name, self.shader, True)], fifth_slot=True)
        with self.p.open('rb') as f:
            self.assertEqual(uepak.magic_back(f), (0xc8, 8))
            self.assertEqual(uepak.footer(f)[3], ['', '', '', '', 'Zlib'])
        path = str(self.p)
        k = uepak.Pak(path)
        with k.f: self.assertEqual(k.read(name), self.shader)
        self.assertEqual(uepak.pak_shader_sources(path), (True, False))
        jobs, _ = ss.ue_legacy_jobs([(path, self.p.name)])
        self.assertEqual(jobs, [(path, self.p.name, (ss.UE_LEGACY, (name,)))])

    def test_short_v8_mixed_pak_job_and_finish(self):
        name, inline = 'Content/Test.ushaderbytecode', 'Content/Test.uexp'
        stored = b'LSLGSP' + bytes(range(40))
        hashes = (bytes(range(20)), bytes(range(20, 40)))
        data = v1_archive([(hashes[0], zlib.compress(self.shader), len(self.shader), 0), (hashes[1], stored, len(stored), 3)])
        pak(self.p, [(name, data, False), (inline, self.shader, True)], short=True)
        path = str(self.p)
        k = uepak.Pak(path)
        with k.f:
            self.assertEqual(k.methods, ['Zlib', '', '', ''])
            self.assertEqual(k.read(inline), self.shader)
            self.assertEqual(k.read(name), data)
        self.assertEqual(uepak.unreal_paks(self.tmp.name, inline=True), [path])
        self.assertEqual(uepak.unreal_paks(self.tmp.name, inline=False), [path])
        jobs, _ = ss.ue_legacy_jobs([(path, self.p.name)])
        self.assertEqual(jobs, [(path, self.p.name, (ss.UE_LEGACY, (inline,)))])
        work, out = self.p.parent / 'work', self.p.parent / 'ue_shaders'
        listing, need_oodle = uepak.job(self.tmp.name, 'unused-main', str(work))
        self.assertEqual(([entry['shaders'] for entry in listing], need_oodle), ([2], 0))
        self.assertEqual(uepak.finish(str(work), str(out)), [Path(name).name])
        decoded = (out / Path(name).name).read_bytes()
        archive = uepak.archive(decoded)
        self.assertEqual(archive['maps'], [])
        self.assertEqual(archive['indices'], [])
        self.assertEqual(decoded[12:52], b''.join(hashes))
        self.assertEqual(archive['shaders'], [(0, len(self.shader), len(self.shader), 0), (len(self.shader), len(stored), len(stored), 3)])
        self.assertEqual(decoded[archive['code']:], self.shader + stored)

    def test_invalid_v1_records(self):
        comp = zlib.compress(self.shader)
        valid = v1_archive([(bytes(20), comp, len(self.shader), 0)])
        for data in (valid[:12], valid[:-1], valid[:28] + bytes([255]) * 8 + valid[36:],
                     v1_archive([(bytes(20), comp, 0xffffffff, 0)]),
                     v1_archive([(bytes(20), comp, len(self.shader), 255)]),
                     v1_archive([(bytes(20), comp, len(self.shader) + 1, 0)]),
                     v1_archive([(bytes(20), comp[:-1], len(self.shader), 0)]),
                     v1_archive([(bytes(20), comp + b'garbage', len(self.shader), 0)]),
                     v1_archive([(bytes(20), b'invalid', 7, 0)])):
            with self.subTest(size=len(data)), self.assertRaisesRegex(ValueError, 'test archive:'):
                uepak.archive_v1_decoded(data, 'test archive')

    def test_invalid_v1_archive_does_not_queue_oodle(self):
        name = 'Content/Damaged.ushaderbytecode'
        data = v1_archive([(bytes(20), b'bad zlib', len(self.shader), 3)])
        pak(self.p, [(name, data, False)], short=True)
        self.assertEqual(uepak.pak_shader_sources(str(self.p)), (False, True))
        work = self.p.parent / 'work'
        with self.assertRaisesRegex(ValueError, r'Damaged\.ushaderbytecode: shader 0 does not decode'):
            uepak.job(self.tmp.name, 'unused-main', str(work))
        self.assertFalse((work / 'oodle.job').exists())


if __name__ == '__main__':
    unittest.main()
