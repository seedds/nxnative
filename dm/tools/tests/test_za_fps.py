"""Execute Z-A's emitted interval hook, including its native graphics setter."""
import importlib.util
import struct
import sys
import types
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
uc: Any = None
arm: Any = None
try:
    uc = importlib.import_module('unicorn')
    arm = importlib.import_module('unicorn.arm64_const')
except ImportError:
    pass

PATCH = Path(__file__).resolve().parents[2] / 'games/0100F43008C44000/patches.py'
BUILD = 'b1f12fd919eae86ab8a978317677e64b'


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class Timing(unittest.TestCase):
    base, area, state = 0x10000000, 0x16000000, 0x17000000
    settings, graphics, pointer, stack = 0x18000000, 0x18001000, 0x18002000, 0x19000000
    site, resume = 0x1b296d8, 0x1b29738

    def setUp(self):
        self.assertTrue(PATCH.is_file(), 'Z-A needs a build-gated frame-step hook')
        spec = importlib.util.spec_from_file_location('za_patches', PATCH)
        assert spec is not None and spec.loader is not None
        self.patch = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.patch)
        self.u = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        for a, n in ((self.base, 0x3200000), (self.area, 0x10000), (self.state, 0x4000),
                     (self.settings, 0x3000), (self.stack, 0x1000)):
            self.u.mem_map(a, n)
        self.cursor = self.area - self.base
        self.sites = {}
        ctx = types.SimpleNamespace(here=lambda: self.cursor, emit=self.emit,
                                    site=self.site_check, put=self.put, state=self.state-self.base)
        self.patch.FRAME_STEP[BUILD](ctx)
        self.u.mem_write(self.base+0x113488, bytes.fromhex('c0035fd6'))
        self.u.mem_write(self.base+0x17fafd4, bytes.fromhex('014001b9c0035fd6'))
        self.u.mem_write(self.pointer, struct.pack('<Q', self.graphics))
        self.u.mem_write(self.settings+0x20, bytes([1, 0]))
        self.u.reg_write(arm.UC_ARM64_REG_SP, self.stack+0x800)
        self.u.reg_write(arm.UC_ARM64_REG_X19, 0x12345678)
        self.u.reg_write(arm.UC_ARM64_REG_X21, 0x87654321)
        self.u.hook_add(uc.UC_HOOK_CODE, self.get_graphics,
                        begin=self.base+0x113488, end=self.base+0x113488)

    def emit(self, word):
        self.u.mem_write(self.base+self.cursor, struct.pack('<I', word))
        self.cursor += 4

    def site_check(self, offset, original):
        self.sites[offset] = original

    def put(self, offset, words, reason):
        self.u.mem_write(self.base+offset, struct.pack('<'+'I'*len(words), *words))

    def get_graphics(self, u, address, size, data):
        u.reg_write(arm.UC_ARM64_REG_X0, self.pointer)

    def run_step(self, step, target=60):
        self.u.mem_write(self.state, struct.pack('<f', target))
        self.u.mem_write(self.state+0x20, struct.pack('<I', step))
        self.u.reg_write(arm.UC_ARM64_REG_X9, self.settings)
        self.u.reg_write(arm.UC_ARM64_REG_W8, 4)  # the game's measured late-frame estimate
        end = self.base+(self.resume if target else self.site+4)
        self.u.emu_start(self.base+self.site, end, count=200)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_PC), end)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_X19), 0x12345678)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_X21), 0x87654321)
        return self.u.reg_read(arm.UC_ARM64_REG_W20)

    def stepped(self):
        return struct.unpack('<Q', self.u.mem_read(self.state+0x30, 8))[0]

    def test_only_verified_build_and_original_instruction(self):
        self.assertEqual(set(self.patch.FRAME_STEP), {BUILD})
        self.assertEqual(self.sites[self.site], '3481c039')

    def test_sixty_and_thirty_advance_one_second(self):
        # The native clock adapters consume a zero-based index into this table.
        # Reverting to the original interval floor makes the 60 fps case run 2x.
        native_ns = (16666667, 33333334, 50000000)
        for target, step, frames in ((60, 1, 60), (30, 2, 30)):
            total = 0
            for _ in range(frames):
                mode = self.run_step(step, target)
                self.assertEqual(mode, step-1)
                self.assertEqual(struct.unpack('<I', self.u.mem_read(self.graphics+0x140, 4))[0], mode)
                total += native_ns[mode]
            self.assertLess(abs(total-1000000000), 100)

    def test_variable_steps_reach_native_clock_and_counter(self):
        for step in (1, 2, 3, 2, 1)*20:
            self.assertEqual(self.run_step(step), step-1)
        self.assertEqual(self.stepped(), 180)

    def test_startup_uses_selected_cap_before_host_sample(self):
        self.assertEqual(self.run_step(0, 60), 0)
        self.assertEqual(self.run_step(0, 30), 1)
        self.assertEqual(self.stepped(), 3)

    def test_disabled_preserves_original_signed_mode_and_registers(self):
        self.u.reg_write(arm.UC_ARM64_REG_X16, 0x1234)
        self.u.reg_write(arm.UC_ARM64_REG_X17, 0x5678)
        self.u.reg_write(arm.UC_ARM64_REG_NZCV, 0xa0000000)
        self.u.mem_write(self.settings+0x20, b'\xff')
        self.assertEqual(self.run_step(3, 0), 0xffffffff)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_X16), 0x1234)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_X17), 0x5678)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_NZCV), 0xa0000000)
        self.assertEqual(self.stepped(), 0)


if __name__ == '__main__':
    unittest.main()
