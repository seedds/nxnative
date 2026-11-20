"""Unreal's NVN dynamic buffers keep 4 copies, found without a build ID (dm/tools/ue_ring.py) on synthetic images, no game files: the variable's name, its registration with the value 3,
the lock's rotation over the slots, and the constructor's load of the count in the forms the Unreal mains have. A broken link changes nothing, with a [RED] reason once the name is there."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from nso_asm import ADRP, ADDI
import ue_ring

TEXT, RO, DATA, END = 0x8000, 0x8000, 0xa000, 0xc000
NAME, VAR, REG, LOCK, CTOR = 0x8400, 0xa218, 0x1000, 0x2000, 0x3000
STUR_Q18, LOC_XZR = 0x3c818000, 0xf8028c1f | 24 << 5   # stur q0, [x0, #0x18]; str xzr, [x24, #0x28]!
LDR_W8_X8 = 0xb9400108   # ldr w8, [x8]
STR_W8_X20_5C = 0xb9005e88   # str w8, [x20, #0x5c]
TBZ_W21_1 = 0x36080015 | 3 << 5   # tbz w21, #1, +12


def put(img, at, *words): struct.pack_into('<' + 'I' * len(words), img, at, *words)


def ldr_w(rt, rn, addr): return 0xb9400000 | ((addr & 0xfff) // 4) << 10 | rn << 5 | rt
def ldr_x(rt, rn, addr): return 0xf9400000 | ((addr & 0xfff) // 8) << 10 | rn << 5 | rt


def image(value=3, name=True, registered=True, rotation=True, layout=True):
    img = bytearray(END)
    if name: img[NAME:NAME + 44] = (ue_ring.NAME + '\0').encode('utf-16le')
    put(img, VAR, value)
    if registered: put(img, REG, ADRP(1, REG, NAME), ADDI(1, 1, NAME & 0xfff), ADRP(2, REG + 8, VAR), ADDI(2, 2, VAR & 0xfff))
    if rotation:   # ldr w8, [x21, #0x5c]; cmp w8, #2; b.lo; ldr w9, [x21, #0x60]; add w9, w9, #1; udiv w10, w9, w8; msub w8, w10, w8, w9; str w8, [x21, #0x60]; add x8, x21, w8, uxtw #3; ldr x8, [x8, #8]
        put(img, LOCK, 0xb9405ea8, 0x7100091f, 0x54000103, 0xb94062a9, 0x11000529, 0x1ac8092a, 0x1b08a548, 0xb90062a8, 0x8b284ea8, 0xf9400508)
    if layout: put(img, CTOR, STUR_Q18, LOC_XZR)
    return img


def direct(img):
    """Unreal Engine 4.27: tbz; adrp x8; ldr w8, [x8, #lo]; str w8, [x20, #0x5c]"""
    a = CTOR + 0x40
    put(img, a, TBZ_W21_1, ADRP(8, a + 4, VAR), ldr_w(8, 8, VAR), STR_W8_X20_5C)
    return a + 8


class Ring(unittest.TestCase):
    def test_direct_load(self):
        img = image(); load = direct(img)
        off, old, new, why = ue_ring.find(bytes(img), TEXT, RO, DATA)
        self.assertEqual((off, old, new), (load, ldr_w(8, 8, VAR), 0x52800088), why)   # mov w8, #4

    def test_load_through_the_got(self):   # OCTOPATH TRAVELER II
        img = image(); a = CTOR + 0x40; got = DATA + 0x800
        put(img, a, TBZ_W21_1, ADRP(8, a + 4, got), ldr_x(8, 8, got), LDR_W8_X8, STR_W8_X20_5C)
        import loader_census
        keep = loader_census.got_values
        loader_census.got_values = lambda image: {got: VAR}
        try: off, old, new, why = ue_ring.find(bytes(img), TEXT, RO, DATA)
        finally: loader_census.got_values = keep
        self.assertEqual((off, new), (a + 12, 0x52800088), why)

    def test_store_through_a_base_made_earlier(self):   # Pikmin 4: str xzr, [x8, #0x5c]! in the constructor, then str w10, [x8]
        img = image(); a = CTOR + 0x40
        put(img, CTOR + 8, 0xf805cc1f | 8 << 5)
        put(img, a, TBZ_W21_1, ADRP(10, a + 4, VAR), ldr_w(10, 10, VAR), 0xb900010a)
        off, _, new, why = ue_ring.find(bytes(img), TEXT, RO, DATA)
        self.assertEqual((off, new), (a + 8, 0x5280008a), why)   # mov w10, #4

    def test_store_through_a_base_not_at_the_count(self):
        img = image(); a = CTOR + 0x40
        put(img, a, TBZ_W21_1, ADRP(10, a + 4, VAR), ldr_w(10, 10, VAR), 0xb900010a)
        off, _, _, why = ue_ring.find(bytes(img), TEXT, RO, DATA)
        self.assertIsNone(off); self.assertTrue(why.startswith('[RED] '), why)

    def test_no_name_is_no_ring_and_no_red(self):
        img = image(name=False); direct(img)
        off, _, _, why = ue_ring.find(bytes(img), TEXT, RO, DATA)
        self.assertIsNone(off); self.assertFalse(why.startswith('[RED] '), why)

    def test_another_default_is_no_ring_and_no_red(self):   # Unreal Engine 5
        img = image(value=2); direct(img)
        off, _, _, why = ue_ring.find(bytes(img), TEXT, RO, DATA)
        self.assertIsNone(off); self.assertFalse(why.startswith('[RED] '), why)

    def test_broken_links_change_nothing_and_say_so(self):
        for kw in ({'registered': False}, {'rotation': False}, {'layout': False}):
            img = image(**kw); direct(img)
            off, _, _, why = ue_ring.find(bytes(img), TEXT, RO, DATA)
            self.assertIsNone(off, kw); self.assertTrue(why.startswith('[RED] '), (kw, why))

    def test_two_loads_change_nothing(self):
        img = image(); direct(img)
        a = CTOR + 0x80
        put(img, a, TBZ_W21_1, ADRP(8, a + 4, VAR), ldr_w(8, 8, VAR), STR_W8_X20_5C)
        off, _, _, why = ue_ring.find(bytes(img), TEXT, RO, DATA)
        self.assertIsNone(off); self.assertTrue(why.startswith('[RED] '), why)

    def test_a_load_of_another_variable_is_no_load(self):
        img = image(); a = CTOR + 0x40
        put(img, a, TBZ_W21_1, ADRP(8, a + 4, VAR + 8), ldr_w(8, 8, VAR + 8), STR_W8_X20_5C)
        off, _, _, why = ue_ring.find(bytes(img), TEXT, RO, DATA)
        self.assertIsNone(off); self.assertTrue(why.startswith('[RED] '), why)


if __name__ == '__main__':
    unittest.main()
