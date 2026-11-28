import tempfile
import unittest
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from analyze_fence_timeline import analyze  # noqa: E402

COLUMNS = 'kind cpu_ns frame thread sync fence latest done site value'.split()

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
            text = f'# fence_trace_v1 CPU=CLOCK_UPTIME_RAW_ns; cutoff=999999 first=10 last=20 records={count} dropped={dropped}; cutoff is NOT completion, missing joins remain unknown\n'
            text += '\t'.join(COLUMNS) + '\n'
            text += ''.join('\t'.join(str(e[k]) for k in COLUMNS) + '\n' for e in events)
            p.write_text(text)
            return analyze(p)

    def test_valid_cross_frame_pair_and_missing_target(self):
        result = self.parse([event(1, 100, 4), event(3, 101, 4), event(7, 1100, 4), event(2, 2100, 4, frame=11)], dropped=2)
        wait = result['waits'][0]
        self.assertTrue(wait['complete_pair'])
        self.assertEqual(wait['duration_cpu_ms'], 0.002)
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

    def test_event_kinds_and_identity_grouping(self):
        events = [event(1, 100, 4), event(3, 101, 4), event(4, 150, 4, value=1), event(5, 160, 4, value=1),
                  event(6, 170, 4, value=900), event(7, 400, 4, value=0), event(2, 410, 4, value=2),
                  event(1, 500, 5), event(2, 600, 5, value=1)]
        result = self.parse(events)
        self.assertEqual([w['sync'] for w in result['waits']], [4, 5])
        self.assertEqual([e['kind'] for e in result['waits'][0]['events']], [1, 3, 4, 5, 6, 7, 2])
        self.assertEqual(result['waits'][0]['result'], 2)
        self.assertEqual(result['waits'][1]['duration_cpu_ms'], 0.0001)
        self.assertNotIn('fences', result)

    def test_commit_and_feedback_kinds_are_rejected(self):
        for kind in (8, 11, 12):
            with self.assertRaises(ValueError):
                self.parse([event(kind, 100)])

    def test_count_mismatch(self):
        with self.assertRaises(ValueError):
            self.parse([event(1, 100, 4)], declared=2)


if __name__ == '__main__':
    unittest.main()
