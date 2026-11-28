"""The stack-free CSEL rewrite must preserve the selected value, flags and guest stack."""
import ast
import struct
import unittest

from dm.tools.tests import test_x18_sysreg as slots

SITE = slots.SITE
PATCH = slots.PATCH_BASE
KEY = slots.KEY_PAGE
TSD = slots.TSD
K18 = slots.SLOT['K18']
scope = dict(slots.scope, patch=bytearray(), fix=[])
functions = {'select_x18', 'select_scratch', 'site_select'}
constants = {'SELECT_READS', 'SELECT_WRITES'}
nodes: list[ast.stmt] = [
    n for n in slots.tree.body if isinstance(n, ast.FunctionDef) and n.name in functions
    or isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id in constants for t in n.targets)
]
# The emitter's existing helpers use their module globals, including the test patch buffer.
helpers = {'here', 'emit', 'emit_key_addr', 'emit_slot_addr'}
nodes.extend(n for n in slots.tree.body if isinstance(n, ast.FunctionDef) and n.name in helpers)
exec(compile(ast.Module(body=nodes, type_ignores=[]), str(slots.TOOLS / 'patch_nso.py'), 'exec'), scope)


def select(rd, rn, rm, wide):
    # csel Rd,Rn,Rm,gt
    return 0x1A80C000 | (wide << 31) | (rm << 16) | (rn << 5) | rd


class ScratchProof(unittest.TestCase):
    def scratch(self, code):
        start = SITE + 4
        ins = {start + 4 * n: scope['dis'](word, start + 4 * n) for n, word in enumerate(code)}
        return scope['select_scratch'](SITE, ins, start + len(code) * 4, 11)

    def test_write_on_both_paths(self):
        branch = slots.nso_asm.BEQ(SITE + 4, SITE + 16)
        escape = slots.nso_asm.B(SITE + 12, SITE + 0x100)
        kill = slots.nso_asm.MOVX(9, 0)
        self.assertEqual(self.scratch([branch, kill, escape, kill]), 9)
        self.assertIsNone(self.scratch([branch, kill, escape, slots.nso_asm.ADDI(9, 9, 1)]))

    def test_load_address_is_read_before_overwrite(self):
        self.assertIsNone(self.scratch([slots.nso_asm.LDRX(9, 9)]))

    def test_call_may_read_any_unproven_register(self):
        self.assertIsNone(self.scratch([slots.nso_asm.BL(SITE + 4, SITE + 0x100), 0xD65F03C0]))


@unittest.skipIf(slots.uc is None, 'optional ARM64 execution dependency: unicorn')
class Run(unittest.TestCase):
    def test_select_with_unmapped_stack(self):
        uc, arm = slots.uc, slots.arm
        for wide in (False, True):
            for rn, rm in ((11, 18), (18, 12), (18, 31)):
                for gt, nzcv in ((True, 0), (False, 1 << 30)):
                    with self.subTest(wide=wide, rn=rn, rm=rm, gt=gt):
                        word = select(11, rn, rm, wide)
                        ins = scope['dis'](word, SITE)
                        scope['patch'].clear()
                        scope['fix'].clear()
                        scope['site_select'](ins, scope['select_x18'](ins), 9)
                        mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
                        for base in (0, PATCH, KEY, TSD): mu.mem_map(base, 0x10000)
                        mu.mem_write(PATCH, bytes(scope['patch']))
                        mu.mem_write(KEY, struct.pack('<Q', K18))
                        value18 = 0xF00D00005678ABCD
                        mu.mem_write(TSD + K18, struct.pack('<Q', value18))
                        mu.reg_write(arm.UC_ARM64_REG_TPIDRRO_EL0, TSD)
                        mu.reg_write(arm.UC_ARM64_REG_SP, 0x10000000)
                        mu.reg_write(arm.UC_ARM64_REG_NZCV, nzcv)
                        values = {9: 9, 11: 0xBAAC000012345678, 12: 0xCDEF0000CAFE0123, 18: 0xDEAD}
                        for reg, value in values.items(): mu.reg_write(arm.UC_ARM64_REG_X0 + reg, value)
                        mu.emu_start(PATCH, SITE + 4)
                        chosen = rn if gt else rm
                        expected = value18 if chosen == 18 else values.get(chosen, 0)
                        self.assertEqual(mu.reg_read(arm.UC_ARM64_REG_X11), expected if wide else expected & 0xFFFFFFFF)
                        self.assertEqual(mu.reg_read(arm.UC_ARM64_REG_SP), 0x10000000)
                        self.assertEqual(mu.reg_read(arm.UC_ARM64_REG_NZCV), nzcv)
                        self.assertEqual(mu.reg_read(arm.UC_ARM64_REG_X18), values[18])
                        self.assertEqual(mu.mem_read(TSD + K18, 8), struct.pack('<Q', value18))
                        if chosen != 18: self.assertEqual(mu.reg_read(arm.UC_ARM64_REG_X9), values[9])


if __name__ == '__main__':
    unittest.main()
