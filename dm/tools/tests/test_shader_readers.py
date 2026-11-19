"""Regression tests for shader reader formats using generated inputs."""
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'dm/gfx/tools'))
previous = os.environ.get('DM_GAME')
os.environ['DM_GAME'] = '010029B0118E8000'
try:
    import shader_split as ss
    import vk_shaders as vk
finally:
    if previous is None:
        del os.environ['DM_GAME']
    else:
        os.environ['DM_GAME'] = previous


def effect():
    records = []
    for vertex, shader_type in ((1, 1), (0, 5)):
        control = bytearray(0x880)
        control[:4] = ss.CTL_MAGIC
        code = bytearray(0x40)
        code[:4] = ss.GFX_MAGIC
        struct.pack_into('<I', code, 0x30, shader_type << 10)
        struct.pack_into('<I', control, 0x704, len(code))
        records.append(struct.pack('<BII', vertex, len(control) + len(code) + 8, len(control))
                       + control + struct.pack('<I', len(code)) + code)
    return b'MGFX\x0a\x28' + bytes(4) + b''.join(records) + b'MGFX'


def vertex_spirv(entry):
    """A generated SPIR-V vertex shader with an empty body."""
    name = entry.encode() + b'\0'
    name += bytes(-len(name) % 4)
    name_words = struct.unpack(f'<{len(name) // 4}I', name)
    instructions = [
        (17, 1), (14, 0, 1), (15, 0, 3, *name_words),
        (19, 1), (33, 2, 1), (54, 1, 3, 0, 2), (248, 4), (253,), (56,),
    ]
    words = [0x07230203, 0x10000, 0, 5, 0]
    for opcode, *operands in instructions:
        words.extend(((len(operands) + 1) << 16 | opcode, *operands))
    return struct.pack(f'<{len(words)}I', *words)


class ShaderReaders(unittest.TestCase):
    def test_embedded_mgfx10(self):
        self.assertEqual([row[1] for row in ss.mgfx_shaders(b'prefix' + effect(), 'main')], ['vs', 'ps'])
        self.assertEqual(ss.mgfx_shaders(effect()[:-4], 'main'), [])

    def test_deflated_xnb_effect(self):
        compressor = zlib.compressobj(wbits=-zlib.MAX_WBITS)
        xnb = b'XNBS\x05\x01' + bytes(4) + b'EffectReader\0' + effect()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'effect.zxnb'
            path.write_bytes(compressor.compress(xnb) + compressor.flush())
            _, shaders, _, extra = ss.scan_file((str(path), path.name))
            self.assertEqual({row[1] for row in shaders.values()}, {'vs', 'ps'})
            self.assertEqual(extra, {})

    @unittest.skipUnless(os.access(vk.SPIRV_CROSS, os.X_OK), 'spirv-cross is not built')
    def test_spirv_entry_point_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            for entry in ('main', 'VertexEntry'):
                with self.subTest(entry=entry):
                    path = Path(tmp) / (entry + '.spv')
                    path.write_bytes(vertex_spirv(entry))
                    key, stage, _, msls, _ = vk.one(str(path), tmp, tmp)
                    self.assertEqual(stage, 'vert')
                    self.assertIn(f'vk_{key}(', Path(msls[0]).read_text())


if __name__ == '__main__':
    unittest.main()
