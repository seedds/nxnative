"""HAL Laboratory's frame interval and dynamic-resolution bounds (dm/tools/hal_fps.py), on synthetic code, no game files: Kirby and the Forgotten Land's layout (the frame manager's
switch on +0x60 of its object, the constructor clearing it and storing the object + 0x10 into a global, three heap roots in a row), no frame manager, a frame manager whose object no
constructor stores, a global something else also sets, two objects that qualify, and heap roots that are not three in a row."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from nso_asm import ADRP
import hal_fps

RET = 0xd65f03c0
TEXT, RO, DATA, END = 0x8000, 0x8000, 0xa000, 0xc000
GLOBAL, GLOBAL2, ROOTS, FIELD, IFACE = 0xa728, 0xa740, 0xa7a0, 0x60, 0x10


def ldr_w(t, n, off): return 0xB9400000 | (off // 4) << 10 | n << 5 | t
def str_w(t, n, off): return 0xB9000000 | (off // 4) << 10 | n << 5 | t
def str_x(t, n, off): return 0xF9000000 | (off // 8) << 10 | n << 5 | t
def ldr_x(t, n, off): return 0xF9400000 | (off // 8) << 10 | n << 5 | t
def addi(d, n, imm): return 0x91000000 | imm << 10 | n << 5 | d
def cmp_w(n, imm): return 0x7100001F | imm << 10 | n << 5
B_EQ, NOP = 0x54000040, 0xd503201f
def cbnz_w(t): return 0x35000040 | t


def manager(at, field=FIELD, sixty=True):
    """ldr w8, [x19, #F]; cmp w8, #2; b.eq; cmp w8, #1; b.eq; cbnz w8; then the 60 Hz vsync in nanoseconds (movz / movk w9, #0xfe502a)"""
    w = [ldr_w(8, 19, field), cmp_w(8, 2), B_EQ, cmp_w(8, 1), B_EQ, cbnz_w(8), NOP]
    return w + ([0x52800000 | 0x502a << 5 | 9, 0x72A00000 | 0xfe << 5 | 9] if sixty else []) + [RET]


def constructor(g=GLOBAL, field=FIELD, iface=IFACE):
    """add x10, x0, #D; adrp x11, global; str wzr, [x0, #F]; str x10, [x11, #lo]"""
    return lambda at: [addi(10, 0, iface), ADRP(11, at + 4, g), str_w(31, 0, field), str_x(10, 11, g & 0xfff), RET]


def clear(g=GLOBAL, value=31): return lambda at: [ADRP(8, at, g), str_x(value, 8, g & 0xfff), RET]   # str xzr (or x1), [x8, #lo]


def root(g): return lambda at: [ADRP(8, at, g), ldr_x(20, 8, g & 0xfff)] + list(struct.unpack('<3I', hal_fps.ROOT_TAIL)) + [RET]


def image(*seqs):
    img = bytearray(END)
    at = 0x1000
    for s in seqs:
        words = s(at)
        struct.pack_into('<' + 'I' * len(words), img, at, *words)
        at += 4 * len(words) + 0x20
    return img


def find(img): return hal_fps.find(bytes(img), TEXT, RO, DATA)


KIRBY = (manager, constructor(), clear(), root(ROOTS), root(ROOTS + 8), root(ROOTS + 16))


class FindTest(unittest.TestCase):
    def test_kirby(self):
        out, why = find(image(*KIRBY))
        self.assertEqual(out['frame'], (GLOBAL, FIELD - IFACE), why)
        self.assertEqual(out['dr'], (ROOTS + 16, hal_fps.HOPS, hal_fps.DR_FIELD), why)
        self.assertNotIn('[RED]', why)

    def test_nothing(self):
        out, why = find(image(constructor(), lambda a: manager(a, sixty=False)))
        self.assertEqual(out, {}, why)
        self.assertNotIn('[RED]', why)

    def test_no_constructor_fails_closed(self):
        out, why = find(image(manager, constructor(field=0x64)))
        self.assertNotIn('frame', out)
        self.assertIn('[RED] frame interval', why)

    def test_global_set_elsewhere(self):
        out, why = find(image(manager, constructor(), clear(value=1)))   # str x1, [global]: not only this object
        self.assertNotIn('frame', out, why)

    def test_two_objects_fail_closed(self):
        out, why = find(image(manager, constructor(), constructor(g=GLOBAL2)))
        self.assertNotIn('frame', out)
        self.assertIn('[RED] frame interval', why)

    def test_roots_not_three_in_a_row(self):
        out, why = find(image(*KIRBY[:3], root(ROOTS), root(ROOTS + 8), root(ROOTS + 24)))
        self.assertNotIn('dr', out)
        self.assertIn('[RED] dynamic-resolution bounds', why)
        self.assertEqual(out['frame'], (GLOBAL, FIELD - IFACE))


if __name__ == '__main__':
    unittest.main()
