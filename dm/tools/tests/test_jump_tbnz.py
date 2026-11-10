"""Bit-test bounded tables, using decoded ARM64 and the actual rewriter driver."""
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
from nso_asm import ADRP, ADDI, ADDR, B, BR, NOP

RET = 0xd65f03c0


def tbnz(reg, bit, at, to): return 0x37000000 | (bit & 31) << 19 | (bit >> 5) << 31 | (((to - at) // 4) & 0x3fff) << 5 | reg
def mask(dst, src, bit): return 0x92400000 | bit << 10 | src << 5 | dst
def load(ent, base, idx): return 0xb8a07800 | idx << 16 | base << 5 | ent
def mov(dst, src, wide=True): return (0xaa0003e0 if wide else 0x2a0003e0) | src << 16 | dst


class BitTestTable(unittest.TestCase):
    TABLE = 0x2040
    TARGETS = [0, 40, 44, 48]

    def setUp(self):
        self.img = bytearray(0x4000)
        for i, target in enumerate(self.TARGETS): struct.pack_into('<i', self.img, self.TABLE + 4 * i, target - self.TABLE)
        struct.pack_into('<i', self.img, self.TABLE + 16, 0x1000)  # not a fifth entry
        self.scope: dict[str, Any] = dict(re=re, struct=struct, IMG=self.img, tabjumps={}, jumps={})
        tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
        names = {'num', 'written_w', 'writes_reg', 'jump_tbnz_masked'}
        exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names], type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), self.scope)

    def words(self, base=11, src=15, idx=13, ent=13, bit=2):
        # A bit-2 test plus mask7 admits 0..3. The table contains signed offsets.
        return [ADRP(base, 0, self.TABLE), ADDI(base, base, self.TABLE & 0xfff), NOP,
                tbnz(src, bit, 12, 40), mask(idx, src, bit), load(ent, base, idx), ADDR(18, ent, base), BR(18),
                RET, RET, RET, RET, RET]

    def run_site(self, words):
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = [p for i, word in enumerate(words) for p in md.disasm(struct.pack('<I', word), 4 * i)]
        j = next(i for i, p in enumerate(ins) if p.mnemonic == 'br' and p.op_str == 'x18')
        match = self.scope.get('jump_tbnz_masked')
        result = match(ins, j) if match else None
        return result[0] if result else None

    def test_reads_only_bit_test_fallthrough_entries(self):
        self.assertEqual(self.run_site(self.words()), self.TARGETS)
        # Bit0 admits only index0; even the next valid-looking word is not read.
        self.assertEqual(self.run_site(self.words(bit=0)), [0])
        self.assertEqual(self.run_site(self.words(bit=1)), [0, 40])

    def test_register_aliases_and_unrelated_vector_writes(self):
        for base, src, idx, ent in [(29, 30, 18, 18), (30, 29, 29, 15), (18, 15, 13, 13)]:
            with self.subTest(base=base, src=src, idx=idx, ent=ent):
                self.assertEqual(self.run_site(self.words(base, src, idx, ent)), self.TARGETS)
        words = self.words(); words[2] = 0xbd400000 | 31 << 5 | 11  # ldr s11,[sp], unrelated to x11
        self.assertEqual(self.run_site(words), self.TARGETS)

    def test_guard_and_mask_must_prove_the_same_index(self):
        for slot, word in [(3, tbnz(14, 2, 12, 40)), (3, tbnz(15, 1, 12, 40)),
                           (3, tbnz(15, 2, 12, 40) ^ 0x01000000), (3, tbnz(15, 32, 12, 40)),
                           (4, mask(13, 15, 3)), (4, mask(13, 15, 2) & ~((1 << 31) | (1 << 22))),
                           (4, mask(12, 15, 2)), (5, load(14, 11, 13)), (6, ADDR(18, 13, 10))]:
            with self.subTest(slot=slot, word=hex(word)):
                words = self.words(); words[slot] = word
                self.assertIsNone(self.run_site(words))
        for target in (4, 8, 12, 16, 20, 24, 28):
            words = self.words(); words[3] = tbnz(15, 2, 12, target)
            self.assertIsNone(self.run_site(words))

    def test_load_must_preserve_its_base_until_the_add(self):
        self.assertIsNone(self.run_site(self.words(ent=11)))
        self.assertIsNone(self.run_site(self.words(idx=11)))

    def test_dispatch_cannot_contain_intervening_writes_or_control_flow(self):
        for slot in (4, 5, 6, 7):
            for word in (mov(13, 0, False), mov(18, 0), 0xd63f0000, B(4 * slot, 48), 0xe7ffdefe):
                with self.subTest(slot=slot, word=hex(word)):
                    words = self.words(); words.insert(slot, word)
                    self.assertIsNone(self.run_site(words))

    def test_base_provenance_cannot_cross_a_clobber_call_jump_or_gap(self):
        for slot, word in [(0, NOP), (1, mov(11, 0)), (1, ADDI(11, 11, 0x40) & 0x7fffffff),
                           (2, mov(11, 0, False)), (2, 0x94000000), (2, B(8, 12)),
                           (2, 0xd4000001), (2, 0xe7ffdefe)]:
            with self.subTest(slot=slot, word=hex(word)):
                words = self.words(); words[slot] = word
                self.assertIsNone(self.run_site(words))
        words = self.words(); words[0:2] = [ADRP(30, 0, self.TABLE), ADDI(30, 30, 0x40)]; words[2] = mov(11, 30)
        # This exact fallback accepts a direct base only; copies have a separate proof.
        self.assertIsNone(self.run_site(words))

    def test_incoming_edges_must_reexecute_base_and_guard(self):
        for target in (4, 8, 12, 16, 20, 24, 28):
            for branch in (B(36, target), B(36, target) | 0x80000000, tbnz(0, 0, 36, target)):
                with self.subTest(target=target, branch=hex(branch)):
                    words = self.words(); words[9] = branch
                    self.assertIsNone(self.run_site(words))
        words = self.words(); words[9] = B(36, 0)
        self.assertEqual(self.run_site(words), self.TARGETS)
        for registry, value in [('tabjumps', [16]), ('jumps', (12, 2))]:
            self.scope[registry][0x100] = value
            self.assertIsNone(self.run_site(self.words()))
            self.scope[registry].clear()

    def test_targets_must_be_decoded_outside_the_protected_run(self):
        for target in (4, 12, 16, 20, 28, 42, 0x1000):
            with self.subTest(target=target):
                struct.pack_into('<i', self.img, self.TABLE, target - self.TABLE)
                self.assertIsNone(self.run_site(self.words()))
        struct.pack_into('<i', self.img, self.TABLE, -self.TABLE)
        words = self.words(); words[10] = 0xe7ffdefe
        self.assertIsNone(self.run_site(words))

    def test_table_extent_is_checked_before_reading(self):
        words = self.words(); words[0] = ADRP(11, 0, 0x4000)
        self.assertIsNone(self.run_site(words))
        # A valid bit31 mask would need 2^31 entries, beyond this image.
        self.assertIsNone(self.run_site(self.words(bit=31)))
        del self.img[self.TABLE + 15:]
        self.assertIsNone(self.run_site(self.words()))

    def rewrite(self, incoming=None, later_table=False, older=False):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from test_nso_match import image
        words = self.words(); words[0] = ADRP(11, 0x100, self.TABLE)
        words[3] = tbnz(15, 2, 0x10c, 0x128)
        if older: words[3:5] = [0x71000dbf, 0x54000088]  # cmp w13,#3; b.hi +16
        functions = {0x100: words}
        if incoming is not None: functions[0x200] = [B(0x200, incoming), RET]
        if later_table:
            functions[0x200] = [ADRP(10, 0x200, 0x2080), ADDI(10, 10, 0x80), 0x710001bf,
                                0x54000088, load(13, 10, 13), ADDR(18, 13, 10), BR(18), RET]
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

    def test_rewriter_accepts_the_exact_target_set_and_keeps_old_matches(self):
        for older in (False, True):
            with self.subTest(older=older):
                run, targets = self.rewrite(older=older)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertEqual(targets, [[0x100, 0x128, 0x12c, 0x130]])

    def test_rewriter_checks_incoming_edges_from_other_functions(self):
        run, _ = self.rewrite(incoming=0x110)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('branch bypasses bit-test table proof', run.stderr)
        run, targets = self.rewrite(incoming=0x100)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(targets, [[0x100, 0x128, 0x12c, 0x130]])

    def test_rewriter_checks_tables_resolved_later(self):
        run, _ = self.rewrite(later_table=True)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('jump table bypasses bit-test table proof', run.stderr)


if __name__ == '__main__': unittest.main()
