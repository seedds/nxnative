import tempfile
import unittest
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from analyze_fence_timeline import analyze  # noqa: E402

COLUMNS = 'kind cpu_ns frame thread sync fence latest done site value gpu_begin_s gpu_end_s'.split()

def event(kind: int, time: int, sync: int = 0, fence: int = 7, **extra: int | float):
    row: dict[str, int | float] = dict.fromkeys(COLUMNS, 0)
    row.update(kind=kind, cpu_ns=time, frame=10, thread=3, sync=sync, fence=fence)
    row.update(extra)
    return row


class TimelineTests(unittest.TestCase):
    def parse(self, events, dropped=0, declared=None):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / 'trace.tsv'
            count = len(events) if declared is None else declared
            text = f'# fence_trace_v1 CPU=CLOCK_UPTIME_RAW_ns GPU=Metal_seconds; cutoff=999999 first=10 last=20 records={count} dropped={dropped}; cutoff is NOT completion, missing joins remain unknown\n'
            text += '\t'.join(COLUMNS) + '\n'
            text += ''.join('\t'.join(str(e[k]) for k in COLUMNS) + '\n' for e in events)
            p.write_text(text)
            return analyze(p)

    def test_valid_cross_frame_pair_and_missing_target(self):
        result = self.parse([event(1, 100, 4), event(3, 101, 4), event(7, 1100, 4), event(2, 2100, 4, frame=11)], dropped=2)
        wait = result['waits'][0]
        self.assertTrue(wait['complete_pair'])
        self.assertEqual(wait['duration_cpu_ms'], 0.002)
        self.assertEqual(wait['target_status'], 'not_observed')
        self.assertIsNone(wait['target_submission'])
        self.assertEqual(result['dropped'], 2)
        self.assertEqual(len(wait['events']), 4)

    def test_reversed_and_inconsistent_endpoints(self):
        wait = self.parse([event(1, 100, 4), event(2, 99, 4, fence=8)])['waits'][0]
        self.assertFalse(wait['complete_pair'])
        self.assertEqual(wait['endpoint_issues'], ['end_before_begin', 'endpoint_identity_mismatch'])
        self.assertNotIn('duration_cpu_ms', wait)

    def test_duplicate_and_missing_wait_endpoints(self):
        for rows in ([event(1, 100, 4)], [event(1, 100, 4), event(1, 101, 4), event(2, 200, 4)]):
            self.assertFalse(self.parse(rows)['waits'][0]['complete_pair'])

    def test_fence_order_and_gpu_timestamp_availability(self):
        for start, end in ((0, 0), (2, 1)):
            fence = self.parse([event(8, 100), event(9, 99), event(10, 98), event(11, 120, gpu_begin_s=start, gpu_end_s=end)])['fences'][0]
            self.assertEqual(len(fence['invalid_order']), 2)
            self.assertNotIn('commit_bracket_cpu_ms', fence)
            self.assertNotIn('record_to_commit_begin_cpu_ms', fence)
            self.assertNotIn('gpu_span_ms', fence)
        fence = self.parse([event(11, 120, gpu_begin_s=1, gpu_end_s=1.001)])['fences'][0]
        self.assertAlmostEqual(fence['gpu_span_ms'], 1)
        self.assertEqual(fence['missing_or_duplicate'], [8, 9, 10])

    def test_duplicate_feedback_and_count_mismatch(self):
        fence = self.parse([event(11, 120), event(11, 121)])['fences'][0]
        self.assertIn(11, fence['missing_or_duplicate'])
        self.assertNotIn('gpu_span_ms', fence)
        with self.assertRaises(ValueError):
            self.parse([event(1, 100, 4)], declared=2)


if __name__ == '__main__':
    unittest.main()
