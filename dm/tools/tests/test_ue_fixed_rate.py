"""Unreal's UEngine::FixedFrameRate found by its reflection entry and GEngine by the code that reaches it (dm/tools/ue_fixed_rate.py), on synthetic images, no game files: the offset after
the array size (UE 4.22-5.0) and after a setter and a getter (UE 5.1+, DRAGON BALL: Sparking! ZERO), the name shared with bUseFixedFrameRate's tail, GEngine through its GOT slot, no code
reaching it (nothing, no red line), two globals (red), and an entry of another kind (not taken), and a load alone (no proof: Pikmin 4's coincides with another object's field)."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from nso_asm import ADRP, LDRX
import ue_fixed_rate

RET = 0xd65f03c0
TEXT, RO, DATA, END = 0x8000, 0x8000, 0xa000, 0xc000
MOD0, DYN, RELA, NAME, ENTRY, GOT, GOT2, GENGINE, OTHER, FIELD = 0x40, 0x9000, 0x9100, 0x8100, 0xa100, 0xa200, 0xa208, 0xa5d8, 0xa600, 0x9c8


def ldr_s(t, n, off): return 0xBD400000 | (off // 4) << 10 | n << 5 | t
def str_s(t, n, off): return 0xBD000000 | (off // 4) << 10 | n << 5 | t


def image(ue51=True, kind=ue_fixed_rate.FLOAT, users=(GOT,), store=True):
    """main with the name, its reflection entry and code that loads GEngine from each GOT slot of users and stores (or loads) a float at FIELD"""
    img = bytearray(END)
    struct.pack_into('<I', img, 4, MOD0); img[MOD0:MOD0 + 4] = b'MOD0'; struct.pack_into('<i', img, MOD0 + 4, DYN - MOD0)
    img[NAME:NAME + 19] = b'bUseFixedFrameRate\0'   # the linker shares "FixedFrameRate" with its tail: NAME + 4
    img[ENTRY + 24] = kind; struct.pack_into('<i', img, ENTRY + 32, 1)
    struct.pack_into('<I', img, ENTRY + (56 if ue51 else 36), FIELD)
    rel = [(ENTRY, NAME + 4), (GOT, GENGINE), (GOT2, OTHER)]   # R_AARCH64_RELATIVE: the entry's name, the GOT slots of GEngine and of another global
    struct.pack_into('<qQqQqQ', img, DYN, 7, RELA, 8, 24 * len(rel), 0, 0)
    for k, (at, v) in enumerate(rel): struct.pack_into('<QQq', img, RELA + 24 * k, at, 1027, v)
    at = 0x1000
    for slot in users:
        words = [ADRP(8, at, slot), LDRX(8, 8, (slot & 0xfff) // 8), LDRX(8, 8), (str_s if store else ldr_s)(0, 8, FIELD), RET]
        struct.pack_into('<' + 'I' * len(words), img, at, *words); at += 0x40
    return img


def find(img): return ue_fixed_rate.find(bytes(img), TEXT, RO, DATA)


class FindTest(unittest.TestCase):
    def test_ue51_layout(self):
        g, f, why = find(image())
        self.assertEqual((g, f), (GENGINE, FIELD), why)

    def test_ue4_layout(self):
        g, f, why = find(image(ue51=False))
        self.assertEqual((g, f), (GENGINE, FIELD), why)

    def test_a_load_alone_is_no_proof(self):   # Pikmin 4: a load at the offset through another object's global (a gravity of -980 there)
        g, f, why = find(image(store=False))
        self.assertIsNone(g)
        self.assertFalse(why.startswith('[RED] '), why)

    def test_no_code_reaches_it(self):   # Princess Peach: Showtime!: the fixed frame rate stays the config's, and that is no red line
        g, f, why = find(image(users=()))
        self.assertIsNone(g)
        self.assertFalse(why.startswith('[RED] '), why)

    def test_two_globals_are_red(self):
        g, f, why = find(image(users=(GOT, GOT2)))
        self.assertIsNone(g)
        self.assertTrue(why.startswith('[RED] ') and 'more than one global' in why, why)

    def test_an_entry_of_another_kind(self):   # bUseFixedFrameRate's own entry is a bool (0x0C)
        g, f, why = find(image(kind=0x0C))
        self.assertIsNone(g)
        self.assertEqual(why, 'no UEngine::FixedFrameRate')


if __name__ == '__main__':
    unittest.main()
