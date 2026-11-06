"""Execute Ocarina's emitted frame step stub: the game's step, or the measured one when that is longer."""
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

BASE, AREA, STATE, SITE = 0x4000000, 0x800000, 0x900000, 0x6e6cc4


class Ctx:
    state = STATE

    def __init__(self, machine):
        self.machine, self.cursor, self.sites = machine, AREA, {}

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


def run(constant, target, seconds):
    machine = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
    machine.mem_map(BASE, 0xa00000)
    machine.reg_write(arm.UC_ARM64_REG_CPACR_EL1, 3 << 20)   # the FP unit is off at reset
    ctx = Ctx(machine)
    profile.real_speed(ctx)
    machine.mem_write(BASE + STATE + 0x38, struct.pack('<f', seconds))
    machine.reg_write(arm.UC_ARM64_REG_S1, float_reg(constant))
    machine.reg_write(arm.UC_ARM64_REG_S0, float_reg(target))
    machine.emu_start(BASE + SITE, BASE + SITE + 4, count=16)
    step = struct.unpack('<f', struct.pack('<I', machine.reg_read(arm.UC_ARM64_REG_S8) & 0xffffffff))[0]
    mark = struct.unpack('<I', machine.mem_read(BASE + STATE + 0x3c, 4))[0]
    return step, mark, ctx, machine.reg_read(arm.UC_ARM64_REG_PC)


@unittest.skipIf(unicorn is None, 'optional ARM64 execution dependency: unicorn')
class RealSpeed(unittest.TestCase):
    def test_registered_build(self):
        self.assertEqual(profile.FRAME_STEP, {'e0f3ccfaf50065ae4ea01eb5aeb13b1a': profile.real_speed})

    def test_no_measurement_keeps_the_games_step(self):
        step, mark, ctx, pc = run(60.0, 60.0, 0.0)
        self.assertEqual(step, 1.0)
        self.assertEqual(mark, 1)
        self.assertEqual(ctx.sites, {SITE: '2818201e'})
        self.assertEqual(pc, BASE + SITE + 4)

    def test_a_slow_frame_steps_by_its_time(self):
        self.assertAlmostEqual(run(60.0, 60.0, 0.05)[0], 3.0, places=5)
        self.assertAlmostEqual(run(30.0, 30.0, 0.1)[0], 3.0, places=5)

    def test_never_below_the_games_step(self):
        self.assertEqual(run(60.0, 60.0, 0.001)[0], 1.0)


if __name__ == '__main__':
    unittest.main()
