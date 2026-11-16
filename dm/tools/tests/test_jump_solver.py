"""jump_solver: offset jump tables proved over a function's control flow, on decoded AArch64 and through the rewriter itself. Each form a game brought is here with
that game's instruction sequence (docs/reference/rewriter.md names them), and so are the ways a proof must fail."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import jump_solver
from nso_asm import ADRP, ADDI, ADDR, B, BR, NOP

RET, SVC, TRAP = 0xd65f03c0, 0xd4000001, 0xe7ffdefe   # TRAP: a word Capstone does not decode
EQ, NE, HS, LO, HI, LS, GE = 0, 1, 2, 3, 8, 9, 10


def cmp_w(r, n): return 0x7100001f | n << 10 | r << 5
def cmp_x(r, n): return 0xf100001f | n << 10 | r << 5
def bcond(cond, at, to): return 0x54000000 | (((to - at) >> 2) & 0x7ffff) << 5 | cond
def tbnz(r, bit, at, to): return 0x37000000 | (bit & 31) << 19 | (((to - at) >> 2) & 0x3fff) << 5 | r
def tbz(r, bit, at, to): return tbnz(r, bit, at, to) & ~0x01000000
def cbz(r, at, to): return 0x34000000 | (((to - at) >> 2) & 0x7ffff) << 5 | r
def and_w(d, s, k): return 0x12000000 | (k - 1) << 10 | s << 5 | d       # and wD, wS, #(2^k - 1)
def and_x(d, s, k): return 0x92400000 | (k - 1) << 10 | s << 5 | d       # and xD, xS, #(2^k - 1)
def zext(d, s): return 0x92407c00 | s << 5 | d                            # and xD, xS, #0xffffffff
def tst_w(s, k): return 0x72000000 | (k - 1) << 10 | s << 5 | 31          # tst wS, #(2^k - 1)
def mov_w(d, s): return 0x2a0003e0 | s << 16 | d
def mov_x(d, s): return 0xaa0003e0 | s << 16 | d
def mvn_w(d, s): return 0x2a2003e0 | s << 16 | d
def movz(d, n): return 0x52800000 | n << 5 | d
def sub_w(d, s, n): return 0x51000000 | n << 10 | s << 5 | d
def ldr_w(d): return 0xb9400000 | 2 << 10 | 31 << 5 | d                   # ldr wD, [sp, #8]: a 32-bit value
def ldr_x(d): return 0xf9400000 | 1 << 10 | 31 << 5 | d                   # ldr xD, [sp, #8]: a 64-bit one
def ldr_s(d): return 0xbd400000 | 2 << 10 | 31 << 5 | d                   # ldr sD, [sp, #8]: not a general-purpose register
def ldrsw(e, b, i): return 0xb8a07800 | i << 16 | b << 5 | e              # ldrsw xE, [xB, xI, lsl #2]
def ldrh(e, b, i): return 0x78607800 | i << 16 | b << 5 | e               # ldrh wE, [xB, xI, lsl #1]
def add_lsl2(d, n, m): return 0x8b000800 | m << 16 | n << 5 | d           # add xD, xN, xM, lsl #2
def adr(d, at, to): off = to - at; return 0x10000000 | (off & 3) << 29 | ((off >> 2) & 0x7ffff) << 5 | d
def bl(at, to): return 0x94000000 | (((to - at) >> 2) & 0x3ffffff)
def ldrb_reg(e, b, i): return 0x38606800 | i << 16 | b << 5 | e           # ldrb wE, [xB, xI]
def add_w(d, s, n): return 0x11000000 | n << 10 | s << 5 | d              # add wD, wS, #n
def lsr_x(d, s, n): return 0xd340fc00 | n << 16 | s << 5 | d              # lsr xD, xS, #n
def ubfx_x(d, s, lsb, k): return 0xd3400000 | lsb << 16 | (lsb + k - 1) << 10 | s << 5 | d   # ubfx xD, xS, #lsb, #k
RETAB = 0xd65f0fff


class Solver(unittest.TestCase):
    TABLE = 0x4000

    def setUp(self): self.img = bytearray(0x6000)

    def table(self, targets, at=None, poison=0x1000, fmt='<i', rel=None):
        """Entries (target - rel, rel defaulting to the table itself), then a word that is no entry of this table"""
        at = self.TABLE if at is None else at; rel = at if rel is None else rel; size = struct.calcsize(fmt)
        for i, t in enumerate(targets): struct.pack_into(fmt, self.img, at + size * i, (t - rel) // (4 if fmt == '<H' else 1))
        struct.pack_into('<i', self.img, at + size * len(targets), poison)

    def solve(self, words, entries=(), outside=(), landing=None):
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = [p for i, w in enumerate(words) for p in md.disasm(struct.pack('<I', w), 4 * i)]
        return jump_solver.solve(ins, self.img, lambda t: 0 <= t < 0x4000, entries, outside, landing)

    def site(self, words, entries=(), outside=(), at=None, landing=None):
        found, failed = self.solve(words, entries, outside, landing)
        at = next(4 * i for i, w in enumerate(words) if w == BR(18)) if at is None else at
        return found.get(at)


class Bounds(Solver):
    """Each game's form, then its variations"""
    def base(self, r=4): return [ADRP(r, 0, self.TABLE), ADDI(r, r, self.TABLE & 0xfff)]

    def test_compare_and_b_hi(self):
        # Fire Emblem Engage main+0x12eec94: cmp w18,#N; b.hi; ldrsw x18,[xB,x18,lsl #2]; add x18,x18,xB
        t = [36, 40, 44, 48, 52]; self.table(t)
        w = self.base() + [ldr_w(18), cmp_w(18, 4), bcond(HI, 16, 56), ldrsw(18, 4, 18), ADDR(18, 18, 4), BR(18)] + [RET] * 6
        self.assertEqual(self.site(w), t)

    def test_the_branch_that_acts_on_the_compare_decides(self):
        # issue #977: b.hs leaves one entry fewer than b.hi; a compare a branch does not act on bounds nothing; a nearer compare with its own guard is the bound
        t = [40, 44, 48, 52, 56]; self.table(t)
        def run(guard):
            w = self.base() + [ldr_w(18)] + guard + [ldrsw(18, 4, 18), ADDR(18, 18, 4), BR(18)]
            return self.site(w + [RET] * (16 - len(w)))
        self.assertEqual(run([cmp_w(18, 4), bcond(HS, 16, 60), NOP]), t[:4])
        self.assertEqual(run([cmp_w(18, 4), bcond(HI, 16, 60), cmp_w(18, 1)]), t)
        self.assertEqual(run([cmp_w(18, 4), bcond(HI, 16, 60), cmp_w(18, 1), bcond(HI, 24, 60)]), t[:2])
        self.assertIsNone(run([cmp_w(18, 4), NOP, NOP]))
        self.assertIsNone(run([cmp_w(18, 4), bcond(GE, 16, 60), NOP]))   # signed: a negative index passes
        self.assertIsNone(run([cmp_w(18, 4), cmp_w(0, 1), bcond(HI, 20, 60)]))   # another compare set the flags

    def test_b_ls_into_the_dispatch(self):
        t = [36, 40, 44, 48, 52]; self.table(t)
        w = self.base() + [ldr_w(18), cmp_w(18, 4), bcond(LS, 16, 24), B(20, 64), ldrsw(18, 4, 18), ADDR(18, 18, 4), BR(18)] + [RET] * 6
        self.assertEqual(self.site(w), t)

    def test_copies_before_and_after_the_check(self):
        # Link's Awakening subsdk1+0xae9a4 / Super Mario Odyssey subsdk0: cmp w21,#7; b.hi; mov w18,w21. DRAGON QUEST XI S main+0x2dd8cd0: mov w0,w18, 41 instructions, cmp w18,#4
        t = [64 + 4 * i for i in range(5)]; self.table(t)
        after = self.base() + [ldr_w(21), cmp_w(21, 4), bcond(HI, 16, 60), mov_w(18, 21), ldrsw(18, 4, 18), ADDR(18, 18, 4), BR(18)] + [RET] * 12
        self.assertEqual(self.site(after), t)
        before = self.base() + [ldr_w(18), mov_w(0, 18)] + [NOP] * 41 + [cmp_w(18, 4), bcond(HI, 4 * 46, 4 * 60), ldrsw(18, 11, 0), ADDR(18, 18, 11), BR(18)]
        before[0:2] = [ADRP(11, 0, self.TABLE), ADDI(11, 11, 0)]
        t2 = [4 * len(before) + 4 * i for i in range(5)]; self.table(t2)
        self.assertEqual(self.site(before + [RET] * 8), t2)

    def test_masked_copy_and_offset_checks(self):
        # Link's Awakening GLSLC subsdk0+0x2f403c: and w16,w18,#0x3f; cmp w16,#0xe; b.hi; ...; and x18,x18,#0x3f. subsdk0+0x1be6e8: sub w0,w18,#0xd1; cmp w0,#0x31
        t = [48 + 4 * i for i in range(15)]; self.table(t)
        w = self.base() + [ldr_w(18), and_w(16, 18, 6), cmp_w(16, 14), bcond(HI, 20, 44), and_x(18, 18, 6), ldrsw(18, 4, 18), ADDR(18, 18, 4), BR(18)] + [RET] * 20
        self.assertEqual(self.site(w), t)
        t = [44 + 4 * i for i in range(50)]; self.table(t)
        w = self.base() + [ldr_w(18), sub_w(0, 18, 0xd1), cmp_w(0, 0x31), bcond(HI, 20, 40), ldrsw(18, 4, 0), ADDR(18, 18, 4), BR(18)] + [RET] * 55
        self.assertEqual(self.site(w), t)

    def test_64_bit_compare_and_32_bit_values(self):
        # OCTOPATH TRAVELER main+0x1676f30: cmp x22,#0x10 bounds the 64-bit index. A 32-bit compare bounds it only where the upper half is known zero
        t = [36 + 4 * i for i in range(5)]; self.table(t)
        def run(load, check):
            return self.site(self.base() + [load, check, bcond(HI, 16, 56), ldrsw(18, 4, 22), ADDR(18, 18, 4), BR(18)] + [RET] * 10)
        self.assertEqual(run(ldr_x(22), cmp_x(22, 4)), t)
        self.assertEqual(run(ldr_w(22), cmp_w(22, 4)), t)
        self.assertIsNone(run(ldr_x(22), cmp_w(22, 4)))   # ldr x22: the upper half is unknown, the index is not bounded

    def test_shifted_down_then_a_32_bit_compare(self):
        # ASTRAL CHAIN main+0x6db104: lsr x17,x11,#32; cmp w17,#5; b.hi; ldrsw x17,[x13,x17,lsl #2]: the shift leaves the upper half zero
        t = [40 + 4 * i for i in range(5)]; self.table(t)
        def run(shift): return self.site(self.base() + [ldr_x(11), shift, cmp_w(22, 4), bcond(HI, 20, 60), ldrsw(18, 4, 22), ADDR(18, 18, 4), BR(18)] + [RET] * 10)
        self.assertEqual(run(lsr_x(22, 11, 32)), t)
        self.assertEqual(run(ubfx_x(22, 11, 8, 32)), t)
        self.assertIsNone(run(lsr_x(22, 11, 31)))   # 33 bits: the compare leaves the top one unknown

    def test_widening_keeps_the_parts_that_held(self):
        # ASTRAL CHAIN main+0x6db1a4: w12 - 2 checked against 3 on four paths of a loop; the root's full range grows past the visits allowed, its low 32 bits do not
        r = ('p', 1, 12, 64); old = jump_solver._mk(2, 5, jump_solver.FULL, 2, 5); new = jump_solver._mk(0, jump_solver.M64, jump_solver.FULL, 2, 5)
        self.assertEqual(jump_solver._widen(old, new, r)[3:], (2, 5))
        self.assertEqual(jump_solver._widen(new, jump_solver._mk(0, jump_solver.M64, jump_solver.FULL, 0, 9), r), jump_solver._default(r))

    def test_zero_extended_after_the_check(self):
        # Mario Tennis Aces main+0x35837c: cmp w8,#4; b.hi; and x18,x8,#0xffffffff
        t = [40 + 4 * i for i in range(5)]; self.table(t)
        w = self.base(16) + [ldr_x(8), cmp_w(8, 4), bcond(HI, 16, 60), zext(18, 8), ldrsw(18, 16, 18), ADDR(18, 18, 16), BR(18)] + [RET] * 8
        self.assertEqual(self.site(w), t)

    def test_complemented_index(self):
        # Return of the Obra Dinn main+0x6eea88: tst w17,#7; b.eq; mvn w17,w17; and x18,x17,#7: 1..7 complemented, 7 entries where the mask allows 8
        t = [44 + 4 * i for i in range(7)]; self.table(t)
        w = self.base(16) + [ldr_w(17), tst_w(17, 3), bcond(EQ, 16, 72), mvn_w(17, 17), and_x(18, 17, 3), ldrsw(18, 16, 18), ADDR(18, 18, 16), BR(18)] + [RET] * 10
        self.assertEqual(self.site(w), t)
        w[5] = NOP   # no test: the mask's 8 entries, the eighth not an instruction
        self.assertIsNone(self.site(w))

    def test_mask_whose_highest_value_leaves(self):
        # Tied Together: and w18,w17,#7; cmp w18,#7; b.eq: 7 entries
        t = [40 + 4 * i for i in range(7)]; self.table(t)
        w = self.base(16) + [ldr_w(17), and_w(18, 17, 3), cmp_w(18, 7), bcond(EQ, 20, 68), ldrsw(18, 16, 18), ADDR(18, 18, 16), BR(18)] + [RET] * 10
        self.assertEqual(self.site(w), t)

    def test_bit_test_before_and_after_the_mask(self):
        # ULTRA STREET FIGHTER II main+0x590ee8: tbnz w17,#3; and x18,x17,#0xf. Manual Samuel main+0x6d9d44: and w13,w17,#7; tbnz w17,#2; ldrsw x13,[x14,x13,lsl #2]
        t = [44 + 4 * i for i in range(8)]; self.table(t)
        w = self.base(13) + [ldr_w(17), tbnz(17, 3, 12, 40), and_x(18, 17, 4), ldrsw(18, 13, 18), ADDR(18, 18, 13), BR(18), RET] + [RET] * 10
        self.assertEqual(self.site(w), t)
        t = [40 + 4 * i for i in range(4)]; self.table(t)
        w = self.base(14) + [ldr_w(17), and_w(13, 17, 3), tbnz(17, 2, 16, 40), ldrsw(13, 14, 13), ADDR(18, 13, 14), movz(11, 0), BR(18)] + [RET] * 6
        self.assertEqual(self.site(w), t)
        for guard in (tbnz(16, 2, 16, 40), tbnz(17, 1, 16, 40), tbz(17, 2, 16, 40)):   # another register, another bit, the other way
            with self.subTest(guard=hex(guard)):
                w[4] = guard
                self.assertIsNone(self.site(w))

    def test_check_on_a_mask_made_in_another_block(self):
        # FINAL FANTASY CRYSTAL CHRONICLES Remastered main+0x2d103cc: and x6,x17,#0x7ff; cbz ... in the predecessor; and w18,w17,#0x7ff; cmp w18,#3; b.hi; ldrsw x18,[x27,x6,lsl #2]
        t = [56 + 4 * i for i in range(4)]; self.table(t)
        w = self.base(27) + [ldr_w(17), and_x(6, 17, 11), cbz(17, 16, 28), B(20, 28), RET,
                             and_w(18, 17, 11), cmp_w(18, 3), bcond(HI, 36, 72), ldrsw(18, 27, 6), ADDR(18, 18, 27), BR(18)] + [RET] * 8
        self.assertEqual(self.site(w), t)
        w[3] = and_x(6, 17, 8)   # a narrower mask in the predecessor: S & 0x7ff <= 3 bounds S & 0xff as well
        self.assertEqual(self.site(w), t)
        w[3] = and_x(6, 17, 12)   # a wider one does not: bit 11 is free, 0x804 entries
        self.assertIsNone(self.site(w))

    def test_adr_relative_entries_and_halfword_tables(self):
        # Tears of the Kingdom main+0x393bb0: adr x18,#L; ldrsw x1,[x17,x0,lsl #2]; add x18,x18,x1. Animal Crossing: New Horizons: ldrh; adr; add x18,x18,xE,lsl #2
        t = [40 + 4 * i for i in range(6)]; self.table(t, rel=24)
        w = self.base(17) + [ldr_w(0), cmp_w(0, 5), bcond(HI, 16, 64), NOP, adr(18, 24, 24), ldrsw(1, 17, 0), ADDR(18, 18, 1), BR(18)] + [RET] * 10
        self.assertEqual(self.site(w), t)
        t = [40 + 4 * i for i in range(3)]; self.table(t, fmt='<H', rel=24)
        w[7:9] = [ldrh(1, 17, 0), add_lsl2(18, 18, 1)]; w[3] = cmp_w(0, 2)
        self.assertEqual(self.site(w), t)

    def test_bases_far_back_copied_and_set_later(self):
        # ASTRAL CHAIN main+0x6db4a0: x24 set 441 instructions back; Super Smash Bros. Ultimate main+0x2b74458: adrp x7 + add, mov x30,x7 ... mov x7,x30;
        # Blasphemous main+0xae95f8: the loop's base set after the dispatch in address order, then a branch back
        n = 450
        w = self.base(24) + [mov_x(30, 24)] + [NOP] * n + [mov_x(7, 30), ldr_w(18), cmp_w(18, 2), bcond(HI, 4 * (n + 6), 4 * (n + 14)), ldrsw(18, 7, 18), ADDR(18, 18, 7), BR(18)] + [RET] * 6
        t = [4 * (n + 10) + 4 * i for i in range(3)]; self.table(t)
        self.assertEqual(self.site(w), t)
        w = [B(0, 28), ldr_w(18), cmp_w(18, 2), bcond(HI, 12, 40), ldrsw(18, 17, 18), ADDR(18, 18, 17), BR(18), ADRP(17, 28, self.TABLE), ADDI(17, 17, 0), B(36, 4), RET, RET, RET]
        t = [40, 44, 48]; self.table(t)
        self.assertEqual(self.site(w), t)

    def test_page_zero_numbers_are_no_base(self):
        # Cuphead 1.3.7 main+0x59dc90: x12 is one loop's base and another's constant 1 (mov w12,#1); a path with a number in page zero never reaches a table load
        t = [48, 52, 56]; self.table(t)
        w = [ldr_w(8), cbz(8, 4, 20), ADRP(12, 8, self.TABLE), ADDI(12, 12, 0), B(16, 24), movz(12, 1), ldr_w(18), cmp_w(18, 2), bcond(HI, 32, 60),
             ldrsw(18, 12, 18), ADDR(18, 18, 12), BR(18)] + [RET] * 6
        self.assertEqual(self.site(w), t)
        w[5] = ADRP(12, 20, 0x5000)   # another real base on that path: two tables, no proof
        self.assertIsNone(self.site(w))


