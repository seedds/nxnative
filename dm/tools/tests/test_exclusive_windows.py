"""Synthetic AArch64 exclusive windows, without game binaries."""
import ast
from itertools import combinations
from pathlib import Path
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import nso_asm

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
md.detail = True
scope = dict(vars(nso_asm), re=re, struct=struct, combinations=combinations, md=md,
             patch=bytearray(), PATCH_BASE=0x10000, KEY_PAGE_OFF=0x8000,
             KEY_INDEX={'K18': 0}, fix=[])
names = {'here', 'emit', 'emit_key_addr', 'emit_slot_addr', 'dis', 'regset', 'num',
         'has18', 'regs_in', 'pick', 'subst', 'find_windows', 'window_adrp', 'check_window_branches', 'site_window', 'site_retry_window', 'BX', 'reach_via'}   # BX and reach_via (with REACH, _holes and _hops): the site's branches, through a hop where a b does not reach
tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names or isinstance(n, ast.Assign) and {getattr(t, 'id', None) for t in ast.walk(n.targets[0])} & {'REACH', '_holes'}],
                        type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), scope)
scope['EXCL_LD'] = ('ldxr', 'ldaxr', 'ldxp', 'ldaxp', 'ldxrb', 'ldaxrb', 'ldxrh', 'ldaxrh')
scope['EXCL_ST'] = ('stxr', 'stlxr', 'stxp', 'stlxp', 'stxrb', 'stlxrb', 'stxrh', 'stlxrh')


def decode(words, base=0x1000):
    return list(md.disasm(struct.pack('<' + 'I' * len(words), *words), base))


def cbz(reg, frm, to):
    return 0x34000000 | (((to - frm) >> 2) & 0x7ffff) << 5 | reg


# ldaxrb w18,[x1]; cbz w18, store; clrex; b out;
# stlxrb w2,w3,[x1]; cbz w2, load
LDAXRB = 0x085ffc00 | 1 << 5 | 18
STLXRB = 0x0800fc00 | 2 << 16 | 1 << 5 | 3
CLREX = 0xd5033f5f


