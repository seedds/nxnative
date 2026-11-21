"""Execute Ocarina of Time's frame-step stub (patches.py frame_step): the host's step × base / 60.

Runs the emitted code under Unicorn (an optional dependency). Base 30 is the framework's half rate, two calcs a drawn frame (bit 0
of its +0x24f the mode, bit 1 the calc that is not drawn): each calc takes half the frame's step, and the frame's vsyncs are
counted once. PR #1996 took the step alone there, twice the speed. In rewritten code x17 does not survive a store, so the
`clobber_x17` runs zero it after every store, as the rewriter can: a stub that keeps something in it across one fails here.
The harness is HookLink's (dsi-hub).
"""
import importlib
import importlib.util
from pathlib import Path
import struct
import sys
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))

spec = importlib.util.spec_from_file_location('oot_patches', REPO / 'dm/games/0400486024DEC000/patches.py')
assert spec is not None and spec.loader is not None
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)
try:
    unicorn = importlib.import_module('unicorn')
    arm = importlib.import_module('unicorn.arm64_const')
except ImportError:
    unicorn = arm = None

BASE, FUNC, AREA, STATE, FRAMEWORK = 0x4000000, 0x6e6be0, 0x800000, 0x900000, 0x700000
SITE = FUNC + 0xe4


class Ctx:
    state = STATE
    base = FUNC

    def __init__(self, machine):
        self.machine, self.cursor, self.sites, self.others, self.capabilities = machine, AREA, {}, {}, set()

    def here(self):
        return self.cursor

    def emit(self, word):
        self.machine.mem_write(BASE + self.cursor, struct.pack('<I', word))
        self.cursor += 4

    def site(self, offset, original):
        self.sites[offset] = original

    def put(self, offset, words, reason):
        self.machine.mem_write(BASE + offset, b''.join(struct.pack('<I', w) for w in words))


def f32(v):
    return struct.unpack('<I', struct.pack('<f', v))[0]


def run(base_rate, game_rate, target, step, clobber_x17=False, half_rate=0, stepped=0):
    """One pass of the site: (ratio, vsyncs counted, pc). half_rate: the framework's +0x24f (0, or 3 / 1 for the two calcs of a half-rate frame)"""
    assert unicorn is not None and arm is not None
    m = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
    m.mem_map(BASE, 0xa00000)
    m.reg_write(arm.UC_ARM64_REG_CPACR_EL1, 3 << 20)   # the FP unit is off at reset
    ctx = Ctx(m)
    profile.frame_step(ctx)
    m.mem_write(BASE + STATE + 0x00, struct.pack('<f', target))
    m.mem_write(BASE + STATE + 0x20, struct.pack('<I', step))
    m.mem_write(BASE + STATE + 0x30, struct.pack('<Q', stepped))
    m.mem_write(BASE + FRAMEWORK + 0x24f, bytes([half_rate]))
    m.reg_write(arm.UC_ARM64_REG_X21, BASE + FRAMEWORK)
    m.reg_write(arm.UC_ARM64_REG_S1, f32(base_rate))
    m.reg_write(arm.UC_ARM64_REG_S0, f32(game_rate))
    if clobber_x17:
        x17 = arm.UC_ARM64_REG_X17

        def after_store(uc, access, address, size, value, data):   # the rewriter's store sequence may leave x17 = 0
            uc.reg_write(x17, 0)
        m.hook_add(unicorn.UC_HOOK_MEM_WRITE, after_store)
    m.emu_start(BASE + SITE, BASE + SITE + 4, count=64)
    ratio = struct.unpack('<f', struct.pack('<I', m.reg_read(arm.UC_ARM64_REG_S8) & 0xffffffff))[0]
    return ratio, struct.unpack('<Q', m.mem_read(BASE + STATE + 0x30, 8))[0], m.reg_read(arm.UC_ARM64_REG_PC)


@unittest.skipIf(unicorn is None, 'optional ARM64 execution dependency: unicorn')
class FrameStep(unittest.TestCase):
    def test_at_full_rate_the_ratio_is_the_hosts_step_and_every_calc_counts(self):
        for step in (1, 2, 3):
            self.assertEqual(run(60.0, 60.0, 60.0, step, stepped=7), (float(step), 7 + step, BASE + SITE + 4))

    def test_half_rate_takes_half_the_step_at_each_of_its_two_calcs(self):
        for step in (1, 2, 3):   # 30 / 30 = 1 twice a frame on the console; here the two calcs of a frame add up to its step
            first, stepped, _ = run(30.0, 30.0, 60.0, step, half_rate=3)
            self.assertEqual((first, stepped), (step / 2, 0))   # the calc that is not drawn does not count the frame's vsyncs
            second, stepped, _ = run(30.0, 30.0, 60.0, step, half_rate=1, stepped=stepped)
            self.assertEqual((first + second, stepped), (step, step))   # counted once, at the drawn calc

    def test_x17_lost_after_a_store_is_survived(self):
        for base_rate, bits, ratio in ((60.0, 0, 2.0), (30.0, 3, 1.0), (30.0, 1, 1.0)):
            self.assertEqual(run(base_rate, base_rate, 60.0, 2, clobber_x17=True, half_rate=bits)[::2], (ratio, BASE + SITE + 4))

    def test_target_zero_runs_the_games_division(self):
        self.assertEqual(run(60.0, 30.0, 0.0, 0), (2.0, 0, BASE + SITE + 4))

    def test_step_not_decided_yet_runs_the_games_division(self):   # the first frames
        self.assertEqual(run(60.0, 30.0, 60.0, 0), (2.0, 0, BASE + SITE + 4))
        self.assertEqual(run(30.0, 30.0, 60.0, 0, half_rate=3), (1.0, 0, BASE + SITE + 4))


if __name__ == '__main__':
    unittest.main()