class FoundOnTheCorpus(Solver):
    """Forms the comparison with the recognizers on every supported game's modules brought out"""
    def test_index_loaded_from_a_byte_table(self):
        # OCTOPATH TRAVELER subsdk0+0x1a7210 (a movie middleware): ldrb w10,[x9,x10]; cmp w10,#0xa; b.hi; ldrsw x10,[x11,x10,lsl #2]: the loaded byte is the index
        t = [44 + 4 * i for i in range(11)]; self.table(t)
        w = [ADRP(11, 0, self.TABLE), ADDI(11, 11, 0), ldr_x(9), ldr_w(10), ldrb_reg(10, 9, 10), cmp_w(10, 10), bcond(HI, 24, 96), ldrsw(10, 11, 10), ADDR(18, 10, 11), BR(18)] + [RET] * 16
        self.assertEqual(self.site(w), t)

    def test_pointer_authenticated_returns(self):
        # the Switch 2 SDK's sdk module ends functions with retab: a return, not unknown control flow
        t = [36 + 4 * i for i in range(3)]; self.table(t)
        w = [ADRP(4, 0, self.TABLE), ADDI(4, 4, 0), ldr_w(18), cmp_w(18, 2), bcond(HI, 16, 48), ldrsw(18, 4, 18), ADDR(18, 18, 4), BR(18), RETAB, RETAB, RETAB, RETAB, RETAB]
        self.assertEqual(self.site(w), t)

    def test_functions_entered_by_calls_inside_the_range(self):
        # DRAGON QUEST XI S main: 70 MB of text without unwind entries is analysed in pieces of many functions, each entered by a bl from another
        t = [52 + 4 * i for i in range(3)]; self.table(t)
        w = [bl(0, 12), RET, NOP, ADRP(4, 12, self.TABLE), ADDI(4, 4, 0), ldr_w(18), cmp_w(18, 2), bcond(HI, 28, 64), ldrsw(18, 4, 18), ADDR(18, 18, 4), BR(18), RET, RET] + [RET] * 6
        self.assertEqual(self.site(w), t)

    def test_function_starts_in_text_without_unwind_entries(self):
        # DRAGON QUEST XI S main+0x3edcbf0: a virtual function after another's ret and its trap padding, in text without unwind entries: only an indirect call reaches it
        t = [48 + 4 * i for i in range(3)]; self.table(t)
        w = [RET, 0, ADRP(4, 8, self.TABLE), ADDI(4, 4, 0), ldr_w(18), cmp_w(18, 2), bcond(HI, 20, 60), ldrsw(18, 4, 18), ADDR(18, 18, 4), BR(18), RET, RET] + [RET] * 6
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = [p for i, x in enumerate(w) for p in md.disasm(struct.pack('<I', x), 4 * i)]
        self.assertNotIn(36, jump_solver.solve(ins, self.img, lambda a: True)[0])   # a function its FDE describes: dead code
        self.assertEqual(jump_solver.solve(ins, self.img, lambda a: True, unwound=False)[0].get(36), t)

    def test_a_copy_stays_tied_to_its_source_through_a_loop(self):
        # Mario + Rabbids Kingdom Battle subsdk0+0x7340b8: mov w18,w17 at the loop's head, then w17 checked again before the dispatch; w17 counts up around the loop
        t = [48 + 4 * i for i in range(5)]; self.table(t)
        w = [ADRP(9, 0, self.TABLE), ADDI(9, 9, 0), ldr_w(17), mov_w(18, 17), cmp_w(17, 4), bcond(HI, 20, 68), ldrsw(18, 9, 18), ADDR(18, 18, 9), BR(18), RET, RET,
             RET, add_w(17, 17, 1), B(52, 12), RET, RET, RET, RET]
        self.table([48, 52, 56, 60, 64])
        self.assertEqual(self.site(w), [48, 52, 56, 60, 64])


