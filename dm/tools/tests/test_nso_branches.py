import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from nso_branches import Branches, MARKER, padding_slots, REACH
from nso_asm import B, BL


class BranchTests(unittest.TestCase):
    def test_direct_range_endpoints(self):
        b = Branches()
        for target in (-(1 << 27), (1 << 27) - 4):
            self.assertEqual(b.encode(0, target), B(0, target))
            self.assertEqual(b.encode(0, target, True), BL(0, target))
        self.assertFalse(b.far)

    def test_far_targets_keep_full_signed_offsets_and_link(self):
        b = Branches()
        for source, target, link in ((-0x1000000, 0x10000000, False), (0x16000000, 0x8000000, True)):
            self.assertEqual(b.encode(source, target, link), MARKER)
            self.assertEqual(b.far[source], (target, int(link)))

    def test_replaced_speculative_branch_removes_redirect(self):
        b = Branches()
        b.encode(0, 1 << 28)
        b.encode(0, 4)
        self.assertFalse(b.far)

    def test_alignment_is_not_relaxed(self):
        with self.assertRaises(AssertionError):
            Branches().encode(0, (1 << 28) + 1)

    @staticmethod
    def fixture():
        image = bytearray(128)
        struct.pack_into('<I', image, 16, 0xd65f03c0)  # ret; zero alignment up to function at 32
        struct.pack_into('<I', image, 60, 0xd65f0fff)  # retab
        return image, [(0, 20), (32, 64)]

    def test_only_terminal_zero_alignment(self):
        image, ranges = self.fixture()
        self.assertEqual(padding_slots(image, ranges, image), [20, 24, 28])
        for word in (0xd503201f, 0x94000001, 0xd4200020):  # nop, bl, brk: no proved terminal return
            struct.pack_into('<I', image, 16, word)
            self.assertEqual(padding_slots(image, ranges, image), [])

    def test_referenced_or_modified_padding_is_not_allocated(self):
        from nso_asm import ADRP
        image, ranges = self.fixture()
        for word in (B(0, 24), 0x100000c0, 0x580000c0, 0x540000c0, 0x340000c0, 0x360000c0):  # adr/ldr/b.cond/cbz/tbz +24
            struct.pack_into('<I', image, 0, word)
            self.assertNotIn(24, padding_slots(image, ranges, image))
        struct.pack_into('<I', image, 0, ADRP(0, 0, 0))
        self.assertEqual(padding_slots(image, ranges, image), [])
        struct.pack_into('<I', image, 0, 0)
        struct.pack_into('<Q', image, 100, 24)  # 4-aligned pointer / relocation addend
        self.assertNotIn(24, padding_slots(image, ranges, image))
        original = bytes(image); struct.pack_into('<I', image, 28, 1)
        self.assertNotIn(28, padding_slots(original, ranges, image))

    def test_bl_relay_keeps_the_call_at_its_original_site(self):
        image, ranges = self.fixture(); b = Branches(); source = -REACH + 32
        self.assertEqual(b.encode(source, 64, True), MARKER)
        written = {}; rows = b.resolve(image, bytes(image), ranges, written.__setitem__)
        self.assertEqual(rows, [(source, 20, 64, 1)])
        self.assertEqual(written[source], BL(source, 20))
        self.assertEqual(struct.unpack_from('<I', image, 20)[0], B(20, 64))
        self.assertFalse(b.far)

    def test_reuse_and_exhaustion(self):
        image, ranges = self.fixture(); text = bytes(image); b = Branches(); written = {}
        for i in range(4): b.encode(-REACH + 32 + i * 4, 64)
        rows = b.resolve(image, text, ranges, written.__setitem__)
        self.assertEqual(len(rows), 4)
        self.assertEqual({r[1] for r in rows}, {20})
        image, ranges = self.fixture(); b = Branches()
        for i in range(4): b.encode(-REACH + 32 + i * 4, 64 + i * 4)
        rows = b.resolve(image, bytes(image), ranges, written.__setitem__)
        self.assertEqual(len(rows), 3)
        self.assertEqual(len(b.far), 1)

    def test_out_of_reach_pool_leaves_the_exact_fallback(self):
        image, ranges = self.fixture(); b = Branches(); b.encode(-REACH, 64)
        self.assertFalse(b.resolve(image, bytes(image), ranges, lambda *_: self.fail('unreachable relay')))
        self.assertEqual(b.far, {-REACH: (64, 0)})

    def test_no_unwind_or_large_nonzero_unaligned_holes(self):
        image, ranges = self.fixture()
        for unsafe in ([], [(0, 20), (36, 64)], [(0, 20), (64, 80)]):
            self.assertFalse(padding_slots(image, unsafe, image))
        original = bytes(image); struct.pack_into('<I', image, 16, 0xd503201f)
        self.assertFalse(padding_slots(original, ranges, image))
        image, ranges = self.fixture(); struct.pack_into('<I', image, 24, 0xd503201f)
        self.assertFalse(padding_slots(image, ranges, image))

    def test_generated_references_also_protect_padding(self):
        image, ranges = self.fixture(); b = Branches()
        for i, target in enumerate((20, 24, 28)): b.encode(i * 4, target)
        b.encode(-REACH + 32, 64)
        self.assertFalse(b.resolve(image, bytes(image), ranges, lambda *_: self.fail('referenced padding')))
