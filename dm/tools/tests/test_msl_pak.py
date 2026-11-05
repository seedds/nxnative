#!/usr/bin/env python3
import shutil, sys, tempfile, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import msl_pak


class TestMslPak(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='msl_pak_test_'))
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_pack_and_unpack_roundtrip(self):
        src_dir = self.tmp / 'sources'
        src_dir.mkdir()
        files = {
            'inputs.tsv': b'stamp\t010086A022444000\n',
            '0001_b.metallib': b'MTLLIB_HEADER_TEST_123',
            '0001_b.json': b'{"stage": "vertex", "bindings": [0, 1]}',
            '0002_bp.metallib': b'MTLLIB_PULLING_HEADER_456789',
            'z_last.metallib': b'LAST_FILE_DATA',
        }
        for name, content in files.items():
            (src_dir / name).write_bytes(content)

        pak_path = self.tmp / 'test.pak'
        count = msl_pak.pack(src_dir.glob('*'), pak_path)
        self.assertEqual(count, len(files))
        self.assertTrue(pak_path.is_file())

        entries = msl_pak.list_entries(pak_path)
        # Verify entries are sorted alphabetically by filename
        entry_names = [e[0] for e in entries]
        self.assertEqual(entry_names, sorted(files.keys()))

        out_dir = self.tmp / 'unpacked'
        msl_pak.unpack(pak_path, out_dir)

        for name, content in files.items():
            self.assertTrue((out_dir / name).is_file())
            self.assertEqual((out_dir / name).read_bytes(), content)


if __name__ == '__main__':
    unittest.main()
