"""REDengine's shader containers (The Witcher 3): the RDHS v3 caches (content/*/shadernx.cache, a run of zstd records
{u64 hash, u8 1, u32 raw length, u32 stored length}, each holding a GLSLC output, a name index and a trailer naming it) and the
FZLC chunked zstd of staticshadernx.cache ({u64, u64, u32 length, GLSLC output} records in the joined chunks). Synthetic bytes only."""
import contextlib
import io
import struct
import tempfile
import unittest
from pathlib import Path

from dm.tools.tests.test_split_gzip import ss
import dm.tools.fuzz.fuzz_shader_split as fz


def rdhs(records, version=3):
    """A shadernx.cache: the records ({u64 hash, u8 1, u32 raw length, u32 stored length, zstd frame} of each (hash, raw) given), the index, then the trailer"""
    zstd = ss.zstd_mod(); out = bytearray()
    for h, raw in records:
        stored = zstd.compress(raw)
        out += struct.pack('<QBII', h, 1, len(raw), len(stored)) + stored
    ioff = len(out); out += b'index' + bytes(0x20)
    out += struct.pack('<IIQ', len(records), 0, 0) + struct.pack('<QQ', ioff, len(out) - ioff) + struct.pack('<Q', ioff) + b'RDHS' + struct.pack('<I', version)
    return bytes(out)


def fzlc_file(payload, chunk=0x1000):
    """'FZLC', u32 chunk count, per chunk {u32 offset, u32 stored length, u32 length} of a zstd frame holding a slice of the payload"""
    zstd = ss.zstd_mod(); chunks = [(zstd.compress(payload[i:i + chunk]), min(chunk, len(payload) - i)) for i in range(0, len(payload), chunk)]
    head = b'FZLC' + struct.pack('<I', len(chunks)); table = b''; at = 8 + 12 * len(chunks)
    for stored, us in chunks: table += struct.pack('<3I', at, len(stored), us); at += len(stored)
    return head + table + b''.join(c for c, _ in chunks)


def static_records(*outputs):
    """staticshadernx.cache's payload: {u64 hash, u64 hash, u32 length, GLSLC output} records laid end to end"""
    return b''.join(struct.pack('<QQI', i, i + 1, len(o)) + o for i, o in enumerate(outputs)) + bytes(0x1c)


class Rdhs(unittest.TestCase):
    def scan(self, data, rel='shadernx.cache'):
        with tempfile.TemporaryDirectory() as t, contextlib.redirect_stdout(io.StringIO()) as log:
            path = Path(t) / rel; path.write_bytes(data)
            result = ss.scan_file((str(path), rel))
        return result, log.getvalue()

    def test_records_hold_their_glslc_outputs(self):
        d = rdhs([(0x1111, fz.glslc([fz.VS])), (0x2222, fz.glslc([fz.PS]))])
        (rel, keys, pairs, extra), log = self.scan(d)
        self.assertEqual(log, '')
        self.assertEqual({v[1] for v in keys.values()}, {'vs', 'ps'})
        self.assertEqual(len(keys), 2)
        self.assertTrue(all('#RDHS@' in v[0] and '#GLSLC@' in v[0] for v in keys.values()))

    def test_every_stage(self):
        vs2 = fz.stage(0x0469)   # a second, distinct vertex shader
        d = rdhs([(i, fz.glslc([s])) for i, s in enumerate((fz.VS, fz.PS, vs2))])
        (rel, keys, pairs, extra), log = self.scan(d)
        self.assertEqual(len(keys), 3)
        self.assertEqual(sorted(v[1] for v in keys.values()), ['ps', 'vs', 'vs'])

    def test_not_rdhs_is_untouched(self):
        self.assertEqual(ss.rdhs_shaders(bytes(0x100), '/x'), [])
        self.assertEqual(ss.rdhs_shaders(b'tiny', '/x'), [])
        d = rdhs([(1, fz.glslc([fz.VS]))])
        self.assertEqual(ss.rdhs_shaders(d[:-12] + b'NONE' + d[-4:], '/x'), [])   # no trailer magic: not read

    def test_broken_run_is_named(self):
        d = bytearray(rdhs([(1, fz.glslc([fz.VS])), (2, fz.glslc([fz.VS]))]))
        d[8] = 9   # record 0's type byte
        with self.assertRaisesRegex(SystemExit, 'breaks the run'):
            ss.rdhs_shaders(bytes(d), '/bad')

    def test_size_mismatch_is_named(self):
        d = bytearray(rdhs([(1, fz.glslc([fz.VS]))]))
        struct.pack_into('<I', d, 0x09, 1)   # the raw length no longer matches the frame's
        with self.assertRaisesRegex(SystemExit, 'header says'):
            ss.rdhs_shaders(bytes(d), '/bad')


class Fzlc(unittest.TestCase):
    def scan(self, data, rel='staticshadernx.cache'):
        with tempfile.TemporaryDirectory() as t, contextlib.redirect_stdout(io.StringIO()) as log:
            path = Path(t) / rel; path.write_bytes(data)
            result = ss.scan_file((str(path), rel))
        return result, log.getvalue()

    def test_chunked_glslc_records(self):
        d = fzlc_file(static_records(fz.glslc([fz.VS]), fz.glslc([fz.PS]), fz.glslc([fz.stage(0x0469)])))
        (rel, keys, pairs, extra), log = self.scan(d)
        self.assertEqual(log, '')
        self.assertEqual(len(keys), 3)
        self.assertEqual(sorted(v[1] for v in keys.values()), ['ps', 'vs', 'vs'])
        self.assertTrue(all('#FZLC#GLSLC@' in v[0] for v in keys.values()))

    def test_a_record_may_span_chunks(self):
        d = fzlc_file(static_records(fz.glslc([fz.VS]), fz.glslc([fz.PS])), chunk=0x200)
        (rel, keys, pairs, extra), log = self.scan(d)
        self.assertEqual(len(keys), 2)

    def test_bad_table_is_named(self):
        with self.assertRaisesRegex(SystemExit, 'chunk table'):
            ss.fzlc_shaders(b'FZLC' + struct.pack('<I', 1 << 28), '/bad')

    def test_non_zstd_chunk_is_named(self):
        d = bytearray(fzlc_file(static_records(fz.glslc([fz.VS]))))
        struct.pack_into('<3I', d, 8, len(d) - 0x14, 0x14, 0x100)   # point the chunk at the trailer, which is not a frame
        with self.assertRaisesRegex(SystemExit, 'not a zstd frame'):
            ss.fzlc_shaders(bytes(d), '/bad')

    def test_stored_chunk(self):
        """The DLCs' staticshadernx.cache stubs keep their one short chunk uncompressed (stored length == raw length)"""
        payload = static_records(fz.glslc([fz.VS]))
        d = b'FZLC' + struct.pack('<I', 1) + struct.pack('<3I', 0x14, len(payload), len(payload)) + payload
        (rel, keys, pairs, extra), log = self.scan(d)
        self.assertEqual(log, '')
        self.assertEqual(len(keys), 1)
        self.assertEqual(list(keys.values())[0][1], 'vs')


if __name__ == '__main__':
    unittest.main()
