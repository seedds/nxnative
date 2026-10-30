"""Execute BOTW's emitted camera writer on ARM64, without game files.

Optional dependency: unicorn (macOS JIT needs execution outside the sandbox).
"""
import importlib
import importlib.util
from pathlib import Path
import struct
import sys
import types
import unittest
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
try:
    uc: Any = importlib.import_module('unicorn')
    arm: Any = importlib.import_module('unicorn.arm64_const')
except ImportError:
    uc = arm = None

spec = importlib.util.spec_from_file_location('botw_patches', Path(__file__).resolve().parents[2] / 'games/01007EF00011E000/patches.py')
assert spec is not None and spec.loader is not None
patch = importlib.util.module_from_spec(spec); spec.loader.exec_module(patch)


class BuildGate(unittest.TestCase):
    def test_only_verified_main_build_advertises_camera_hook(self):
        builds = {bid for bid, names in patch.CAPABILITIES.items() if 'CAMERA_ASPECT' in names}
        self.assertEqual(builds, {'cd57b23fa4bbad65803d9788c01821ee'})
        self.assertTrue(all(bid in patch.FRAME_STEP for bid in builds))


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class CameraWriter(unittest.TestCase):
    base, site, area, state, obj = 0x4000000, 0x10edae0, 0x1f00000, 0x1e00000, 0x7000000

    def setUp(self):
        self.u = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        self.u.mem_map(self.base, 0x2000000)
        self.u.mem_map(self.obj, 0x4000)
        self.cursor = self.area
        self.original = b''
        ctx = types.SimpleNamespace(here=lambda: self.cursor, emit=self.emit, site=self.check_site,
                                    put=self.put, state=self.state, render_scale=0)
        # Exercise the registry's full entry, not a separate test implementation.
        patch.FRAME_STEP['cd57b23fa4bbad65803d9788c01821ee'](ctx)
        self.assertEqual(len(self.original), 13 * 4)
        self.end = self.base + self.site + len(self.original)
        self.u.mem_write(self.obj, bytes([0xa5]) * 0x4000)
        for reg, value in ((arm.UC_ARM64_REG_X19, self.obj), (arm.UC_ARM64_REG_X20, self.obj+0x1000),
                           (arm.UC_ARM64_REG_X21, self.obj+0x2000), (arm.UC_ARM64_REG_W22, 1),
                           (arm.UC_ARM64_REG_X16, 0x12345678), (arm.UC_ARM64_REG_X17, 0x87654321),
                           (arm.UC_ARM64_REG_NZCV, 0xa0000000)):
            self.u.reg_write(reg, value)
        self.u.mem_write(self.obj+0x106c, struct.pack('<2f', 0.25, 7500))
        # Rectangle x0,y0,x1,y1 = 10,20,1290,740 => 16:9, not x1/y1.
        self.u.mem_write(self.obj+0x2038, struct.pack('<4f', 10, 20, 1290, 740))

    def emit(self, word):
        self.u.mem_write(self.base+self.cursor, struct.pack('<I', word)); self.cursor += 4

    def check_site(self, offset, original):
        if offset == self.site:
            self.original = bytes.fromhex(original)
            self.assertEqual(len(self.original), 13*4)

    def put(self, offset, words, why):
        self.u.mem_write(self.base+offset, struct.pack('<'+'I'*len(words), *words))

    def desired(self, ratio):
        self.u.mem_write(self.base+self.state+0x40, struct.pack('<f', ratio))

    def read_float(self, address):
        return struct.unpack('<f', self.u.mem_read(address, 4))[0]

    def execute(self):
        stop = self.u.hook_add(uc.UC_HOOK_CODE, lambda u, a, n, d: u.emu_stop(), begin=self.end, end=self.end)
        try:
            self.u.emu_start(self.base+self.site, self.end, count=100)
        finally:
            self.u.hook_del(stop)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_PC), self.end)
        self.assertEqual(self.read_float(self.obj+0xa8), 0.25)
        self.assertEqual(self.read_float(self.obj+0xac), 7500)
        self.assertEqual(self.u.mem_read(self.obj+0x18, 1), b'\x01')
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_NZCV), 0xa0000000)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_X16), 0x12345678)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_X17), 0x87654321)
        return self.read_float(self.obj+0xc0), self.read_float(self.base+self.state+0x44)

    def test_override_tracks_updates_and_disable_acknowledges_original(self):
        self.assertEqual(self.read_float(self.base+self.state+0x44), 0)
        for ratio in (21/9, 32/9, 0, 2.16, 0):
            self.desired(ratio)
            aspect, acknowledged = self.execute()
            self.assertAlmostEqual(aspect, ratio or 16/9, places=6)
            self.assertAlmostEqual(acknowledged, ratio, places=6)

    def test_disabled_writer_matches_all_original_observable_state(self):
        self.desired(0)
        initial = self.u.context_save()
        image = bytes(self.u.mem_read(self.obj, 0x4000))
        preserved = [getattr(arm, f'UC_ARM64_REG_X{i}') for i in range(31) if i not in (8, 9)]
        patched_integer_regs = [self.u.reg_read(reg) for reg in preserved]
        self.execute()
        self.assertEqual([self.u.reg_read(reg) for reg in preserved], patched_integer_regs)
        patched_image = bytes(self.u.mem_read(self.obj, 0x4000))
        patched_regs = [self.u.reg_read(getattr(arm, f'UC_ARM64_REG_Q{i}')) for i in range(32)]
        self.u.context_restore(initial)
        self.u.mem_write(self.obj, image)
        # Run the asserted original instructions in a separate, never-translated block.
        baseline = self.base+self.area+0x10000
        self.u.mem_write(baseline, self.original)
        self.u.emu_start(baseline, baseline+len(self.original), count=13)
        self.assertEqual([self.u.reg_read(reg) for reg in preserved], patched_integer_regs)
        self.assertEqual(bytes(self.u.mem_read(self.obj, 0x4000)), patched_image)
        self.assertEqual([self.u.reg_read(getattr(arm, f'UC_ARM64_REG_Q{i}')) for i in range(32)], patched_regs)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_NZCV), 0xa0000000)

    def test_acknowledgement_follows_projection_store(self):
        self.desired(21/9)
        observed = []
        def write(u, access, address, size, value, data):
            if address == self.base+self.state+0x44:
                observed.append(self.read_float(self.obj+0xc0))
        self.u.hook_add(uc.UC_HOOK_MEM_WRITE, write)
        self.execute()
        self.assertEqual(len(observed), 1)
        self.assertAlmostEqual(observed[0], 21/9, places=6)


if __name__ == '__main__': unittest.main()
