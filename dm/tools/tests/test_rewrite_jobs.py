"""Default module CPU shares grow as modules finish; explicit overrides remain fixed."""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

PATH = Path(__file__).resolve().parents[1] / 'rewrite_jobs.py'


class Budget(unittest.TestCase):
    def setUp(self):
        self.assertTrue(PATH.exists(), 'adaptive rewrite allocation is not implemented')
        spec = importlib.util.spec_from_file_location('rewrite_jobs', PATH)
        assert spec is not None and spec.loader is not None
        self.jobs = importlib.util.module_from_spec(spec); spec.loader.exec_module(self.jobs)
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def test_budget_grows_without_shrinking_active_allocations(self):
        b = self.jobs.Budget(self.root, {'main': 100, 'sdk': 20, 'tiny': 1}, 8)
        first = b.update(['main', 'sdk', 'tiny'])
        self.assertEqual(sum(first.values()), 8)
        self.assertGreater(first['main'], first['sdk'])
        self.assertEqual(first['tiny'], 1)
        second = b.update(['main', 'sdk'])
        self.assertEqual(sum(second.values()), 8)
        self.assertTrue(all(second[k] >= first[k] for k in second))
        self.assertEqual(b.update(['main']), {'main': 8})
        self.assertEqual((self.root / 'main').read_text(), '8')
        self.assertFalse((self.root / 'tiny').exists())

    def test_new_modules_reserve_a_slot_before_redistribution(self):
        b = self.jobs.Budget(self.root, {'a': 10, 'b': 10, 'c': 10}, 2)
        self.assertEqual(b.update(['a', 'b']), {'a': 1, 'b': 1})
        self.assertEqual(b.update(['a', 'c']), {'a': 1, 'c': 1})
        self.assertEqual(b.update(['c']), {'c': 2})

    def test_worker_count_reads_each_new_allocation(self):
        path = self.root / 'main'; path.write_text('1')
        with mock.patch.dict(os.environ, {'DM_REWRITE_BUDGET': str(path)}):
            self.assertEqual(self.jobs.workers(8), 1)
            path.write_text('6'); self.assertEqual(self.jobs.workers(8), 6)
        with mock.patch.dict(os.environ, {}, clear=True): self.assertEqual(self.jobs.workers(3), 3)

    def test_size_uses_uncompressed_text_for_nso_and_nro(self):
        for magic, offset, pos in ((b'NSO0', 0, 0x18), (b'NRO0', 0x10, 0x24)):
            data = bytearray(0x100); data[offset:offset+4] = magic; struct.pack_into('<I', data, pos, 0x123000)
            p = self.root / magic.decode(); p.write_bytes(data)
            self.assertEqual(self.jobs.text_size(p), 0x123000)


if __name__ == '__main__': unittest.main()
