"""Execute Ocarina's emitted frame step stub: the host's step, or the measured frame time when that is longer."""
import importlib
import importlib.util
from pathlib import Path
import struct
import sys
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))

spec = importlib.util.spec_from_file_location('oot_speed', REPO / 'dm/games/0400486024DEC000/patches.py')
assert spec is not None and spec.loader is not None
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)
try:
    unicorn = importlib.import_module('unicorn')
    arm = importlib.import_module('unicorn.arm64_const')
except ImportError:
    unicorn = arm = None

BASE, AREA, STATE, FUNCTION, SITE = 0x4000000, 0x800000, 0x900000, 0x6e6be0, 0x6e6cc4


class Ctx:
    state, base = STATE, FUNCTION

    def __init__(self, machine):
        self.machine, self.cursor, self.sites = machine, AREA, {}
        self.others, self.capabilities, self.render_scale = {}, set(), 0

    def here(self):
        return self.cursor

    def emit(self, word):
        self.machine.mem_write(BASE + self.cursor, struct.pack('<I', word))
        self.cursor += 4

    def site(self, offset, original):
        self.sites[offset] = original

    def put(self, offset, words, reason):
        self.machine.mem_write(BASE + offset, b''.join(struct.pack('<I', w) for w in words))


def float_reg(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def run(constant, rate, seconds, target=60.0, step=1):
    machine = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
    machine.mem_map(BASE, 0xa00000)
    machine.reg_write(arm.UC_ARM64_REG_CPACR_EL1, 3 << 20)   # the FP unit is off at reset
    ctx = Ctx(machine)
    profile.frame_step(ctx)
    machine.mem_write(BASE + STATE, struct.pack('<f', target))
    machine.mem_write(BASE + STATE + 0x20, struct.pack('<I', step))
    machine.mem_write(BASE + STATE + 0x38, struct.pack('<f', seconds))
    machine.reg_write(arm.UC_ARM64_REG_S1, float_reg(constant))
    machine.reg_write(arm.UC_ARM64_REG_S0, float_reg(rate))
    machine.emu_start(BASE + SITE, BASE + SITE + 4, count=32)
    ratio = struct.unpack('<f', struct.pack('<I', machine.reg_read(arm.UC_ARM64_REG_S8) & 0xffffffff))[0]
    mark = struct.unpack('<I', machine.mem_read(BASE + STATE + 0x3c, 4))[0]
    stepped = struct.unpack('<Q', machine.mem_read(BASE + STATE + 0x30, 8))[0]
    return ratio, mark, ctx, machine.reg_read(arm.UC_ARM64_REG_PC), stepped


@unittest.skipIf(unicorn is None, 'optional ARM64 execution dependency: unicorn')
class RealSpeed(unittest.TestCase):
    def test_placed_by_its_function(self):
        self.assertFalse(getattr(profile, 'FRAME_STEP', {}))
        self.assertIs(profile.MATCH_FRAME_STEP['main'][0]['stub'], profile.frame_step)

    def test_no_measurement_keeps_the_hosts_step(self):
        ratio, mark, ctx, pc, stepped = run(60.0, 60.0, 0.0)
        self.assertEqual(ratio, 1.0)
        self.assertEqual(mark, 1)
        self.assertEqual(stepped, 1)
        self.assertEqual(ctx.sites, {SITE: '2818201e'})
        self.assertEqual(pc, BASE + SITE + 4)
        self.assertEqual(run(60.0, 60.0, 0.0, step=2)[0], 2.0)
        self.assertEqual(run(30.0, 60.0, 0.0, step=2)[0], 1.0)

    def test_a_slow_frame_steps_by_its_time(self):
        self.assertAlmostEqual(run(60.0, 60.0, 0.05)[0], 3.0, places=5)
        self.assertAlmostEqual(run(30.0, 30.0, 0.1)[0], 3.0, places=5)
        self.assertAlmostEqual(run(60.0, 60.0, 0.0185)[0], 1.11, places=5)

    def test_never_below_the_hosts_step(self):
        self.assertEqual(run(60.0, 60.0, 0.001)[0], 1.0)
        self.assertEqual(run(60.0, 60.0, 0.02, step=2)[0], 2.0)

    def test_no_target_or_no_step_runs_the_games_division(self):
        for target, step in ((0.0, 1), (60.0, 0)):
            ratio, mark, _, pc, stepped = run(60.0, 30.0, 0.05, target=target, step=step)
            self.assertEqual((ratio, mark, stepped, pc), (2.0, 0, 0, BASE + SITE + 4))


if __name__ == '__main__':
    unittest.main()
