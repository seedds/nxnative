"""VBF/KIT block decoding and unaligned Phyre NVN programs, without game files."""
from pathlib import Path
import hashlib
import os
import random
import struct
import sys
import tempfile
import unittest
import zlib
import lz4.block

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
os.environ.setdefault('DM_GAME', '010015100B514000')
import shader_split as split
import vbf


def program(stage):
    code = bytearray(0x100); code[:4] = split.GFX_MAGIC
    struct.pack_into('<I', code, 0x30, {'vs': 1, 'ps': 5}[stage] << 10)
    ctl = bytearray(0x880); ctl[:4] = split.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    return bytes(code + ctl)


class Vbf(unittest.TestCase):
    def test_raw_compressed_and_shared_members(self):
        payload = bytes(range(256)) * 256 + b'last block' * 100
        compressed = zlib.compress(payload[65536:])
        names = b'a.fx.phyre\0shared.fx.phyre\0'
        hlen = 16 + 2 * 64 + 4 + len(names) + 6
        header = bytearray(struct.pack('<4sIQ', b'SRYK', hlen, 2) + bytes(64))
        header += struct.pack('<IIQQQ', 0, 0, len(payload), hlen, 0)
        header += struct.pack('<IIQQQ', 2, 0, 5, 0xffffffffffffffff, 11)
        header += struct.pack('<I', len(names) + 4) + names + struct.pack('<3H', 0, len(compressed), 5)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'test.vbf'; path.write_bytes(header + payload[:65536] + compressed)
            entries = vbf.entries(path)
            self.assertEqual(len(entries), 1)
            with path.open('rb') as f: self.assertEqual(vbf.read(f, entries[0]), payload)
            path.write_bytes(path.read_bytes()[:-2])
            with path.open('rb') as f:
                with self.assertRaises(ValueError): vbf.read(f, entries[0])

    def test_effect_and_loose_programs_use_same_key(self):
        vs, ps = program('vs'), program('ps')
        effect = b'RYHP' + bytes(9) + vs + bytes(7) + ps
        found = split.phyre_shaders(effect, 'effect')
        self.assertEqual([x[1] for x in found], ['vs', 'ps'])
        loose = struct.pack('<II', 0x100, 0x880) + vs
        self.assertEqual(split.skey(*found[0][2:]), split.skey(*split.phyre_shaders(loose, 'loose')[0][2:]))
        bad = bytearray(effect); struct.pack_into('<I', bad, 13 + 0x100 + 0x704, 0x104)
        self.assertEqual([x[1] for x in split.phyre_shaders(bad, 'bad')], ['ps'])
        self.assertEqual(split.phyre_shaders(b'RYHP' + split.CTL_MAGIC, 'short'), [])
        literal = bytearray(vs); literal[0x80:0x84] = split.CTL_MAGIC
        self.assertEqual([x[1] for x in split.phyre_shaders(b'RYHP' + literal, 'literal')], ['vs'])

    def test_headerless_compute_is_bounded_by_the_effect(self):
        cs = bytearray(program('vs')); cs[:0x100] = bytes(0x100)
        struct.pack_into('<3I', cs, 0x100 + 0x748, 8, 4, 1)
        effect = b'RYHP' + bytes(9) + cs + program('ps')
        out = []; split.scan_blob(effect, 'effect', out)
        self.assertEqual([x[1] for x in out], ['cs', 'ps'])
        self.assertIsNone(split.check('effect', *out[0][1:]))
        self.assertEqual(split.phyre_shaders(cs, 'unframed'), [])
        for at, value in ((0x748, 0), (0x704, 0x100000), (0x20, 0x100000)):
            bad = bytearray(effect); struct.pack_into('<I', bad, 13 + 0x100 + at, value)
            self.assertEqual([x[1] for x in split.phyre_shaders(bad, 'bad')], ['ps'])
        bad = bytearray(effect); bad[13 + 4:13 + 8] = split.CTL_MAGIC
        self.assertEqual([x[1] for x in split.phyre_shaders(bad, 'overlap')], ['ps'])

    def test_effect_keeps_the_parent_archive_budget(self):
        effect = b'RYHP' + bytes(9) + program('ps')
        with self.assertRaisesRegex(SystemExit, 'archive members add up to more than'):
            split.scan_blob(effect, '/member', [], left=[1, 1])


