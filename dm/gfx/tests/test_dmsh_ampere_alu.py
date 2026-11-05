#!/usr/bin/env python3
"""Ampere ALU instructions through the offline translator (dmsh_xlate, arch sm86). Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import os, pathlib, sys, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))   # its sibling test module, also when the file runs alone (python -m unittest <file>, as CI does)
import test_dmsh_ampere_mem as mem   # noqa: E402

# UBREV UR4, UR4 as Mario Kart World's DrivePass compute shader 0f682a395b9c31aa encodes it (opcode 0x2be); untranslated, its dispatches were skipped
UBREV = (0x000fc80008000000, 0x00000004000472be)
# UP2UR UR4, UPR, URZ, 0x1 as Mario Kart World's DrivePass pixel shader 0877d576e4f92b8a encodes it (opcode 0x883); untranslated, the shader was skipped
UP2UR = (0x000fe20008000000, 0x000000013f047883)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class Brev(unittest.TestCase):
    translate = mem.Suatom.translate

    def test_ubrev_translates(self):
        msl = self.translate(mem.inst(*UBREV) + mem.inst(*mem.EXIT))
        self.assertIn('main0', msl)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class Up2ur(unittest.TestCase):
    translate = mem.Suatom.translate

    def test_up2ur_translates(self):
        msl = self.translate(mem.inst(*UP2UR) + mem.inst(*mem.EXIT))
        self.assertIn('main0', msl)


class HalfSecond(mem.Translated):
    """Two-source half ops use the operand at bit 32, including constants.

    PR #981 identified the immediate form. MKW 1.8.0's 30b9551d6d1e8638
    and 86cacbfbef07a0fc also use the constant-buffer form (0x630).
    A shared store keeps the result live; R0 deliberately remains undefined.
    """
    def kept(self, opcode, slot32):
        tid = mem.inst(0x000e260000002100, 0x0000000000067919)
        half = mem.inst(0x03ffe20000000800, slot32 << 32 | 6 << 24 | 6 << 16 | 0x7000 | opcode)
        store = mem.inst(0x03ffe80000000800, 6 << 32 | 255 << 24 | 0x7388)
        return self.translate(tid + half + store + mem.inst(*mem.EXIT), shared=256)

    def test_immediate_is_used_by_add_and_multiply(self):
        for opcode in (0x430, 0x432):
            with self.subTest(opcode=opcode):
                self.assertRegex(self.kept(opcode, 0x40004000), r'\b2\.0\b')

    def test_constant_buffer_second_operand_is_read(self):
        for opcode in (0x630, 0x632):
            with self.subTest(opcode=opcode):
                self.assertIn('cp_c3', self.kept(opcode, 3 << 22 | 0x20))


if __name__ == '__main__':
    unittest.main()
