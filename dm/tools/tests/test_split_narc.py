"""Camelot NARC shader discovery from synthetic archives, without game data (#1131)."""
import contextlib
import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest import mock

import lz4.block

from dm.tools.tests.test_split_gzip import REPO, fz, ss


def archive(members):
    table = bytearray(struct.pack('<4sII', b'BTAF', 12 + 8 * len(members), len(members)))
    data = bytearray()
    for member in members:
        start = len(data); data += member
        table += struct.pack('<II', start, len(data))
        data += bytes(-len(data) % 4)
    names = struct.pack('<4sIIHH', b'BTNF', 16, 4, 0, 1)
    image = struct.pack('<4sI', b'GMIF', 8 + len(data)) + data
    size = 16 + len(table) + len(names) + len(image)
    return struct.pack('<4sHHIHH', b'NARC', 0xfffe, 0, size, 16, 3) + table + names + image


def member(tag, data):
    if tag in (0x50, 0x60):
        header = (b'\x50\0\0\0' + len(data).to_bytes(4, 'big') if tag == 0x50
                  else b'\x60' + len(data).to_bytes(3, 'big'))
        return header + b''.join(b'\0' + data[i:i + 8] for i in range(0, len(data), 8))
    if tag == 0x94:
        return b'\x94\0\0\0' + struct.pack('<I', len(data)) + lz4.block.compress(data, store_size=False)
    if tag in (0x10, 0x20, 0x30):
        obj = zlib.compressobj(wbits=-15 if tag == 0x30 else 15)
        payload = obj.compress(data) + obj.flush()
    else:
        payload = data
    return bytes([tag]) + len(data).to_bytes(4, 'big') + bytes(11) + payload


