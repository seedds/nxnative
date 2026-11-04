"""Check the ordering semantics of individual Ampere barrier encodings."""
import unittest
from test_dmsh_ampere_mem import Translated, EXIT, inst


class BarrierScopes(Translated):
    def membar(self, scope):
        return self.translate(inst(0x000fec0000000000 | (scope << 12), 0x7992) + inst(*EXIT))

    def test_cta_orders_workgroup_memory(self):
        self.assertRegex(self.membar(0), r'threadgroup_barrier\([^;]*mem_flags::mem_threadgroup')

    def test_wider_scopes_order_device_memory(self):
        for scope in (2, 3, 5):
            with self.subTest(scope=scope):
                self.assertIn('threadgroup_barrier(mem_flags::mem_device', self.membar(scope))

    def test_errbar_is_not_silently_dropped(self):
        msl = self.translate(inst(0x000fc00000000000, 0x79ab) + inst(*EXIT))
        self.assertIn('threadgroup_barrier(mem_flags::mem_device', msl)

    def test_unknown_scope_is_refused(self):
        with self.assertRaisesRegex(AssertionError, 'MEMBAR form'):
            self.membar(1)


if __name__ == '__main__':
    unittest.main()
