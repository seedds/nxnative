"""System register accesses whose register is x18 (issue #2123), without game binaries: the rewritten block copies between thread slots and never names x18.

Optional dependency: unicorn, which runs the blocks. On macOS its JIT needs execution outside the sandbox."""
import ast
import importlib
from itertools import combinations
from pathlib import Path
import re
import struct
import sys
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import nso_asm

try:
    uc, arm = importlib.import_module('unicorn'), importlib.import_module('unicorn.arm64_const')
except ImportError:
    uc = arm = None

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
md.detail = True
PATCH_BASE, KEY_PAGE, TSD, STACK = 0x10000, 0x20000, 0x30000, 0x40000
KEY_INDEX = {k: i for i, k in enumerate(('K18', 'KTLS', 'KTLR', 'KGATE'))}
SLOT = {'K18': 0x40, 'KTLS': 0x48, 'KTLR': 0x50, 'KGATE': 0x58}   # byte offsets from tpidrro_el0, as the key page holds them
scope = dict(vars(nso_asm), re=re, struct=struct, combinations=combinations, md=md,
             patch=bytearray(), PATCH_BASE=PATCH_BASE, KEY_PAGE_OFF=KEY_PAGE, KEY_INDEX=KEY_INDEX, fix=[])
names = {'here', 'emit', 'emit_key_addr', 'emit_slot_addr', 'dis', 'regset', 'num', 'regs_in', 'pick',
         'site_slot_copy', 'site_sysreg_read', 'site_tpidr_write', 'BX', 'reach_via'}   # BX and reach_via (with REACH, _holes and _hops): the site's branches, through a hop where a b does not reach
tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names or isinstance(n, ast.Assign) and {getattr(t, 'id', None) for t in ast.walk(n.targets[0])} & {'REACH', '_holes'}],
                        type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), scope)

SITE = 0x1000                      # the original instruction's address; every block ends with b SITE + 4
MRS_TPIDR, MRS_TPIDRRO, MSR_TPIDR, MRS_CTR = 0xD53BD040, 0xD53BD060, 0xD51BD040, 0xD53B0020
MOVZ_CTR = 0xD2A00080              # movz xN, #4, lsl #16


def block(word):
    scope['patch'].clear(); scope['fix'].clear()
    ins = scope['dis'](word, SITE)
    if word & ~31 == MRS_TPIDR: scope['site_sysreg_read'](ins, 'KTLS')
    elif word & ~31 == MRS_TPIDRRO: scope['site_sysreg_read'](ins, 'KTLR')
    elif word & ~31 == MSR_TPIDR: scope['site_tpidr_write'](ins)
    else: assert word == MRS_CTR | 18; scope['site_slot_copy'](ins, MOVZ_CTR | 15, 'K18')   # as the rewriter's ctr case does
    return bytes(scope['patch'])


class StaticShape(unittest.TestCase):
    def test_x18_forms_never_name_x18(self):
        for word in (MRS_TPIDR | 18, MRS_TPIDRRO | 18, MSR_TPIDR | 18, MRS_CTR | 18):
            with self.subTest(word=word):
                code = block(word)
                ins = list(md.disasm(code, PATCH_BASE))
                self.assertEqual(len(ins) * 4, len(code))
                self.assertFalse([i for i in ins if re.search(r'\b[xw]18\b', i.op_str)], [f'{i.mnemonic} {i.op_str}' for i in ins])
                self.assertEqual((ins[-1].mnemonic, ins[-1].operands[0].imm), ('b', SITE + 4))

    def test_other_registers_unchanged(self):
        # The register forms other than x18 keep their blocks: x16 borrowed in the red zone, the value in xN
        ins = list(md.disasm(block(MRS_TPIDR | 0), PATCH_BASE))
        self.assertEqual([f'{i.mnemonic} {i.op_str}' for i in ins][:2], ['stur x16, [sp, #-0x10]', 'mrs x0, tpidrro_el0'])


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class Run(unittest.TestCase):
    def run_block(self, word, slots, regs=None):
        assert uc is not None and arm is not None
        code = block(word)
        mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        for base in (0, PATCH_BASE, KEY_PAGE, TSD, STACK): mu.mem_map(base, 0x10000)
        mu.mem_write(PATCH_BASE, code)
        for k, i in KEY_INDEX.items(): mu.mem_write(KEY_PAGE + 8 * i, struct.pack('<Q', SLOT[k]))
        for k, v in slots.items(): mu.mem_write(TSD + SLOT[k], struct.pack('<Q', v))
        mu.reg_write(arm.UC_ARM64_REG_TPIDRRO_EL0, TSD)
        mu.reg_write(arm.UC_ARM64_REG_SP, STACK + 0x8000)
        regs = regs or {r: 0x1000 + r for r in range(31) if r != 18}
        for r, v in regs.items(): mu.reg_write(arm.UC_ARM64_REG_X0 + r, v)
        mu.reg_write(arm.UC_ARM64_REG_X18, 0xdead)   # what the Apple platform register happens to hold: never read
        mu.emu_start(PATCH_BASE, SITE + 4)
        out = {k: struct.unpack('<Q', bytes(mu.mem_read(TSD + SLOT[k], 8)))[0] for k in SLOT}
        for r, v in regs.items(): self.assertEqual(mu.reg_read(arm.UC_ARM64_REG_X0 + r), v, f'x{r}')
        return out

    def test_mrs_x18_tpidr_el0_writes_k18(self):
        out = self.run_block(MRS_TPIDR | 18, {'K18': 0x33200, 'KTLS': 0x7bdf11c420})
        self.assertEqual(out['K18'], 0x7bdf11c420)

    def test_mrs_x18_tpidrro_el0_writes_k18(self):
        out = self.run_block(MRS_TPIDRRO | 18, {'K18': 0x33200, 'KTLR': 0x7be3b6c000})
        self.assertEqual(out['K18'], 0x7be3b6c000)

    def test_msr_tpidr_el0_x18_reads_k18(self):
        out = self.run_block(MSR_TPIDR | 18, {'K18': 0x1234500, 'KTLS': 0})
        self.assertEqual((out['KTLS'], out['K18']), (0x1234500, 0x1234500))

    def test_mrs_x18_ctr_el0_writes_k18(self):
        out = self.run_block(MRS_CTR | 18, {'K18': 0x33200})
        self.assertEqual(out['K18'], 4 << 16)


if __name__ == '__main__':
    unittest.main()
