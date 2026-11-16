"""Synthetic unwind functions, relocations and vtables for Monolith timing.

The test registers identities of its own small AArch64 functions; discovery,
whole-function matching and all address/layout checks use production code.
No function bodies or other bytes from games are included.
"""
from dataclasses import replace
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from nso_asm import ADRP, LDRX
from nso_match import Image
import monolith_fps as fps

TEXT, RO, DATA, END = 0x1000, 0x1000, 0x2000, 0x4000
WAIT, DURATION, SETTER, PERIOD = 0x100, 0x200, 0x300, 0x400
DYN, RELA, GOT, GLOBAL, CONTEXT, VTABLE = 0x1100, 0x1200, 0x2100, 0x2300, 0x2400, 0x2500
LITERAL, MARGIN = 0x1800, 0x1810
RET = 0xd65f03c0


def ldr_w(d, n): return 0xb9400000 | n << 5 | d
def str_w(d, n): return 0xb9000000 | n << 5 | d
def ldr_s(d, n, off): return 0xbd400000 | (off // 4) << 10 | n << 5 | d
def movz(d, value): return 0x52800000 | value << 5 | d
def movk_hi(d, value): return 0x72a00000 | value << 5 | d


def address(at, reg, slot):
    return [ADRP(reg, at, slot), LDRX(reg, reg, (slot & 0xfff) // 8)]


def reader(at, slots, extra=False, margin=False):
    out = address(at, 19, slots[0]) + [ldr_w(8, 19)]
    out += address(at + 12, 20, slots[2]) + [LDRX(0, 20), LDRX(9, 0), LDRX(9, 9, 0x140 // 8), 0xd63f0120]
    out += [0x1e220101, 0x1e210800]  # scvtf s1,w8; fmul s0,s0,s1
    if extra: out += [0x1e202800]  # fadd s0,s0,s0 makes this a distinct complete function
    if margin:
        out += [ADRP(10, at + 4 * len(out), MARGIN), ldr_s(2, 10, MARGIN & 0xfff)]
    return out + [RET]


def setter(at, slots, interleaved=False):
    a, b = address(at, 8, slots[0]), address(at + 8, 9, slots[1])
    if interleaved: out = a + b
    else:
        # Both forms occur in the reviewed engine, with equivalent data flow.
        out = [ADRP(8, at, slots[0]), ADRP(9, at + 4, slots[1]), a[1], b[1]]
    return out + [ldr_w(10, 8), str_w(0, 8), str_w(10, 9), RET]


def image(shift=0, interval=GLOBAL, previous=None, interleaved=False, literal=False, margin=False, duplicate=False):
    """Build complete unwind tables and RELA records for synthetic engine code."""
    previous = interval + 4 if previous is None else previous
    waits = [WAIT + shift] + ([WAIT + shift + 0x500] if duplicate else [])
    starts = [waits[0], DURATION + shift, SETTER + shift, PERIOD + shift]
    slots = [GOT + 8 * i for i in range(3)]
    period = [ADRP(8, starts[3], LITERAL), ldr_s(0, 8, LITERAL & 0xfff), RET] if literal else [
        movz(8, 0x8889), movk_hi(8, 0x3c88), 0x1e270100, RET]
    functions = {starts[0]: reader(starts[0], slots, margin=margin),
                 starts[1]: reader(starts[1], slots, extra=True),
                 starts[2]: setter(starts[2], slots, interleaved), starts[3]: period}
    if duplicate: functions[waits[1]] = reader(waits[1], slots, margin=margin)
    out = bytearray(END)
    struct.pack_into('<I', out, 4, 0x20); out[0x20:0x24] = b'MOD0'
    struct.pack_into('<i', out, 0x24, DYN - 0x20)
    struct.pack_into('<ii', out, 0x30, 0x3000 - 0x20, 0x300c + len(functions) * 8 - 0x20)
    out[0x3000:0x3004] = bytes((1, 0x1b, 3, 0x3b))
    struct.pack_into('<iI', out, 0x3004, 0x3200 - 0x3004, len(functions))
    struct.pack_into('<II', out, 0x3200, 16, 0)
    out[0x3208:0x3211] = b'\x01zR\0\x04\x78\x1e\x01\x1b'
    for i, (start, words) in enumerate(sorted(functions.items())):
        struct.pack_into('<' + 'I' * len(words), out, start, *words)
        fde = 0x3240 + i * 24
        struct.pack_into('<ii', out, 0x300c + i * 8, start - 0x3000, fde - 0x3000)
        struct.pack_into('<IIiI', out, fde, 16, fde + 4 - 0x3200, start - (fde + 8), len(words) * 4)
    relocations = [*zip(slots, (interval, previous, CONTEXT)), (VTABLE, starts[3]), (VTABLE + 24, starts[0])]
    struct.pack_into('<qQqQqQ', out, DYN, 7, RELA, 8, len(relocations) * 24, 0, 0)
    for i, (slot, value) in enumerate(relocations): struct.pack_into('<QQq', out, RELA + 24 * i, slot, 1027, value)
    struct.pack_into('<ii', out, interval, -1, -1)
    struct.pack_into('<II', out, LITERAL, fps.PERIOD, 0)
    struct.pack_into('<I', out, MARGIN, fps.MARGIN)
    return out, starts


def implementation(img, starts, margin=False):
    module = Image(img, TEXT)
    def signature(start): return fps.Signature(module.functions[start] - start, module.fingerprint(start))
    margin_offsets = (44, 48) if margin else None
    return fps.Implementation('synthetic engine', signature(starts[0]), signature(starts[1]), signature(starts[2]), margin_offsets)


class Discovery(unittest.TestCase):
    def setUp(self):
        self.original, self.starts = image()
        self.reviewed = implementation(self.original, self.starts)

    def find(self, img, reviewed=None):
        with patch.object(fps, 'REVIEWED', (reviewed or self.reviewed,)):
            return fps.find(bytes(img), TEXT, RO, DATA)

    def rejected(self, img, text, reviewed=None):
        off, why = self.find(img, reviewed)
        self.assertIsNone(off, why)
        self.assertTrue(why.startswith('[RED] '), why)
        self.assertIn(text, why)

    def test_relocated_complete_functions_and_globals(self):
        moved, _ = image(shift=0x80, interval=GLOBAL + 0x40)
        self.assertEqual(self.find(moved)[0], GLOBAL + 0x40)

    def test_interleaved_setter_codegen(self):
        img, starts = image(interleaved=True)
        self.assertEqual(self.find(img, implementation(img, starts))[0], GLOBAL)

    def test_literal_period(self):
        img, starts = image(literal=True)
        reviewed = implementation(img, starts)
        self.assertEqual(self.find(img, reviewed)[0], GLOBAL)
        struct.pack_into('<I', img, LITERAL, 0x3d088889)  # 1/30: code fingerprint alone cannot see this change
        self.rejected(img, '1/60 period', reviewed)

    def test_literal_margin_is_checked_beyond_code_identity(self):
        img, starts = image(margin=True)
        reviewed = implementation(img, starts, margin=True)
        self.assertEqual(self.find(img, reviewed)[0], GLOBAL)
        struct.pack_into('<I', img, MARGIN, 0x3f800000)
        self.rejected(img, 'margin literal', reviewed)

    def test_ambiguous_waits(self):
        self.rejected(image(duplicate=True)[0], '2 matching waits')

    def test_changed_duration_is_an_incomplete_implementation(self):
        changed = bytearray(self.original)
        struct.pack_into('<I', changed, DURATION + 40, 0x1e202800)
        self.rejected(changed, '0 duration getters')

    def test_wait_and_duration_must_read_the_same_current_global(self):
        changed = bytearray(self.original)
        # Change only the wait's relocated interval slot, preserving its code identity.
        alternate = GOT + 0x40
        struct.pack_into('<I', changed, WAIT + 4, LDRX(19, 19, (alternate & 0xfff) // 8))
        struct.pack_into('<QQq', changed, RELA + 5 * 24, alternate, 1027, GLOBAL + 0x80)
        struct.pack_into('<Q', changed, DYN + 24, 6 * 24)
        self.rejected(changed, 'do not share')

    def test_previous_value_must_be_adjacent(self):
        self.rejected(image(previous=GLOBAL + 8)[0], 'not beside')

    def test_only_writable_data_can_hold_the_interval(self):
        self.rejected(image(interval=LITERAL)[0], 'outside writable')

    def test_missing_got_relocation(self):
        changed = bytearray(self.original)
        struct.pack_into('<Q', changed, RELA + 8, 0)
        self.rejected(changed, 'no relative relocation')

    def test_unexpected_initial_value(self):
        changed = bytearray(self.original); struct.pack_into('<i', changed, GLOBAL, 60)
        self.rejected(changed, 'unexpected initial')

    def test_wrong_or_missing_period_vtable(self):
        changed = bytearray(self.original)
        struct.pack_into('<q', changed, RELA + 3 * 24 + 16, SETTER)
        self.rejected(changed, '1/60 period')

    def test_no_unwind_information_is_not_a_match(self):
        off, why = self.find(bytes(END))
        self.assertIsNone(off)
        self.assertFalse(why.startswith('[RED] '), why)

    def test_two_reviewed_implementations_fail_closed(self):
        with patch.object(fps, 'REVIEWED', (self.reviewed, replace(self.reviewed, name='second engine'))):
            off, why = fps.find(self.original, TEXT, RO, DATA)
        self.assertIsNone(off)
        self.assertIn('2 complete implementations', why)


if __name__ == '__main__': unittest.main()
