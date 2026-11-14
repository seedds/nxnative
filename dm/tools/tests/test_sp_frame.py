"""patch_nso.py's search for stack frames of an odd size (odd_sp_frames), on hand-assembled words, without game files."""
import ast
import re
from pathlib import Path
import struct
from typing import Any
import unittest

import capstone

tree = ast.parse((Path(__file__).resolve().parents[1] / 'patch_nso.py').read_text())   # the program loads and rewrites an image: compile only the search
ns: dict[str, Any] = {'struct': struct, 're': re}
exec(compile(ast.Module([n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ('odd_sp_frames', 'sp_adjust')], []), 'patch_nso.py', 'exec'), ns)
find, sp_adjust = ns['odd_sp_frames'], ns['sp_adjust']
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); md.detail = True

def sub_sp(imm): return 0xD10003FF | imm << 10
def add_sp(imm): return 0x910003FF | imm << 10
def str_sp(rt, off): return 0xF90003E0 | off // 8 << 10 | rt
def ldr_sp(rt, off): return 0xF94003E0 | off // 8 << 10 | rt
NOP, SVC_29, MOV_X1_X9, RET, B_8, BL = 0xD503201F, 0xD4000521, 0xAA0903E1, 0xD65F03C0, 0x14000002, 0x94000040
STP_PRE = 0xA9BF7BFD   # stp x29, x30, [sp, #-0x10]!
MOV_X0_SP = 0x910003E0   # mov x0, sp
def img(*w): return b''.join(struct.pack('<I', x) for x in w)
def frame(imm, *body): return img(NOP, sub_sp(imm), *body, add_sp(imm), RET)
def found(code): return find(code, [(0, len(code))], md)
def asm(word): i = next(md.disasm(struct.pack('<I', word), 0)); return f'{i.mnemonic} {i.op_str}'

def civ7_block():
    """Civilization VII main+0x4e8a14: sub sp,sp,#0x98; str x1..x17 at [sp, #0..#0x80]; mov x1,x9; svc #0x29; str x1,[sp,#0x88]; the loads back; add sp,sp,#0x98"""
    saves = [str_sp(r, 8 * (r - 1)) for r in range(1, 18)]
    loads = [ldr_sp(r, 8 * (r - 1)) for r in range(1, 18)]
    return frame(0x98, *saves, MOV_X1_X9, SVC_29, str_sp(1, 0x88), *loads, ldr_sp(8, 0x88))


class OddSpFrames(unittest.TestCase):
    def test_civ7_getinfo_block_found(self):
        code = civ7_block()
        self.assertEqual(found(code), [(4, len(code) - 8, 0x98)])

    def test_aligned_frames_left_alone(self):   # compiled code: every frame a multiple of 16
        self.assertEqual(found(frame(0x90, str_sp(1, 0), ldr_sp(1, 0))), [])

    def test_calls_inside_the_frame_allowed(self):
        self.assertEqual(found(frame(0x18, str_sp(1, 0), BL, ldr_sp(1, 0))), [(4, 20, 0x18)])

    def test_access_above_the_frame_rejected(self):   # an incoming stack argument would move when the frame grows
        self.assertEqual(found(frame(0x98, ldr_sp(0, 0x98))), [])
        self.assertEqual(found(frame(0x98, ldr_sp(0, 0xa0))), [])

    def test_last_slot_of_the_frame_is_inside(self):   # 8 bytes at 0x90 end at 0x98
        self.assertEqual(found(frame(0x98, ldr_sp(0, 0x90))), [(4, 12, 0x98)])

    def test_sp_writes_branches_and_escapes_rejected(self):
        for name, word in [('writeback', STP_PRE), ('branch', B_8), ('ret', RET), ('sp copied', MOV_X0_SP)]:
            with self.subTest(name):
                self.assertEqual(found(frame(0x98, str_sp(1, 0), word, ldr_sp(1, 0))), [])

    def test_largest_frames_round_to_the_shifted_form(self):   # 0xff8 rounds to 0x1000, past the 12-bit field: #1, lsl #12, not #0, lsl #12
        self.assertEqual(found(frame(0xff8, str_sp(1, 0xff0))), [(4, 12, 0xff8)])
        for op, name in [(0xD10003FF, 'sub'), (0x910003FF, 'add')]:
            with self.subTest(name):
                self.assertEqual(asm(sp_adjust(op, 0xa0)), f'{name} sp, sp, #0xa0')
                self.assertEqual(asm(sp_adjust(op, 0x1000)), f'{name} sp, sp, #1, lsl #12')

    def test_unmatched_sub_rejected(self):
        self.assertEqual(found(img(NOP, sub_sp(0x98), str_sp(1, 0), add_sp(0xa0), RET)), [])
        self.assertEqual(found(img(NOP, sub_sp(0x98), *[NOP] * 80, add_sp(0x98), RET)), [])   # no add within the window


if __name__ == '__main__': unittest.main()