class Kit(unittest.TestCase):
    def archive(self, tmp, payloads):
        """Two shards, mixed stored/compressed members, and a padded index."""
        path = Path(tmp) / 'Data.kit'; header = bytearray(0x25 + 17)
        header[:4] = split.KIT
        struct.pack_into('<QI', header, 0x10, 17, len(payloads))
        shards = [bytearray(), bytearray()]
        for i, (payload, compressed) in enumerate(payloads):
            data = bytearray()
            if compressed:
                for at in range(0, len(payload), 65536):
                    block = lz4.block.compress(payload[at:at + 65536], store_size=False, dict=payload[max(0, at - 65536):at])
                    data += struct.pack('<I', len(block)) + block
            else: data += payload
            part = i % 2
            header += struct.pack('<IHIHQQ20sII', 9 if compressed else 1, part, len(shards[part]), 0, len(data), len(payload), hashlib.sha1(payload).digest(), 0, 0)
            shards[part] += data
        path.write_bytes(header)
        for i, shard in enumerate(shards): Path(f'{path}.{i}').write_bytes(shard)
        return path

    def test_chained_dictionary_raw_members_and_scanner(self):
        first = random.Random(7).randbytes(65536)
        payload = first + first[-16000:]
        effect = b'RYHP' + bytes(9) + program('vs') + program('ps')
        with tempfile.TemporaryDirectory() as tmp:
            path = self.archive(tmp, [(payload, True), (effect, False)])
            entries = split.kit_entries(str(path))
            self.assertEqual([split.kit_read(str(path), e) for e in entries], [payload, effect])
            stored = Path(f'{path}.0').read_bytes(); at = 4 + struct.unpack_from('<I', stored)[0]
            n = struct.unpack_from('<I', stored, at)[0]
            with self.assertRaises(lz4.block.LZ4BlockError): lz4.block.decompress(stored[at + 4:at + 4 + n], uncompressed_size=16000)
            files = [(str(p), p.name) for p in Path(tmp).iterdir()]
            unrelated = str(path) + '.notes'; files.append((unrelated, 'Data.kit.notes'))
            jobs, paths = split.kit_jobs(files, parts=1)
            self.assertEqual(paths, {str(path), str(path) + '.0', str(path) + '.1'})
            self.assertNotIn(unrelated, paths)
            self.assertEqual(len(jobs), 1)
            _, found, _, extra = split.scan_file(jobs[0])
            self.assertEqual(extra, {})
            self.assertEqual(set(found), {split.skey(c, k) for _, _, c, k in split.phyre_shaders(effect, 'loose')})

    def test_raw_reads_decoded_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self.archive(tmp, [(b'raw member', False)])
            d = bytearray(path.read_bytes()); struct.pack_into('<Q', d, 0x25 + 17 + 12, 9999); path.write_bytes(d)
            self.assertEqual(split.kit_read(str(path), split.kit_entries(str(path))[0]), b'raw member')

    def test_shared_effect_keeps_game_rows_with_or_without_dlc(self):
        effect = b'RYHP' + bytes(9) + program('vs') + program('ps')
        with tempfile.TemporaryDirectory() as tmp:
            path = self.archive(tmp, [(effect, True)])
            jobs, _ = split.kit_jobs([(str(path), 'Data.kit')], parts=1)
            game = split.scan_file(jobs[0])
            dlc = split.scan_file((str(path), 'aoc/1/Data.kit', jobs[0][2]))
            self.assertEqual(len(game[1]), 2)
            self.assertTrue(all(v[0].startswith('/aoc/1/') for v in dlc[1].values()))
            expected = split.merge_keys([game])
            for results in ([dlc, game], [game, dlc]):
                keys, pairs, per_src = split.merge_keys(results)
                self.assertEqual((keys, pairs), expected[:2])
                self.assertEqual(+per_src, +expected[2])

    def test_index_and_member_bounds(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self.archive(tmp, [(b'member', True)])
            original = path.read_bytes(); base = 0x25 + 17
            variants = [original[:20], original[:-1]]
            for at, fmt, value in ((0x10, '<Q', len(original)), (0x18, '<I', 100), (base + 10, '<H', 1), (base + 6, '<I', 99999), (base + 12, '<Q', 99999)):
                bad = bytearray(original); struct.pack_into(fmt, bad, at, value); variants.append(bad)
            for bad in variants:
                with self.subTest(index=bytes(bad).hex()):
                    path.write_bytes(bad)
                    with self.assertRaises(SystemExit): split.kit_entries(str(path))
            path.write_bytes(original); Path(f'{path}.0').unlink()
            with self.assertRaises(SystemExit): split.kit_entries(str(path))

    def test_lz4_bounds_lengths_and_hash(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self.archive(tmp, [(b'payload' * 100, True)])
            entry = split.kit_entries(str(path))[0]; shard = Path(f'{path}.0'); original = shard.read_bytes()
            cases = [original[:2], original[:-1], struct.pack('<I', 0), struct.pack('<I', len(original)), original + b'trailing']
            for data in cases:
                with self.subTest(data=data[:16]):
                    shard.write_bytes(data); e = (*entry[:3], len(data), *entry[4:])
                    with self.assertRaises(SystemExit): split.kit_read(str(path), e)
            shard.write_bytes(original)
            for size in (entry[4] - 1, entry[4] + 1):
                with self.assertRaises(SystemExit): split.kit_read(str(path), (*entry[:4], size, *entry[5:]))
            with self.assertRaises(SystemExit): split.kit_read(str(path), (*entry[:-1], bytes(20)))
            shard.write_bytes(original[:-1])
            with self.assertRaises(SystemExit): split.kit_read(str(path), entry)


if __name__ == '__main__': unittest.main()
