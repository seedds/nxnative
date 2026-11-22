"""KIT archives (kit.py) and the split's reading of them (kit_jobs), without game files."""
from pathlib import Path
import hashlib
import os
import random
import struct
import sys
import tempfile
import unittest
import lz4.block

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
os.environ.setdefault('DM_GAME', '010015100B514000')
import shader_split as split
import kit
from test_vbf import program


class Kit(unittest.TestCase):
    def archive(self, tmp, payloads):
        """Two shards, mixed stored/compressed members, and a padded index."""
        path = Path(tmp) / 'Data.kit'; header = bytearray(0x25 + 17)
        header[:4] = kit.MAGIC
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
            entries = kit.entries(str(path))
            self.assertEqual([kit.read(str(path), e) for e in entries], [payload, effect])
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
            self.assertEqual(kit.read(str(path), kit.entries(str(path))[0]), b'raw member')

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
                    with self.assertRaises(ValueError): kit.entries(str(path))
            path.write_bytes(original); Path(f'{path}.0').unlink()
            with self.assertRaises(ValueError): kit.entries(str(path))

    def test_lz4_bounds_lengths_and_hash(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self.archive(tmp, [(b'payload' * 100, True)])
            entry = kit.entries(str(path))[0]; shard = Path(f'{path}.0'); original = shard.read_bytes()
            cases = [original[:2], original[:-1], struct.pack('<I', 0), struct.pack('<I', len(original)), original + b'trailing']
            for data in cases:
                with self.subTest(data=data[:16]):
                    shard.write_bytes(data); e = (*entry[:3], len(data), *entry[4:])
                    with self.assertRaises(ValueError): kit.read(str(path), e)
            shard.write_bytes(original)
            for size in (entry[4] - 1, entry[4] + 1):
                with self.assertRaises(ValueError): kit.read(str(path), (*entry[:4], size, *entry[5:]))
            with self.assertRaises(ValueError): kit.read(str(path), (*entry[:-1], bytes(20)))
            shard.write_bytes(original[:-1])
            with self.assertRaises(ValueError): kit.read(str(path), entry)

    def test_parts_inside_a_romfs_image(self):
        """The NSP script hands the split a RomFS image, not a folder: the index and its parts are read through romfs"""
        import romfs, romfs_image
        effect = b'RYHP' + bytes(9) + program('vs') + program('ps')
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 'src'; src.mkdir()
            self.archive(str(src), [(effect, True), (b'raw member', False)])
            img = str(Path(tmp) / 'RomFS.img'); romfs_image.pack(str(src), img)
            files = [(os.path.join(d, n), os.path.relpath(os.path.join(d, n), img)) for d, _, ns in romfs.os.walk(img) for n in ns]
            jobs, paths = split.kit_jobs(files, parts=1)
            self.assertEqual(sorted(paths), sorted(p for p, _ in files))
            rel, found, _, extra = split.scan_file(jobs[0])
            self.assertEqual((rel, extra), ('Data.kit', {}))
            self.assertEqual(set(found), {split.skey(c, k) for _, _, c, k in split.phyre_shaders(effect, 'loose')})

    def test_split_names_the_damaged_archive(self):
        """kit.py raises ValueError; the split leaves a damaged member's range out with a red line naming the member, and stops on a damaged index"""
        effect = b'RYHP' + bytes(9) + program('vs')
        with tempfile.TemporaryDirectory() as tmp:
            path = self.archive(tmp, [(effect, True)])
            jobs, _ = split.kit_jobs([(str(path), 'Data.kit')], parts=1)
            Path(f'{path}.0').write_bytes(b'\0' * len(Path(f'{path}.0').read_bytes()))
            rel, found, _, extra = split.scan_file(jobs[0])
            self.assertEqual((rel, found, extra[0]), ('Data.kit', {}, 'unreadable'))
            self.assertIn('Data.kit#0: LZ4 block is outside the KIT member', extra[1])
            Path(f'{path}.0').unlink()
            with self.assertRaisesRegex(SystemExit, r'\[RED\] .*Data\.kit: KIT entry 0 has no part 0'): split.kit_jobs([(str(path), 'Data.kit')])


if __name__ == '__main__': unittest.main()
