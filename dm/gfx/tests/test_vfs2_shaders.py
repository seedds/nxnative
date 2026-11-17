"""A VFS2 shader stream must expand to a complete GLSLC record."""
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
with patch.dict(os.environ, {'DM_GAME': '010020D01AD24000'}):
    import shader_split as sh


class Vfs2Shaders(unittest.TestCase):
    def test_reads_only_the_stream_with_the_correct_length(self):
        code = bytearray(0x100)
        code[:4] = sh.GFX_MAGIC
        struct.pack_into('<I', code, 0x30, 0x461)
        control = bytearray(0x838)
        control[:4] = sh.CTL_MAGIC
        struct.pack_into('<I', control, 0x20, len(control))
        struct.pack_into('<I', control, 0x704, len(code))
        header = bytearray(0x48)
        header[:4] = b'\x91\x68\x86\x19'
        size = len(header) + len(control) + len(code)
        struct.pack_into('<I', header, 0x44, size)
        stream = zlib.compress(header + control + code)
        archive = b'VFS2\x07\0\0\0' + struct.pack('<I', size) + stream
        archive += struct.pack('<I', size + 1) + stream
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'data.vfs'
            path.write_bytes(archive)
            self.assertEqual(sh.vfs2_shaders(path, 'data.vfs'), [
                ('data.vfs#VFS2@0xc#GLSLC@0x0', 'vs', control, code),
            ])
