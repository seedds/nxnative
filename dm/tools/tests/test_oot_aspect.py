"""Execute Ocarina of Time's emitted screen-shape stubs on ARM64, without game files.

Optional dependency: unicorn. On macOS its JIT needs execution outside the sandbox.
"""
import importlib
import importlib.util
from pathlib import Path
import struct
import sys
import types
from typing import Any
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
uc: Any = None   # unicorn and its ARM64 constants when installed (typed Any: the tests are skipped without it)
arm: Any = None
try:
    uc, arm = importlib.import_module('unicorn'), importlib.import_module('unicorn.arm64_const')
except ImportError:
    pass

spec = importlib.util.spec_from_file_location('oot_patches', Path(__file__).resolve().parents[2] / 'games/0400486024DEC000/patches.py')
assert spec is not None and spec.loader is not None
patch = importlib.util.module_from_spec(spec); spec.loader.exec_module(patch)


def camera(r):
    """The camera's x / y multipliers the loader writes for a shape (loader.c screen_any_write)"""
    if not r: return (1, 1)
    l = r if r >= 1 else 1 / r
    xl, yl = (16 / 9 / l, 1) if l >= 16 / 9 else (1, l * 9 / 16)
    return (xl, yl) if r >= 1 else (yl * 16 / 9, xl * 9 / 16)


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class OutputSize(unittest.TestCase):
    # The setter's first instruction (ldp w8,w9,[x0,#0x10]) at SITE in a code area; the stub branches back to SITE + 4
    base, area, state, obj, req = 0x4000000, 0x4100000, 0x4200000, 0x6000000, 0x6001000
    SITE = 0x1000

    def setUp(self):
        self.u = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        for a, n in ((self.base, 0x100000), (self.area, 0x10000), (self.state, 0x4000), (self.obj, 0x2000)):
            self.u.mem_map(a, n)
        self.u.mem_write(self.base + self.SITE, struct.pack('<2I', 0x29422408, 0xd503201f))   # ldp w8,w9,[x0,#0x10]; nop
        self.cursor = self.area - self.base
        self.ctx = types.SimpleNamespace(here=lambda: self.cursor, emit=self.emit, site=lambda off, orig: None, put=self.put,
                                         state=self.state - self.base, others={'output size': self.SITE})
        patch.screen_shape(self.ctx)
        self.u.mem_write(self.obj + 0x10, struct.pack('<2I', 1111, 2222))   # the size the settings hold now

    def emit(self, w):
        self.u.mem_write(self.base + self.cursor, struct.pack('<I', w)); self.cursor += 4

    def put(self, off, words, why):
        self.u.mem_write(self.base + off, struct.pack('<' + 'I' * len(words), *words))

    def shape(self, ratio):
        f = (ratio, max(ratio * 9 / 16, 1), max(16 / (9 * ratio), 1)) if ratio else (0, 1, 1)
        self.u.mem_write(self.state + 0xa0, struct.pack('<3f', *f))
        self.u.mem_write(self.state + 0xc4, struct.pack('<2f', *camera(ratio)))

    def call(self, w, h):
        """Run the setter's entry with x1 -> {w, h}; the size it goes on with (x1's pair) and the registers its first instruction loads"""
        self.u.mem_write(self.req, struct.pack('<2I', w, h))
        self.u.reg_write(arm.UC_ARM64_REG_X0, self.obj); self.u.reg_write(arm.UC_ARM64_REG_X1, self.req)
        end = self.base + self.SITE + 4
        stop = self.u.hook_add(uc.UC_HOOK_CODE, lambda u, a, n, d: u.emu_stop(), begin=end, end=end)
        try: self.u.emu_start(self.base + self.SITE, end, count=100)
        finally: self.u.hook_del(stop)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_PC), end)
        self.assertEqual((self.u.reg_read(arm.UC_ARM64_REG_W8), self.u.reg_read(arm.UC_ARM64_REG_W9)), (1111, 2222))   # the original first instruction ran
        x1 = self.u.reg_read(arm.UC_ARM64_REG_X1)
        self.assertEqual(struct.unpack('<2I', self.u.mem_read(self.req, 8)), (w, h))   # the caller's struct (the init's config) is never written
        return x1, struct.unpack('<2I', self.u.mem_read(x1, 8))

    def factors(self): return struct.unpack('<2f', self.u.mem_read(self.state + 0xb8, 8))

    def test_16_9_is_the_setter_as_it_is(self):
        self.u.mem_write(self.state + 0xb8, struct.pack('<2f', 1.2375, 1))   # from an earlier, wider size
        self.shape(0)
        self.assertEqual(self.call(2048, 1152), (self.req, (2048, 1152)))
        self.assertEqual(self.factors(), (0, 0))   # the camera: 16:9

    def test_the_camera_multipliers_go_with_the_size(self):
        for ratio in (2.2, 1.43, 0.46):
            self.shape(ratio); self.call(2048, 1152)
            self.assertEqual(self.factors(), tuple(struct.unpack('<2f', struct.pack('<2f', *camera(ratio)))))

    def test_wider_keeps_the_height(self):
        for ratio, docked, handheld in ((2.2, (2536, 1152), (1904, 864)), (2.4, (2768, 1152), (2072, 864)), (21 / 9, (2688, 1152), (2016, 864))):
            self.shape(ratio)
            self.assertEqual(self.call(2048, 1152), (self.state + 0xb0, docked), ratio)
            self.assertEqual(self.call(1536, 864), (self.state + 0xb0, handheld), ratio)

    def test_taller_keeps_the_width(self):
        for ratio, docked, handheld in ((1.43, (2048, 1432), (1920, 1344)), (1.3, (2048, 1576), (1920, 1480)), (4 / 3, (2048, 1536), (1920, 1440))):
            self.shape(ratio)
            self.assertEqual(self.call(2048, 1152), (self.state + 0xb0, docked), ratio)
            self.assertEqual(self.call(1920, 1080), (self.state + 0xb0, handheld), ratio)

    def test_any_shape_keeps_the_pixels_in_bounds(self):
        for ratio in (1179 / 2556, 0.75, 1.0, 3.0, 5.0):   # an iPhone held upright, a portrait window, a square, ultrawide beyond 21:9
            self.shape(ratio)
            _, (w, h) = self.call(2048, 1152)
            self.assertLessEqual(w * h, 1.5 * 2048 * 1152 * 1.01, ratio)
            self.assertGreater(w * h, 1.5 * 2048 * 1152 * 0.97, ratio)   # scaled down only as far as the bound
            self.assertLess(abs(w / h / ratio - 1), 0.01, ratio)
            self.assertEqual((w % 8, h % 8), (0, 0))

    def test_the_shape_holds_to_a_multiple_of_8(self):
        for ratio in (1.3, 1.43, 1.6, 2.0, 2.2, 2.4):
            self.shape(ratio)
            _, (w, h) = self.call(2048, 1152)
            self.assertEqual((w % 8, h % 8), (0, 0))
            self.assertLess(abs(w / h / ratio - 1), 0.004, ratio)


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class CameraProjection(unittest.TestCase):
    # The camera copy's last matrix store (str q18,[x20,#0x30]) at FN + 0x2ec; the stub branches back after it. x19 the render camera, x20 its matrix (+0x148)
    base, area, state, cam = 0x4000000, 0x4100000, 0x4200000, 0x6000000
    FN = 0x1000; SITE = FN + 0x2ec
    M = (1.20628, 0, 0, 0, 0, 2.14451, 0, 0, 0, 0, -1.00001, -0.200001, 0, 0, -1, 0)   # the game's 16:9 projection (fovy 50°, near 0.1)

    def setUp(self):
        self.u = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        for a, n in ((self.base, 0x100000), (self.area, 0x10000), (self.state, 0x4000), (self.cam, 0x1000)):
            self.u.mem_map(a, n)
        self.u.mem_write(self.base + self.SITE, struct.pack('<2I', 0x3d800e92, 0xd503201f))   # str q18,[x20,#0x30]; nop
        self.cursor = self.area - self.base
        ctx = types.SimpleNamespace(here=lambda: self.cursor, emit=self.emit, site=lambda off, orig: None, put=self.put, state=self.state - self.base, others={'camera copy': self.FN})
        patch.camera_projection(ctx)

    emit, put = OutputSize.emit, OutputSize.put

    def shape(self, ratio, asked=None):
        """The factors the output size was made with (state +0xb8 / +0xbc, 0 = 16:9) and, apart, the shape the host asks for now (+0xa0..: the camera ignores it)"""
        f = lambda r: (r, max(r * 9 / 16, 1), max(16 / (9 * r), 1)) if r else (0, 1, 1)
        self.u.mem_write(self.state + 0xa0, struct.pack('<3f', *f(ratio if asked is None else asked)))
        self.u.mem_write(self.state + 0xb8, struct.pack('<2f', *camera(ratio)) if ratio else bytes(8))

    def run_copy(self):
        """The matrix as the copy leaves it (rows 0..2 stored, row 3 in q18, rows 0 / 1 also in v4 / v5), aspect 16:9 at +0x1d4; the matrix, v4, v5 and the aspect after"""
        m = struct.pack('<16f', *self.M)
        self.u.mem_write(self.cam + 0x148, m[:48] + bytes(16)); self.u.mem_write(self.cam + 0x1d4, struct.pack('<f', 16 / 9))
        self.u.reg_write(arm.UC_ARM64_REG_V4, int.from_bytes(m[0:16], 'little')); self.u.reg_write(arm.UC_ARM64_REG_V5, int.from_bytes(m[16:32], 'little'))
        self.u.reg_write(arm.UC_ARM64_REG_V18, int.from_bytes(m[48:64], 'little'))
        self.u.reg_write(arm.UC_ARM64_REG_X19, self.cam); self.u.reg_write(arm.UC_ARM64_REG_X20, self.cam + 0x148)
        self.u.reg_write(arm.UC_ARM64_REG_NZCV, 0x60000000)   # Z and C from the fcmp before: the b.eq after needs them
        end = self.base + self.SITE + 4
        stop = self.u.hook_add(uc.UC_HOOK_CODE, lambda u, a, n, d: u.emu_stop(), begin=end, end=end)
        try: self.u.emu_start(self.base + self.SITE, end, count=100)
        finally: self.u.hook_del(stop)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_PC), end)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_NZCV) & 0xf0000000, 0x60000000)
        v = lambda r: struct.unpack('<4f', self.u.reg_read(r).to_bytes(16, 'little'))
        return (struct.unpack('<16f', self.u.mem_read(self.cam + 0x148, 64)), v(arm.UC_ARM64_REG_V4), v(arm.UC_ARM64_REG_V5),
                struct.unpack('<f', self.u.mem_read(self.cam + 0x1d4, 4))[0])

    def test_16_9_is_the_copy_as_it_is(self):
        self.shape(0)
        m, v4, v5, aspect = self.run_copy()
        for a, b in zip(m, self.M): self.assertAlmostEqual(a, b, 5)
        self.assertAlmostEqual(aspect, 16 / 9, 5)

    def test_the_size_set_not_the_shape_asked(self):
        self.shape(0, asked=1.565)   # a window resized while playing: the game still renders 16:9
        m, v4, v5, aspect = self.run_copy()
        for a, b in zip(m, self.M): self.assertAlmostEqual(a, b, 5)
        self.shape(2.2, asked=1.565)
        self.assertAlmostEqual(self.run_copy()[0][5] / self.run_copy()[0][0], 2.2, 3)

    def test_shapes(self):
        for ratio in (2.2, 2.4, 1.43, 1.3, 0.7, 1179 / 2556, 9 / 16, 1.0):
            self.shape(ratio)
            m, v4, v5, aspect = self.run_copy()
            self.assertAlmostEqual(m[5] / m[0], ratio, 3, ratio)                       # the projection's aspect is the screen's
            if ratio > 16 / 9: self.assertAlmostEqual(m[5], self.M[5], 5)            # wider: the vertical angle kept
            elif ratio >= 1: self.assertAlmostEqual(m[0], self.M[0], 5)              # taller landscape: the horizontal angle kept
            elif 1 / ratio >= 16 / 9: self.assertAlmostEqual(m[0], self.M[5], 4)     # a phone upright: its width gets landscape's vertical angle
            else: self.assertAlmostEqual(m[5], self.M[0], 4)                         # an iPad upright: its height gets landscape's horizontal angle
            for a, b in zip(m[8:], self.M[8:]): self.assertAlmostEqual(a, b, 5)      # z and w rows untouched
            for a, b in zip(v4 + v5, m[:8]): self.assertAlmostEqual(a, b, 6)          # the frustum code after reads the same rows
            self.assertAlmostEqual(aspect, ratio, 3, ratio)


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class ShapeChange(unittest.TestCase):
    # The display update's flag load (ldp w11,w9,[x0,#8]) at FN + 0x18; the stub branches back after it
    base, area, state, obj = 0x4000000, 0x4100000, 0x4200000, 0x6000000
    FN = 0x1000; SITE = FN + 0x18

    def setUp(self):
        self.u = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        for a, n in ((self.base, 0x100000), (self.area, 0x10000), (self.state, 0x4000), (self.obj, 0x1000)):
            self.u.mem_map(a, n)
        self.u.mem_write(self.base + self.SITE, struct.pack('<2I', 0x2941240b, 0xd503201f))
        self.cursor = self.area - self.base
        patch.shape_change(types.SimpleNamespace(here=lambda: self.cursor, emit=self.emit, site=lambda off, orig: None, put=self.put, state=self.state - self.base, others={'display update': self.FN}))
        self.u.mem_write(self.obj + 8, struct.pack('<2I', 0x1004, 0x2))   # flags, the mode word

    emit, put = OutputSize.emit, OutputSize.put

    def update(self):
        self.u.reg_write(arm.UC_ARM64_REG_X0, self.obj)
        end = self.base + self.SITE + 4
        stop = self.u.hook_add(uc.UC_HOOK_CODE, lambda u, a, n, d: u.emu_stop(), begin=end, end=end)
        try: self.u.emu_start(self.base + self.SITE, end, count=50)
        finally: self.u.hook_del(stop)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_W9), 0x2)
        return self.u.reg_read(arm.UC_ARM64_REG_W11)

    def test_a_new_shape_recomputes_the_size_once(self):
        self.u.mem_write(self.state + 0xac, struct.pack('<I', 1))   # the shape at start: one re-apply at the first update
        self.assertEqual(self.update(), 0x21004)
        self.assertEqual(self.update(), 0x1004)
        self.u.mem_write(self.state + 0xac, struct.pack('<I', 2))   # a window resized
        self.assertEqual(self.update(), 0x21004)
        self.assertEqual(self.update(), 0x1004)
        self.assertEqual(struct.unpack('<I', self.u.mem_read(self.state + 0xc0, 4))[0], 2)


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class WindowSize(unittest.TestCase):
    # setCrop (mov w8,#1 first) at SITE: win x0, x w1, y w2, w w3, h w4; the stub branches back to SITE + 4
    base, area, win = 0x4000000, 0x4100000, 0x6000000
    SITE = 0x1000

    def setUp(self):
        self.u = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        for a, n in ((self.base, 0x100000), (self.area, 0x10000), (self.win, 0x1000)):
            self.u.mem_map(a, n)
        self.u.mem_write(self.base + self.SITE, struct.pack('<2I', 0x52800028, 0xd503201f))
        self.cursor = self.area - self.base
        patch.window_size(types.SimpleNamespace(here=lambda: self.cursor, emit=self.emit, site=lambda off, orig: None, put=self.put, others={'window crop': self.SITE}))
        self.u.mem_write(self.win + 8, struct.pack('<2f', 2560, 1440))

    emit, put = OutputSize.emit, OutputSize.put

    def crop(self, w, h):
        """setCrop(win, 0, 0, w, h): the window's texture size and its re-create flag after"""
        self.u.mem_write(self.win + 0xc8, b'\0')
        for r, v in ((arm.UC_ARM64_REG_X0, self.win), (arm.UC_ARM64_REG_X1, 0), (arm.UC_ARM64_REG_X2, 0), (arm.UC_ARM64_REG_X3, w), (arm.UC_ARM64_REG_X4, h)): self.u.reg_write(r, v)
        end = self.base + self.SITE + 4
        stop = self.u.hook_add(uc.UC_HOOK_CODE, lambda u, a, n, d: u.emu_stop(), begin=end, end=end)
        try: self.u.emu_start(self.base + self.SITE, end, count=50)
        finally: self.u.hook_del(stop)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_W8), 1)   # the original first instruction ran
        self.assertEqual([self.u.reg_read(r) for r in (arm.UC_ARM64_REG_X3, arm.UC_ARM64_REG_X4)], [w, h])
        return struct.unpack('<2f', self.u.mem_read(self.win + 8, 8)), self.u.mem_read(self.win + 0xc8, 1)[0]

    def test_a_crop_that_fits_changes_nothing(self):
        self.assertEqual(self.crop(2048, 1152), ((2560, 1440), 0))
        self.assertEqual(self.crop(2536, 1152), ((2560, 1440), 0))

    def test_a_larger_crop_grows_the_textures(self):
        self.assertEqual(self.crop(2048, 1576), ((2560, 1576), 1))   # 1.3:1
        self.assertEqual(self.crop(2688, 1152), ((2688, 1576), 1))   # then 21:9: the larger of each
        self.assertEqual(self.crop(2048, 1152), ((2688, 1576), 0))   # back to 16:9: no re-creation


