"""Exercise actual AArch64 thunk shapes without any game files."""
import ast
from pathlib import Path
import re
import struct
import sys
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from nso_asm import STP, STUR, STRX

tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
scope = {'re': re}
exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef)
    and n.name in ('tail_call', 'tail_call_at_end')], type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), scope)

# Standard frame, null guard to error block, pointer load, argument move,
# frame restore, tail jump, error call, trap padding.
THUNK = [0xa9bf7bfd, 0x910003fd, 0xb40000a1, 0xf9400812,
         0xaa0103e0, 0xa8c17bfd, 0xd61f0240, 0x94000400, 0]


def decode(words):
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    return list(md.disasm(struct.pack('<' + 'I' * len(words), *words), 0))


class TailCall(unittest.TestCase):
    def check(self, words, j=6):
        ins = decode(words)
        self.assertEqual(len(ins), len(words))
        return scope['tail_call'](ins, j)

    def test_framed_thunk_with_error_block(self):
        self.assertTrue(self.check(THUNK))
        self.assertTrue(self.check(THUNK[:-1]))

    def test_legacy_terminal_patterns_are_unchanged(self):
        for words in ([0xf9400812, 0xd61f0240],
                      [0xf9400812, 0xb4000081, 0xaa0103e0, 0xd61f0240],
                      [0xaa0103f2, 0xd61f0240]):
            ins = decode(words)
            self.assertEqual(self.check(words, len(words) - 1), scope['tail_call_at_end'](ins, len(ins) - 1))
        self.assertTrue(self.check([0xf9400812, 0xb4000081, 0xaa0103e0, 0xd61f0240], 3))

    def test_clobbers_calls_branches_and_traps_rejected(self):
        for word in (0x52800012, 0xaa0003f2, 0x94000400, 0xd63f0000,
                     0x14000001, 0xb4000020, 0xd65f03c0, 0, 0x910043ff):
            with self.subTest(word=hex(word)):
                words = THUNK.copy(); words[4] = word
                self.assertFalse(self.check(words))

    def test_required_frame_and_guard(self):
        for index, word in ((0, 0xd503201f), (1, 0xd503201f), (2, 0xb4000021),
                            (2, 0xb50000a1), (3, 0xaa0003f2), (5, 0xd503201f),
                            (7, 0xd503201f), (7, 0x97fffffd), (8, 0xaa0003e0)):
            with self.subTest(index=index, word=hex(word)):
                words = THUNK.copy(); words[index] = word
                self.assertFalse(self.check(words))

    def test_decoding_gap_is_rejected(self):
        ins = decode(THUNK)
        del ins[4]
        self.assertFalse(scope['tail_call'](ins, 5))

    def test_unframed_mid_function_pointer_jump_rejected(self):
        self.assertFalse(self.check([0xf9400812, 0xaa0103e0, 0xd61f0240, 0x94000400], 2))
        # The broader rule proposed in #188 accepts an unframed guarded block.
        # Keep the framed fallback's boundary even when a null guard is present.
        self.assertFalse(self.check([0xb4000061, 0xf9400812, 0xd61f0240, 0x94000400], 2))

    def test_stores_must_not_overwrite_saved_frame(self):
        for base in (31, 29):   # sp and the unchanged frame pointer
            for word in (STRX(0, base), STRX(0, base, 1), STUR(0, base, 15),
                         STP(0, 1, base, -8), STP(0, 1, base, 8)):
                with self.subTest(base=base, word=hex(word)):
                    words = THUNK.copy(); words[4] = word
                    self.assertFalse(self.check(words))

    def test_stack_and_frame_register_aliases_rejected(self):
        for word in (0x1100001f, 0x9100001f,   # mov wsp, w0; mov sp, x0
                     0x2a0003fd, 0xaa0003fd,   # mov w29, w0; mov x29, x0
                     0x2a0003fe, 0xaa0003fe):  # mov w30, w0; mov x30, x0
            with self.subTest(word=hex(word)):
                words = THUNK.copy(); words[4] = word
                self.assertFalse(self.check(words))

    def test_caller_stack_argument_stores_are_accepted(self):
        for base in (31, 29):
            for word in (STRX(0, base, 2), STUR(0, base, 16), STP(0, 1, base, 16)):
                with self.subTest(base=base, word=hex(word)):
                    words = THUNK.copy(); words[4] = word
                    self.assertTrue(self.check(words))

    def test_unproven_store_addresses_are_rejected(self):
        for word in (STRX(0, 9, 2), STUR(0, 9, 16), STP(0, 1, 9, 16),
                     0xf8206be0):   # str x0, [sp, x0]: dynamic offset may hit the frame
            with self.subTest(word=hex(word)):
                words = THUNK.copy(); words[4] = word
                self.assertFalse(self.check(words))

    def test_multi_guard_framed_thunk_with_error_blocks(self):
        # 16-byte frame with multiple forward guards to error suffix
        words = [
            0xa9bf7bfd,  # 0x00: stp x29, x30, [sp, #-0x10]!
            0x910003fd,  # 0x04: mov x29, sp
            0xb40000c0,  # 0x08: cbz x0, #0x20
            0xf9400012,  # 0x0c: ldr x18, [x0]
            0xb4000092,  # 0x10: cbz x18, #0x20
            0xf9400652,  # 0x14: ldr x18, [x18, #8]
            0xa8c17bfd,  # 0x18: ldp x29, x30, [sp], #0x10
            0xd61f0240,  # 0x1c: br x18
            0x940003f8,  # 0x20: bl #0x1000
            0x00000000,  # 0x24: udf #0
        ]
        self.assertTrue(self.check(words, 7))
        # Branch inside body targeting internal body must be rejected
        bad_words = words.copy()
        bad_words[2] = 0x14000002  # b #0x10
        self.assertFalse(self.check(bad_words, 7))
        # Branch in suffix jumping back into body must be rejected
        bad_suffix = words.copy()
        bad_suffix[8] = 0x17fffffc  # b #0x0
        self.assertFalse(self.check(bad_suffix, 7))

    def test_n_byte_framed_thunk_with_error_blocks(self):
        # N-byte frame (callee saves and stack args)
        words = [
            0xd10083ff,  # 0x00: sub sp, sp, #0x20
            0xa9017bfd,  # 0x04: stp x29, x30, [sp, #0x10]
            0x910043fd,  # 0x08: add x29, sp, #0x10
            0xb40000c0,  # 0x0c: cbz x0, #0x24
            0xf9400012,  # 0x10: ldr x18, [x0]
            0xaa0103e0,  # 0x14: mov x0, x1
            0xa9417bfd,  # 0x18: ldp x29, x30, [sp, #0x10]
            0x910083ff,  # 0x1c: add sp, sp, #0x20
            0xd61f0240,  # 0x20: br x18
            0x940003f7,  # 0x24: bl #0x1000
            0x00000000,  # 0x28: udf #0
        ]
        self.assertTrue(self.check(words, 8))
        # Stack size mismatch must be rejected
        mismatch_stack = words.copy()
        mismatch_stack[7] = 0x9100c3ff  # add sp, sp, #0x30
        self.assertFalse(self.check(mismatch_stack, 8))
        # Register x18 modified by arithmetic rather than load must be rejected
        arithmetic_x18 = words.copy()
        arithmetic_x18[4] = 0x8b000252  # add x18, x18, x0
        self.assertFalse(self.check(arithmetic_x18, 8))
        # Clobbering stack pointer in body must be rejected
        alias_sp = words.copy()
        alias_sp[5] = 0x9100001f  # mov sp, x0
        self.assertFalse(self.check(alias_sp, 8))


if __name__ == '__main__':
    unittest.main()
