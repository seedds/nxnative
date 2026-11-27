"""Synthetic checks for REDengine 3's GLSLC caches and REDengine 4's RDHS caches with NVN2 stages."""
import contextlib
import io
import struct
import tempfile
import unittest
from pathlib import Path

from dm.tools.tests.test_split_gzip import fz, ss

ZSTD = ss.zstd_mod()


def program(salt):
    """A GLSLC output of a vertex and a pixel stage whose code differs by salt, so each program has keys of its own"""
    stages = []
    for ctl, code in (fz.VS, fz.PS):
        code = bytearray(code); code[-8:] = bytes.fromhex('0f00000000000000'); code[-16:-8] = struct.pack('<Q', 0x0f00 + salt); stages.append((ctl, bytes(code)))
    return fz.glslc(stages)


def rdhs(blobs, index=b'materials/a.w2mg\0', raw_delta=0):
    d = bytearray()
    for i, b in enumerate(blobs):
        z = ZSTD.compress(b); d += struct.pack('<QBII', 0x1000 + i, 1, len(b) + raw_delta, len(z)) + z
    at = len(d)
    return bytes(d) + index + struct.pack('<Q4sI', at, ss.RDHS, 3)


def fzlc(data, chunk, compress=True):
    parts = [data[i:i + chunk] for i in range(0, len(data), chunk)]
    stored = [ZSTD.compress(p) if compress else p for p in parts]
    at = 12 + 12 * len(parts); head = bytearray(ss.FZLC + struct.pack('<II', len(parts), at)); pos = at
    for p, s in zip(parts, stored): pos += len(s); head += struct.pack('<3I', len(s), len(p), pos)
    return bytes(head) + b''.join(stored)


