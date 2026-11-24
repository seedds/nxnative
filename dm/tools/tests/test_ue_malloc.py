"""Unreal's FMallocBinned3 found without a build ID (dm/tools/ue_malloc.py) on synthetic images, no game files: the name, its getter, the vtable (in place or through
relative relocations), the constructor and its calls; a broken link changes nothing, with a [RED] reason once the name is there."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from nso_asm import ADRP, ADDI, B, NOP, STRX
import ue_malloc

RET = 0xd65f03c0
TEXT, RO, DATA, END = 0x8000, 0x8000, 0xa000, 0xc000
NAME, VT, STUB = 0x8100, 0xa110, 0x1500   # the string, the vtable's address point, a trivial slot
GETTER, DTOR0, DTOR1, CTOR, CALLER = 0x1000, 0x1100, 0x1200, 0x1300, 0x2000


def bl(frm, to): return B(frm, to) | 0x80000000
def put(img, at, *words): struct.pack_into('<' + 'I' * len(words), img, at, *words)
def store_vt(at): return [ADRP(8, at, VT), ADDI(8, 8, VT & 0xfff), STRX(8, 0), RET]   # the vtable into [x0]


def image(slot=19, calls=1, relocated=False, name=True):
    img = bytearray(END)
    if name: img[NAME:NAME + 16] = 'Binned3'.encode('utf-16-le') + b'\0\0'
    put(img, GETTER, ADRP(0, GETTER, NAME), ADDI(0, 0, NAME & 0xfff), RET)
    for f in (DTOR0, DTOR1, CTOR): put(img, f, *store_vt(f))
    put(img, STUB, RET)
    for k in range(calls): put(img, CALLER + 8 * k, NOP, bl(CALLER + 8 * k + 4, CTOR))
    slots = [DTOR0, DTOR1] + [STUB] * (slot - 2) + [GETTER] + [STUB] * 3
    if not relocated:
        for k, v in enumerate(slots): struct.pack_into('<Q', img, VT + 8 * k, v)
        return img
    # The vtable's words are 0 in place: R_AARCH64_RELATIVE relocations (RELA, in ro as the linker puts them) give them
    struct.pack_into('<I', img, 4, 0x20); img[0x20:0x24] = b'MOD0'; struct.pack_into('<i', img, 0x24, 0xb800 - 0x20)
    rela = 0x9000
    for k, v in enumerate(slots): struct.pack_into('<QQq', img, rela + 24 * k, VT + 8 * k, 1027, v)
    for k, (tag, v) in enumerate(((7, rela), (8, 24 * len(slots)), (0, 0))): struct.pack_into('<qQ', img, 0xb800 + 16 * k, tag, v)
    return img


def find(img): return ue_malloc.find(bytes(img), TEXT, RO, DATA)


class FindTest(unittest.TestCase):
    def test_found_in_place(self):
        for getter in (17, 19):
            with self.subTest(getter=getter):
                call, vt, slot, why = find(image(slot=getter))
                self.assertEqual((call, vt, slot), ((CALLER + 4,), VT, getter), why)

    def test_found_through_relocations(self):
        call, vt, slot, why = find(image(slot=26, relocated=True))
        self.assertEqual((call, vt, slot), ((CALLER + 4,), VT, 26), why)

    def test_no_name_is_not_red(self):
        call, _, _, why = find(image(name=False))
        self.assertIsNone(call)
        self.assertFalse(why.startswith('[RED] '), why)

    def test_unknown_layout_is_red(self):
        call, _, _, why = find(image(slot=20))
        self.assertIsNone(call)
        self.assertTrue(why.startswith('[RED] ') and 'slot 20' in why, why)

    def test_every_call_is_cut(self):   # DRAGON BALL Sparking! ZERO: one lazy construction inlined twice
        call, vt, slot, why = find(image(calls=2))
        self.assertEqual((call, vt, slot), ((CALLER + 4, CALLER + 12), VT, 19), why)

    def test_a_jump_to_the_constructor_is_red(self):   # a tail call: a nop there would fall through
        img = image(); put(img, CALLER + 16, B(CALLER + 16, CTOR))
        call, _, _, why = find(img)
        self.assertIsNone(call)
        self.assertTrue(why.startswith('[RED] ') and '1 calls and 1 jumps' in why, why)


TI, TI_NAME, OTHER_NAME = 0xa800, 0x8200, 0x8240   # the type_info object (data), its name and another class's (ro)


def image_rtti(name=None, ti=TI, name_at=TI_NAME, relocated=False, **kw):
    img = image(relocated=relocated, **kw)
    img[TI_NAME:TI_NAME + 17] = b'14FMallocBinned3\0'
    img[OTHER_NAME:OTHER_NAME + 9] = b'7FMalloc\0'
    if name is not None: img[name_at:name_at + len(name)] = name
    # type_info: its own vtable pointer (an import, 0 in place), the name, the base class's type_info
    if relocated:   # the two pointers are 0 in place and come from R_AARCH64_RELATIVE entries after the vtable's
        rela = 0x9000; n = struct.unpack_from('<Q', img, 0xb800 + 16 + 8)[0] // 24
        for k, (at, v) in enumerate(((VT - 8, ti), (ti + 8, name_at))): struct.pack_into('<QQq', img, rela + 24 * (n + k), at, 1027, v)
        struct.pack_into('<qQ', img, 0xb800 + 16, 8, 24 * (n + 2))
    else:
        struct.pack_into('<Q', img, VT - 8, ti)
        struct.pack_into('<Q', img, ti + 8, name_at)
    return img


# A build compiled with RTTI (CAPTAIN TSUBASA 2: WORLD FIGHTERS 1.0.6, GitHub issue #3036): the words before the address point are (0, the class's type_info), not (0, 0)
class RttiTest(unittest.TestCase):
    def test_a_vtable_with_its_class_rtti_is_found(self):
        for slot in (19, 26):
            for relocated in (False, True):
                with self.subTest(slot=slot, relocated=relocated):
                    call, vt, got, why = find(image_rtti(slot=slot, relocated=relocated))
                    self.assertEqual((call, vt, got), ((CALLER + 4,), VT, slot), why)

    def test_another_class_rtti_is_red(self):   # the word before the slots names another class: not this vtable's header
        call, _, _, why = find(image_rtti(name_at=OTHER_NAME))
        self.assertIsNone(call)
        self.assertTrue(why.startswith('[RED] '), why)

    def test_a_longer_class_name_is_red(self):   # "14FMallocBinned3" must end there (FMallocBinned3Extra would be 19...)
        call, _, _, why = find(image_rtti(name=b'14FMallocBinned3x\0'))
        self.assertIsNone(call)
        self.assertTrue(why.startswith('[RED] '), why)

    def test_a_pointer_outside_the_data_is_red(self):   # a code or ro address, an odd one, one past the image
        for ti in (0x1500, 0x8300, TI + 4, END - 8):
            with self.subTest(ti=hex(ti)):
                img = image_rtti(); struct.pack_into('<Q', img, VT - 8, ti)
                call, _, _, why = find(img)
                self.assertIsNone(call)
                self.assertTrue(why.startswith('[RED] '), why)

    def test_a_nonzero_offset_to_top_is_red(self):
        img = image_rtti(); struct.pack_into('<q', img, VT - 16, -16)
        call, _, _, why = find(img)
        self.assertIsNone(call)
        self.assertTrue(why.startswith('[RED] '), why)

    def test_no_rtti_still_found(self):
        call, vt, got, why = find(image(slot=22))
        self.assertEqual((call, vt, got), ((CALLER + 4,), VT, 22), why)


if __name__ == '__main__':
    unittest.main()
