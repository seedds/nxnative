"""Unity's ALLOC_BUCKET block limit (dm/tools/unity_bucket.py) on synthetic code, no game files: Unity 2019's constant 255 blocks of 4 MB cut to 63, Unity 2020's one block left,
Unity 2021's sizes in registers left with a plain reason, and the name referenced twice refused with a red one."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from nso_asm import ADRP, ADDI
import unity_bucket

TEXT, RO, DATA, END = 0x8000, 0x8000, 0xa000, 0xc000
NAME, CTOR = 0x8100, 0x6000


def movz(d, v, hw=0): return 0x52800000 | hw << 21 | v << 5 | d
def mov(d, s): return 0xAA0003E0 | s << 16 | d
def bl(frm, to): return 0x94000000 | (((to - frm) >> 2) & 0x3FFFFFF)


def setup(at, blocks=255, regs=False):
    """the name into x1, x0 = the object, w2 granularity 16, w3 8 buckets, w4 4 MB blocks, w5 the limit (regs: x2-x5 copied from x21-x24), bl the constructor"""
    w = [ADRP(1, at, NAME), ADDI(1, 1, NAME & 0xfff), mov(0, 20)]
    w += [mov(2, 21), mov(3, 22), mov(4, 23), mov(5, 24)] if regs else [movz(2, 16), movz(3, 8), movz(4, 0x40, 1), movz(5, blocks)]
    return w + [bl(at + 4 * len(w), CTOR), 0xD65F03C0]


def image(*seqs):
    img = bytearray(END)
    img[NAME - 1:NAME + 13] = b'\0ALLOC_BUCKET\0'
    struct.pack_into('<I', img, CTOR, 0xD65F03C0)
    at = 0x1000
    for s in seqs:
        words = s(at)
        struct.pack_into('<' + 'I' * len(words), img, at, *words); at += 4 * len(words) + 0x40
    return img


def find(img): return unity_bucket.find(bytes(img), TEXT, RO, DATA)


class FindTest(unittest.TestCase):
    def test_unity2019_cut(self):
        img = image(setup)
        cut, why = find(img)
        self.assertIsNotNone(cut, why)
        assert cut is not None
        self.assertEqual(cut[0], 0x1000 + 4 * 6)
        self.assertEqual((cut[1], cut[2]), ('e51f8052', 'e5078052'))   # mov w5, #255 → mov w5, #63: the registered cut of both Subnautica builds

    def test_unity2020_one_block(self):
        cut, why = find(image(lambda a: setup(a, blocks=1)))
        self.assertIsNone(cut)
        self.assertFalse(why.startswith('[RED] '), why)

    def test_sizes_at_run_time(self):
        cut, why = find(image(lambda a: setup(a, regs=True)))
        self.assertIsNone(cut)
        self.assertFalse(why.startswith('[RED] '), why)

    def test_two_references_is_red(self):
        cut, why = find(image(setup, setup))
        self.assertIsNone(cut)
        self.assertTrue(why.startswith('[RED] '), why)


if __name__ == '__main__':
    unittest.main()
