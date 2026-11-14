"""shader_split: REDengine 3's shader caches (The Witcher 3: Wild Hunt — Complete Edition). shadernx.cache (rdhs_shaders) is a run of records {u64 hash, u8 1, u32 raw length,
u32 stored length, one zstd frame}, each a whole GLSLC output, ended by an index and the footer {u64 index offset, 'RDHS', u32 3}; staticshadernx.cache (fzlc_shaders) is a
'FZLC' header and a table of chunks whose joined zstd frames hold the GLSLC outputs, which run across chunk boundaries. Synthetic bytes only."""
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

    def test_flag_without_footer_is_not_a_cache(self):   # byte 8 is 1 but there is no RDHS footer: read as a plain file, where the program is still found
        data = bytes(8) + b'\x01' + bytes(7) + program(1)
        (rel, keys, pairs, extra), log = self.scan(data, 'other.bin')
        self.assertEqual(log, ''); self.assertEqual(len(keys), 2)


if __name__ == '__main__':
    unittest.main()
