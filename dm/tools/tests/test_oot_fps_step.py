"""Execute Ocarina of Time's emitted simulation step without game files."""

from importlib import import_module
import importlib.util
from pathlib import Path
import struct
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

PROFILE = TOOLS.parent / "games/0400486024DEC000/patches.py"
SPEC = importlib.util.spec_from_file_location("oot_patches", PROFILE)
assert SPEC is not None and SPEC.loader is not None
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)


class SimulationStep(unittest.TestCase):
    base, hook, stub, state = 0x10000000, 0x6e6c98, 0x5100190, 0x4ea4000
    obj, stack = 0x18000000, 0x18001000

    def setUp(self):
        try:
            uc = import_module("unicorn")
            self.arm = import_module("unicorn.arm64_const")
        except ImportError:
            self.skipTest("optional ARM64 execution dependency: unicorn")
        self.cpu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        for address in (self.base + (self.hook & ~0xfff), self.base + (self.stub & ~0xfff),
                        self.base + self.state, self.obj, self.stack):
            self.cpu.mem_map(address, 4096)
        self.cursor = self.stub
        self.sites, self.cuts = [], []
        PATCH.FRAME_STEP["e0f3ccfaf50065ae4ea01eb5aeb13b1a"](self)
        self.assertEqual(self.sites, [(self.hook, "a0ca42bd")])
        self.assertEqual(self.cuts, [self.hook])
        self.end = self.base + self.hook + 4
        self.cpu.hook_add(uc.UC_HOOK_CODE, lambda cpu, *_: cpu.emu_stop(),
                          begin=self.end, end=self.end)

    def here(self):
        return self.cursor

    def site(self, offset, original):
        self.sites.append((offset, original))

    def emit(self, word):
        self.cpu.mem_write(self.base + self.cursor, struct.pack("<I", word))
        self.cursor += 4

    def put(self, offset, words, _reason):
        self.cuts.append(offset)
        self.cpu.mem_write(self.base + offset, struct.pack("<" + "I" * len(words), *words))

    def stepped_vsyncs(self):
        return struct.unpack("<Q", self.cpu.mem_read(self.base + self.state + 0x30, 8))[0]

    def run_step(self, target, vsyncs, original_fps=59):
        arm = self.arm
        self.cpu.mem_write(self.base + self.state, struct.pack("<f", target))
        self.cpu.mem_write(self.base + self.state + 0x20, struct.pack("<I", vsyncs))
        self.cpu.mem_write(self.obj + 0x2c8, struct.pack("<I", original_fps))
        preserved = {
            arm.UC_ARM64_REG_X8: 0x123456789,
            arm.UC_ARM64_REG_X15: 0x987654321,
            arm.UC_ARM64_REG_X16: 0x345678912,
            arm.UC_ARM64_REG_X17: 0x765432198,
            arm.UC_ARM64_REG_X21: self.obj,
            arm.UC_ARM64_REG_SP: self.stack + 0x800,
            arm.UC_ARM64_REG_NZCV: 0xa0000000,
        }
        for register, value in preserved.items():
            self.cpu.reg_write(register, value)
        self.cpu.emu_start(self.base + self.hook, self.end, count=64)
        self.assertEqual(self.cpu.reg_read(arm.UC_ARM64_REG_PC), self.end)
        for register, value in preserved.items():
            self.assertEqual(self.cpu.reg_read(register), value)
        # The unchanged guest SCVTF consumes the integer bits in s0.
        return self.cpu.reg_read(arm.UC_ARM64_REG_S0)

    def test_target_zero_keeps_the_original_fps(self):
        for step, original in ((1, 59), (3, 47)):
            self.assertEqual(self.run_step(0, step, original), original)
        self.assertEqual(self.stepped_vsyncs(), 0)

    def test_first_step_follows_the_requested_target(self):
        self.assertEqual(self.run_step(30, 0), 30)
        self.assertEqual(self.stepped_vsyncs(), 2)
        self.assertEqual(self.run_step(60, 0), 60)
        self.assertEqual(self.stepped_vsyncs(), 3)

    def test_mixed_steps_keep_exact_integer_fps_and_vsync_count(self):
        for vsyncs in (1, 2, 1, 3, 2):
            self.assertEqual(self.run_step(60, vsyncs), 60 // vsyncs)
        self.assertEqual(self.stepped_vsyncs(), 9)
        self.assertEqual(self.run_step(0, 2, 60), 60)
        self.assertEqual(self.stepped_vsyncs(), 9)
