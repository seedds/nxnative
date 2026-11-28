"""Fenrir's gzip-wrapped BNSH records, using synthetic shaders only."""
import gzip
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'fuzz'))
import fuzz_shader_split as fz
ss = fz.ss


def record(data):
    packed = gzip.compress(data, mtime=0)
    return b'NSGZ    ' + struct.pack('<Q', len(packed)) + packed


def package(*records):
    return b'HSPK' + bytes(0x41c) + b''.join(records)


class Nsgz(unittest.TestCase):
    def scan(self, data):
        out = []; ss.scan_blob(data, '/shader.hsg', out)
        return out

    def test_multiple_records(self):
        out = self.scan(package(record(fz.bnsh()), bytes(32), record(fz.bnsh())))
        self.assertEqual([s for _, s, _, _ in out], ['vs', 'ps', 'vs', 'ps'])
        self.assertEqual([(c, b) for _, _, c, b in out], [fz.VS, fz.PS] * 2)
        self.assertNotEqual(out[0][0], out[2][0])

    def test_high_ratio_standalone_and_many_records(self):
        # Synthetic BNSH compresses >30x; top-level scans must not reject valid sparse records at 16x.
        single = record(fz.bnsh())
        self.assertGreater(len(fz.bnsh()), ss.NEST * len(single))
        self.assertEqual([(c, b) for _, _, c, b in self.scan(single)], [fz.VS, fz.PS])
        many = package(*([record(fz.bnsh())] * 10))
        self.assertGreater(10 * len(fz.bnsh()), ss.NEST * len(many))
        self.assertEqual(len(self.scan(many)), 20)

    def test_loose_file_reaches_scanner_through_prefilter(self):
        data = package(record(fz.bnsh()))
        self.assertNotIn(b'BNSH\0\0\0\0', data)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'shader.hsg'; path.write_bytes(data)
            _, keys, _, extra = ss.scan_file((str(path), 'shader.hsg'))
        self.assertEqual(extra, {})
        self.assertEqual({v[1] for v in keys.values()}, {'vs', 'ps'})

    def test_embedded_in_main_skips_literal_magics(self):
        literal_oob = b'NSGZ    ' + struct.pack('<Q', 0x40000000)
        literal_zero = b'NSGZ    ' + bytes(16)
        literal_non_gzip = b'NSGZ    ' + struct.pack('<Q', 24) + b'not a gzip stream at all'
        blob = bytes(256) + literal_oob + literal_zero + literal_non_gzip + record(fz.bnsh()) + b'NSGZ    '
        out = self.scan(blob)
        self.assertEqual([(c, b) for _, _, c, b in out], [fz.VS, fz.PS])

    def test_corrupt_embedded_gzip_record_still_fails(self):
        r = record(fz.bnsh())
        bad = r[:8] + struct.pack('<Q', len(r) - 17) + r[16:-1]
        with self.assertRaisesRegex(SystemExit, 'record length'): self.scan(bytes(256) + bad)

    def test_nested_archive(self):
        out = self.scan(fz.sarc([(b'shader.hsg', package(record(fz.bnsh())))]))
        self.assertEqual([(c, b) for _, _, c, b in out], [fz.VS, fz.PS])

    def test_nested_archive_shares_expansion_budget(self):
        bomb = fz.sarc([(b'a.hsg', package(record(bytes(1 << 20))))])
        with self.assertRaisesRegex(SystemExit, 'expansion budget'): self.scan(bomb)

    def test_truncated_header(self):
        with self.assertRaises(SystemExit): self.scan(package(b'NSGZ    '))

    def test_record_past_end(self):
        with self.assertRaisesRegex(SystemExit, 'past the package'): self.scan(package(record(fz.bnsh()))[:-1])

    def test_bad_gzip(self):
        with self.assertRaisesRegex(SystemExit, 'invalid gzip'): self.scan(package(b'NSGZ    ' + struct.pack('<Q', 4) + b'bad!'))

    def test_truncated_gzip(self):
        r = record(fz.bnsh())
        r = r[:8] + struct.pack('<Q', len(r) - 17) + r[16:-1]
        with self.assertRaisesRegex(SystemExit, 'record length'): self.scan(package(r))

    def test_trailing_gzip_data(self):
        r = record(fz.bnsh())
        r = r[:8] + struct.pack('<Q', len(r) - 15) + r[16:] + b'!'
        with self.assertRaisesRegex(SystemExit, 'record length'): self.scan(package(r))

    def test_wrong_payload(self):
        with self.assertRaisesRegex(SystemExit, 'not BNSH'): self.scan(package(record(b'wrong')))

    def test_wrong_bnsh_length(self):
        d = bytearray(fz.bnsh()); struct.pack_into('<I', d, 0x1c, len(d) + 1)
        with self.assertRaisesRegex(SystemExit, 'BNSH length'): self.scan(package(record(d)))

    def test_top_level_expansion_bound(self):
        with mock.patch.object(ss, 'NSGZ_MAX', 1 << 16):
            with self.assertRaisesRegex(SystemExit, 'expansion budget'): self.scan(package(record(bytes(1 << 20))))


if __name__ == '__main__': unittest.main()
