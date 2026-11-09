"""Tests for vertex_ab dual-part vertex shader merging (dmsh_build.py vertex_ab).

Verifies merging of VertexA and VertexB Maxwell vertex shader programs, testing both the
original Catherine: Full Body layout (where c1 immediately follows Part B) and the
Farming Simulator 26 layout (where c1 is placed between Part A and Part B).
"""
import ast
from pathlib import Path
import struct
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))

tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
func = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'vertex_ab'][0]
scope = {'struct': struct, 'sys': sys}
exec(compile(ast.Module(body=[func], type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
vertex_ab = scope['vertex_ab']


class TestVertexAB(unittest.TestCase):
    def _make_sph(self, stage_type):
        sph = [0] * 20
        # word 0: low 5 bits must be 1, bits 10..13 are stage type (0 for VertexA, 1 for VertexB)
        sph[0] = (stage_type << 10) | 1
        return struct.pack('<20I', *sph)

    def test_non_vertex_ab_ignored(self):
        # Short buffers
        self.assertIsNone(vertex_ab('short', b'', b''))
        # Bad code magic
        self.assertIsNone(vertex_ab('bad_magic', bytes(0x100), bytes(0x800)))
        # Type 1 shader (standard vertex shader, not dual-part)
        code = bytearray(0x200)
        code[:4] = bytes.fromhex('78563412')
        struct.pack_into('<I', code, 0x30, (1 << 10) | 1)  # stage type 1
        ctl = bytearray(0x800)
        self.assertIsNone(vertex_ab('type1', bytes(code), bytes(ctl)))
        # Type 0 shader but with zeroed offsets at ctl+0x748 (Ryujinx translated as-is)
        struct.pack_into('<I', code, 0x30, (0 << 10) | 1)  # stage type 0
        self.assertIsNone(vertex_ab('no_parts', bytes(code), bytes(ctl)))

    def test_catherine_layout(self):
        # In Catherine: Full Body, c1o is at 0x700, and b_sph is at 0x730 (c1o + 0x30 == b_sph).
        # Part A: SPH at 0x30, len 0x70 (0x50 SPH + 0x20 insts)
        # Part B: SPH at 0x730, len 0x70 (0x50 SPH + 0x20 insts)
        # Tail: c1 is at code[b_sph + b_len:]
        a_sph, a_len = 0x30, 0x70
        b_sph, b_len = 0x730, 0x70
        c1o = 0x700

        total_code_len = b_sph + b_len + 0x80
        code = bytearray(total_code_len)
        code[:4] = bytes.fromhex('78563412')
        code[a_sph:a_sph + 0x50] = self._make_sph(0)
        code[b_sph:b_sph + 0x50] = self._make_sph(1)

        # Place an EXIT instruction in Part A (at offset 8 within instructions, i.e. a_sph + 0x50 + 8)
        # ins >> 52 == 0xe30 and ins & 0x1f == 0xf
        exit_ins = (0xe30 << 52) | 0xf
        struct.pack_into('<Q', code, a_sph + 0x50 + 8, exit_ins)

        # Constant buffer c1 payload at tail
        c1_payload = b'CATHERINE_C1_TAIL_DATA'
        code[b_sph + b_len:b_sph + b_len + len(c1_payload)] = c1_payload

        ctl = bytearray(0x800)
        struct.pack_into('<I', ctl, 0x700, c1o)
        struct.pack_into('<6I', ctl, 0x748, a_sph, a_len, 0, b_sph, b_len, 0)

        res = vertex_ab('catherine', bytes(code), bytes(ctl))
        self.assertIsNotNone(res)
        merged_code, c1 = res
        self.assertEqual(c1[:len(c1_payload)], c1_payload)
        # Check that Part A's EXIT got converted to BRA (0xe24 opcode)
        new_ins = struct.unpack_from('<Q', merged_code, 0x30 + 0x50 + 8)[0]
        self.assertEqual(new_ins >> 52, 0xe24)

    def test_farming_simulator_layout(self):
        # In Farming Simulator 26, c1o sits between Part A and Part B:
        # Part A: SPH at 0x30, len 0x70 (0x50 SPH + 0x20 insts)
        # c1o = 0x400
        # Part B: SPH at 0x530 (b_sph - 0x30 = 0x500), len 0x70
        # Here c1o + 0x30 (0x430) < b_sph (0x530)
        # c1 is sliced from code[c1o : b_sph - 0x30] (0x400..0x500)
        a_sph, a_len = 0x30, 0x70
        c1o = 0x400
        b_sph, b_len = 0x530, 0x70

        total_code_len = b_sph + b_len + 0x80
        code = bytearray(total_code_len)
        code[:4] = bytes.fromhex('78563412')
        code[a_sph:a_sph + 0x50] = self._make_sph(0)
        code[b_sph:b_sph + 0x50] = self._make_sph(1)

        # Place EXIT in Part A
        exit_ins = (0xe30 << 52) | 0xf
        struct.pack_into('<Q', code, a_sph + 0x50 + 8, exit_ins)

        # c1 payload at c1o
        c1_payload = b'FS26_INTERMEDIATE_C1_DATA'.ljust(b_sph - 0x30 - c1o, b'\x00')
        code[c1o:c1o + len(c1_payload)] = c1_payload

        ctl = bytearray(0x800)
        struct.pack_into('<I', ctl, 0x700, c1o)
        struct.pack_into('<6I', ctl, 0x748, a_sph, a_len, 0, b_sph, b_len, 0)

        res = vertex_ab('fs26', bytes(code), bytes(ctl))
        self.assertIsNotNone(res)
        merged_code, c1 = res
        self.assertEqual(c1, c1_payload)
        new_ins = struct.unpack_from('<Q', merged_code, 0x30 + 0x50 + 8)[0]
        self.assertEqual(new_ins >> 52, 0xe24)

    def test_malformed_b_sph_exits(self):
        # b_sph overlaps or is before Part A
        a_sph, a_len = 0x30, 0x70
        b_sph, b_len = 0x50, 0x70  # overlaps with a
        code = bytearray(0x200)
        code[:4] = bytes.fromhex('78563412')
        code[a_sph:a_sph + 0x50] = self._make_sph(0)
        code[b_sph:b_sph + 0x50] = self._make_sph(1)
        ctl = bytearray(0x800)
        struct.pack_into('<6I', ctl, 0x748, a_sph, a_len, 0, b_sph, b_len, 0)
        with self.assertRaises(SystemExit) as cm:
            vertex_ab('bad_order', bytes(code), bytes(ctl))
        self.assertIn('[RED] bad_order: vertex shader of shader type 0', str(cm.exception))


if __name__ == '__main__':
    unittest.main()