def nvn2_record(key_size, stage, metadata=b'', attribute=None):
    code = bytearray(0x100)
    struct.pack_into('<I', code, 0, {'vs': 0x400, 'ps': 0x1400, 'cs': 0}[stage])
    if attribute is not None: code[0x18 + attribute] = 0x02
    ctl = bytearray(0xd8)
    struct.pack_into('<4sI', ctl, 0, ss.CTL_MAGIC, 0xc9)
    struct.pack_into('<I16xI', ctl, 0x30, len(ctl), len(code))
    if stage == 'cs': ctl[0x56] = 5
    header = struct.pack('<8xII28xI', len(code), len(ctl), len(metadata) // 3)
    payload = header + code + ctl + metadata
    return bytes(key_size) + struct.pack('<I', len(payload)) + payload


class RedEngineCaches(unittest.TestCase):
    def scan(self, data, name):
        with tempfile.TemporaryDirectory() as t, contextlib.redirect_stdout(io.StringIO()) as log:
            path = Path(t) / 'cache'; path.write_bytes(data)
            result = ss.scan_file((str(path), name))
        return result, log.getvalue()

    def test_material_cache_records(self):
        (rel, keys, pairs, extra), log = self.scan(rdhs([program(1), program(2)]), 'content/content0/shadernx.cache')
        self.assertEqual(log, ''); self.assertEqual(extra, {})
        self.assertEqual(len(keys), 4); self.assertEqual(sorted(v[1] for v in keys.values()), ['ps', 'ps', 'vs', 'vs'])
        self.assertEqual(len(pairs), 2)

    def test_static_cache_joins_chunks(self):   # each program is cut across chunks: read chunk by chunk, no output would be whole
        data = program(1) + program(2)
        (rel, keys, pairs, extra), log = self.scan(fzlc(data, len(data) // 5), 'content/content0/staticshadernx.cache')
        self.assertEqual(log, ''); self.assertEqual(len(keys), 4); self.assertEqual(len(pairs), 2)

    def test_static_cache_stub(self):   # the DLCs' copies: one chunk kept uncompressed, no shaders
        (rel, keys, pairs, extra), log = self.scan(fzlc(bytes(28), 28, compress=False), 'dlc/dlc1/content/staticshadernx.cache')
        self.assertEqual((keys, log), ({}, ''))

    def test_damaged_record_is_named(self):
        (rel, keys, pairs, extra), log = self.scan(rdhs([program(1)], raw_delta=4), 'content/patch0/shadernx.cache')
        self.assertEqual(keys, {}); self.assertEqual(extra[0], 'unreadable')
        self.assertIn('content/patch0/shadernx.cache: record 0 at 0x0 decompresses to', log)

    def test_damaged_first_record_flag_is_named(self):
        data = bytearray(rdhs([program(1)])); data[8] = 2
        (_, keys, pairs, extra), log = self.scan(data, 'content/patch0/shadernx.cache')
        self.assertEqual((keys, pairs, extra[0]), ({}, set(), 'unreadable'))
        self.assertIn('record 0 at 0x0: flag 2', log)

    def test_zstd_errors_name_the_record_or_chunk(self):
        static = fzlc(program(1), len(program(1)))
        static_at = struct.unpack_from('<I', static, 8)[0]
        frames = [('shadernx.cache', rdhs([program(1)]), 17, 'record 0 at 0x0'),
                  ('staticshadernx.cache', static, static_at, f'chunk 0 at {static_at:#x}')]
        for name, cache, at, where in frames:
            with self.subTest(name=name):
                data = bytearray(cache); data[at + 4] |= 0x08   # the Zstandard frame descriptor's reserved bit
                (_, keys, pairs, extra), log = self.scan(data, name)
                self.assertEqual((keys, pairs, extra[0]), ({}, set(), 'unreadable'))
                self.assertIn(f'{name}: {where}: invalid zstd frame', log)

    def test_missing_static_chunk_reports_an_unreadable_file(self):
        first = program(1)
        data = bytearray(fzlc(first + program(2), len(first), compress=False))
        struct.pack_into('<I', data, 24, 0)   # second chunk's stored length, with its declared raw length intact
        (_, keys, pairs, extra), log = self.scan(data, 'content/content0/staticshadernx.cache')
        self.assertEqual((keys, pairs, extra[0]), ({}, set(), 'unreadable'))
        self.assertIn(f'chunk 1 at {36 + len(first):#x}: {len(first)} decoded bytes but no stored data', log)

    def test_flag_without_footer_is_not_a_cache(self):   # byte 8 is 1 but there is no RDHS footer: read as a plain file, where the program is still found
        data = bytes(8) + b'\x01' + bytes(7) + program(1)
        (rel, keys, pairs, extra), log = self.scan(data, 'other.bin')
        self.assertEqual(log, ''); self.assertEqual(len(keys), 2)

    def test_nvn2_material_stages_with_variable_metadata(self):
        records = nvn2_record(16, 'vs', b'123456') + nvn2_record(16, 'ps')
        footer = struct.pack('<8xQ24x', len(records)) + ss.RDHS_MATERIAL
        (_, keys, pairs, extra), log = self.scan(records + b'index' + footer, 'materials.cache')
        self.assertEqual({v[1] for v in keys.values()}, {'vs', 'ps'})
        self.assertEqual((pairs, extra, log), (set(), {}, ''))

    def test_nvn2_static_record_count_and_start(self):
        records = b'metadata' + ss.CTL_MAGIC + b'\xc9\0\0\0' + bytes(64)
        records += nvn2_record(8, 'cs') + nvn2_record(8, 'ps', b'123')
        for count in (2, 1):
            with self.subTest(count=count):
                footer = struct.pack('<4xI8x', count) + ss.RDHS_STATIC + bytes(8)
                (_, keys, pairs, extra), log = self.scan(records + footer, 'static.cache')
                if count == 2:
                    self.assertEqual({v[1] for v in keys.values()}, {'cs', 'ps'})
                    self.assertEqual((pairs, extra, log), (set(), {}, ''))
                else:
                    self.assertEqual((keys, pairs, extra[0]), ({}, set(), 'unreadable'))
                    self.assertIn('read 2 static shaders, expected 1', log)

    def test_nvn2_material_record_stops_at_index(self):
        record = nvn2_record(16, 'vs')
        footer = struct.pack('<8xQ24x', len(record) - 1) + ss.RDHS_MATERIAL
        (_, keys, pairs, extra), log = self.scan(record + footer, 'materials.cache')
        self.assertEqual((keys, pairs, extra[0]), ({}, set(), 'unreadable'))
        self.assertIn('RDHS record at 0x0: inconsistent payload', log)

    def test_nvn2_cover_reads_the_sph_at_the_start_of_code(self):
        records = nvn2_record(16, 'vs') + nvn2_record(16, 'ps', attribute=3) + nvn2_record(16, 'ps', attribute=12)
        footer = struct.pack('<8xQ24x', len(records)) + ss.RDHS_MATERIAL
        (_, keys, _, _), log = self.scan(records + footer, 'materials.cache')
        cover, pixels = ss.separable_pairs(keys)
        self.assertEqual(cover, {(v, p) for v in keys if keys[v][1] == 'vs' for p in keys if keys[p][1] == 'ps'})
        self.assertEqual((len(pixels), log), (2, ''))


if __name__ == '__main__':
    unittest.main()
