"""CC4 name/hash mapping and bounded OODL reads, without game files."""
import contextlib
import io
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from dm.tools.tests.test_split_gzip import fz, ss
import ttdat


def archive(payload, raw=None, compact=False):
    names = b'shaders\0sample.nnx_shaders\0unrelated.bin\0'
    rows = struct.pack('>I4H', 0xffffffff, 0, 0, 0, 0)
    rows += struct.pack('>I4H', 0, 0, 0, 0, 0)
    rows += struct.pack('>I4H', 8, 1, 0, 0, 1)
    rows += struct.pack('>I4H', 27, 0, 0, 0, 2)
    count, at = 2, 0x100
    # Records deliberately differ from name order. The name record's hint is
    # zero, so only the full-path hash can find the shader.
    records = struct.pack('>QIIQII', 8, 4, 4, at | (9 << 56), len(payload), len(payload) if raw is None else raw)
    hashes = struct.pack('>QQ', ttdat.name_hash('unrelated.bin'), ttdat.name_hash('shaders/sample.nnx_shaders'))
    body = names + bytes(4) + rows + struct.pack('>iI', -13, count) + records + hashes
    table = struct.pack('>I8si4I', 28 + len(body), ttdat.MAGIC, -13, 2, count, 4, len(names)) + body
    off = (at + len(payload) + 255) & ~255
    pointer = (0xffffffff ^ ((off >> 8) - 1)) if compact else off
    return struct.pack('<II', pointer, len(table)) + b'data' + bytes(at - 12) + payload + bytes(off - at - len(payload)) + table


class Ttdat(unittest.TestCase):
    def entries(self, data):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'GAME.DAT'; path.write_bytes(data)
            return ttdat.entries(str(path))

    def test_hash_maps_name_to_record_and_compact_index_pointer(self):
        ctl, code = fz.VS; payload = ctl + code
        for compact in (False, True):
            entries = self.entries(archive(payload, compact=compact)); assert entries is not None
            self.assertEqual(entries, [('shaders/sample.nnx_shaders', 0x100, len(payload), len(payload))])
            self.assertEqual(ttdat.read(io.BytesIO(archive(payload)), entries[0], None), payload)
        self.assertEqual(ttdat.name_hash('SHADERS\\SAMPLE.NNX_SHADERS'), ttdat.name_hash('/shaders/sample.nnx_shaders'))

    def test_only_shader_members_become_split_jobs(self):
        ctl, code = fz.VS
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'GAME.DAT'; path.write_bytes(archive(ctl + code))
            jobs, taken = ss.ttdat_jobs([(str(path), path.name)])
            self.assertEqual(taken, {str(path)}); self.assertEqual(len(jobs), 1)
            rel, keys, pairs, extra = ss.scan_file(jobs[0])
            self.assertEqual(list(keys), [ss.skey(ctl, code)])
            self.assertEqual((rel, pairs, extra), ('GAME.DAT', set(), {}))

    def test_reflection_between_control_and_code(self):
        ctl, code = fz.VS
        result = ss.ttdat_programs(ctl + bytes(0x5a8) + code, 'older')
        self.assertEqual([v[1:] for v in result], [('vs', ctl, code)])
        with self.assertRaises(SystemExit): ss.ttdat_programs(ctl + bytes(0x5a8) + code + code, 'ambiguous')
        with self.assertRaises(SystemExit): ss.ttdat_programs(ctl + bytes(0x5a8) + ctl + code, 'next record')

    def test_chunked_oodle_and_stored_chunk(self):
        payload = struct.pack('<4sII', b'OODL', 3, 10) + b'cmp'
        payload += struct.pack('<4sII', b'OODL', 4, 4) + b'last'
        decoder = mock.Mock(return_value=b'decoded123')
        self.assertEqual(ttdat.read(io.BytesIO(payload), ('test', 0, len(payload), 14), decoder), b'decoded123last')
        decoder.assert_called_once_with(b'cmp', 10)

    def test_invalid_chunks_never_publish_partial_member(self):
        good = struct.pack('<4sII', b'OODL', 3, 10) + b'cmp'
        for payload, size in [(good[:-1], 10), (good, 9), (good, 11), (good + b'x', 10), (b'OODL' + bytes(8), 10)]:
            with self.subTest(payload=payload, size=size), self.assertRaises(ValueError):
                ttdat.read(io.BytesIO(payload), ('test', 0, len(payload), size), lambda *_: b'decoded123')
        with self.assertRaises(ValueError): ttdat.read(io.BytesIO(good), ('test', 0, len(good), 10), lambda *_: b'bad')
        oversized = struct.pack('<4sII', b'OODL', ttdat.MAX_MEMBER + 1, 10)
        with self.assertRaises(ValueError): ttdat.read(io.BytesIO(oversized), ('test', 0, ttdat.MAX_MEMBER + 13, 10), lambda *_: b'bad')

    def test_unrecognized_data_is_left_for_other_readers(self):
        self.assertIsNone(self.entries(b'not a TT Games archive'))

    def test_invalid_index_is_reported_by_splitter(self):
        data = bytearray(archive(b'bytes'))
        off = struct.unpack_from('<I', data)[0]
        struct.pack_into('>I', data, off + 20, 0xffffffff)
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            path = Path(tmp) / 'bad.dat'; path.write_bytes(data)
            jobs, consumed = ss.ttdat_jobs([(str(path), path.name)])
            self.assertEqual(consumed, {str(path)})
            self.assertEqual(ss.scan_file(jobs[0])[3][0], 'unreadable')

    def test_missing_name_hash_does_not_select_another_record(self):
        data = bytearray(archive(b'bytes')); data[-8:] = bytes(8)
        with self.assertRaisesRegex(ValueError, 'no matching CC4 file hash'): self.entries(data)

    def test_member_outside_data_region_is_rejected(self):
        data = bytearray(archive(b'bytes')); off = struct.unpack_from('<I', data)[0]
        names = struct.unpack_from('>I', data, off + 28)[0]
        record = off + 32 + names + 4 + 48 + 8 + 16
        struct.pack_into('>Q', data, record, off)
        with self.assertRaisesRegex(ValueError, 'range'): self.entries(data)

    def test_shader_sources_request_decoder_for_ttgames(self):
        from profile_guess import shader_sources
        result = shader_sources({'nvnBootstrapLoader'}, 0, [], 0, 0, oodle_ttdat=5)
        self.assertTrue(result['oodle_packs'][0]); self.assertIn('TT Games', result['oodle_packs'][1])


if __name__ == '__main__': unittest.main()
