"""Ocarina of Time's frame-ratio stub (dm/games/0400486024DEC000/patches.py frame_step), assembled as the rewriter places it and run natively on an arm64 Mac against a state
page, with the game's own registers (s0 the int frame rate as a float, s1 the base rate 60 or 30). The ratio the game steps by is step × base / unit: in ticks of 1/120 s (state
+0x38 120, written by the loader for the FRAME_STEP_120 capability) 2 / 3 / 4 ticks are 60 / 40 / 30 fps; with +0x38 0 (a page nothing wrote there) the unit is a vsync of 1/60 s,
as before. A target of 0, or a step not decided yet, runs the game's own division. The units stepped are counted at +0x30. No game files needed; skipped where arm64 code cannot run."""
import ctypes
import importlib.util
import platform
import struct
import sys
import types
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
from nso_asm import B   # noqa: E402

NATIVE = sys.platform == 'darwin' and platform.machine() == 'arm64'
FN = 0x1000                     # the function's start (module offset): the site is +0xe4
SITE = FN + 0xe4
RET = 0xd65f03c0
# The caller: regs[0] s0 (low word), [1] s1, [2] the stub; loads them, calls the stub, which branches back to the site + 4 (a ret there returns here), and stores s8 into regs[3];
# d8 is saved around it (clang-assembled: stp x29,x30 / str d8 / str x19 / mov x19,x0 / ldr s0,[x19] / ldr s1,[x19,#8] / ldr x9,[x19,#16] / blr x9 / str s8,[x19,#24] / restore / ret)
TRAMPOLINE = (0xa9be7bfd, 0xfd000be8, 0xf9000ff3, 0xaa0003f3, 0xbd400260, 0xbd400a61, 0xf9400a69, 0xd63f0120, 0xbd001a68, 0xfd400be8, 0xf9400ff3, 0xa8c27bfd, 0xd65f03c0)


def f32(x): return struct.unpack('<I', struct.pack('<f', x))[0]
def from32(b): return struct.unpack('<f', struct.pack('<I', b & 0xffffffff))[0]


def load_patches():
    spec = importlib.util.spec_from_file_location('oot_patches', REPO / 'dm/games/0400486024DEC000/patches.py')
    assert spec is not None and spec.loader is not None
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m


@unittest.skipUnless(NATIVE, 'runs arm64 code natively: an arm64 Mac only')
class FrameRatioStub(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        libc = cls.libc = ctypes.CDLL(None)
        libc.mmap.restype = ctypes.c_void_p
        libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
        prot_all, private_anon, jit = 7, 0x1002, 0x800
        cls.code = libc.mmap(None, 0x4000, prot_all, private_anon | jit, -1, 0)
        cls.page = libc.mmap(None, 0x4000, 3, private_anon, -1, 0)   # the state page, read-write
        assert cls.code not in (None, ctypes.c_void_p(-1).value) and cls.page not in (None, ctypes.c_void_p(-1).value)
        image = bytearray(0x4000); pos = [0x2000]; puts = {}

        def emit(w): struct.pack_into('<I', image, pos[0], w); pos[0] += 4
        ctx = types.SimpleNamespace(here=lambda: pos[0], emit=emit, site=lambda off, orig: None, put=lambda off, words, why: puts.__setitem__(off, words), state=cls.page - cls.code,
                                    render_scale=0, render_scale_min=0, base=FN, others={}, capabilities=set())
        m = load_patches()
        for attr in ('dynamic_resolution', 'render_scale', 'screen_shape'):
            setattr(m, attr, lambda c: None)   # their own stubs and tests; only the frame ratio here
        m.frame_step(ctx)
        cls.ctx = ctx
        assert list(puts) == [SITE]
        cls.start = SITE + (((puts[SITE][0] & 0x3ffffff) ^ 0x2000000) - 0x2000000) * 4
        assert puts[SITE] == [B(SITE, cls.start)]
        struct.pack_into('<I', image, SITE + 4, RET)
        cls.tramp = 0x3800
        struct.pack_into(f'<{len(TRAMPOLINE)}I', image, cls.tramp, *TRAMPOLINE)
        libc.pthread_jit_write_protect_np(0)
        ctypes.memmove(cls.code, bytes(image), len(image))
        libc.pthread_jit_write_protect_np(1)
        libc.sys_icache_invalidate(ctypes.c_void_p(cls.code), ctypes.c_size_t(len(image)))
        cls.call = ctypes.CFUNCTYPE(None, ctypes.c_void_p)(cls.code + cls.tramp)

    def state32(self, off, value=None):
        p = ctypes.c_uint32.from_address(self.page + off)
        if value is not None: p.value = value
        return p.value

    def stepped(self, value=None):
        p = ctypes.c_uint64.from_address(self.page + 0x30)
        if value is not None: p.value = value
        return p.value

    def ratio(self, base=60.0, fps=30):
        """One pass through the stub at the site with the game's base rate (s1) and int frame rate (s0); returns s8, the ratio the game steps by"""
        regs = (ctypes.c_uint64 * 4)(f32(float(fps)), f32(base), self.code + self.start, 0)
        self.call(ctypes.addressof(regs))
        return from32(regs[3])

    def setUp(self):
        self.state32(0, f32(60.0)); self.state32(0x20, 0); self.state32(0x38, 0); self.stepped(0)

    def test_declares_the_tick_unit(self):
        self.assertIn('FRAME_STEP_120', self.ctx.capabilities)

    def test_ticks_of_1_120_s(self):
        self.state32(0x38, 120)
        for ticks, at60, at30 in ((2, 1.0, 0.5), (3, 1.5, 0.75), (4, 2.0, 1.0)):   # 60 / 40 / 30 fps
            self.state32(0x20, ticks)
            self.assertEqual(self.ratio(60.0), at60, f'{ticks} ticks at base 60')
            self.assertEqual(self.ratio(30.0), at30, f'{ticks} ticks at base 30')
        self.assertEqual(self.stepped(), 2 * (2 + 3 + 4), 'the ticks stepped are counted')

    def test_payback_tick(self):   # a hold of 3 with one tick paid back: 4 ticks of game time, 1/30 s
        self.state32(0x38, 120); self.state32(0x20, 4)
        self.assertEqual(self.ratio(60.0), 2.0)

    def test_vsyncs_without_a_unit(self):   # a page whose +0x38 nothing wrote: vsyncs of 1/60 s, the stub's ratio before the unit
        for vs, at60 in ((1, 1.0), (2, 2.0), (3, 3.0)):
            self.state32(0x20, vs)
            self.assertEqual(self.ratio(60.0), at60)
            self.assertEqual(self.ratio(30.0), at60 / 2)
        self.assertEqual(self.stepped(), 2 * (1 + 2 + 3))

    def test_the_games_own_division(self):
        self.state32(0x38, 120)
        self.state32(0x20, 3); self.state32(0, 0)   # target 0: the game's interval logic
        self.assertEqual(self.ratio(60.0, fps=30), 2.0)
        self.state32(0, f32(40.0)); self.state32(0x20, 0)   # step not decided yet
        self.assertEqual(self.ratio(60.0, fps=60), 1.0)
        self.assertEqual(self.stepped(), 0, 'nothing counted on the game\'s own path')


if __name__ == '__main__':
    unittest.main()
