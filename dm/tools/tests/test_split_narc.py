"""Loose NARC files reach the reader however their members are packed: packing hides the bytes the split's prefilter looks for. Synthetic bytes only (issue #1131)."""
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

import lz4.block

from dm.tools.tests.test_split_gzip import REPO, fz, ss

BNSH = b'BNSH\0\0\0\0'


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


def lz(data):
    """Camelot's LZ (cra.lz_undo) with back references over runs, which is what hides BNSH's zeros, and the distance 0 that ends it."""
    tokens, i = [], 0
    while i < len(data):
        run = 0
        while i and run < 15 and i + run < len(data) and data[i + run] == data[i - 1]: run += 1
        if run >= 2: tokens.append((1, bytes([0x10 | run, 0]))); i += run
        else: tokens.append((0, data[i:i + 1])); i += 1
    tokens.append((1, b'\0\0'))
    out = bytearray()
    for at in range(0, len(tokens), 8):
        group = tokens[at:at + 8]
        out.append(-sum(kind << (7 - k) for k, (kind, _) in enumerate(group)) & 0xff)
        for _, body in group: out += body
    return bytes(out)


def member(tag, data):
    if tag == 0x50: return b'\x50\0\0\0' + len(data).to_bytes(4, 'big') + lz(data)
    if tag == 0x60: return b'\x60' + len(data).to_bytes(3, 'big') + lz(data)
    if tag == 0x94: return b'\x94\0\0\0' + struct.pack('<I', len(data)) + lz4.block.compress(data, store_size=False)
    if tag in (0x10, 0x20, 0x30):
        packer = zlib.compressobj(wbits=-15 if tag == 0x30 else 15)
        data = packer.compress(data) + packer.flush()
    return bytes([tag]) + len(data).to_bytes(4, 'big') + bytes(11) + data


def chunks(data):
    packed = member(0x10, data)
    head = b'MMMM' + struct.pack('<III', len(data), 1, len(packed))
    return head + bytes(-len(head) % 16) + packed


