"""The NXCX archive scan preserves the loose-file reader's shader results."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_unity_text_glslc import glslc_output, ss   # noqa: E402


class NxcxSplit(unittest.TestCase):
    def test_glslc_magic_split_across_chunks(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'textures.mrg'
            with path.open('wb') as f:
                f.write(b'NXCX' + bytes(12))
                f.seek((16 << 20) - 2)
                f.write(glslc_output())
            args = (str(path), path.name)
            actual = ss.scan_file_in(args)
            with patch.object(ss, 'nxcx_shaders', return_value=None):
                expected = ss.scan_file_in(args)
            self.assertEqual(actual, expected)
            self.assertEqual(len(actual[1]), 1)

    def test_defers_when_another_embedded_format_may_be_present(self):
        with tempfile.TemporaryFile() as f:
            f.write(b'NXCX' + bytes(12) + b'BNSH\0\0\0\0')
            f.seek(0)
            self.assertIsNone(ss.nxcx_shaders(f, '/textures.mrg'))