class ExclusiveWindows(unittest.TestCase):
    def setUp(self):
        scope['patch'].clear()
        scope['fix'].clear()

    def rewrite(self, functions):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from test_nso_match import image

        raw = image(functions)
        raw[0x1100:0x111c] = raw[0x20:0x3c]
        struct.pack_into('<I', raw, 4, 0x1100)
        struct.pack_into('<ii', raw, 0x1110, 0x3000 - 0x1100, 0x300c + 8 * len(functions) - 0x1100)
        raw[0x10:0x14] = b'NRO0'
        struct.pack_into('<I', raw, 0x18, len(raw))
        struct.pack_into('<6I', raw, 0x20, 0, 0x1000, 0x1000, 0x2000, 0x3000, 0x1000)
        struct.pack_into('<I', raw, 0x38, 0)
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 'input.nro'
            src.write_bytes(raw)
            run = subprocess.run([sys.executable, str(TOOLS / 'patch_nso.py'), str(src), tmp, 'nro0'],
                                 env=os.environ | {'DM_GAME': '', 'DM_REWRITE_J': '1'},
                                 capture_output=True, text=True)
            sites = Path(tmp) / 'nro0.sites.tsv'
            rows = [line.split('\t') for line in sites.read_text().splitlines()] if sites.exists() else []
            return run, rows

    def test_real_rewriter_accepts_branch_to_store_inside_window(self):
        words = [LDAXRB, cbz(18, 0x104, 0x110), CLREX,
                 nso_asm.B(0x10c, 0x118), STLXRB, cbz(2, 0x114, 0x100), 0xd65f03c0]
        run, rows = self.rewrite({0x100: words})
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn(['0x100', 'W', '0x118'], rows)

    def test_external_function_cannot_enter_window_interior(self):
        words = [LDAXRB, cbz(18, 0x104, 0x110), CLREX,
                 nso_asm.B(0x10c, 0x118), STLXRB, cbz(2, 0x114, 0x100), 0xd65f03c0]
        run, _ = self.rewrite({0x100: words, 0x200: [nso_asm.B(0x200, 0x110), 0xd65f03c0]})
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('branch into window from another function 200', run.stderr)

    def test_retry_tail_cannot_reenter_uncopied_window_prefix(self):
        words = [LDAXRB, cbz(18, 0x104, 0x110), CLREX,
                 nso_asm.B(0x10c, 0x118), STLXRB, cbz(2, 0x114, 0x108)]
        words += [nso_asm.NOP] * ((0x200 - 0x118) // 4)
        words += [LDAXRB, cbz(18, 0x204, 0x110), 0xd65f03c0]
        run, _ = self.rewrite({0x100: words})
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('retry copy branches into window 114', run.stderr)

    def test_undecoded_gap_in_window_is_rejected(self):
        words = [LDAXRB, nso_asm.B(0x104, 0x10c), 0xffffffff, STLXRB,
                 cbz(2, 0x110, 0x100), 0xd65f03c0]
        run, _ = self.rewrite({0x100: words})
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('noncontiguous window 100', run.stderr)

    def check_unconditional_branch(self, target):
        words = [LDAXRB, CLREX, nso_asm.B(0x1008, target), STLXRB, cbz(2, 0x1010, 0x1000)]
        seg = decode(words)
        scope['site_window'](seg)
        copied = list(md.disasm(bytes(scope['patch']), scope['PATCH_BASE']))
        load = next(x for x in copied if x.mnemonic == 'ldaxrb')
        branch = next(x for x in copied if x.address == load.address + 8)
        self.assertEqual(branch.mnemonic, 'b')
        dest = branch.operands[0].imm
        if 0x1000 <= target < 0x1014:
            self.assertEqual(dest, load.address + target - 0x1000)
        else:
            stub = [x for x in copied if dest <= x.address < dest + 16]
            self.assertEqual([x.mnemonic for x in stub], ['str', 'ldp', 'b', 'nop'])
            self.assertEqual(stub[2].operands[0].imm, target)
        fallthrough = [x for x in copied if load.address + 20 <= x.address < load.address + 36]
        self.assertEqual([x.mnemonic for x in fallthrough], ['str', 'ldp', 'b', 'nop'])
        self.assertEqual(fallthrough[2].operands[0].imm, 0x1014)

    def test_unconditional_branch_to_internal_store(self):
        self.check_unconditional_branch(0x100c)

    def test_unconditional_branch_to_internal_head(self):
        self.check_unconditional_branch(0x1000)

    def test_unconditional_branch_to_outside(self):
        self.check_unconditional_branch(0x1020)

    def test_external_branch_with_live_reservation_is_rejected(self):
        seg = decode([LDAXRB, nso_asm.B(0x1004, 0x1020), STLXRB])
        with self.assertRaisesRegex(AssertionError, 'branch leaves window with live reservation'):
            scope['site_window'](seg)

    def test_branch_skipping_clrex_is_rejected(self):
        seg = decode([LDAXRB, cbz(18, 0x1004, 0x100c), CLREX,
                      nso_asm.B(0x100c, 0x1020), STLXRB])
        with self.assertRaisesRegex(AssertionError, 'branch leaves window with live reservation'):
            scope['site_window'](seg)

    def test_internal_branch_skipping_store_cannot_fall_out_live(self):
        seg = decode([LDAXRB, nso_asm.B(0x1004, 0x100c), STLXRB,
                      cbz(2, 0x100c, 0x1000)])
        with self.assertRaisesRegex(AssertionError, 'branch leaves window with live reservation'):
            scope['site_window'](seg)

    def test_internal_branch_cannot_leave_conditionally_live(self):
        seg = decode([LDAXRB, nso_asm.B(0x1004, 0x1008),
                      cbz(18, 0x1008, 0x1020), STLXRB])
        with self.assertRaisesRegex(AssertionError, 'branch leaves window with live reservation'):
            scope['site_window'](seg)

    def test_retry_copy_keeps_both_branches_in_copy(self):
        retry = decode([LDAXRB, cbz(18, 0x2004, 0x1010)], 0x2000)
        tail = decode([STLXRB, cbz(2, 0x1014, 0x2000)], 0x1010)
        scope['site_retry_window']([(retry, 0x2008), (tail, 0x1018)])
        copied = list(md.disasm(bytes(scope['patch']), scope['PATCH_BASE']))
        load = next(x for x in copied if x.mnemonic == 'ldaxrb')
        store = next(x for x in copied if x.mnemonic == 'stlxrb')
        branches = [x for x in copied if x.mnemonic == 'cbz']
        self.assertEqual([x.operands[-1].imm for x in branches], [store.address, load.address])
        fallthrough = [x for x in copied if load.address + 8 <= x.address < load.address + 24]
        self.assertEqual([x.mnemonic for x in fallthrough], ['str', 'ldp', 'b', 'nop'])
        self.assertEqual(fallthrough[2].operands[0].imm, 0x2008)


if __name__ == '__main__':
    unittest.main()
