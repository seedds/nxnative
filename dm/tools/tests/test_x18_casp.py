"""Compare and swap of a register pair that is x18, x19 (site_casp), without game binaries: the rewritten block never names x18, and run on a thread slot area it does what the
instruction does with the guest's x18 in the slot K18.

Optional dependency: unicorn, which runs the blocks. On macOS its JIT needs execution outside the sandbox."""
import ast
import importlib
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
PATCH_BASE, KEY_PAGE, TSD, STACK, MEM = 0x10000, 0x20000, 0x30000, 0x40000, 0x50000
KEY_INDEX = {k: i for i, k in enumerate(('K18', 'KTLS', 'KTLR', 'KGATE'))}
K18 = 0x40                                   # the slot's byte offset from tpidrro_el0, as the key page holds it
scope = dict(vars(nso_asm), re=re, struct=struct, md=md, patch=bytearray(), PATCH_BASE=PATCH_BASE, KEY_PAGE_OFF=KEY_PAGE, KEY_INDEX=KEY_INDEX, fix=[])
names = {'here', 'emit', 'emit_key_addr', 'emit_slot_addr', 'dis', 'regset', 'num', 'regs_in', 'casp_pair18', 'site_casp'}
tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names], type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), scope)

SITE = 0x1000                                # the original instruction's address; the block ends with b SITE + 4


def casp(rs, rt, rn, x=True, acq=True, rel=True):
    """casp{a}{l} Rs, Rs + 1, Rt, Rt + 1, [Rn]"""
    return 0x08207C00 | x << 30 | acq << 22 | rs << 16 | rel << 15 | rn << 5 | rt


CASES = {   # Sonic Frontiers Definitive Edition main+0x19ce5c4 first
    'caspal x18, x19, x4, x5, [x16]': casp(18, 4, 16),
    'casp x4, x5, x18, x19, [x16]': casp(4, 18, 16, acq=False, rel=False),
    'caspa w18, w19, w2, w3, [x0]': casp(18, 2, 0, x=False, rel=False),
    'caspl x18, x19, x18, x19, [x6]': casp(18, 18, 6, acq=False),
}


def block(word):
    scope['patch'].clear(); scope['fix'].clear()
    ins = scope['dis'](word, SITE)
    assert scope['casp_pair18'](ins), f'{ins.mnemonic} {ins.op_str}'
    scope['site_casp'](ins)
    return ins, bytes(scope['patch'])


class StaticShape(unittest.TestCase):
    def test_cases_disassemble_as_named(self):
        for text, word in CASES.items():
            ins = scope['dis'](word, SITE)
            self.assertEqual(f'{ins.mnemonic} {ins.op_str}', text)

    def test_block_never_names_x18(self):
        for text, word in CASES.items():
            with self.subTest(text):
                _, code = block(word)
                ins = list(md.disasm(code, PATCH_BASE))
                self.assertEqual(len(ins) * 4, len(code))
                self.assertFalse([i for i in ins if re.search(r'\b[xw]18\b', i.op_str)], [f'{i.mnemonic} {i.op_str}' for i in ins])
                self.assertEqual(sum(i.mnemonic.startswith('casp') for i in ins), 1)   # the memory access is still one instruction
                self.assertEqual((ins[-1].mnemonic, ins[-1].operands[0].imm), ('b', SITE + 4))

    def test_other_casp_and_base_x18_are_not_this_site(self):
        for word in (casp(4, 6, 16), casp(4, 6, 18)):   # no pair at x18 (a base x18 is subst's: one register field)
            self.assertFalse(scope['casp_pair18'](scope['dis'](word, SITE)))


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class Run(unittest.TestCase):
    def run_case(self, word, mem, x18, regs):
        """The block on a machine whose guest x18 is in K18 and the hardware x18 holds junk; → (memory pair, K18, registers)"""
        assert uc is not None and arm is not None
        ins, code = block(word)
        mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        mu.ctl_set_cpu_model(arm.UC_CPU_ARM64_MAX)   # LSE atomics
        for base in (0, PATCH_BASE, KEY_PAGE, TSD, STACK, MEM): mu.mem_map(base, 0x10000)
        mu.mem_write(PATCH_BASE, code)
        mu.mem_write(KEY_PAGE + 8 * KEY_INDEX['K18'], struct.pack('<Q', K18))
        mu.mem_write(TSD + K18, struct.pack('<Q', x18))
        mu.mem_write(MEM, struct.pack('<QQ', *mem))
        mu.reg_write(arm.UC_ARM64_REG_TPIDRRO_EL0, TSD)
        mu.reg_write(arm.UC_ARM64_REG_SP, STACK + 0x8000)
        for r in range(31):
            if r != 18: mu.reg_write(arm.UC_ARM64_REG_X0 + r, regs.get(r, 0x1000 + r))
        mu.reg_write(arm.UC_ARM64_REG_X18, 0xdead)   # what the Apple platform register happens to hold: never read
        mu.emu_start(PATCH_BASE, SITE + 4)
        out = {r: mu.reg_read(arm.UC_ARM64_REG_X0 + r) for r in range(31) if r != 18}
        return struct.unpack('<QQ', bytes(mu.mem_read(MEM, 16))), struct.unpack('<Q', bytes(mu.mem_read(TSD + K18, 8)))[0], out

    def test_compare_pair_x18_swaps_and_fails(self):
        word = casp(18, 4, 16)
        new = {4: 0xaaaa, 5: 0xbbbb, 16: MEM}
        # equal: memory takes x4, x5; the compare pair keeps the old value (it is what was compared)
        mem, k18, r = self.run_case(word, (0x1111, 0x2222), 0x1111, {**new, 19: 0x2222})
        self.assertEqual((mem, k18, r[19]), ((0xaaaa, 0xbbbb), 0x1111, 0x2222))
        # unequal: memory unchanged, the compare pair (guest x18 in K18, x19) takes the memory's value
        mem, k18, r = self.run_case(word, (0x3333, 0x4444), 0x1111, {**new, 19: 0x2222})
        self.assertEqual((mem, k18, r[19]), ((0x3333, 0x4444), 0x3333, 0x4444))
        for n in (0, 1, 2, 3, 4, 5, 16): self.assertEqual(r[n], new.get(n, 0x1000 + n), f'x{n}')   # scratch registers restored

    def test_new_value_pair_x18(self):
        mem, k18, r = self.run_case(casp(4, 18, 16, acq=False, rel=False), (0x1111, 0x2222), 0xaaaa, {4: 0x1111, 5: 0x2222, 16: MEM, 19: 0xbbbb})
        self.assertEqual((mem, k18, r[19]), ((0xaaaa, 0xbbbb), 0xaaaa, 0xbbbb))

    def test_w_pair_zero_extends(self):
        # 32-bit pair: the old value is written to w18 / w19, clearing their upper halves as a w write does
        mem, k18, r = self.run_case(casp(18, 2, 0, x=False, rel=False), (0x0000000400000003, 0), 0xffffffff00000001, {0: MEM, 19: 0xffffffff00000002})
        self.assertEqual((k18, r[19]), (3, 4))


if __name__ == '__main__':
    unittest.main()