def hud_space(r):
    """The HUD's layout space for a shape: the camera's view of 3840x2160, grown to hold all of it upright"""
    mx, my = camera(r); k = max(mx, my, 1)
    return 3840 * k / mx, 2160 * k / my


def applied(u, state, r):
    """The camera multipliers the output size was made with (state +0xb8 / +0xbc; 0 = 16:9), which the HUD's stubs read"""
    u.mem_write(state + 0xb8, struct.pack('<2f', *camera(r)) if r else bytes(8))


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class HudUpload(unittest.TestCase):
    # The screen draw's uniform block copy: memcpy's return (x0 = the copy) at FN + 0x4c (mov x0,x21); x20 the block, x21 the ring object
    base, area, state, blk, ring = 0x4000000, 0x4100000, 0x4200000, 0x6000000, 0x6100000
    FN = 0x1000; SITE = FN + 0x4c

    def setUp(self):
        self.u = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        for a, n in ((self.base, 0x100000), (self.area, 0x10000), (self.state, 0x4000), (self.blk, 0x1000), (self.ring, 0x1000)):
            self.u.mem_map(a, n)
        self.u.mem_write(self.base + self.SITE, struct.pack('<2I', 0xaa1503e0, 0xd503201f))   # mov x0,x21; nop
        self.cursor = self.area - self.base
        patch.hud(types.SimpleNamespace(here=lambda: self.cursor, emit=self.emit, site=lambda off, orig: None, put=self.put, state=self.state - self.base, others={'hud upload': self.FN}))

    emit, put = OutputSize.emit, OutputSize.put

    @staticmethod
    def block(w=3840.0, h=2160.0, other=None):
        """A screen's block as main+0x773b90 leaves it: its orthographic view-projection, column major (rows x, y, z, w), the y-flipped copy, the size twice"""
        m = (2 / w, 0, 0, 0, 0, 2 / h, 0, 0, 0, 0, -0.5, 0, 0, 0, 0.5, 1)   # columns: (x, y, z, w) of each
        f = (m[0], -m[1], m[2], m[3], m[4], -m[5], m[6], m[7], m[8], -m[9], m[10], m[11], m[12], -m[13], m[14], m[15])
        return struct.pack('<16f16f4f', *m, *f, w, h, *(other or (w, h)))

    def upload(self, blk):
        """Run the stub on a copy of blk's 0x90 bytes; the copy after, as floats, and the block (never written)"""
        self.u.mem_write(self.blk, bytes(8) + blk); self.u.mem_write(self.ring, blk[:0x90])
        for r, v in ((arm.UC_ARM64_REG_X0, self.ring), (arm.UC_ARM64_REG_X20, self.blk), (arm.UC_ARM64_REG_X21, 0x1234)): self.u.reg_write(r, v)
        end = self.base + self.SITE + 4
        stop = self.u.hook_add(uc.UC_HOOK_CODE, lambda u, a, n, d: u.emu_stop(), begin=end, end=end)
        try: self.u.emu_start(self.base + self.SITE, end, count=100)
        finally: self.u.hook_del(stop)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_PC), end)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_X0), 0x1234)   # the original instruction ran
        self.assertEqual(bytes(self.u.mem_read(self.blk + 8, len(blk))), blk)
        return struct.unpack('<36f', self.u.mem_read(self.ring, 0x90))

    def shape(self, r): applied(self.u, self.state, r)

    def test_16_9_is_the_copy_as_it_is(self):
        self.shape(0)
        self.assertEqual(self.upload(self.block()), struct.unpack('<36f', self.block()[:0x90]))

    def test_a_capture_is_left_as_it_is(self):
        self.shape(2.2)
        b = self.block(other=(1984.0, 1184.0))   # main+0x154b660: the capture's own size beside the screen's
        self.assertEqual(self.upload(b), struct.unpack('<36f', b[:0x90]))

    def test_shapes(self):
        for r in (2.2, 2.4, 1.43, 1.3, 1.0, 0.7, 1179 / 2556, 9 / 16):
            self.shape(r)
            c = self.upload(self.block())
            w, h = c[32], c[33]
            self.assertAlmostEqual(w / h, r, 3, r)                                    # the HUD's space has the output's shape
            self.assertEqual((c[34], c[35]), (w, h))
            ew, eh = hud_space(r)
            self.assertAlmostEqual(w, ew, 0, r); self.assertAlmostEqual(h, eh, 0, r)
            self.assertGreaterEqual(w, 3839.9, r); self.assertGreaterEqual(h, 2159.9, r)   # all of the 16:9 space in view
            if r >= 1: self.assertTrue(abs(w - 3840) < 0.5 or abs(h - 2160) < 0.5, r)   # landscape: the HUD keeps its size
            self.assertAlmostEqual(c[0], 2 / w, 8, r); self.assertAlmostEqual(c[5], 2 / h, 8, r)
            self.assertAlmostEqual(c[16], 2 / w, 8, r); self.assertAlmostEqual(c[21], -2 / h, 8, r)   # the y-flipped copy too
            for i in (2, 3, 10, 11, 14, 15, 18, 19, 26, 27, 30, 31):                  # z and w untouched
                self.assertEqual(c[i], struct.unpack('<36f', self.block()[:0x90])[i], (r, i))


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class LayoutSize(unittest.TestCase):
    # The getter (ldr x8,[x0]; add x0,x8,#0x200; ret) at FN; the screen's init at INIT calls it at +0x350 and +0x394
    base, area, state, obj, res = 0x4000000, 0x4100000, 0x4200000, 0x6000000, 0x6100000
    FN, INIT, CALLER = 0x1000, 0x8000, 0x9000

    def setUp(self):
        self.u = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        for a, n in ((self.base, 0x100000), (self.area, 0x10000), (self.state, 0x4000), (self.obj, 0x1000), (self.res, 0x1000)):
            self.u.mem_map(a, n)
        self.u.mem_write(self.base + self.FN, struct.pack('<3I', 0xf9400008, 0x91080100, 0xd65f03c0))
        self.cursor = self.area - self.base
        patch.hud(types.SimpleNamespace(here=lambda: self.cursor, emit=self.emit, site=lambda off, orig: None, put=self.put, state=self.state - self.base,
                                        others={'layout size': self.FN, 'screen init': self.INIT}))
        self.u.mem_write(self.obj, struct.pack('<Q', self.res)); self.u.mem_write(self.res + 0x200, struct.pack('<2f', 3840, 2160))

    emit, put = OutputSize.emit, OutputSize.put

    def get(self, ret):
        """Call the getter with lr = ret (a module offset); the pair x0 points to after"""
        self.u.reg_write(arm.UC_ARM64_REG_X0, self.obj); self.u.reg_write(arm.UC_ARM64_REG_LR, self.base + ret)
        end = self.base + ret
        stop = self.u.hook_add(uc.UC_HOOK_CODE, lambda u, a, n, d: u.emu_stop(), begin=end, end=end)
        try: self.u.emu_start(self.base + self.FN, end, count=100)
        finally: self.u.hook_del(stop)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_PC), end)
        x0 = self.u.reg_read(arm.UC_ARM64_REG_X0)
        return x0, struct.unpack('<2f', self.u.mem_read(x0, 8))

    def test_16_9_is_the_layout_size(self):
        applied(self.u, self.state, 0)
        self.assertEqual(self.get(self.CALLER), (self.res + 0x200, (3840, 2160)))

    def test_the_hud_size_for_conversions(self):
        for r in (2.2, 1.43, 1179 / 2556):
            applied(self.u, self.state, r)
            x0, (w, h) = self.get(self.CALLER)
            self.assertEqual(x0, self.state + 0xd0)
            ew, eh = hud_space(r)
            self.assertAlmostEqual(w, ew, 0, r); self.assertAlmostEqual(h, eh, 0, r)
            self.assertEqual(struct.unpack('<2f', self.u.mem_read(self.res + 0x200, 8)), (3840, 2160))   # the layout file in memory is never written

    def test_the_screen_init_gets_the_layout_size(self):
        applied(self.u, self.state, 2.2)
        for ret in (self.INIT + 0x354, self.INIT + 0x398):   # its projection is widened at each upload
            self.assertEqual(self.get(ret), (self.res + 0x200, (3840, 2160)))


if __name__ == '__main__':
    unittest.main()
