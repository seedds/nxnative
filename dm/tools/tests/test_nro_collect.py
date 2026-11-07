"""NRR layout selection must follow the header, even when both tables look valid."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import nro_collect


def header(kind=0):
    data = bytearray(0x1000)
    data[:4] = b'NRR0'
    data[5] = data[6] = data[0x20] = kind
    meta = 0x330 if kind == 0 else 0x4B0
    struct.pack_into('<I', data, meta + 8, len(data))
    struct.pack_into('<II', data, meta + 0x10, meta + 0x20, 1)
    data[meta + 0x20:meta + 0x40] = b'A' * 32
    return data


class NrrHeaders(unittest.TestCase):
    def collect(self, data):
        with tempfile.TemporaryDirectory() as root:
            (Path(root) / 'sample.nrr').write_bytes(data)
            return nro_collect.nrr_hashes(root)

    def test_header_selects_table_even_when_other_layout_is_plausible(self):
        for kind, other in ((0, 0x4B0), (1, 0x330)):
            with self.subTest(kind=kind):
                data = header(kind)
                struct.pack_into('<I', data, other + 8, len(data))
                struct.pack_into('<II', data, other + 0x10, other + 0x20, 1)
                data[other + 0x20:other + 0x40] = b'B' * 32
                self.assertEqual(self.collect(data), {b'A' * 32: 'sample.nrr'})

    def test_sign_key_generation_is_not_layout(self):
        data = header()
        data[4] = 7
        self.assertEqual(self.collect(data), {b'A' * 32: 'sample.nrr'})

    def test_unknown_or_mixed_discriminators_name_header(self):
        for offset in (5, 6, 0x20):
            for value in (1, 2):
                with self.subTest(offset=offset, value=value):
                    data = header()
                    data[offset] = value
                    with self.assertRaisesRegex(SystemExit, 'sample.nrr: unsupported NRR header'):
                        self.collect(data)

    def test_truncated_headers_and_bad_magic(self):
        for kind, limit in ((0, 0x350), (1, 0x4D0)):
            for length in (0, 3, 0x20, limit - 1):
                with self.subTest(kind=kind, length=length):
                    with self.assertRaisesRegex(SystemExit, 'sample.nrr: .*NRR header'):
                        self.collect(header(kind)[:length])
        data = header()
        data[:4] = b'NOPE'
        with self.assertRaisesRegex(SystemExit, 'sample.nrr: invalid NRR header: magic=4e4f5045'):
            self.collect(data)

    def test_table_bounds_use_declared_size_and_reject_swapped_fields(self):
        for kind, meta in ((0, 0x330), (1, 0x4B0)):
            cases = ((meta + 0x1F, meta + 0x20, 0), (0x1001, meta + 0x20, 1),
                     (0x1000, meta, 1), (0x1000, 0x1001, 0),
                     (0x1000, meta + 0x20, 0xFFFFFFFF),
                     (meta + 0x20, meta + 0x20, 1), (0x1000, 1, meta + 0x20))
            for size, offset, count in cases:
                with self.subTest(kind=kind, size=size, offset=offset, count=count):
                    data = header(kind)
                    struct.pack_into('<I', data, meta + 8, size)
                    struct.pack_into('<II', data, meta + 0x10, offset, count)
                    with self.assertRaisesRegex(SystemExit, 'sample.nrr: invalid NRR hash bounds'):
                        self.collect(data)

    def test_empty_table_is_not_an_unknown_layout(self):
        data = header()
        struct.pack_into('<I', data, 0x344, 0)
        self.assertEqual(self.collect(data), {})


if __name__ == '__main__':
    unittest.main()
