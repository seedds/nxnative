"""Replay the emitted game patch at its native Add/ExecuteAndWait boundary."""
import importlib
import importlib.util
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
import unittest

try:
    uc = importlib.import_module('unicorn')
except ImportError:
    uc = None

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'dm/tools'))
spec = importlib.util.spec_from_file_location('dream_buffet_patches', ROOT / 'dm/games/0100A8E016236000/patches.py')
assert spec is not None and spec.loader is not None
patches = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patches)

CODE, ADD, EXECUTE, STOP = 0x100000, 0x110000, 0x120000, 0x130000
DATA, STACK = 0x200000, 0x300000


@unittest.skipIf(uc is None, 'instruction replay needs unicorn')
class Initialization(unittest.TestCase):
    def setUp(self):
        assert uc is not None
        self.arm = importlib.import_module('unicorn.arm64_const')
        self.vm = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        for address, size in ((CODE, 0x40000), (DATA, 0x10000), (STACK, 0x10000)):
            self.vm.mem_map(address, size)
        self.context, self.args, self.owner, self.instance = DATA + 0x1000, DATA + 0x2000, DATA + 0x3000, DATA + 0x4020
        self.put(DATA, self.context)
        self.put(self.context + 0x88, self.args)
        self.put(self.args, self.owner)
        self.put(self.args + 8, self.instance)
        self.put(self.args + 16, DATA + 0x7000)
        self.put(self.instance - 8, DATA + 0x5000)
        self.put(DATA + 0x5008, DATA + 0x6000)
        self.vm.mem_write(ADD, bytes.fromhex('ff0302d1'))  # original first instruction: sub sp,sp,#0x80
        self.vm.mem_write(EXECUTE, bytes.fromhex('fd7bbda9'))
        words = []
        def put_site(address, instructions, reason):
            self.vm.mem_write(address, struct.pack('<' + 'I' * len(instructions), *instructions))
        ctx = SimpleNamespace(base=ADD, others={'execute and wait': EXECUTE, 'task runner': 0x140000},
                              here=lambda: CODE + 4 * len(words), emit=words.append,
                              site=lambda address, original: self.assertEqual(bytes(self.vm.mem_read(address, len(bytes.fromhex(original)))), bytes.fromhex(original)),
                              put=put_site)
        patches.initialize_base_system(ctx)
        if words:
            self.vm.mem_write(CODE, struct.pack('<' + 'I' * len(words), *words))
        self.pending, self.executed = [], []
        self.base_ready = False
        self.vm.hook_add(uc.UC_HOOK_CODE, self.native_call)

    def put(self, address, value):
        self.vm.mem_write(address, struct.pack('<Q', value))

    def string(self, address):
        return bytes(self.vm.mem_read(address, 128)).split(b'\0', 1)[0].decode()

    def native_call(self, vm, pc, size, user):
        if pc not in (ADD + 4, EXECUTE):
            return
        self.assertEqual(vm.reg_read(self.arm.UC_ARM64_REG_SP) % 16, 0)
        self.assertEqual(vm.reg_read(self.arm.UC_ARM64_REG_X0), DATA)
        if pc == ADD + 4:
            self.pending.append(self.string(DATA + 0x7000))
            vm.reg_write(self.arm.UC_ARM64_REG_SP, vm.reg_read(self.arm.UC_ARM64_REG_SP) + 0x80)
            vm.reg_write(self.arm.UC_ARM64_REG_X0, 0xdead)  # Add does not return its original context.
        else:
            # An allowed adverse schedule: Info consumes its dependency first
            # if both jobs were incorrectly left in the same parallel batch.
            for task in sorted(self.pending, key=lambda name: name != 'InitializeInfo'):
                if task == 'InitializeInfo':
                    self.assertTrue(self.base_ready, 'Info cached a null GamePauseManager before BaseSystem finished')
                if task == 'InitializeBaseSystem':
                    self.base_ready = True
                self.executed.append(task)
            self.pending.clear()
        vm.reg_write(self.arm.UC_ARM64_REG_PC, vm.reg_read(self.arm.UC_ARM64_REG_LR))

    def call(self, address):
        saved = {index: 0x900000 + index for index in range(19, 30)}
        for index, value in saved.items():
            self.vm.reg_write(getattr(self.arm, f'UC_ARM64_REG_X{index}'), value)
        self.vm.reg_write(self.arm.UC_ARM64_REG_X0, DATA)
        self.vm.reg_write(self.arm.UC_ARM64_REG_SP, STACK + 0xfff0)
        self.vm.reg_write(self.arm.UC_ARM64_REG_LR, STOP)
        self.vm.emu_start(address, STOP, count=2000)
        self.assertEqual(self.vm.reg_read(self.arm.UC_ARM64_REG_PC), STOP)
        self.assertEqual(self.vm.reg_read(self.arm.UC_ARM64_REG_SP), STACK + 0xfff0)
        for index, value in saved.items():
            self.assertEqual(self.vm.reg_read(getattr(self.arm, f'UC_ARM64_REG_X{index}')), value)

    def add(self, method, klass='Scn.Step.Modules'):
        self.vm.mem_write(DATA + 0x6000, klass.encode() + b'\0')
        self.vm.mem_write(DATA + 0x7000, method.encode() + b'\0')
        self.call(ADD)

    def test_dependent_setup_waits_but_independent_jobs_stay_queued(self):
        self.add('InitializeBaseSystem')
        self.add('InitializeInfo')
        self.add('InitializeDecor')
        self.assertEqual(self.pending, ['InitializeInfo', 'InitializeDecor'])
        self.call(EXECUTE)
        self.assertEqual(self.executed, ['InitializeBaseSystem', 'InitializeInfo', 'InitializeDecor'])

    def test_other_methods_and_classes_do_not_drain_the_queue(self):
        for method, klass in [('OnUpdate', 'Scn.Step.Modules'),
                              ('InitializeBaseSystemExtra', 'Scn.Step.Modules'),
                              ('InitializeBaseSystem', 'Scn.Step.ModulesExtra'),
                              ('InitializeBaseSystem', 'Other.Modules')]:
            with self.subTest(method=method, klass=klass):
                self.add(method, klass)
        self.assertEqual(len(self.pending), 4)
        self.assertFalse(self.executed)


if __name__ == '__main__':
    unittest.main()
