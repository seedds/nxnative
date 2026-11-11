import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from renderdragon import HEADER, spirv_blobs


class MaterialTests(unittest.TestCase):
    def test_length_bounded_modules_ignore_surrounding_records(self):
        blob = struct.pack('<7I', 0x07230203, 0x10000, 0, 10, 0, 0x20011, 1)
        record = struct.pack('<I', len(blob)) + blob
        self.assertEqual(list(spirv_blobs(HEADER + b'Vulkan' + record + b'next' + record)), [blob, blob])

    def test_unknown_container_is_not_scanned(self):
        self.assertEqual(list(spirv_blobs(b'unknown\x03\x02\x23\x07')), [])

    def test_truncated_and_invalid_instruction_records_fail(self):
        blob = struct.pack('<6I', 0x07230203, 0x10000, 0, 10, 0, 0)
        for size in (len(blob), len(blob) + 4):
            with self.assertRaises(ValueError):
                list(spirv_blobs(HEADER + struct.pack('<I', size) + blob))
