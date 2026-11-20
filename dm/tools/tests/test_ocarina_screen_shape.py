"""Execute Ocarina of Time's screen-shape stub for the movie's viewport on ARM64, without game files: the title screen's background and the opening are a 16:9
movie drawn into the whole frame, so on a taller screen the stub keeps its viewport 16:9 about the centre, and leaves it alone at 16:9 or with the shape off.

Optional dependency: unicorn. On macOS its JIT needs execution outside the sandbox.
"""
import importlib.util
from pathlib import Path
import struct
import sys
import types
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
try:
    import unicorn as uc
    from unicorn import arm64_const as arm
except ImportError:
    uc = arm = None

spec = importlib.util.spec_from_file_location('ocarina_patches', Path(__file__).resolve().parents[2] / 'games/0400486024DEC000/patches.py')
patch = importlib.util.module_from_spec(spec); spec.loader.exec_module(patch)
OTHERS = patch.MATCH_FRAME_STEP['main'][0]['others']


class Placement(unittest.TestCase):
    def test_movie_draw_site(self):
        # the movie's draw is one of the screen shape's functions, and its stub replaces the instruction after the viewport is built (ldp x9, x8, [x19, #0x18] at +0x64)
        self.assertEqual(OTHERS['movie draw']['size'], 0x158)
        sites = {}
        ctx = types.SimpleNamespace(here=lambda: 0x8000000, emit=lambda w: None, site=lambda off, orig: sites.__setitem__(off, orig), put=lambda off, words, why: None,
                                    state=0x9000000, render_scale=0, step_max=0, base=0x100000, others={k: None for k in OTHERS} | {'movie draw': 0x200000}, capabilities=set())
        patch.screen_shape(ctx)
        self.assertEqual(sites, {0x200064: '69a241a9'})


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class MovieViewport(unittest.TestCase):
    base, area, state, frame = 0x4000000, 0x2000000, 0x9000000, 0xa000000
    fn = 0x3b5c710

    def setUp(self):
        self.u = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        for a, n in ((self.base, 0x4000000), (self.state, 0x1000), (self.frame, 0x1000)):
            self.u.mem_map(a, n)
        self.cursor = self.area
        ctx = types.SimpleNamespace(here=lambda: self.cursor, emit=self.emit, site=lambda off, orig: None, put=self.put, state=self.state - self.base,
                                    render_scale=0, step_max=0, base=0, others={k: None for k in OTHERS} | {'movie draw': self.fn}, capabilities=set())
        patch.screen_shape(ctx)
        self.u.mem_write(self.base + self.fn + 0x68, struct.pack('<I', 0xd503201f))   # nop: where the stub returns

    def emit(self, w):
        self.u.mem_write(self.base + self.cursor, struct.pack('<I', w)); self.cursor += 4

    def put(self, off, words, why):
        self.u.mem_write(self.base + off, struct.pack('<' + 'I' * len(words), *words))

    def run_viewport(self, oh, rect):
        self.u.mem_write(self.state + 0x48, struct.pack('<I', oh))
        self.u.mem_write(self.frame + 0x100 - 0x28 + 8, struct.pack('<4f', *rect))
        self.u.reg_write(arm.UC_ARM64_REG_X29, self.frame + 0x100)
        self.u.reg_write(arm.UC_ARM64_REG_X19, self.frame + 0x800)
        end = self.base + self.fn + 0x68
        self.u.emu_start(self.base + self.fn + 0x64, end, count=100)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_PC), end)
        return struct.unpack('<4f', self.u.mem_read(self.frame + 0x100 - 0x28 + 8, 16))

    def test_taller_screens_keep_the_movie_16_9(self):
        self.assertEqual(self.run_viewport(1440, (0, 0, 2560, 1920)), (0, 240, 2560, 1680))   # 4:3: bars of 240 above and below
        self.assertEqual(self.run_viewport(1200, (0, 0, 2560, 1600)), (0, 80, 2560, 1520))    # 16:10
        self.assertEqual(self.run_viewport(1280, (0, 0, 1920, 1280)), (0, 100, 1920, 1180))   # 3:2 at 1920 wide

    def test_16_9_and_shape_off_stay(self):
        self.assertEqual(self.run_viewport(1440, (0, 0, 2560, 1440)), (0, 0, 2560, 1440))     # a 16:9 frame
        self.assertEqual(self.run_viewport(0, (0, 0, 2560, 1920)), (0, 0, 2560, 1920))        # shape off (oh 0): the game's own


if __name__ == '__main__':
    unittest.main()
