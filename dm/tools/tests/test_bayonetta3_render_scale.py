"""Bayonetta 3's render scale stubs (dm/games/01004A4010FEA000/patches.py): the size policy, the profile's memory for it, and the stubs' machine code, which runs here in
dm/tools/a64emu.py over a made-up image that holds only the sites' instructions. No game files needed. The check on the game's own set-up code (which views fit which
backings, the calls at scale 1.0 against the unpatched main) needs the main and is described in the game's note."""
import importlib.util
import math
from pathlib import Path
import random
import struct
import sys
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools')); sys.path.insert(0, str(REPO / 'dm/games'))
import a64emu   # noqa: E402
import game   # noqa: E402

ID = '01004A4010FEA000'
spec = importlib.util.spec_from_file_location('bayonetta3_patches', REPO / 'dm/games' / ID / 'patches.py')
assert spec and spec.loader
P = importlib.util.module_from_spec(spec); spec.loader.exec_module(P)

STATE, PATCH, TABLE = 0x800000, 0x810000, 0x700000
SCALES = [1.0, 1.05, 1.2, 1.25, 1.35, 1.5, 1.6, 1.75, 1.9, 2.0, 1.2345, 1.0001, 1.99999]
LEVELS = [(1600, 896), (1600, 896), (1504, 848), (1440, 816), (1360, 768), (1280, 720), (1024, 576), (640, 360)]   # the docked levels (main+0x182d670 / 0x182d570)
SECOND = [(1280, 720), (1280, 720), (1120, 628), (960, 540), (800, 448), (720, 404), (640, 360), (640, 360)]      # the second family's docked levels
MB = 1 << 20


def site_value(word):
    """the value a site's own instruction gives: a movz's immediate, None for a load (the test supplies it) or the first stub's compare"""
    if word & 0x7f800000 == 0x52800000: return ((word >> 5) & 0xffff) << (16 * ((word >> 21) & 3))
    return None


