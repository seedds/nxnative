"""Carried signed-offset jump targets, using synthetic AArch64 instructions only."""
import ast
from pathlib import Path
import re
import struct
import sys
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from nso_asm import ADRP, ADDI, ADDR, BR, B, NOP

tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
scope = {'re': re, 'struct': struct, 'tabjumps': {}, 'jumps': {}}
exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef)
                             and n.name in ('num', 'copied_base', 'jump_carried_offsets')],
                        type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), scope)


class CarriedOffsets(unittest.TestCase):
    def recognize(self, changes=None, target_changes=None):
        # An unsigned bound, a signed table load, a copied base, and a target
        # computed before 24 instructions of unrelated initialization.
        table = 0x4040
        words = [ADRP(10, 0, table), ADDI(10, 10, table & 0xfff),
                 0x7100001f | (2 << 10) | (9 << 5), 0x54000008 | (((148 - 12) // 4) << 5),
                 NOP, 0xb8a07800 | (9 << 16) | (10 << 5) | 9,
                 0xaa0003e0 | (10 << 16) | 12, 0x2a0003e0 | 10,
                 ADDR(18, 9, 12)] + [NOP] * 24 + [BR(18)] + [0xd65f03c0] * 4
        for index, word in (changes or {}).items():
            words[index] = word
        img = bytearray(0x5000)
        targets = [136, 140, 144]
        for i, target in (target_changes or {}).items():
            targets[i] = target
        for i, target in enumerate(targets):
            struct.pack_into('<i', img, table + 4 * i, target - table)
        scope['IMG'] = img
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        md.detail = True
        instructions = list(md.disasm(struct.pack('<' + 'I' * len(words), *words), 0))
        self.assertEqual(len(instructions), len(words))
        return scope['jump_carried_offsets'](instructions, 33)

    def test_delayed_target_with_copied_and_clobbered_original_base(self):
        self.assertEqual(self.recognize(), [136, 140, 144])

    def test_target_survives_later_index_and_base_initialization(self):
        self.assertEqual(self.recognize({10: 0x2a0003e9, 11: 0x2a0003ec}), [136, 140, 144])

    def test_target_clobber_is_rejected(self):
        self.assertIsNone(self.recognize({12: 0x2a0003f2}))

    def test_control_flow_before_dispatch_is_rejected(self):
        for word in (0xd63f0000, 0x54000040, 0x14000002, 0xd4000001):
            with self.subTest(word=word):
                self.assertIsNone(self.recognize({12: word}))

    def test_offset_clobber_before_add_is_rejected(self):
        self.assertIsNone(self.recognize({7: 0x2a0003e9}))

    def test_unproven_base_copy_is_rejected(self):
        self.assertIsNone(self.recognize({6: 0xaa0003e0 | (11 << 16) | 12}))

    def test_index_clobber_after_guard_is_rejected(self):
        self.assertIsNone(self.recognize({4: 0x2a0003e9}))

    def test_wrong_guard_is_rejected(self):
        self.assertIsNone(self.recognize({3: 0x54000000 | (((148 - 12) // 4) << 5)}))

    def test_bad_table_target_is_rejected(self):
        for target in (137, 0x1000, -4, 20):
            with self.subTest(target=target):
                self.assertIsNone(self.recognize(target_changes={0: target}))

    def test_incoming_edge_cannot_bypass_the_guard(self):
        self.assertIsNone(self.recognize({37: B(148, 20)}))


if __name__ == '__main__':
    unittest.main()
