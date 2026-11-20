"""Check the two Maxwell VertexA/B constant layouts without game data."""
import ast
from pathlib import Path
import struct
import sys
import unittest


path = Path(__file__).resolve().parents[1] / 'tools/dmsh_build.py'
function = next(n for n in ast.parse(path.read_text()).body if isinstance(n, ast.FunctionDef) and n.name == 'vertex_ab')
scope = dict(struct=struct, sys=sys)
exec(compile(ast.Module(body=[function], type_ignores=[]), str(path), 'exec'), scope)
vertex_ab = scope['vertex_ab']


def paired_shader(c1_offset, trailing):
    code = bytearray(0x190)
    code[:4] = bytes.fromhex('78563412')
    struct.pack_into('<I', code, 0x30, 0x61)
    struct.pack_into('<I', code, 0x120, 0x461)
    struct.pack_into('<Q', code, 0x88, 0xe30 << 52 | 0xf)
    control = bytearray(0x880)
    struct.pack_into('<I', control, 0x700, c1_offset)
    struct.pack_into('<6I', control, 0x748, 0x30, 0x70, 0, 0x120, 0x70, 0)
    return code + trailing, control


class VertexABTests(unittest.TestCase):
    def test_catherine_keeps_c1_after_b(self):
        trailing = bytes(range(0x10))
        code, ctl = paired_shader(0xf0, trailing)
        _, c1 = vertex_ab('synthetic', bytes(code), ctl)
        self.assertEqual(c1, trailing)

    def test_borderlands_reads_c1_between_a_and_b(self):
        code, ctl = paired_shader(0xa0, bytes(0x10))
        constants = struct.pack('<2f', 0.01, 0.001) + bytes(0x78)
        code[0xa0:0x120] = constants
        merged, c1 = vertex_ab('synthetic', bytes(code), ctl)
        self.assertEqual(c1, constants)
        self.assertEqual(len(merged), 0xc0)
        self.assertEqual(struct.unpack_from('<Q', merged, 0x88)[0], 0xe24 << 52 | 0x10 << 20 | 0xf)
        code[-1] = 1
        with self.assertRaisesRegex(SystemExit, 'part B carries constants'):
            vertex_ab('synthetic', bytes(code), ctl)

    def test_rejects_a_part_shorter_than_its_sph(self):
        code, ctl = paired_shader(0xf0, bytes(0x10))
        struct.pack_into('<I', ctl, 0x758, 0x30)
        with self.assertRaisesRegex(SystemExit, 'without a VertexB part'):
            vertex_ab('synthetic', bytes(code), ctl)
