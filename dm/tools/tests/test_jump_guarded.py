"""jump_guarded_tail's bit-test and far-compare forms (ULTRA STREET FIGHTER II 1.1.1 main+0x590ee8 and +0x63607c), on decoded ARM64 and through the actual rewriter driver.
Its zero-extended and complemented forms are tested in test_jump_copies.py."""
import ast
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from typing import Any
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from nso_asm import ADRP, ADDI, ADDR, B, BL, BR, NOP

RET = 0xd65f03c0


def tbnz(reg, bit, at, to): return 0x37000000 | (bit & 31) << 19 | (bit >> 5) << 31 | (((to - at) // 4) & 0x3fff) << 5 | reg
def mask(dst, src, bit): return 0x92400000 | bit << 10 | src << 5 | dst   # and xD, xS, #(2^(bit + 1) - 1)
def load(ent, base, idx): return 0xb8a07800 | idx << 16 | base << 5 | ent   # ldrsw xE, [xB, xI, lsl #2]
def mov(dst, src, wide=True): return (0xaa0003e0 if wide else 0x2a0003e0) | src << 16 | dst
def cmp(reg, imm): return 0x7100001f | imm << 10 | reg << 5   # cmp wR, #imm
def bhi(at, to): return 0x54000008 | (((to - at) // 4) & 0x7ffff) << 5
def blo(at, to): return 0x54000003 | (((to - at) // 4) & 0x7ffff) << 5
def movz(reg, imm): return 0x52800000 | imm << 5 | reg   # mov wR, #imm


def decode(words):
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
    return [p for i, word in enumerate(words) for p in md.disasm(struct.pack('<I', word), 4 * i)]


class Recognizer(unittest.TestCase):
    TABLE = 0x2040

    def setUp(self):
        self.img = bytearray(0x4000)
        self.scope: dict[str, Any] = dict(re=re, struct=struct, IMG=self.img, tabjumps={}, jumps={})
        tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
        names = {'num', 'written_w', 'writes_reg', 'copied_base', 'jump_offsets', 'guarded_count', 'null_base', 'writes_reg_any', 'check_before_copy', 'table_base', 'forward_table_base',
                 'guarded_run', 'jump_guarded_tail'}
        exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names], type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), self.scope)

    def table(self, targets, poison):
        for i, target in enumerate(targets): struct.pack_into('<i', self.img, self.TABLE + 4 * i, target - self.TABLE)
        struct.pack_into('<i', self.img, self.TABLE + 4 * len(targets), poison - self.TABLE)   # the next word: never a target of this dispatch

    def run_site(self, words):
        ins = decode(words)
        r = self.scope['jump_guarded_tail'](ins, next(i for i, p in enumerate(ins) if p.mnemonic == 'br' and p.op_str == 'x18'))
        return r[0] if r else None


class BitTest(Recognizer):
    TARGETS = [0, 40, 44, 48]

    def setUp(self):
        super().setUp(); self.table(self.TARGETS, 0x1000)

    def words(self, base=11, src=15, idx=13, ent=13, bit=2):
        # tbnz w15,#2 then and x13,x15,#7 admit 0..3
        return [ADRP(base, 0, self.TABLE), ADDI(base, base, self.TABLE & 0xfff), NOP,
                tbnz(src, bit, 12, 40), mask(idx, src, bit), load(ent, base, idx), ADDR(18, ent, base), BR(18),
                RET, RET, RET, RET, RET]

    def test_reads_only_the_fall_through_entries(self):
        self.assertEqual(self.run_site(self.words()), self.TARGETS)
        self.assertEqual(self.run_site(self.words(bit=0)), [0])
        self.assertEqual(self.run_site(self.words(bit=1)), [0, 40])

    def test_register_aliases_and_unrelated_writes(self):
        for base, src, idx, ent in [(29, 30, 18, 18), (30, 29, 29, 15), (18, 15, 13, 13)]:
            with self.subTest(base=base, src=src, idx=idx, ent=ent):
                self.assertEqual(self.run_site(self.words(base, src, idx, ent)), self.TARGETS)
        words = self.words(); words[2] = 0xbd400000 | 31 << 5 | 11   # ldr s11,[sp], not x11
        self.assertEqual(self.run_site(words), self.TARGETS)

    def test_guard_and_mask_must_bound_the_same_index(self):
        for slot, word in [(3, tbnz(14, 2, 12, 40)), (3, tbnz(15, 1, 12, 40)), (3, tbnz(15, 2, 12, 40) ^ 0x01000000),   # another register, bit, or tbz
                           (4, mask(13, 15, 3)), (4, mask(13, 15, 1)), (4, mask(12, 15, 2)), (5, load(14, 11, 13)), (6, ADDR(18, 13, 10))]:
            with self.subTest(slot=slot, word=hex(word)):
                words = self.words(); words[slot] = word
                self.assertIsNone(self.run_site(words))
        for target in (16, 20, 24, 28):   # the guard's branch must leave the run
            words = self.words(); words[3] = tbnz(15, 2, 12, target)
            self.assertIsNone(self.run_site(words))

    def test_load_must_keep_its_base_until_the_add(self):
        self.assertIsNone(self.run_site(self.words(ent=11)))
        self.assertIsNone(self.run_site(self.words(idx=11)))

    def test_no_write_or_control_flow_after_the_guard(self):
        clobber = {4: mov(15, 0, False), 5: mov(13, 0, False), 6: mov(11, 0), 7: mov(18, 0)}   # the source, the index, the base, x18
        for slot in (4, 5, 6, 7):
            for word in (clobber[slot], 0xd63f0000, B(4 * slot, 48), 0xd4000001, 0xe7ffdefe):
                with self.subTest(slot=slot, word=hex(word)):
                    words = self.words(); words.insert(slot, word)
                    self.assertIsNone(self.run_site(words))

    def test_base_follows_copied_base(self):
        words = self.words(); words[0:3] = [ADRP(30, 0, self.TABLE), ADDI(30, 30, self.TABLE & 0xfff), mov(11, 30)]   # copied: accepted, as copied_base accepts it
        self.assertEqual(self.run_site(words), self.TARGETS)
        for word in (mov(11, 0, False), BL(8, 0x1000)):   # a 32-bit write of the base, a call while it is caller-saved
            with self.subTest(word=hex(word)):
                words = self.words(); words[2] = word
                self.assertIsNone(self.run_site(words))

    def test_loops_around_the_dispatch(self):
        # ULTRA STREET FIGHTER II main+0x590ee8: the base built before two loops whose back edges land before the guard; the guard may be re-entered too
        words = [ADRP(13, 0, self.TABLE), ADDI(13, 13, self.TABLE & 0xfff), NOP,
                 tbnz(17, 2, 12, 48), mask(18, 17, 2), load(18, 13, 18), ADDR(18, 18, 13), BR(18),
                 RET, RET, RET, RET, blo(48, 8), blo(52, 12), RET]
        struct.pack_into('<i', self.img, self.TABLE, 4 - self.TABLE)
        self.assertEqual(self.run_site(words), [4, 40, 44, 48])
        for target in (16, 20, 24, 28):   # past the guard
            with self.subTest(target=target):
                w = list(words); w[13] = blo(52, target)
                self.assertIsNone(self.run_site(w))

    def test_tables_resolved_before_may_not_land_past_the_guard(self):
        for registry, value in [('tabjumps', [16]), ('jumps', (12, 2))]:
            with self.subTest(registry=registry):
                self.scope[registry][0x100] = value
                self.assertIsNone(self.run_site(self.words()))
                self.scope[registry].clear()

    def test_targets_are_decoded_instructions_outside_the_run(self):
        for target in (16, 20, 28, 42, 0x1000):
            with self.subTest(target=target):
                struct.pack_into('<i', self.img, self.TABLE, target - self.TABLE)
                self.assertIsNone(self.run_site(self.words()))
        struct.pack_into('<i', self.img, self.TABLE, -self.TABLE)
        words = self.words(); words[10] = 0xe7ffdefe   # target 40 undecodable
        self.assertIsNone(self.run_site(words))

    def test_table_must_fit_the_image(self):
        words = self.words(); words[0] = ADRP(11, 0, 0x4000)
        self.assertIsNone(self.run_site(words))
        self.assertIsNone(self.run_site(self.words(bit=30)))   # 2^30 entries


class FarCompare(Recognizer):
    TARGETS = [4 * (26 + i) for i in range(3)]

    def setUp(self):
        super().setUp(); self.table(self.TARGETS, 0x1000)

    def words(self):
        # ULTRA STREET FIGHTER II main+0x63607c: cmp w18,#N; b.hi, then the base, the load and constants moved into registers: the cmp 24 instructions before the br
        w = [mov(18, 0, False), cmp(18, 2), bhi(8, 4 * 31), ADRP(0, 12, self.TABLE), ADDI(0, 0, self.TABLE & 0xfff), movz(9, 1), load(18, 0, 18)]
        w += [movz(10 + i % 6, i) for i in range(10)] + [ADDR(18, 18, 0)] + [movz(10 + i % 6, i) for i in range(7)] + [BR(18)]
        return w + [RET] * 6

    def test_reads_n_plus_one_entries(self):
        words = self.words()
        self.assertEqual(words.index(BR(18)) - 1, 24)   # the cmp is 24 instructions before the br
        ins = decode(words)
        self.assertIsNone(self.scope['jump_offsets'](ins, words.index(BR(18))))   # out of jump_offsets' reach
        self.assertEqual(self.run_site(words), self.TARGETS)

    def test_index_unchanged_from_the_check(self):
        for slot, word in ((5, mov(18, 9, False)), (5, B(20, 24)), (5, BL(20, 0x1000)), (12, mov(0, 9)), (20, mov(18, 9))):
            with self.subTest(slot=slot, word=hex(word)):
                words = self.words(); words[slot] = word
                self.assertIsNone(self.run_site(words))

    def test_a_compare_that_guards_nothing_is_no_bound(self):
        words = self.words(); words[2] = NOP
        self.assertIsNone(self.run_site(words))
        words = self.words(); words[2] = 0x54000000 | ((4 * 31 - 8) // 4) << 5   # b.eq: no bounds check
        self.assertIsNone(self.run_site(words))


class Driver(unittest.TestCase):
    """The rewriter's own driver: a bit-test table, and the checks after every function for tables resolved later and branches from other functions"""
    TABLE = 0x2040

    def rewrite(self, incoming=None, later_table=False, older=False):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from test_nso_match import image
        words = [ADRP(11, 0x100, self.TABLE), ADDI(11, 11, self.TABLE & 0xfff), NOP, tbnz(15, 2, 0x10c, 0x128), mask(13, 15, 2), load(13, 11, 13), ADDR(18, 13, 11), BR(18),
                 RET, RET, RET, RET, RET]
        if older: words[3:5] = [0x71000dbf, 0x54000088]   # cmp w13,#3; b.hi: jump_offsets reads it first
        functions = {0x100: words}
        if incoming is not None: functions[0x200] = [B(0x200, incoming), RET]
        if later_table:
            functions[0x200] = [ADRP(10, 0x200, 0x2080), ADDI(10, 10, 0x80), 0x710001bf, 0x54000088, load(13, 10, 13), ADDR(18, 13, 10), BR(18), RET]
        raw = image(functions)
        targets = [0x100, 0x128, 0x12c, 0x130]
        for i, target in enumerate(targets): struct.pack_into('<i', raw, self.TABLE + 4 * i, target - self.TABLE)
        struct.pack_into('<i', raw, self.TABLE + 16, 0x1000)
        if later_table: struct.pack_into('<i', raw, 0x2080, 0x110 - 0x2080)
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

    def test_exact_target_set_and_older_matches_kept(self):
        for older in (False, True):
            with self.subTest(older=older):
                run, targets = self.rewrite(older=older)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertEqual(targets, [[0x100, 0x128, 0x12c, 0x130]])

    def test_branches_from_other_functions(self):
        run, _ = self.rewrite(incoming=0x110)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('branch past a guard', run.stderr)
        for to in (0x100, 0x10c):   # the function's start, the guard itself
            with self.subTest(to=to):
                run, targets = self.rewrite(incoming=to)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertEqual(targets, [[0x100, 0x128, 0x12c, 0x130]])

    def test_tables_resolved_later(self):
        run, _ = self.rewrite(later_table=True)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('jump table past a guard', run.stderr)


if __name__ == '__main__': unittest.main()
