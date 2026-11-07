"""nso_asm.B(): a branch out of reach names its source, target and offset, so a layout that puts a site out of a stub's reach shows where."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import nso_asm


class BranchTests(unittest.TestCase):
    def test_in_reach(self):
        self.assertEqual(nso_asm.B(0x1000, 0x1008), 0x14000002)
        self.assertEqual(nso_asm.B(0x8000000, 0), 0x16000000)   # -128 MiB is the farthest back

    def test_beyond_reach_names_the_branch(self):
        with self.assertRaisesRegex(AssertionError, r'to=0x6f00578 frm=0xfc02320 off=-0x8d01da8 \(beyond ±128 MiB\)$'):
            nso_asm.B(0xfc02320, 0x6f00578)   # Starlink: Battle for Atlas, before the split kept to the back area's reach

    def test_misaligned(self):
        with self.assertRaisesRegex(AssertionError, r'off=0x6 \(misaligned\)$'): nso_asm.B(0, 6)


if __name__ == '__main__':
    unittest.main()
