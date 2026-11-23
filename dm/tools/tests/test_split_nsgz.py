"""Fenrir's gzip-wrapped BNSH records, using synthetic shaders only."""
import gzip
import struct
import sys
import unittest
from pathlib import Path

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

    def test_embedded_in_main(self):
        out = self.scan(bytes(1024) + record(fz.bnsh()))
        self.assertEqual([(c, b) for _, _, c, b in out], [fz.VS, fz.PS])

    def test_nested_archive(self):
        out = self.scan(fz.sarc([(b'shader.hsg', package(record(fz.bnsh())))]))
        self.assertEqual([(c, b) for _, _, c, b in out], [fz.VS, fz.PS])

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

    def test_expansion_bound(self):
        with self.assertRaisesRegex(SystemExit, 'exceed 16 times'): self.scan(package(record(bytes(1 << 20))))


if __name__ == '__main__': unittest.main()
