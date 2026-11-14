"""Unreal console variables found by their registered names (dm/tools/ue_cvar.py) on synthetic code, no game files: UE 4.18's constructor form with the object loaded before the
name, UE 4.2x / 5's register call with the value pointer stored after it (and a base constructor between), a lookup that only reads the name, a name registered twice, UTF-32 names,
the next variable's registration not taken for this one's; and every profile's ue_fps_cvar is a name the rewriter looks for."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'games'))
from nso_asm import ADRP, ADDI, B, BLR, LDRX, STRX
import ue_cvar
import game

RET = 0xd65f03c0
TEXT, RO, DATA, END = 0x8000, 0x8000, 0xa000, 0xc000
NAME, OTHER, OBJ, OBJ2, CTOR = 0x8100, 0x8200, 0xa5c8, 0xa700, 0x6000


def bl(frm, to): return B(frm, to) | 0x80000000
def mov(d, s): return 0xAA0003E0 | s << 16 | d   # mov xD, xS


def put_name(img, at, s, z=2): img[at:at + (len(s) + 1) * z] = s.encode('utf-16-le' if z == 2 else 'utf-32-le') + b'\0' * z


def ctor_form(at, name, obj, nxt=None):
    """UE 4.18: the object into x19, then the name into x1, x0 = x19, bl the constructor; then (nxt) the next variable registered inline, whose value pointer goes into another object"""
    w = [ADRP(19, at, obj), ADDI(19, 19, obj & 0xfff), ADRP(1, at + 8, name), ADDI(1, 1, name & 0xfff), mov(0, 19), 0]
    w[5] = bl(at + 20, CTOR)
    if nxt:
        a = at + 4 * len(w)
        w += [LDRX(8, 0), ADRP(1, a + 4, nxt), ADDI(1, 1, nxt & 0xfff), BLR(8), ADRP(19, a + 16, OBJ2), ADDI(19, 19, OBJ2 & 0xfff), LDRX(8, 0), BLR(8), STRX(0, 19, 2)]
    return w + [RET]


def register_form(at, name, obj, store=True, base_ctor=False):
    """UE 4.2x / 5: the name into x1, blr (register); the object into x19, (a base constructor with x0 = the object,) blr (AsVariable), str x0, [x19, #0x10]; without store: a lookup"""
    w = [LDRX(8, 0), ADRP(1, at + 4, name), ADDI(1, 1, name & 0xfff), BLR(8)]
    if not store: return w + [STRX(0, 20, 0), RET]   # the value read into another word
    a = at + 4 * len(w)
    w += [ADRP(19, a, obj), ADDI(19, 19, obj & 0xfff)]
    if base_ctor: w += [mov(1, 0), mov(0, 19), bl(at + 4 * (len(w) + 2), CTOR)]
    return w + [LDRX(8, 0), BLR(8), STRX(0, 19, 2), RET]


BASE = 0xa100   # the inlined form's page base (adrp x22 + add x22, x22, #0x100); OBJ is x22 + 0x4c8


def inline_form(at, name, obj, vtable=True, clobber=False, far=False):
    """UE 4.18 inlined (OCTOPATH TRAVELER): x22 = the page base, made once, then an if / else block (a forward b over the else block) and other work, then the name into x1, blr
    (register), str x0, [x22, #k + 8] (the registered variable), str x8, [x22, #k] (the vtable), blr (the value pointer), str x0, [x22, #k + 0x10]. clobber writes x22 between;
    far ends the else block with a b past the store"""
    k = obj - BASE
    w = [ADRP(22, at, BASE), ADDI(22, 22, BASE & 0xfff), 0, mov(0, 1), mov(2, 3)]
    w += [mov(22, 0)] if clobber else [mov(4, 5)]
    w += [LDRX(8, 0), ADRP(1, at + 4 * (len(w) + 1), name), ADDI(1, 1, name & 0xfff), BLR(8), STRX(0, 22, (k + 8) // 8)]
    if vtable: w += [STRX(8, 22, k // 8)]
    w += [LDRX(8, 0), BLR(8), STRX(0, 22, (k + 0x10) // 8), RET]
    w[2] = B(at + 8, at + 4 * (len(w) + 4 if far else 4))   # skips the else block (one word), or jumps past the store
    return w


def image(*seqs, z=2):
    img = bytearray(END)
    put_name(img, NAME, 't.MaxFPS', z); put_name(img, OTHER, 'r.Other', z)
    struct.pack_into('<I', img, CTOR, RET)
    at = 0x1000
    for s in seqs:
        words = s(at)
        struct.pack_into('<' + 'I' * len(words), img, at, *words)
        at += 4 * len(words) + 0x40
    return img


def find(img, name='t.MaxFPS'): return ue_cvar.find(bytes(img), TEXT, RO, DATA, name)


class FindTest(unittest.TestCase):
    def test_constructor_form(self):
        o, why = find(image(lambda a: ctor_form(a, NAME, OBJ, nxt=OTHER)))
        self.assertEqual(o, OBJ, why)
        o, why = find(image(lambda a: ctor_form(a, NAME, OBJ, nxt=OTHER)), 'r.Other')   # the next variable, registered inline
        self.assertEqual(o, OBJ2, why)

    def test_register_form(self):
        for base_ctor in (False, True):
            o, why = find(image(lambda a: register_form(a, NAME, 0, store=False), lambda a, base_ctor=base_ctor: register_form(a, NAME, OBJ, base_ctor=base_ctor)))
            self.assertEqual(o, OBJ, why)

    def test_inlined_form(self):
        o, why = find(image(lambda a: inline_form(a, NAME, OBJ), z=4))
        self.assertEqual(o, OBJ, why)

    def test_inlined_form_needs_one_way_in(self):
        for kw in ({'clobber': True}, {'vtable': False}, {'far': True}):   # x22 written between; the registered variable's store alone; a b past the store
            o, why = find(image(lambda a, kw=kw: inline_form(a, NAME, OBJ, **kw)))
            self.assertIsNone(o, kw)
        # a branch from elsewhere (at 0x1000) into the span (the second sequence, at 0x1048): a path to the store that skips the adrp
        o, why = find(image(lambda a: [B(a, 0x1048 + 4 * 6), RET], lambda a: inline_form(a, NAME, OBJ)))
        self.assertIsNone(o, why)

    def test_utf32_name(self):
        o, why = find(image(lambda a: register_form(a, NAME, OBJ), z=4))
        self.assertEqual(o, OBJ, why)

    def test_lookup_only_is_not_found(self):
        o, why = find(image(lambda a: register_form(a, NAME, 0, store=False)))
        self.assertIsNone(o)
        self.assertFalse(why.startswith('[RED] '), why)

    def test_registered_twice_is_red(self):
        o, why = find(image(lambda a: register_form(a, NAME, OBJ), lambda a: register_form(a, NAME, OBJ2)))
        self.assertIsNone(o)
        self.assertTrue(why.startswith('[RED] ') and '2 objects' in why, why)

    def test_find_all(self):
        found, why = ue_cvar.find_all(bytes(image(lambda a: register_form(a, NAME, OBJ))), TEXT, RO, DATA)
        self.assertEqual(found, {'t.MaxFPS': OBJ})
        self.assertEqual(len(why), len(ue_cvar.NAMES) - 1)


class ProfileTest(unittest.TestCase):
    def test_profiles_name_known_variables(self):
        for g in game.all_games():
            v = g.get('ue_fps_cvar', '')
            if v: self.assertIn(v, ue_cvar.NAMES, f'{g.id}: ue_fps_cvar {v} is not in ue_cvar.py NAMES, so the rewriter does not look for it')


if __name__ == '__main__':
    unittest.main()
