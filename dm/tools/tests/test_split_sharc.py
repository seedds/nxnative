"""Read SHARC v11 and reject truncated spans and raw LZMA shader outputs."""
import lzma
from pathlib import Path
import struct
import tempfile
import unittest

from dm.tools.tests.test_split_glslc_section import output, ss


FILTERS = [{'id': lzma.FILTER_LZMA1, 'dict_size': 1 << 20, 'lc': 3, 'lp': 0, 'pb': 2}]
DATA_OFFSET = 0x2c + 24 + 32 + 20
METADATA = b'meta' + bytes(5)
SIDE_DATA = bytes(5 + 16)
NAME = b'\x00\x04test'


def compress(data):
    return b'\x5d' + struct.pack('<I', 1 << 20) + lzma.compress(data, format=lzma.FORMAT_RAW, filters=FILTERS)


def archive(member):
    header = struct.pack('<4s10I', b'RAHS', 11, 0, 0, 0x2c, 1, 1, 1, 0, 0, 0)
    metadata = struct.pack('<IH', 4, 1) + bytes(18)
    shader = struct.pack('<8I', len(member), 1, 1, 0, 0, 0, 0, 1)
    return header + metadata + shader + bytes(20) + METADATA + member + SIDE_DATA + NAME


class SharcTest(unittest.TestCase):
    def test_complete_glslc_without_lzma_end_marker(self):
        shader = output(0x880, 0x880)
        member = compress(shader + bytes(2))[:-1]
        decoder = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=FILTERS)
        self.assertTrue(decoder.decompress(member[5:]).startswith(shader))
        self.assertFalse(decoder.eof)

        with tempfile.TemporaryDirectory() as t:
            path = Path(t) / 'shader_switch.sharc'
            path.write_bytes(archive(member))
            _, keys, _, extra = ss.scan_file((str(path), path.name))
        self.assertEqual(extra, {})
        self.assertEqual(len(keys), 1)
        self.assertEqual(next(iter(keys.values()))[1], 'cs')

    def test_truncated_spans(self):
        member = compress(output(0x880, 0x880))
        d = archive(member)
        member_end = DATA_OFFSET + len(METADATA) + len(member)
        for at, error in (
            (0x2b, 'truncated SHARC header'),
            (DATA_OFFSET - 1, 'SHARC entry tables'),
            (DATA_OFFSET + len(METADATA) - 1, 'SHARC metadata 0'),
            (member_end - 1, 'SHARC compressed member'),
            (member_end + len(SIDE_DATA) - 1, 'SHARC property or side records'),
            (len(d) - len(NAME) + 1, 'SHARC name header'),
            (len(d) - 1, 'SHARC name runs past the file'),
        ):
            with self.subTest(error=error), self.assertRaisesRegex(SystemExit, error):
                ss.sharc_shaders(d[:at], '/synthetic.sharc')

    def test_short_lzma_members(self):
        with self.assertRaisesRegex(SystemExit, 'short SHARC LZMA member'):
            ss.sharc_shaders(archive(bytes(4)), '/synthetic.sharc')

    def test_incomplete_glslc_in_member_of_declared_size(self):
        shader = output(0x880, 0x880)
        for member in (compress(shader)[:-8], compress(shader[:-1])):
            with self.subTest(member_size=len(member)), self.assertRaisesRegex(SystemExit, 'incomplete SHARC GLSLC output'):
                ss.sharc_shaders(archive(member), '/synthetic.sharc')
