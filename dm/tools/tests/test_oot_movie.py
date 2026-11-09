"""Ocarina of Time's movie cut (patches.py MATCH_CUTS): the player's limit of pictures a calc becomes 2.

The player's constructor sets the limit to 1 + (its creation argument >= 2) with cmp w27,#2 … cinc w8,w28,ge (w28 = 1), and the
game passes 1: one picture a calc, so a 60-picture movie fell behind its sound whenever a frame took longer than a vsync. The cut
replaces the cinc with add w8,w28,#1, the limit the player itself takes for a larger argument.
"""
import importlib
import importlib.util
from pathlib import Path
import struct
import sys
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))

spec = importlib.util.spec_from_file_location('oot_patches', REPO / 'dm/games/0400486024DEC000/patches.py')
assert spec is not None and spec.loader is not None
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


def optional(name):
    try:
        return importlib.import_module(name)
    except ImportError:
        return None


capstone, unicorn, arm = optional('capstone'), optional('unicorn'), optional('unicorn.arm64_const')


def rule():
    rules = [r for r in profile.MATCH_CUTS['main'] if r['name'] == profile.movie]
    assert len(rules) == 1, rules
    return rules[0]


def limit(word, argument):
    """The limit the constructor's sequence gives: mov w28,#1; cmp w27,#2; the instruction under test"""
    assert unicorn is not None and arm is not None
    base = 0x10000
    m = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
    m.mem_map(base, 0x1000)
    m.mem_write(base, struct.pack('<3I', 0x5280003c, 0x71000b7f, word))   # mov w28,#1; cmp w27,#2
    m.reg_write(arm.UC_ARM64_REG_X27, argument)
    m.emu_start(base, base + 12)
    return m.reg_read(arm.UC_ARM64_REG_X8) & 0xffffffff


class MovieCut(unittest.TestCase):
    def test_one_instruction_inside_the_function(self):
        r = rule()
        (offset, old, new, _), = r['cuts']
        self.assertEqual((len(bytes.fromhex(old)), len(bytes.fromhex(new))), (4, 4))
        self.assertTrue(offset % 4 == 0 and 0 <= offset < r['size'])

    @unittest.skipIf(capstone is None, 'optional disassembler: capstone')
    def test_the_conditional_increment_becomes_an_add(self):
        assert capstone is not None
        md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        (_, old, new, _), = rule()['cuts']
        text = [[(i.mnemonic, i.op_str) for i in md.disasm(bytes.fromhex(h), 0)] for h in (old, new)]
        self.assertEqual(text, [[('cinc', 'w8, w28, ge')], [('add', 'w8, w28, #1')]])

    @unittest.skipIf(unicorn is None or arm is None, 'optional ARM64 execution dependency: unicorn')
    def test_the_limit_is_two_whatever_the_argument(self):
        (_, old, new, _), = rule()['cuts']
        old, new = (struct.unpack('<I', bytes.fromhex(h))[0] for h in (old, new))
        self.assertEqual([limit(old, a) for a in (1, 2, 3)], [1, 2, 2])   # the game passes 1
        self.assertEqual([limit(new, a) for a in (1, 2, 3)], [2, 2, 2])


if __name__ == '__main__':
    unittest.main()
