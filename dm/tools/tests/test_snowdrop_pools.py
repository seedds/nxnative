"""Snowdrop's three-pool allocator found without a build ID (dm/tools/snowdrop_pools.py) on synthetic images, no game files: both builds' encodings and field offsets, the cuts for a
layout, and fail closed: a part that differs changes nothing, with a [RED] reason once the shape is there."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import snowdrop_pools as sp

RET, NOP = 0xd65f03c0, 0xd503201f
TEXT = 0x4000
CTOR, RESERVE, META, LARGE = 0x1000, 0x2000, 0x2100, 0x2200
W1, X2 = sp.W1_ZERO, sp.X2_ZERO
MOVZ4 = lambda rd: 0xD2C00020 | rd      # mov xD, #4 GB
MOVZ12 = lambda rd: 0xD2C00060 | rd     # mov xD, #12 GB
ORR4 = lambda rd: 0xB26003E0 | rd       # the same, as orr xD, xzr, #bitmask
ORR12 = lambda rd: 0xB26007E0 | rd


def bl(frm, to): return 0x94000000 | ((to - frm) // 4 & 0x3FFFFFF)
def mov(rd, rn): return 0xAA0003E0 | rn << 16 | rd
def stp(rt, rt2, rn, off): return 0xA9000000 | (off // 8 & 0x7F) << 15 | rt2 << 10 | rn << 5 | rt
def add_reg(rd, rn, rm): return 0x8B000000 | rm << 16 | rn << 5 | rd
def add_imm_hi(rd, rn, imm12): return 0x91400000 | imm12 << 10 | rn << 5 | rd


def body(f=0x98, load4=MOVZ4, load12=MOVZ12, w5=0x52A00025, reserve2=RESERVE, x2_second=X2, total_reg=0, end_used=True):
    """The constructor's instructions at CTOR, in Sparks of Hope's order"""
    w = [sp.str_x(8, 0, f), mov(19, 0), stp(8, 8, 0, f + 8)]   # the sizes go to the object, before and after its register is saved
    w = [load4(8), mov(20, 1)] + w
    w += [None, add_imm_hi(22, 0, 0x30), mov(21, 0), mov(0, 22), W1, X2, None, add_reg(8, 22, 0)]
    w += [load12(22), W1, stp(0, 8, 19, f + 0x28), load12(total_reg), x2_second, None]
    w += [mov(1, 0), stp(0, 8, 19, f + 0x18), add_reg(8, 0, 22) if end_used else NOP, w5, None]
    out, pc = [], CTOR
    targets = iter((META, RESERVE, reserve2, LARGE))
    for x in w:
        out.append(bl(pc, next(targets)) if x is None else x)
        pc += 4
    return out


def image(words=None, overwrite_x1=True, extra=None):
    img = bytearray(TEXT)
    words = body() if words is None else words
    struct.pack_into(f'<{len(words)}I', img, CTOR, *words)
    wrapper = [0xD10083FF, mov(1, 0) if overwrite_x1 else NOP, 0x910023E0, bl(RESERVE + 12, 0x3000), RET]
    struct.pack_into(f'<{len(wrapper)}I', img, RESERVE, *wrapper)
    if extra: struct.pack_into(f'<{len(extra)}I', img, 0x3800, *extra)
    return img


def find(img): return sp.find(bytes(img), TEXT)


class FindTest(unittest.TestCase):
    def test_found_both_encodings(self):
        for load4, load12, w5, f in ((MOVZ4, MOVZ12, 0x52A00025, 0x98), (ORR4, ORR12, 0x321003E5, 0x90)):
            s, why = find(image(body(f=f, load4=load4, load12=load12, w5=w5)))
            self.assertIsNotNone(s, why)
            self.assertEqual((s['sizes'], s['field'], s['xs']), (CTOR, f, 19), why)

    def test_nothing_there_is_not_red(self):
        s, why = find(bytearray(TEXT))
        self.assertIsNone(s)
        self.assertFalse(why.startswith('[RED] '), why)

    def test_a_part_that_differs_is_red(self):
        for what, words, kw in (('different calls', body(reserve2=0x2300), {}), ('x2 in use', body(x2_second=NOP), {}), ('12 GB not from x0', body(total_reg=3), {}),
                                ('the end unused', body(end_used=False), {}), ('x1 read', None, {'overwrite_x1': False})):
            s, why = find(image(words, **kw))
            self.assertIsNone(s, what)
            self.assertTrue(why.startswith('[RED] '), f'{what}: {why}')

    def test_two_are_red(self):
        words = body()
        img = image(words)
        at = 0x3000 + 0x100
        moved = [bl(at + i * 4, sp.bl_target(w, CTOR + i * 4)) if sp.is_bl(w) else w for i, w in enumerate(words)]   # the same calls, from the copy's place
        struct.pack_into(f'<{len(moved)}I', img, at, *moved)
        s, why = find(img)
        self.assertIsNone(s)
        self.assertTrue(why.startswith('[RED] 2 '), why)


class CutsTest(unittest.TestCase):
    def apply(self, sizes, **kw):
        img = image(body(**kw))
        s, why = find(img)
        self.assertIsNotNone(s, why)
        cuts = sp.cuts(img, s, sizes)
        for off, old, new, _ in cuts:
            self.assertEqual(bytes(img[off:off + 4]).hex(), old)
            img[off:off + 4] = bytes.fromhex(new)
        return {o: n for o, _, n, _ in cuts}, s

    def test_kingdom_battles_layout_is_the_registered_bytes(self):
        # Kingdom Battle 1.9.589692's cuts, by hand before this rule
        cuts, s = self.apply((2048, 512, 512), f=0x90, load4=ORR4, load12=ORR12, w5=0x321003E5)
        self.assertEqual(sorted(cuts.values()), sorted(('0800a4d2', '1600b8d2', '0100b0d2', '0000b8d2', '615200f9', '05009052')))
        self.assertEqual(cuts[s['sizes']], '0800a4d2')
        self.assertEqual(cuts[s['pages']], '05009052')

    def test_a_larger_64_kb_range_sets_its_own_field(self):
        cuts, s = self.apply((2048, 512, 1280))
        self.assertEqual(cuts[s['w1_meta']], '0100aad2')   # mov x1, #0x50000000
        self.assertEqual(cuts[s['x2_meta']], '614e00f9')   # str x1, [x19, #0x98]
        self.assertEqual(cuts[s['total']], '0000bed2')     # mov x0, #0xf0000000
        self.assertEqual(cuts[s['end']], '1600bed2')       # mov x22, #0xf0000000


class ProfileTest(unittest.TestCase):
    def test_key_and_defaults(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / 'p'; p.mkdir()
            (p / 'game.tsv').write_text('title_id\t0100000000000000\n')
            self.assertEqual(sp.profile_sizes({'DM_GAME': str(p)})[0], sp.DEFAULT)
            (p / 'game.tsv').write_text('snowdrop_pools\t1024\t256\t768\n')
            self.assertEqual(sp.profile_sizes({'DM_GAME': str(p)}), ((1024, 256, 768), 'profile'))
            for bad in ('2048\t512', '2048\t512\t2048', '0\t512\t512', 'a\tb\tc'):
                (p / 'game.tsv').write_text(f'snowdrop_pools\t{bad}\n')
                with self.assertRaises(SystemExit): sp.profile_sizes({'DM_GAME': str(p)})
        self.assertEqual(sp.profile_sizes({})[0], sp.DEFAULT)

    def test_the_two_games_profiles(self):
        games = Path(__file__).resolve().parents[2] / 'games'
        self.assertEqual(sp.profile_sizes({'DM_GAME': str(games / '0100317013770000')})[0], (2048, 512, 1280))
        self.assertEqual(sp.profile_sizes({'DM_GAME': str(games / '010067300059A000')})[0], (2048, 512, 512))


if __name__ == '__main__':
    unittest.main()