class LooseNarc(unittest.TestCase):
    def scan(self, data, name='model/court.narc'):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()) as log:
            path = Path(directory) / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
            result = ss.scan_file((str(path), name))
        return result, log.getvalue()

    def test_packed_members_keep_stages_and_pairs(self):
        raw = fz.bnsh(); (_, expected, pairs, _), _ = self.scan(raw)
        self.assertEqual({v[1] for v in expected.values()}, {'vs', 'ps'}); self.assertEqual(len(pairs), 1)
        packed = {hex(tag): member(tag, raw) for tag in (0x10, 0x20, 0x30, 0x50, 0x60, 0x94)}
        packed['chunks'] = chunks(raw)
        for kind, data in packed.items():
            with self.subTest(kind=kind):
                self.assertNotIn(BNSH, data)   # what the prefilter would have looked for
                (_, keys, got_pairs, extra), log = self.scan(archive([data]))
                self.assertEqual({k: v[1:] for k, v in keys.items()}, {k: v[1:] for k, v in expected.items()})
                self.assertEqual((got_pairs, extra, log), (pairs, {}, ''))

    def test_stored_members_and_empty_entries(self):
        raw = fz.bnsh()
        for tag in (0x70, 0x80):
            with self.subTest(tag=hex(tag)):
                (_, keys, pairs, _), log = self.scan(archive([b'', member(tag, raw), raw]))
                self.assertEqual((len(keys), len(pairs), log), (2, 1, ''))

    def test_the_magic_decides_not_the_name(self):
        data = archive([member(0x10, fz.bnsh())])
        for name in ('data/opaque.bin', 'data/effect.nptl', 'data/content.ntex'):
            with self.subTest(name=name):
                (_, keys, pairs, _), log = self.scan(data, name)
                self.assertEqual((len(keys), len(pairs), log), (2, 1, ''))

    def test_a_narc_without_shaders_is_quiet(self):
        (_, keys, pairs, extra), log = self.scan(archive([member(0x10, b'BNTX' + bytes(64)), b'']))
        self.assertEqual((keys, pairs, extra, log), ({}, set(), {}, ''))

    def test_malformed_archive_and_member_keep_the_reader_diagnosis(self):
        missing_sections = struct.pack('<4sHHIHH', b'NARC', 0xfffe, 0, 16, 16, 0)
        cases = (
            (missing_sections, 'no BTAF or GMIF'),
            (archive([b'\x60\0\0\x02\x80\x12\0']), 'an LZ distance before the start'),
            (archive([b'\x60\0\0\0\x01']), 'an LZ stream cut inside a token'),
            (archive([b'\x94\0\0\0\xff\xff\xff\xff\0\0\0\0']), 'would expand to 4294967295'),
            (archive([b'\x94\0\0\0\x10\0\0\0\xff\xff']), 'an LZ4 member:'),
            (archive([b'Z\0\0\0\x10\0\0\0\0\0\0\0']), 'a zstd member:'),
            (archive([b'MMMM\0\0']), 'MMMM header cut'),
            (archive([b'MMMM' + struct.pack('<II', 0, 9)]), 'MMMM table of 9 chunks'),
        )
        for data, message in cases:
            with self.subTest(message=message):
                with self.assertRaisesRegex(SystemExit, r'\[RED\] /court.narc.*' + message):
                    ss.scan_blob(data, '/court.narc', [])
                fz.fuzzlib.replay('shader_split', fz.check, data)   # accepted under the fuzz harness's allocation guard

    def test_cra_shaders_reports_member_errors_through_red(self):
        import cra
        raw = b'\x94\0\0\0\xff\xff\xff\xff\0\0\0\0'
        plain = raw + bytes(-len(raw) & 15)
        with tempfile.TemporaryDirectory() as directory:
            arc = Path(directory) / '42'
            # 2021 layout: scramble by reversing bits and xoring with keys_2021
            ks = cra.keys_2021('shader/bad.bin')
            scrambled = (int.from_bytes(plain.translate(cra.REV8), 'little')
                         ^ int.from_bytes((ks * (len(plain) // 16 + 1))[:len(plain)], 'little')).to_bytes(len(plain), 'little')
            arc.write_bytes(scrambled)
            entry = (1, 'shader/bad.bin', len(raw), 0, zlib.crc32(raw) ^ cra.SUM_2021, cra.LAYOUT_2021)
            with self.assertRaisesRegex(SystemExit, r'\[RED\] /42/shader/bad\.bin: .*would expand to 4294967295'):
                ss.cra_shaders(str(arc), '42', (entry,), [])

    def test_narc_tables_are_bounded_before_reading(self):
        raw = archive([b'plain'])
        cases = [(b'NARC', 'header cut short')]
        for offset, value, message in ((20, 0, 'invalid NARC section size'), (20, len(raw), 'invalid NARC section size'),
                                       (24, 0xffffffff, 'member table cut short'), (32, 0xffffffff, 'past the GMIF end')):
            data = bytearray(raw); struct.pack_into('<I', data, offset, value); cases.append((bytes(data), message))
        for data, message in cases:
            with self.subTest(message=message), self.assertRaisesRegex(SystemExit, r'\[RED\] /court.narc: .*' + message):
                ss.scan_blob(data, '/court.narc', [])

    def test_cli_matches_a_plain_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); rom = root / 'RomFS'; rom.mkdir()
            source = rom / 'court.narc'; main = root / 'main'; main.write_bytes(b'NSO0' + bytes(0xfc))
            results = []
            for label, data in [('plain', archive([fz.bnsh()])), ('packed', archive([member(0x10, fz.bnsh())]))]:
                source.write_bytes(data); dest = root / label; dest.mkdir()
                outputs = [dest / n for n in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
                proc = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', rom, main, *outputs], capture_output=True, text=True,
                                      env=dict(os.environ, DM_GAME='010015100B514000', J='2'))
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr); self.assertNotIn('[RED]', proc.stdout + proc.stderr)
                self.assertIn('shaders: 2 (', proc.stdout)
                results.append({str(p.relative_to(dest)): p.read_bytes() for p in dest.rglob('*') if p.is_file()})
            self.assertEqual(results[0], results[1])


if __name__ == '__main__': unittest.main()
