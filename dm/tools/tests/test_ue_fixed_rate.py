"""Unreal's UEngine::FixedFrameRate and bUseFixedFrameRate found by their reflection entries and GEngine where FEngineLoop::Init makes the engine (dm/tools/ue_fixed_rate.py), on
synthetic images, no game files: the offset after the array size (UE 4.22-5.0, Pikmin 4) and after a setter and a getter (UE 5.1+, DRAGON BALL: Sparking! ZERO), the name shared with
bUseFixedFrameRate's tail, the flag from its set-bit function, GEngine as a page plus offset (Pikmin 4) and through a GOT slot (Sparking! ZERO, the key the tail of "UGameEngine"), a store
with no reload or virtual call after it (not taken), two creations naming different globals (red), and an entry of another kind (not taken)."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from nso_asm import ADRP, ADDI, LDRX, STRX
import ue_fixed_rate

RET, NOP = 0xd65f03c0, 0xd503201f
TEXT, RO, DATA, END = 0x8000, 0x8000, 0xa000, 0xc000
MOD0, DYN, RELA, NAME, KEY, ENTRY, BOOL, GOT, GENGINE, OTHER, FIELD, SETBIT = 0x40, 0x9000, 0x9100, 0x8100, 0x8200, 0xa100, 0xa180, 0xa200, 0xa5d8, 0xa608, 0x9c8, 0x3000


def blr(n): return 0xD63F0000 | n << 5
def creation(at, base, got, lo=0):
    """FEngineLoop::Init in miniature at word offset at: the key's address into x2, a call, the new object stored at [x25, #lo] (x25 the page of base, or the address its GOT slot
    holds), a call, the reload into x0 and a virtual call"""
    load = [ADRP(25, at, GOT), LDRX(25, 25, (GOT & 0xfff) // 8)] if got else [ADRP(25, at, base)]
    a = at + 4 * len(load)
    return load + [ADRP(2, a, KEY), ADDI(2, 2, KEY & 0xfff), 0x94000000, NOP, STRX(0, 25, lo // 8), 0x94000000,
                   LDRX(0, 25, lo // 8), LDRX(8, 0), LDRX(8, 8, 0x298 // 8), blr(8), RET]


def image(ue51=True, kind=ue_fixed_rate.FLOAT, creations=((GENGINE, False),), reload=True, tail=False):
    img = bytearray(END)
    struct.pack_into('<I', img, 4, MOD0); img[MOD0:MOD0 + 4] = b'MOD0'; struct.pack_into('<i', img, MOD0 + 4, DYN - MOD0)
    img[NAME:NAME + 19] = b'bUseFixedFrameRate\0'   # "FixedFrameRate" is its tail: NAME + 4
    key = ('UGameEngine' if tail else 'GameEngine').encode('utf-16-le') + b'\0\0'
    k0 = KEY - (2 if tail else 0); img[k0:k0 + len(key)] = key   # Sparking! ZERO: the key is the tail of "UGameEngine"
    img[ENTRY + 24] = kind; struct.pack_into('<i', img, ENTRY + 32, 1); struct.pack_into('<I', img, ENTRY + (56 if ue51 else 36), FIELD)
    img[BOOL + 24] = 0x0C; struct.pack_into('<i', img, BOOL + 32, 1)
    struct.pack_into('<4I', img, SETBIT, 0x39400000 | 0x9c4 << 10 | 8, 0x32000000 | 26 << 16 | 8 << 5 | 8, 0x39000000 | 0x9c4 << 10 | 8, RET)   # ldrb / orr #0x40 / strb / ret
    rel = [(ENTRY, NAME + 4), (BOOL, NAME), (BOOL + 0x48, SETBIT), (GOT, GENGINE)]   # R_AARCH64_RELATIVE: names, the set-bit function, GEngine's GOT slot
    struct.pack_into('<qQqQqQ', img, DYN, 7, RELA, 8, 24 * len(rel), 0, 0)
    for k, (at, v) in enumerate(rel): struct.pack_into('<QQq', img, RELA + 24 * k, at, 1027, v)
    at = 0x1000
    for g, got in creations:
        words = creation(at, g & ~0xfff, got, 0 if got else g & 0xfff)
        if not reload: words[-5] = NOP   # no load of GEngine back into x0
        struct.pack_into('<' + 'I' * len(words), img, at, *words); at += 0x100
    return img


def find(img): return ue_fixed_rate.find(bytes(img), TEXT, RO, DATA)


class FindTest(unittest.TestCase):
    def test_ue51_layout_through_the_got_slot(self):   # Sparking! ZERO
        g, f, fl, why = find(image(creations=((GENGINE, True),), tail=True))
        self.assertEqual((g, f, fl), (GENGINE, FIELD, 0x9c4 << 8 | 0x40), why)

    def test_ue4_layout_page_and_offset(self):   # Pikmin 4: str x0, [x19, #0x880] with x19 the page
        g, f, fl, why = find(image(ue51=False))
        self.assertEqual((g, f, fl), (GENGINE, FIELD, 0x9c4 << 8 | 0x40), why)

    def test_two_creations_naming_one_global(self):
        g, f, fl, why = find(image(creations=((GENGINE, False), (GENGINE, True))))
        self.assertEqual(g, GENGINE, why)

    def test_a_store_without_its_reload_is_no_creation(self):
        g, f, fl, why = find(image(reload=False))
        self.assertIsNone(g)
        self.assertFalse(why.startswith('[RED] '), why)

    def test_creations_naming_two_globals_are_red(self):
        g, f, fl, why = find(image(creations=((GENGINE, False), (OTHER, False))))
        self.assertIsNone(g)
        self.assertTrue(why.startswith('[RED] ') and 'more than one global' in why, why)

    def test_an_entry_of_another_kind(self):   # bUseFixedFrameRate's own entry is a bool (0x0C)
        g, f, fl, why = find(image(kind=0x0C))
        self.assertIsNone(g)
        self.assertEqual(why, 'no UEngine::FixedFrameRate')


if __name__ == '__main__':
    unittest.main()