class NarcSources(unittest.TestCase):
    def scan(self, data, name='model/court.narc'):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()) as log:
            path = Path(directory) / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
            result = ss.scan_file((str(path), name))
        return result, log.getvalue()

    def test_compressed_members_preserve_shader_stages_and_pairs(self):
        raw = fz.bnsh(); (_, expected, pairs, _), _ = self.scan(raw)
        self.assertEqual({v[1] for v in expected.values()}, {'vs', 'ps'})
        self.assertEqual(len(pairs), 1)
        for tag in (0x10, 0x20, 0x30, 0x50, 0x60, 0x70, 0x80, 0x94):
            with self.subTest(tag=hex(tag)):
                (_, keys, got_pairs, extra), log = self.scan(archive([member(tag, raw)]))
                self.assertEqual({k: v[1:] for k, v in keys.items()}, {k: v[1:] for k, v in expected.items()})
                self.assertEqual(got_pairs, pairs); self.assertEqual((extra, log), ({}, ''))
                self.assertTrue(all('/court.narc#0' in v[0] for v in keys.values()))

    def test_magic_selects_archives_including_texture_suffix(self):
        data = archive([member(0x10, fz.bnsh())])
        for name in ('data/opaque.bin', 'data/effect.nptl', 'data/content.ntex'):
            with self.subTest(name=name):
                (_, keys, pairs, extra), log = self.scan(data, name)
                self.assertEqual((len(keys), len(pairs), extra, log), (2, 1, {}, ''))

    def test_nested_archive_keeps_shader_pairs(self):
        data = fz.sarc([(b'nested.narc', archive([member(0x30, fz.bnsh())]))])
        found = []; ss.scan_blob(data, '/outer.sarc', found)
        self.assertEqual({v[1] for v in found}, {'vs', 'ps'})
        self.assertEqual(len(ss.var_pairs(found)), 1)

    def test_raw_empty_and_padded_stored_members(self):
        raw = fz.bnsh()
        (_, keys, pairs, extra), log = self.scan(archive([b'', raw, member(0x70, raw) + bytes(8)]))
        self.assertEqual((len(keys), len(pairs), extra, log), (2, 1, {}, ''))
        self.assertEqual(list(ss.narc_members(archive([]), '/empty')), [])

    def test_lzss_back_references_and_extended_lengths(self):
        # Negating 0xf1 gives four literals then four references; the last reference terminates the stream.
        expected = b'ABCDEFGHIJKLIJK' + b'K' * (0x12 + 0x115)
        stream = b'\0ABCDEFGH\xf1IJKL\x43\0\x10\0\x02\x11\0\x05\0\0\0'
        for header in (b'\x50\0\0\0' + len(expected).to_bytes(4, 'big'), b'\x60' + len(expected).to_bytes(3, 'big')):
            with self.subTest(tag=header[0]): self.assertEqual(ss.camelot_lzss(header + stream, '/lzss'), expected)

    def test_lzss_rejects_damaged_references(self):
        for stream, size, error in [(b'\x80\x12\0', 2, 'precedes'), (b'\x80\0\0', 2, 'ends after'),
                                    (b'\xc0A\x1f\0', 2, 'exceeds'), (b'\x80\x10\0', 16, 'length'),
                                    (b'\x80\x11\0\0', 0x110, 'length'), (b'\0', 1, 'literal'), (b'', 1, 'control')]:
            with self.subTest(stream=stream):
                with self.assertRaisesRegex(SystemExit, error): ss.camelot_lzss(b'\x60' + size.to_bytes(3, 'big') + stream, '/bad')

    def test_bad_layouts_name_the_archive_without_partial_shaders(self):
        raw = archive([member(0x10, fz.bnsh())]); bad = [raw[:12], raw[:-1]]
        for at, value in [(8, len(raw) + 1), (20, 8), (24, 2), (28, 0xffffffff), (32, 0xffffffff), (40, 7), (56, 0xffffffff)]:
            data = bytearray(raw); struct.pack_into('<I', data, at, value); bad.append(bytes(data))
        for data in bad:
            with self.subTest(header=data[:36]):
                (rel, keys, pairs, extra), log = self.scan(data)
                self.assertEqual((keys, pairs), ({}, set())); self.assertEqual(extra[0], 'unreadable')
                self.assertIn(rel, extra[1]); self.assertIn('NARC', extra[1]); self.assertTrue(log.startswith('[RED]'))

    def test_bad_members_do_not_publish_earlier_shaders(self):
        raw = fz.bnsh(); packed = member(0x10, raw)
        bad_crc = bytearray(packed); bad_crc[-1] ^= 1
        bad = [packed[:-1], bytes(bad_crc), b'\x10', member(0x70, raw)[:-1], b'\x94\0\0\0\x10\0\0\0\xff']
        for delta in (-1, 1):
            data = bytearray(packed); struct.pack_into('>I', data, 1, len(raw) + delta); bad.append(bytes(data))
        for data in bad:
            with self.subTest(member=data[:16]):
                (_, keys, pairs, extra), log = self.scan(archive([raw, data]))
                self.assertEqual((keys, pairs, extra[0]), ({}, set(), 'unreadable'))
                self.assertIn('court.narc#1', log); self.assertIn('NARC', log)

    def test_texture_and_vertex_buffers_skip_their_large_payloads(self):
        for magic in (b'BNTX', b'vlsr'):
            for tag in (0x10, 0x20, 0x30, 0x50, 0x60, 0x70, 0x80):
                data = bytearray(member(tag, magic + b'ignored'))
                if tag == 0x60: data[1:4] = (0xffffff).to_bytes(3, 'big')
                else: struct.pack_into('>I', data, 4 if tag == 0x50 else 1, 0xffffffff)
                with self.subTest(magic=magic, tag=tag):
                    self.assertEqual(list(ss.narc_members(archive([bytes(data)]), '/textures')), [])

    def test_decoded_budget_counts_all_members(self):
        packed = member(0x10, bytes(4096))
        with mock.patch.object(ss, 'NARC_FLOOR', 4096):
            self.assertEqual(list(ss.narc_members(archive([packed]), '/exact')), [('#0', bytes(4096))])
            with self.assertRaisesRegex(SystemExit, 'decoded byte budget'):
                list(ss.narc_members(archive([packed, member(0x10, b'x')]), '/too-large'))

    def test_nested_compressed_member_uses_shared_decoded_budget(self):
        data = fz.sarc([(b'nested.narc', archive([member(0x10, bytes(1 << 20))]))])
        with self.assertRaisesRegex(SystemExit, 'decoded byte budget'): ss.scan_blob(data, '/outer.sarc', [])

    def test_empty_nested_archives_still_consume_shared_budget(self):
        data = archive([])
        for _ in range(5):
            shared = bytearray(archive([data] + [b''] * 7))
            for i in range(1, 8): struct.pack_into('<II', shared, 28 + i * 8, 0, len(data))
            data = bytes(shared)
        with self.assertRaisesRegex(SystemExit, 'members (add up|exceed)'): ss.scan_blob(data, '/fanout.narc', [])

    def test_cli_matches_plain_archive_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); rom = root / 'RomFS'; rom.mkdir()
            source = rom / 'court.narc'; main = root / 'main'; main.write_bytes(b'NSO0' + bytes(0xfc))
            results = []
            for label, data in [('plain', archive([fz.bnsh()])), ('compressed', archive([member(0x10, fz.bnsh())]))]:
                source.write_bytes(data); dest = root / label; dest.mkdir()
                outputs = [dest / n for n in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
                proc = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', rom, main, *outputs], capture_output=True, text=True,
                                      env=dict(os.environ, DM_GAME='010015100B514000', J='2'))
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr); self.assertNotIn('[RED]', proc.stdout + proc.stderr)
                self.assertIn('shaders: 2 (', proc.stdout)
                results.append({str(p.relative_to(dest)): p.read_bytes() for p in dest.rglob('*') if p.is_file()})
            self.assertEqual(results[0], results[1])


if __name__ == '__main__': unittest.main()
