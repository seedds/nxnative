#!/usr/bin/env python3
"""Ampere ALU instructions through the offline translator (dmsh_xlate, arch sm86). Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import os, pathlib, sys, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))   # its sibling test module, also when the file runs alone (python -m unittest <file>, as CI does)
import test_dmsh_ampere_mem as mem   # noqa: E402

# UP2UR UR4, UPR, URZ, 0x1 as Mario Kart World's DrivePass pixel shader 0877d576e4f92b8a encodes it (opcode 0x883); untranslated, the shader was skipped
UP2UR = (0x000fe20008000000, 0x000000013f047883)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class Up2ur(unittest.TestCase):
    translate = mem.Suatom.translate

    def test_up2ur_translates(self):
        msl = self.translate(mem.inst(*UP2UR) + mem.inst(*mem.EXIT))
        self.assertIn('main0', msl)


if __name__ == '__main__':
    unittest.main()