class Refusals(Solver):
    """What a proof must not survive"""
    T = [36 + 4 * i for i in range(5)]

    def words(self):
        self.table(self.T)
        return [ADRP(4, 0, self.TABLE), ADDI(4, 4, 0), ldr_w(18), cmp_w(18, 4), bcond(HI, 16, 56), ldrsw(18, 4, 18), ADDR(18, 18, 4), BR(18)] + [RET] * 10

    def test_writes_between_the_check_and_the_load(self):
        for word in (mov_w(18, 0), ADDI(18, 18, 1), ldr_w(18), bl(20, 52), SVC):
            with self.subTest(word=hex(word)):
                w = self.words(); w.insert(5, word); w[4] = bcond(HI, 16, 60)
                self.assertIsNone(self.site(w))
        for word in (NOP, ldr_s(18), mov_x(9, 0), cmp_x(18, 0)):   # not the index, or only the flags after the branch
            with self.subTest(word=hex(word)):
                w = self.words(); w.insert(5, word); w[4] = bcond(HI, 16, 60)
                self.assertEqual(self.site(w), self.T)

    def test_a_call_keeps_callee_saved_registers(self):
        t = [40 + 4 * i for i in range(5)]; self.table(t)
        w = [ADRP(19, 0, self.TABLE), ADDI(19, 19, 0), ldr_w(20), cmp_w(20, 4), bcond(HI, 16, 60), bl(20, 60), ldrsw(18, 19, 20), ADDR(18, 18, 19), BR(18)] + [RET] * 8
        self.assertEqual(self.site(w), t)
        w[0:2] = [ADRP(4, 0, self.TABLE), ADDI(4, 4, 0)]; w[6:8] = [ldrsw(18, 4, 20), ADDR(18, 18, 4)]   # x4 is caller-saved
        self.assertIsNone(self.site(w))

    def test_entries_past_the_check(self):
        # a landing pad (nso_code.landing_pads) branching into the run: the unwinder enters it from the call it catches, with x18 whatever the callee left
        t = [36 + 4 * i for i in range(5)]; self.table(t)
        w = [ADRP(19, 0, self.TABLE), ADDI(19, 19, 0), bl(8, 80), ldr_w(18), cmp_w(18, 4), bcond(HI, 20, 60), ldrsw(18, 19, 18), ADDR(18, 18, 19), BR(18)] + [RET] * 9 + [B(72, 24)]
        self.assertEqual(self.site(w), t)   # nothing says 72 runs: dead code
        self.assertIsNone(self.site(w, landing={72: [(8, 12)]}))
        self.assertEqual(self.site(w, landing={72: [(100, 104)]}), t)   # it catches no call of this function
        w = self.words()
        for entry in (8, 20, 24):   # a branch from another function: past the base's initialization or past the check
            with self.subTest(entry=entry):
                self.assertIsNone(self.site(w, entries=[entry]))
                self.assertIsNone(self.site(w, outside=[entry]))   # a table in another function
        self.assertEqual(self.site(w, entries=[0]), self.T)

    def test_a_branch_back_from_the_tables_own_targets(self):
        # code at a target returns into the run with its index still checked: provable, where a syntactic check refused any branch past the guard
        self.table(self.T)
        w = [ADRP(4, 0, self.TABLE), ADDI(4, 4, 0), ldr_w(19), cmp_w(19, 4), bcond(HI, 16, 56), ldrsw(18, 4, 19), ADDR(18, 18, 4), BR(18), RET, B(36, 20)] + [RET] * 8
        self.assertEqual(self.site(w), self.T)
        w[9] = B(36, 24)   # past the load: the add then reads a target, not an entry
        self.assertIsNone(self.site(w))
        w = self.words(); w[9] = B(36, 20)   # back to the load, but x18 now holds a target, not the index
        self.assertIsNone(self.site(w))

    def test_base_paths_and_clobbers(self):
        w = self.words(); w[1] = mov_w(4, 0)   # a 32-bit write of the base
        self.assertIsNone(self.site(w))
        w = [ldr_w(8), cbz(8, 4, 16), ADRP(4, 8, self.TABLE), B(12, 20), ADRP(4, 16, 0x5000), ADDI(4, 4, 0), ldr_w(18), cmp_w(18, 4), bcond(HI, 32, 64),
             ldrsw(18, 4, 18), ADDR(18, 18, 4), BR(18)] + [RET] * 8
        self.assertIsNone(self.site(w))   # two bases reach the load

    def test_tables_and_targets_must_be_real(self):
        w = self.words()
        for bad in (20, 38, 0x1000):   # the load again with x18 a target, not an index; misaligned; outside the function
            with self.subTest(bad=bad):
                self.table([bad] + self.T[1:])
                self.assertIsNone(self.site(w))
        self.table([8] + self.T[1:])   # back before the check: fine
        self.assertEqual(self.site(w), [8] + self.T[1:])
        w[0] = ADRP(4, 0, 0x6000)   # past the image
        self.assertIsNone(self.site(w))

    def test_undecoded_words_and_unknown_control_flow(self):
        w = self.words(); w.insert(5, TRAP); w[4] = bcond(HI, 16, 60)
        self.assertIsNone(self.site(w))   # nothing falls through an undecoded word into the load
        w = self.words(); w[2] = 0xd63f0000   # blr x0 replaces the index
        self.assertIsNone(self.site(w))

    def test_loop_state_constants_stay_tied_to_their_register(self):
        # Manual Samuel main+0x6d9d44: mov w17,#4 is one of the table's own targets, then and w13,w17,#7; tbnz w17,#2: 4 never reaches the load
        t = [36, 40, 44, 48]; self.table(t)
        w = [ADRP(14, 0, self.TABLE), ADDI(14, 14, 0), ldr_w(17), and_w(13, 17, 3), tbnz(17, 2, 16, 52), ldrsw(13, 14, 13), ADDR(18, 13, 14), BR(18), RET,
             movz(17, 4), B(40, 12), B(44, 12), movz(17, 1), RET]
        self.table([36, 40, 44, 48])
        self.assertEqual(self.site(w), [36, 40, 44, 48])


