"""Headerless KTGL chunked zlib must expose whole GLSLC shaders, even across chunks."""
import contextlib
import io
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from dm.tools.tests.test_unity_text_glslc import glslc_output, ss


def chunk(data):
    compressed = zlib.compress(data)
    return struct.pack('<I', len(compressed)) + compressed


class ElixirShaders(unittest.TestCase):
    def scan(self, data, name='pe_pack.elixir.gz'):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / name
            path.write_bytes(data)
            return ss.scan_file((str(path), name))

    def test_shader_spanning_chunks_is_found_and_deduplicated(self):
        shader = glslc_output()
        payload = b'CREA' + bytes(0x3f00 - 4) + shader + shader
        packed = b''.join(chunk(payload[i:i + 0x4000]) for i in range(0, len(payload), 0x4000))
        _, found, pairs, extra = self.scan(packed)
        self.assertEqual(len(found), 1)
        _, stage, control, code = next(iter(found.values()))
        self.assertEqual(stage, 'vs')
        self.assertEqual(control, shader[0x100:0x950])
        self.assertEqual(code, shader[0x950:])
        self.assertEqual(pairs, set())
        self.assertEqual(extra, {})

    def test_uncompressed_elixir_still_uses_generic_shader_scan(self):
        _, found, _, extra = self.scan(b'CREA' + glslc_output())
        self.assertEqual(len(found), 1)
        self.assertEqual(extra, {})

    def test_other_gz_files_are_not_treated_as_elixir(self):
        _, found, _, extra = self.scan(chunk(b'CREA' + glslc_output()), 'unrelated.gz')
        self.assertEqual(found, {})
        self.assertEqual(extra, {})

    def test_damaged_or_oversized_chunks_do_not_publish_partial_shaders(self):
        first = chunk(b'CREA' + glslc_output())
        stream = zlib.compress(b'data')
        cases = {
            'length past file': first + struct.pack('<I', len(stream) + 1) + stream,
            'partial length': first + b'\x10\x00',
            'zero length': first + bytes(4),
            'bad stream': first + struct.pack('<I', 4) + b'bad!',
            'truncated stream': first + struct.pack('<I', len(stream) - 1) + stream[:-1],
            'trailing stream data': first + struct.pack('<I', len(stream) + 1) + stream + b'x',
            'too much inflated data': first + chunk(bytes(0x4001)),
        }
        for reason, packed in cases.items():
            with self.subTest(reason=reason), contextlib.redirect_stdout(io.StringIO()) as log:
                _, found, _, extra = self.scan(packed)
                self.assertEqual(found, {})
                self.assertIsInstance(extra, tuple)
                self.assertEqual(extra[0], 'unreadable')
                self.assertIn('[RED]', log.getvalue())


if __name__ == '__main__':
    unittest.main()
