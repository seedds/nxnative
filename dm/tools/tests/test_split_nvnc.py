"""shader_split: an NVNC shader cache (Alien: Isolation's resources/nvn_shader_cache): a 16-byte header {'NVNC', u32 version 1, u32 count, u32 blob size}, count 64-byte records
with the entry's u32 offset at +0x38 and u32 size at +0x3c, then the blob of whole GLSLC outputs, offsets counted from its start. Synthetic bytes only."""
import contextlib
import io
import struct
import tempfile
import unittest
from pathlib import Path

from dm.tools.tests.test_split_gzip import fz, ss


def program(salt):
    """A GLSLC output of a vertex and a pixel stage whose code differs by salt, so each program has keys of its own"""
    stages = []
    for ctl, code in (fz.VS, fz.PS):
        code = bytearray(code); code[-8:] = bytes.fromhex('0f00000000000000'); code[-16:-8] = struct.pack('<Q', 0x0f00 + salt); stages.append((ctl, bytes(code)))
    return fz.glslc(stages)


def nvnc(entries, version=1, count=None, extra=b''):
    records = bytearray(); blob = bytearray()
    for i, e in enumerate(entries):
        r = bytearray(64); r[:8] = struct.pack('<Q', 0x1000 + i)
        r[0x38:0x40] = struct.pack('<II', len(blob), len(e)); records += r; blob += e
    return b'NVNC' + struct.pack('<III', version, len(entries) if count is None else count, len(blob)) + bytes(records) + bytes(blob) + extra


class NvncCache(unittest.TestCase):
    def scan(self, data):
        with tempfile.TemporaryDirectory() as t, contextlib.redirect_stdout(io.StringIO()) as log:
            path = Path(t) / 'nvn_shader_cache'; path.write_bytes(data)
            result = ss.scan_file((str(path), 'resources/nvn_shader_cache'))
        return result, log.getvalue()

    def test_every_entry_is_read(self):
        (_, keys, pairs, extra), log = self.scan(nvnc([program(1), program(2), program(3)]))
        self.assertEqual((log, extra), ('', {}))
        self.assertEqual(sorted(v[1] for v in keys.values()), ['ps'] * 3 + ['vs'] * 3)
        self.assertEqual(len(pairs), 3)

    def test_entries_in_another_order(self):   # the records name the entries, which need not follow them in order
        a, b = program(1), program(2)
        data = bytearray(nvnc([a, b]))
        data[16 + 0x38:16 + 0x40] = struct.pack('<II', len(a), len(b)); data[80 + 0x38:80 + 0x40] = struct.pack('<II', 0, len(a))
        (_, keys, pairs, extra), log = self.scan(data)
        self.assertEqual((log, extra), ('', {})); self.assertEqual((len(keys), len(pairs)), (4, 2))

    def assert_unreadable(self, data, text):
        (_, keys, pairs, extra), log = self.scan(data)
        self.assertEqual((keys, pairs, extra[0]), ({}, set(), 'unreadable'))
        self.assertIn(text, log)

    def test_unknown_version_is_named(self):
        self.assert_unreadable(nvnc([program(1)], version=2), 'version 2')

    def test_sizes_must_add_up(self):
        self.assert_unreadable(nvnc([program(1)], extra=b'\0'), 'do not add up')

    def test_record_past_the_blob_is_named(self):
        data = bytearray(nvnc([program(1)]))
        data[16 + 0x38:16 + 0x3c] = struct.pack('<I', 0x10000)
        self.assert_unreadable(data, 'entry 0')

    def test_entry_without_glslc_output_is_named(self):
        self.assert_unreadable(nvnc([b'not a shader' * 8]), 'holds no whole GLSLC output')


if __name__ == '__main__':
    unittest.main()