class Driver(unittest.TestCase):
    """The rewriter's driver: the solver's tables in the site list, and entries from other functions found in the whole module"""
    TABLE = 0x2040

    def rewrite(self, incoming=None, byte_table=False):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from test_nso_match import image
        words = [ADRP(11, 0x100, self.TABLE), ADDI(11, 11, self.TABLE & 0xfff), ldr_w(15), and_w(13, 15, 3), tbnz(15, 2, 0x10c, 0x128), ldrsw(13, 11, 13), ADDR(18, 13, 11),
                 BR(18), RET, RET, RET, RET, RET]
        functions = {0x100: words}
        if incoming is not None: functions[0x200] = [B(0x200, incoming), RET]
        if byte_table:   # adr x18,#L; ldrb w0,[x17,x16]; add x18,x18,x0,lsl #2; br x18: 256 pads from L, here the first function's start
            functions[0x200] = [adr(18, 0x200, 0x100), 0x38706a20, add_lsl2(18, 18, 0), BR(18), RET]
        raw = image(functions)
        for i, target in enumerate([0x100, 0x120, 0x124, 0x128]): struct.pack_into('<i', raw, self.TABLE + 4 * i, target - self.TABLE)
        struct.pack_into('<i', raw, self.TABLE + 16, 0x1000)
        raw[0x1100:0x111c] = raw[0x20:0x3c]; struct.pack_into('<I', raw, 4, 0x1100)
        struct.pack_into('<ii', raw, 0x1110, 0x3000 - 0x1100, 0x300c + 8 * len(functions) - 0x1100)
        raw[0x10:0x14] = b'NRO0'; struct.pack_into('<I', raw, 0x18, len(raw))
        struct.pack_into('<6I', raw, 0x20, 0, 0x1000, 0x1000, 0x2000, 0x3000, 0x1000); struct.pack_into('<I', raw, 0x38, 0)
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 'input.nro'; src.write_bytes(raw)
            run = subprocess.run([sys.executable, str(TOOLS / 'patch_nso.py'), str(src), tmp, 'nro0'],
                                 env=os.environ | {'DM_GAME': '010015100B514000', 'DM_REWRITE_J': '1'}, capture_output=True, text=True)
            path = Path(tmp) / 'nro0.sites.tsv'
            rows = [line.split('\t') for line in path.read_text().splitlines()] if path.exists() else []
        return run, [[int(t, 0) for t in row[2].split()] for row in rows if row[1] == 'T']

    def test_exact_targets(self):
        run, targets = self.rewrite()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(targets, [[0x100, 0x120, 0x124, 0x128]])

    def test_branches_from_other_functions(self):
        run, _ = self.rewrite(incoming=0x110)   # the tbnz: the mask is skipped
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('jump table 11c', run.stderr)
        run, _ = self.rewrite(incoming=0x10c)   # the mask, with whatever x11 holds there: no base
        self.assertNotEqual(run.returncode, 0)
        run, targets = self.rewrite(incoming=0x100)   # the function's start
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(targets, [[0x100, 0x120, 0x124, 0x128]])

    def test_tables_in_other_functions(self):
        run, _ = self.rewrite(byte_table=True)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('an entry from another function is not proved', run.stderr)


if __name__ == '__main__': unittest.main()
