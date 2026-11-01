#!/usr/bin/env python3
"""Ampere ALU instructions through the offline translator (dmsh_xlate, arch sm86). Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import os, unittest
import test_dmsh_ampere_mem as mem

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


if __name__ == '__main__':
    unittest.main()