def assemble(scale):
    """an interpreter with every site's instruction at its offset, the stubs behind them and the render scale in the state page, as the loader leaves it"""
    e = a64emu.Emu(b''); pos = [PATCH]; puts = []

    class Ctx:
        state = STATE; render_scale = 0

        @staticmethod
        def here(): return pos[0]

        @staticmethod
        def emit(w): e.w32(pos[0], w); pos[0] += 4

        @staticmethod
        def site(off, orig): assert e.rd(off, len(orig) // 2).hex() == orig, f'original bytes at {off:#x}'

        @staticmethod
        def put(off, words, why):
            assert why
            for i, w in enumerate(words): e.w32(off + 4 * i, w)
            puts.append(off)
    for off, word, _ in P.sites: e.w32(off, word)
    P.FRAME_STEP[P.build_120](Ctx)
    e.wr(STATE + 0x10, struct.pack('<f', scale))
    return e, Ctx, puts


def run_site(e, off, word, value=None, w0=0, flags=(0, 0, 0, 0)):
    """runs from the site to the instruction after it; returns the registers before and after"""
    rnd = random.Random(off)
    e.x = [rnd.getrandbits(64) for _ in range(31)] + [a64emu.Emu.STACK_TOP]; e.x[0] = w0
    if value is not None: e.x[9] = TABLE; e.x[8] = 0x1c; e.w32(TABLE + 0x1c, value)    # ldr wN, [x9, x8]
    e.n, e.z, e.c, e.vf = flags
    before = list(e.x); e.pc = off; steps = 0
    while e.pc != off + 4:
        e.step(); steps += 1
        assert steps < 64, f'the stub of {off:#x} does not come back'
    return before, list(e.x)


class Policy(unittest.TestCase):
    def test_scale_one_is_the_games_value(self):
        for off, word, kind in P.sites:
            v = site_value(word)
            if kind not in 'ICTU' and v is not None: self.assertEqual(P.scaled(kind, v, 1.0), v, f'{off:#x}')
        self.assertEqual(P.scaled('C', 256, 1.0), 256)
        for w, h in LEVELS + SECOND: self.assertEqual((P.scaled('W', w, 1.0), P.scaled('H', h, 1.0)), (w, h))

    def test_light_grid_holds_every_level(self):
        self.assertEqual(P.scaled('T', None, 1.0), (120, 68)); self.assertEqual(P.scaled('U', None, 1.0), 0x7f80)   # the game's own
        self.assertEqual(P.scaled('T', None, 1.6), (160, 90)); self.assertEqual(P.scaled('T', None, 2.0), (200, 113))
        for i in range(1001):
            s = 1.0 + i / 1000; tx, ty = P.scaled('T', None, s)
            for w, h in LEVELS:   # what main+0x686660 asks for at each level: (width + 15) / 16 by (height + 15) / 16
                self.assertLessEqual((P.scaled('W', w, s) + 15) >> 4, tx, s); self.assertLessEqual((P.scaled('H', h, s) + 15) >> 4, ty, s)

    def test_named_sizes(self):
        for s, want in ((1.2, (1920, 1080)), (1.6, (2560, 1440)), (2.0, (3200, 1800))):
            self.assertEqual((P.scaled('W', 1600, s), P.scaled('V', 900, s)), want)       # the window
            self.assertEqual((P.scaled('W', 1600, s), P.scaled('H', 896, s)), want)       # the scene's largest level: every row of the window, not 16 short

    def test_the_scene_never_has_more_rows_than_the_window(self):
        for i in range(2001):
            s = 1.0 + i / 2000
            if s != 1.0: self.assertEqual(P.scaled('H', 896, s), P.scaled('V', 900, s), s)
            self.assertLessEqual(P.scaled('H', 896, s), P.scaled('V', 900, s), s)

    def test_sizes_keep_their_order(self):
        """a view fits its backing by its sizes at every scale where it does in the game: rounding never orders two sizes the other way round"""
        for s in SCALES:
            for kind in 'WH':
                got = [P.scaled(kind, v, s) for v in range(8, 2049, 4)]
                self.assertEqual(got, sorted(got), (kind, s))
            for fam in (LEVELS, SECOND):
                bw, bh = P.scaled('W', fam[0][0], s), P.scaled('H', fam[0][1], s)
                for w, h in fam:
                    self.assertLessEqual(P.scaled('W', w, s), bw); self.assertLessEqual(P.scaled('H', h, s), bh)
            self.assertLessEqual(P.scaled('H', 720, s), P.scaled('W', 1280, s))   # the second family's level 5 takes level 5's height as its width (main+0x69f310)

    def test_alignment(self):
        for s in SCALES[1:]:
            for w, h in LEVELS + SECOND:
                self.assertEqual(P.scaled('W', w, s) % 16, 0); self.assertEqual(P.scaled('H', h, s) % 8, 0)
            pad = P.scaled('P', 1024, s)
            self.assertEqual(pad % 128, 0); self.assertGreaterEqual(pad, P.scaled('V', 900, s)); self.assertLess(pad - P.scaled('V', 900, s), 128)
        self.assertEqual(P.scaled('P', 1024, 1.0), 1024)

    def test_heap_grows_with_the_area(self):
        self.assertEqual(P.scaled('M', 0x10400000, 1.0), 0x10400000)
        for s in SCALES[1:]:
            heap = P.scaled('M', 0x10400000, s)
            self.assertEqual(heap % (2 * MB), 0)
            self.assertGreaterEqual(heap, 260 * MB * s * s * 1.12, s)     # the game's 260 MB times the area, and an eighth (less the rounding of one float)
            self.assertLess(heap, 1 << 32, s)                              # the site's register is 32 bits wide
        for s in (0.5, 0.75, 0.99): self.assertGreaterEqual(P.scaled('M', 0x10400000, s), 0x10400000)   # the first stub makes these 1.0; the heap never shrinks either way

    def test_profile_memory_holds_the_largest_heap(self):
        """the FrameBufferHeap is a memory block of the nn::os heap (pool - used): what it grows by at the largest scale fits beside what the game takes on a Switch"""
        g = game.load(ID)
        pool, used, reserve = g.int('app_pool'), g.int('mem_used'), g.int('mem_reserve')
        heap = (pool - used) & ~0x1fffff
        self.assertLess(heap, 1 << 32)                                                            # no size of the game's needs more than 32 bits
        self.assertLessEqual(heap, reserve - g.int('mem_alias') - g.int('mem_stack') - 0x10000000)   # the heap region (hle_svc.c mem_init_once)
        block = lambda n: (n + 0x108 + 0x1fffff) & ~0x1fffff                                      # main+0xcd5714: the pool's header, whole 2 MB
        fixed = sum(block(n) for n in (0x19320000, 0x6f20000, 0x1120000, 0x2920000, 0x34120000, 0x54420000, 0x2a00000, 0x100000)) + 0x400000   # main+0x95db40, the two other heaps, the thread-local block
        self.assertLessEqual(fixed + block(P.scaled('M', 0x10400000, float(P.render_scale_max))), heap)
        self.assertLessEqual(fixed + block(0x10400000), ((0xCD500000 - used) & ~0x1fffff))        # and the game's own fits a Switch's pool, as a check of the list

    def test_registered_for_the_profiles_build(self):
        builds = {b['main'] for b in game.load(ID).builds().values()}
        self.assertEqual(set(P.FRAME_STEP), {P.build_120}); self.assertIn(P.build_120, builds)
        self.assertEqual(len({off for off, _, _ in P.sites}), len(P.sites))
        self.assertEqual([k for _, _, k in P.sites].count('I'), 1); self.assertEqual(P.sites[0][2], 'I')


class Stubs(unittest.TestCase):
    def test_every_site_is_written_once_and_checked(self):
        e, ctx, puts = assemble(1.0)
        self.assertEqual(sorted(puts), sorted(off for off, _, _ in P.sites))
        self.assertEqual(ctx.render_scale, P.render_scale_max)
        for off, word, _ in P.sites:
            w = e.r32(off); self.assertEqual(w >> 26, 5, f'{off:#x} is not a branch')              # b <stub>
            self.assertGreaterEqual(off + ((w & 0x3ffffff) << 2), PATCH)
        e2 = a64emu.Emu(b'')
        for off, word, _ in P.sites: e2.w32(off, word ^ (0 if off != 0x69f140 else 0x20))          # another immediate at one site: the stubs are not placed
        with self.assertRaises(AssertionError):
            class Bad:
                state = STATE; render_scale = 0
                here = staticmethod(lambda: PATCH); emit = staticmethod(lambda w: None); put = staticmethod(lambda *a: None)
                site = staticmethod(lambda off, orig: self.assertEqual(e2.rd(off, 4).hex(), orig))
            P.FRAME_STEP[P.build_120](Bad)

    def test_the_stubs_compute_the_policy(self):
        for s in SCALES:
            e, _, _ = assemble(s)
            for off, word, kind in P.sites:
                if kind == 'I': continue
                rd = word & 31
                if kind in 'TU':   # the light grid: 120 by 68 tiles in x8 (T), then 4 words a tile in the high half of x8 (U, from the capacity stored at x19 + 0x1f0)
                    tx, ty = P.scaled('T', None, s)
                    e.pc = off; e.x = [0] * 31 + [a64emu.Emu.STACK_TOP]; e.x[rd] = 120 if kind == 'T' else 4; e.x[19] = TABLE; e.n, e.z, e.c, e.vf = 0, 1, 1, 0
                    e.w32(TABLE + 0x1f0, tx); e.w32(TABLE + 0x1f4, ty)
                    while e.pc != off + 4: e.step()
                    self.assertEqual(e.x[rd], tx | ty << 32 if kind == 'T' else 4 | (tx * ty * 4) << 32, f'{off:#x} at {s}')
                    self.assertEqual((e.n, e.z, e.c, e.vf), (0, 1, 1, 0)); self.assertEqual([e.x[r] for r in range(16) if r != rd], [0] * 15); self.assertEqual(e.x[19], TABLE)
                    self.assertGreaterEqual(tx * 16, P.scaled('W', 1600, s)); self.assertGreaterEqual(ty * 16, P.scaled('H', 896, s))   # the largest level's tiles fit
                    self.assertGreaterEqual((tx, ty), (120, 68)); self.assertLess(tx * ty * 16, 1 << 31)
                    if s in (1.0, 1.2): self.assertEqual((tx, ty), (120, 68))
                    continue
                if kind == 'C':   # movk x8, #0x100, lsl #32 over the 4 the game has put in x8: the entries in the high half
                    e.pc = off; e.x = [0] * 31 + [a64emu.Emu.STACK_TOP]; e.x[rd] = 4; e.n, e.z, e.c, e.vf = 1, 0, 1, 1
                    while e.pc != off + 4: e.step()
                    self.assertEqual(e.x[rd], 4 | P.scaled('C', 256, s) << 32, f'{off:#x} at {s}')
                    self.assertEqual((e.n, e.z, e.c, e.vf), (1, 0, 1, 1)); self.assertEqual([e.x[r] for r in range(16) if r != rd], [0] * 15)
                    self.assertGreaterEqual(P.scaled('C', 256, s) * 8, P.scaled('W', 1600, s))   # an entry for every two columns of the quarter-size scene
                    continue
                for value in ([site_value(word)] if site_value(word) is not None else [w if kind == 'W' else h for w, h in LEVELS]):
                    load = site_value(word) is None
                    for flags in ((0, 0, 0, 0), (1, 0, 1, 0), (0, 1, 1, 0), (1, 0, 0, 1)):
                        before, after = run_site(e, off, word, value if load else None, flags=flags)
                        self.assertEqual(after[rd] & 0xffffffff, P.scaled(kind, value, s), f'{off:#x} {kind} {value} at {s}')
                        self.assertEqual(after[rd] >> 32, 0)
                        for r in range(32):
                            if r not in (rd, 16, 17) and not (load and r in (8, 9)): self.assertEqual(after[r], before[r], f'{off:#x}: x{r} changed')
                        self.assertEqual((e.n, e.z, e.c, e.vf), flags, f'{off:#x}: the flags changed')

    def test_first_stub_keeps_the_games_sizes_in_handheld_mode_and_below_one(self):
        (off, word, kind), = [x for x in P.sites if x[2] == 'I']
        for s, mode, want in ((1.2, 0, 1.2), (2.0, 0, 2.0), (1.0, 0, 1.0), (1.2, 1, 1.0), (2.0, 1, 1.0), (0.5, 0, 1.0), (0.95, 0, 1.0), (0.5, 1, 1.0), (float('nan'), 0, 1.0)):
            e, _, _ = assemble(s)
            before, after = run_site(e, off, word, w0=mode, flags=(1, 1, 0, 1))
            got = struct.unpack('<f', e.rd(STATE + 0x10, 4))[0]
            self.assertEqual(got, struct.unpack('<f', struct.pack('<f', want))[0], (s, mode))
            self.assertEqual((e.z, e.n), (int(mode == 0), 0))                                      # the flags of the site's own cmp w0, #0
            for r in range(32):
                if r not in (16, 17): self.assertEqual(after[r], before[r])
        # ... and the sites behind it then give the game's values
        e, _, _ = assemble(1.6); run_site(e, off, word, w0=1)
        for off2, word2, kind2 in P.sites[1:]:
            if site_value(word2) is not None and kind2 not in 'CTU':
                _, after = run_site(e, off2, word2)
                self.assertEqual(after[word2 & 31] & 0xffffffff, site_value(word2), f'{off2:#x}')

    def test_reference_matches_single_precision(self):
        self.assertEqual(P.f32(1.2), struct.unpack('<f', struct.pack('<f', 1.2))[0])
        self.assertTrue(math.isclose(P.k_shape, 900 / 896))


if __name__ == '__main__':
    unittest.main()
