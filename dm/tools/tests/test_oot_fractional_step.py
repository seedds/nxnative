"""Execute Ocarina's emitted frame step stub: the game's step, or the measured one when that is longer."""
import importlib
import importlib.util
from pathlib import Path
import struct
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

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


def run(constant, target, seconds, override=0.):
    assert unicorn is not None and arm is not None
    machine = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
    machine.mem_map(BASE, 0xa00000)
    machine.reg_write(arm.UC_ARM64_REG_CPACR_EL1, 3 << 20)   # the FP unit is off at reset
    machine.reg_write(arm.UC_ARM64_REG_NZCV, 0xa0000000)
    for n in range(31):
        machine.reg_write(getattr(arm, f'UC_ARM64_REG_X{n}'), 0x100000000000 + n)
    for n in range(32):
        machine.reg_write(getattr(arm, f'UC_ARM64_REG_Q{n}'), (0xabcdef1234567890 << 64) | n)
    ctx = Ctx(machine)
    ctx.build_id = profile.fractional_build
    ctx.experimental_fractional = True
    ctx.others = {}
    profile.frame_step(ctx)
    assert ctx.fractional == 2
    machine.mem_write(BASE + STATE + 0x38, struct.pack('<f', seconds))
    machine.mem_write(BASE + STATE + 0x40, struct.pack("<I", 1))  # DRS must stay separate
    machine.mem_write(BASE + STATE + 0x44, struct.pack('<f', override))
    machine.reg_write(arm.UC_ARM64_REG_S1, float_reg(constant))
    machine.reg_write(arm.UC_ARM64_REG_S0, float_reg(target))
    preserved_regs = [getattr(arm, f'UC_ARM64_REG_X{n}') for n in range(31) if n not in (16,17)] + [getattr(arm, f'UC_ARM64_REG_Q{n}') for n in range(32) if n not in (0,8)]
    before_regs = {r: machine.reg_read(r) for r in preserved_regs}
    writes = []
    machine.hook_add(unicorn.UC_HOOK_MEM_WRITE, lambda u, access, address, size, value, data: writes.append((address,size,value)))
    machine.emu_start(BASE + SITE, BASE + SITE + 4, count=24)
    assert before_regs == {r: machine.reg_read(r) for r in preserved_regs}, 'Unexpected register clobber'
    assert machine.mem_read(BASE + STATE + 0x40, 4) == struct.pack("<I", 1)
    assert writes == [(BASE + STATE + 0x3c, 4, 2)], writes
    assert machine.reg_read(arm.UC_ARM64_REG_NZCV) == 0xa0000000, 'NZCV clobbered'
    step = struct.unpack('<f', struct.pack('<I', machine.reg_read(arm.UC_ARM64_REG_S8) & 0xffffffff))[0]
    mark = struct.unpack('<I', machine.mem_read(BASE + STATE + 0x3c, 4))[0]
    return step, mark, ctx, machine.reg_read(arm.UC_ARM64_REG_PC)


class Gating(unittest.TestCase):
    def test_opt_in_and_build_gate_preserve_dynamic_resolution(self):
        for enabled, build in ((False, profile.fractional_build), (True, "unknown"), (True, profile.fractional_build)):
            ctx = SimpleNamespace(experimental_fractional=enabled, build_id=build, base=0x6e6be0, state=STATE,
                                  here=lambda: AREA, emit=Mock(), site=Mock(), put=Mock())
            with patch.object(profile, 'real_speed') as fractional, patch.object(profile, 'dynamic_resolution') as drs:
                profile.frame_step(ctx)
                self.assertEqual(fractional.call_count, int(enabled and build == profile.fractional_build))
                drs.assert_called_once_with(ctx)


@unittest.skipIf(unicorn is None, 'optional ARM64 execution dependency: unicorn')
class RealSpeed(unittest.TestCase):
    def test_high_rate_without_native_budget_change(self):
        for constant in (30., 60.):
            for native in (30., 60.):
                for actual in (30., 60., 90., 100., 120.):
                    with self.subTest(constant=constant, native=native, actual=actual):
                        step, mark, _, pc = run(constant, native, 1./actual, 120.)
                        self.assertAlmostEqual(step * actual / constant, 1., places=6)
                        self.assertEqual(mark, 2)
                        self.assertEqual(pc, BASE + SITE + 4)

    def test_first_frame_and_fallback(self):
        self.assertEqual(run(60.,60.,0.,120.)[0], .5)
        self.assertEqual(run(60.,60.,0.,60.)[0], 1.)
        self.assertEqual(run(60.,60.,0.,30.)[0], 2.)
        self.assertEqual(run(60.,30.,0.,0.)[0], 2.)

    def test_registered_build(self):
        self.assertEqual(profile.fractional_build, "e0f3ccfaf50065ae4ea01eb5aeb13b1a")

    def test_no_measurement_keeps_the_games_step(self):
        step, mark, ctx, pc = run(60.0, 60.0, 0.0)
        self.assertEqual(step, 1.0)
        self.assertEqual(mark, 2)
        self.assertEqual(ctx.sites, {SITE: '2818201e'})
        self.assertEqual(pc, BASE + SITE + 4)

    def test_a_slow_frame_steps_by_its_time(self):
        self.assertAlmostEqual(run(60.0, 60.0, 0.05)[0], 3.0, places=5)
        self.assertAlmostEqual(run(30.0, 30.0, 0.1)[0], 3.0, places=5)

    def test_never_below_the_games_step(self):
        self.assertEqual(run(60.0, 60.0, 0.001)[0], 1.0)


if __name__ == '__main__':
    unittest.main()
