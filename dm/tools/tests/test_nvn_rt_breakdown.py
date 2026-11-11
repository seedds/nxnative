"""Guest overlap is not extra storage, and a family heap is paid for once."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from nvn_rt_breakdown import summarize, overwritten


def target(off=0, guest=100, **kw):
    return dict(pool=1, flags=0x2a1, off=off, guest=guest, format=92,
                samples=1, texture=kw.pop('texture', off + 1), bytes=120,
                heap=0, heap_bytes=0, family=0, written=1, **kw)


class Breakdown(unittest.TestCase):
    def test_union_separates_pools_formats_and_samples(self):
        a = target()
        rows = [a, target(50), dict(a, pool=2, texture=3),
                dict(a, format=115, texture=4), dict(a, samples=4, texture=5)]
        stats = summarize(rows, 20)
        self.assertEqual(stats[92]['guest'], 350)
        self.assertEqual(stats[115]['guest'], 100)

    def test_texture_views_and_family_heap_are_not_double_counted(self):
        a = dict(target(), heap=50, heap_bytes=200, family=1)
        b = dict(a, off=50, texture=2)
        s = summarize([a, dict(a), b], 20)[92]
        self.assertEqual(s['textures'], 2)
        self.assertEqual(s['nominal'], 240)
        self.assertEqual(s['allocated'], 200)
        self.assertEqual(s['family'], 200)
        self.assertEqual(s['guest'], 150)

    def test_overwrite_exceptions_match_nx2navi(self):
        old = target()
        newer = dict(target(50), written=19)
        self.assertTrue(overwritten(old, newer, 20))
        for changed in (dict(newer, off=0), dict(newer, off=10, guest=20),
                        dict(newer, pool=2), dict(newer, off=100),
                        dict(newer, written=1)):
            self.assertFalse(overwritten(old, changed, 20))
        self.assertFalse(overwritten(dict(old, written=19), newer, 20))
        self.assertFalse(overwritten(dict(old, written=0), newer, 20))
        self.assertFalse(overwritten(dict(old, written=20), dict(newer, written=21), 20))

    def test_candidate_in_shared_heap_does_not_release_live_sibling(self):
        old = dict(target(), heap=50, heap_bytes=200, family=1)
        sibling = dict(old, off=200, texture=2)
        newer = dict(target(50), written=19)
        s = summarize([old, sibling, newer], 20)[92]
        self.assertEqual(s['overwritten_nominal'], 120)
        self.assertEqual(s['releasable'], 0)

    def test_recent_write_through_another_view_keeps_the_allocation(self):
        old = target()
        view = dict(old, written=21)
        writer = dict(target(50), written=20)
        self.assertEqual(summarize([old, view, writer], 20)[92]['overwritten_nominal'], 0)


if __name__ == '__main__':
    unittest.main()
