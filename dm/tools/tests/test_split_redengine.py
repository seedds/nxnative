"""shader_split: REDengine 3's shader caches (The Witcher 3: Wild Hunt — Complete Edition). shadernx.cache (rdhs_shaders) is a run of records {u64 hash, u8 1, u32 raw length,
u32 stored length, one zstd frame}, each a whole GLSLC output, ended by an index and the footer {u64 index offset, 'RDHS', u32 3}; staticshadernx.cache (fzlc_shaders) is a
'FZLC' header and a table of chunks whose joined zstd frames hold the GLSLC outputs, which run across chunk boundaries. REDengine 4's caches (Cyberpunk 2077 on the Switch 2,
rdhs4_shaders) hold NVN2 stages as {u64 hash, u32 code length, u32 control length, 0x20 bytes, code area, control section}, behind a footer 'RDHS' version 10 or 8. Synthetic bytes only."""
import contextlib
import io
import struct
import tempfile
import unittest
from pathlib import Path

from dm.tools.tests.test_split_gzip import fz, ss
from dm.tools.tests.test_split_viper import nvn2_stage

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


def rdhs4_shader(sph_type, salt):   # one REDengine 4 shader: the header, the code area, the NVN2 control section
    ctl, code = nvn2_stage(sph_type)
    code = code[:-8] + struct.pack('<Q', salt)
    return struct.pack('<QII', 0x2000 + salt, len(code), len(ctl)) + bytes(0x20) + code + ctl


def rdhs4_material(shaders, trailing=b''):   # records {u64 hash, u64 hash, u32 length} back to back, an index, the version 10 footer
    d = b''.join(struct.pack('<QQI', 0x3000 + i, 0x4000 + i, len(s) + len(trailing)) + s + trailing for i, s in enumerate(shaders))
    return d + b'technique\0' + struct.pack('<Q', len(d)) + ss.RDHS + struct.pack('<I', 10)


def rdhs4_static(shaders):   # records among technique entries, the version 8 footer and its hash
    d = b''.join(b'\x8bm_technique' + struct.pack('<QI', 0x5000 + i, len(s)) + s for i, s in enumerate(shaders))
    return d + ss.RDHS + struct.pack('<IQ', 8, 0x1234)


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

    def test_rdhs4_material_cache(self):   # a vertex and a pixel stage (SPH types 0x401 and 0x1402); trailing bytes after a control section are allowed
        (rel, keys, pairs, extra), log = self.scan(rdhs4_material([rdhs4_shader(0x401, 1), rdhs4_shader(0x1402, 2)], trailing=b'\x0a\x0b\x02'), 'engine/shaderemporium_final.cache')
        self.assertEqual(log, ''); self.assertEqual(extra, {})
        self.assertEqual(sorted(v[1] for v in keys.values()), ['ps', 'vs'])
        self.assertTrue(all('#RDHS4@' in v[0] for v in keys.values()))

    def test_rdhs4_static_cache(self):
        (rel, keys, pairs, extra), log = self.scan(rdhs4_static([rdhs4_shader(0x401, 1), rdhs4_shader(0x401, 1), rdhs4_shader(0x1402, 3)]), 'engine/staticshaderemporium_final.cache')
        self.assertEqual(log, ''); self.assertEqual(sorted(v[1] for v in keys.values()), ['ps', 'vs'])   # the repeated shader is one key

    def test_rdhs4_header_must_name_both_lengths(self):
        data = bytearray(rdhs4_material([rdhs4_shader(0x401, 1)]))
        struct.pack_into('<I', data, 0x14 + 8, 0x10)   # the record's code length no longer matches control +0x44
        (_, keys, pairs, extra), log = self.scan(bytes(data), 'engine/shaderemporium_final.cache')
        self.assertEqual((keys, extra[0]), ({}, 'unreadable'))
        self.assertIn('no record header names its code area', log)

    def test_witcher_footer_is_not_rdhs4(self):   # version 3 at the end stays with rdhs_shaders
        with tempfile.TemporaryDirectory() as t:
            path = Path(t) / 'shadernx.cache'; path.write_bytes(rdhs([program(1)]))
            self.assertIsNone(ss.rdhs4_version(str(path)))
            path.write_bytes(rdhs4_material([rdhs4_shader(0x401, 1)]))
            self.assertEqual(ss.rdhs4_version(str(path)), 10)


if __name__ == '__main__':
    unittest.main()
