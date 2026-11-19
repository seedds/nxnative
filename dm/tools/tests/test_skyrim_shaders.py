"""The Skyrim Switch shader BSA and its FXP control/code records."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))
from dm.tools.tests.test_split_gzip import fz, ss

ss.sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import skyrim_bsa


def fxp():
    stages = []
    for ctl, code in (fz.VS, fz.PS):
        ctl = bytearray(ctl[:0x848])
        struct.pack_into('<I', ctl, 0x20, len(ctl))
        struct.pack_into('<I', ctl, 0x704, len(code))
        stages.append(bytes(ctl) + code)
    return bytes(0x40) + b''.join(stages)


def bsa(payload, version=105, folder=b'shadersfx\0', filename=b'shaders011.fxp\0'):
    header_size = 36
    folder_record_size = 24
    folder_block = bytes([len(folder)]) + folder
    filename_table_offset = header_size + folder_record_size + len(folder_block) + 16
    data_offset = filename_table_offset + len(filename)
    header = struct.pack('<4s8I', b'BSA\0', version, header_size, 3, 1, 1, len(folder), len(filename), 0)
    folder_record = struct.pack('<QIIQ', 0, 1, 0, filename_table_offset - 12)
    file_record = struct.pack('<QII', 0, len(payload), data_offset)
    return header + folder_record + folder_block + file_record + filename + payload


class SkyrimShaders(unittest.TestCase):
    def test_extract_and_scan_the_fxp_member(self):
        data = fxp()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / 'Skyrim - Shaders.bsa'
            archive.write_bytes(bsa(data))
            extracted = skyrim_bsa.extract_shaders(archive, root / 'skyrim_shaders')
            self.assertEqual(len(extracted), 1)
            path, member = extracted[0]
            self.assertEqual(member, 'shadersfx/shaders011.fxp')
            self.assertEqual(Path(path).read_bytes(), data)
            with mock.patch.object(ss.GAME, 'id', skyrim_bsa.SKYRIM_TITLE_ID):
                rel, keys, pairs, extra = ss.scan_file((path, f'Data/Skyrim - Shaders.bsa/{member}'))
            self.assertEqual(rel, f'Data/Skyrim - Shaders.bsa/{member}')
            self.assertEqual({value[1] for value in keys.values()}, {'vs', 'ps'})
            self.assertEqual(len(keys), 2)
            self.assertEqual(pairs, set())
            self.assertEqual(extra, {})

    def test_fxp_reader_is_not_enabled_for_other_games(self):
        data = fxp()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'skyrim_shaders' / 'shadersfx' / 'shaders011.fxp'
            path.parent.mkdir(parents=True)
            path.write_bytes(data)
            with mock.patch.object(ss.GAME, 'id', '010015100B514000'):
                _, keys, _, extra = ss.scan_file((str(path), 'shadersfx/shaders011.fxp'))
            self.assertEqual(keys, {})
            self.assertEqual(extra, {})

    def test_archive_version_and_filename_are_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / 'Skyrim - Shaders.bsa'
            archive.write_bytes(bsa(fxp(), version=104))
            with self.assertRaisesRegex(ValueError, 'version 105'):
                skyrim_bsa.extract_shaders(archive, root / 'out')
            with self.assertRaisesRegex(ValueError, 'expected skyrim - shaders.bsa'):
                skyrim_bsa.extract_shaders(root / 'other.bsa', root / 'out')

    def test_member_paths_cannot_escape_output_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / 'Skyrim - Shaders.bsa'
            archive.write_bytes(bsa(fxp(), folder=b'../outside\0'))
            with self.assertRaisesRegex(ValueError, 'unsafe BSA member path'):
                skyrim_bsa.extract_shaders(archive, root / 'out')


if __name__ == '__main__':
    unittest.main()
