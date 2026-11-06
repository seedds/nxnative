"""Shader scans decode complete Zstandard streams from files and bounded RomFS entries without a bidirectional subprocess pipe."""
import os
from pathlib import Path
import random
import sys
import tempfile
import unittest
from unittest.mock import patch

os.environ.setdefault('DM_GAME', '0400486024DEC000')
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'gfx/tools'))
import shader_split as ss
import romfs_image


class ZstdInput(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.zstd = ss.zstd_mod()
        cls.payload = random.Random(12).randbytes(2 * 1024 * 1024) + b'archive-record\0' * 30000

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.file = self.root / 'archive.zs'

    def test_large_file_and_bounded_image_entry_have_identical_bytes(self):
        self.file.write_bytes(self.zstd.compress(self.payload))
        image = self.root / 'RomFS.img'
        source = self.root / 'files'; source.mkdir()
        (source / 'archive.zs').write_bytes(self.file.read_bytes())
        (source / 'following').write_bytes(b'outside the compressed entry')
        romfs_image.pack(source, image)
        with patch.object(ss.subprocess, 'run', side_effect=AssertionError('decompression must not spawn a pipe-based child')):
            self.assertEqual(ss.unzs(self.file), self.payload)
            self.assertEqual(ss.unzs(str(image / 'archive.zs')), self.payload)

    def test_concatenated_frames_are_all_decoded(self):
        self.file.write_bytes(self.zstd.compress(b'first') + self.zstd.compress(b'') + self.zstd.compress(b'second'))
        self.assertEqual(ss.unzs(self.file), b'firstsecond')

    def test_dictionary_frame_uses_its_named_dictionary(self):
        samples = [b'archive resource texture shader ' * 20 + str(i).encode() for i in range(100)]
        dictionary = self.zstd.train_dict(samples, 1024)
        payload = b'archive resource texture shader ' * 10
        self.file.write_bytes(self.zstd.compress(payload, zstd_dict=dictionary))
        did = ss.zs_dict_id(self.file.read_bytes())
        self.assertNotEqual(did, 0)
        with patch.object(ss, 'ZDICTS', {did: dictionary.dict_content}), patch.object(ss, '_ZD', {}):
            self.assertEqual(ss.unzs(self.file), payload)

    def test_truncation_and_checksum_corruption_are_rejected(self):
        compressed = self.zstd.compress(self.payload, options={self.zstd.CompressionParameter.checksum_flag: 1})
        for data in (compressed[:-1], compressed[:-1] + bytes([compressed[-1] ^ 1])):
            with self.subTest(length=len(data)):
                self.file.write_bytes(data)
                with self.assertRaises(SystemExit) as stopped: ss.unzs(self.file)
                self.assertIn('invalid zstd stream', str(stopped.exception))
                self.assertIn(str(self.file), str(stopped.exception))


if __name__ == '__main__':
    unittest.main()
