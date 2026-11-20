#!/usr/bin/env python3
"""dm/tools/detclock_trace_cmp.py: two DM_DETCLOCK_TRACE files compared by guest time (hle_thread.c writes them; docs/reference/threads.md has the format).
Synthetic traces in a temporary folder; no game files needed."""
import gzip
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import detclock_trace_cmp as cmp

A = """O 0 0 60000000000 16666667
T 0 0 0x1004 30 Presentation Thread
B 0 0 0x1004:a -
J 1 10000105 0 10000105 0x1001:s 0x1001:s
B 1 10000105 0x1019:s 10100209
B 1 10000105 0x101a:s 10100209
J 2 10100209 10000105 10100209 0x102f:a 0x1019:s
P 2 16666667 1 0x1004 1
E 2 16666667 0x101e host
H 2 16666667 0x1021:w host
R 2 16666667 0x1021
J 3 20000105 16666667 20000105 0x1001:l 0x1020:s
P 3 33333334 2 0x1004 1
"""


class TraceCmp(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def write(self, name, text, gz=False):
        p = self.dir / name
        if gz:
            with gzip.open(p, 'wt', encoding='utf-8') as f:
                f.write(text)
        else:
            p.write_text(text, encoding='utf-8')
        return str(p)

    def run_cmp(self, a, b, *args):
        out = io.StringIO()
        old = sys.stdout
        sys.stdout = out
        try:
            code = cmp.main([a, b, *args])
        finally:
            sys.stdout = old
        return code, out.getvalue()

    def test_same_trace(self):
        a = self.write('a', A)
        code, out = self.run_cmp(a, a)
        self.assertEqual(code, 0, out)
        self.assertIn('the same: 12 lines', out)   # the block with no deadline left out
        code, out = self.run_cmp(a, a, '--exact')
        self.assertIn('the same: 13 lines', out)

    def test_races_at_one_time_are_not_differences(self):
        # another jump count (one jump more earlier on), the other thread named in a jump, the two blocks of one time the other way round, another worker of a pool
        # sleeping, a block with no deadline more
        b = A.replace('B 1 10000105 0x1019:s 10100209\nB 1 10000105 0x101a:s 10100209', 'B 1 10000105 0x101a:s 10100209\nB 1 10000105 0x101b:s 10100209')
        b = b.replace('0x102f:a 0x1019:s', '0x1030:a 0x101a:s').replace('P 3 33333334', 'P 4 33333334').replace('B 0 0 0x1004:a -', 'B 0 0 0x1004:a -\nB 0 0 0x1004:l -')
        code, out = self.run_cmp(self.write('a', A), self.write('b.gz', b, gz=True))
        self.assertEqual(code, 0, out)
        code, out = self.run_cmp(self.write('a', A), self.write('b', b), '--exact')
        self.assertEqual(code, 1, out)

    def test_first_difference_with_its_frame(self):
        b = A.replace('J 3 20000105 16666667 20000105 0x1001:l 0x1020:s', 'J 3 21000105 16666667 21000105 0x1001:l 0x1020:s')
        code, out = self.run_cmp(self.write('a', A), self.write('b', b), '--context', '2')
        self.assertEqual(code, 1)
        self.assertIn('first difference at guest time 20.000105 ms (frame 1 in A, 1 in B)', out)
        self.assertIn('A       12  idle jump', out)
        self.assertIn('B       12  idle jump', out)
        self.assertIn('21.000105 ms', out)

    def test_a_block_with_another_deadline(self):
        b = A.replace('B 1 10000105 0x101a:s 10100209', 'B 1 10000105 0x101a:s 10200313')
        code, out = self.run_cmp(self.write('a', A), self.write('b', b))
        self.assertEqual(code, 1, out)
        self.assertIn('only in A at this time (1): B 10000105 s 10100209', out)
        self.assertIn('only in B at this time (1): B 10000105 s 10200313', out)

    def test_kinds(self):
        b = A.replace('E 2 16666667 0x101e host\nH 2 16666667 0x1021:w host\nR 2 16666667 0x1021\n', '')
        code, out = self.run_cmp(self.write('a', A), self.write('b', b))
        self.assertEqual(code, 1, out)
        self.assertIn('host signal', out)
        code, out = self.run_cmp(self.write('a', A), self.write('b', b), '--kinds', 'JP')
        self.assertEqual(code, 0, out)

    def test_a_trace_that_ends_early(self):
        b = A.rsplit('P 3 33333334', 1)[0]
        code, out = self.run_cmp(self.write('a', A), self.write('b', b))
        self.assertEqual(code, 1)
        self.assertIn('B           (the trace ends here)', out)

    def test_input_read_counted_before_or_after_a_present(self):   # the reading and the present at one time: its frame field is the race's, its time and state are not
        a = A + 'I 3 33333334 2 0 0x400 0\nK 3 33333334 2 0x1001 0x400 0\n'
        b = A + 'I 3 33333334 2 0 0x400 0\nK 3 33333334 3 0x1001 0x400 0\n'
        code, out = self.run_cmp(self.write('a', a), self.write('b', b))
        self.assertEqual(code, 0, out)
        code, out = self.run_cmp(self.write('a', a), self.write('b', b.replace('K 3 33333334 3 0x1001 0x400', 'K 3 50000001 3 0x1001 0x400')))
        self.assertEqual(code, 1, out)

    def test_a_last_line_cut_at_exit(self):   # the host's graceful exit flushes nothing: a trace can end in the middle of a line
        code, out = self.run_cmp(self.write('a', A), self.write('b', A + 'J 14'))
        self.assertEqual(code, 0, out)

    def test_bad_kinds(self):
        with self.assertRaises(SystemExit) as e:
            cmp.main([self.write('a', A), self.write('b', A), '--kinds', 'JQ'])
        self.assertEqual(e.exception.code, 2)


if __name__ == '__main__':
    unittest.main()
