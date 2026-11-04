"""The profile's pso_pairs_manual.tsv: the pairs a game needs and its files do not declare are merged into pso_pairs.tsv, so a split keeps them instead of dropping the rows it did not generate."""
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split

VS, PS = 'fa4f282f3b5ed0bd', '421ae232a7b7df44'


class ManualPairsTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix='psopairs')
        self.path = Path(self.dir, 'pso_pairs_manual.tsv')
        patcher = mock.patch.object(split.GAME, 'table', lambda name: self.path)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_a_row_is_a_vertex_key_and_a_pixel_key(self):
        self.path.write_text(f'# a comment\n\n{VS}\t{PS}\n')
        self.assertEqual(split.manual_pairs(), {(VS, PS)})

    def test_no_table_is_no_pair(self):
        self.assertEqual(split.manual_pairs(), set())

    def test_a_short_key_is_refused(self):
        self.path.write_text(f'{VS}\tdeadbeef\n')
        with self.assertRaises(SystemExit) as e:
            split.manual_pairs()
        self.assertIn('16 hex digits each', str(e.exception))

    def test_three_columns_are_refused(self):
        self.path.write_text(f'{VS}\t{PS}\t{PS}\n')
        with self.assertRaises(SystemExit) as e:
            split.manual_pairs()
        self.assertIn(':1:', str(e.exception))

    def test_an_upper_case_key_is_refused(self):
        """The keys the host and the split write are lower case, so an upper-case row is a transcription of one, and a silent lower() would hide a mistyped key."""
        self.path.write_text(f'{VS.upper()}\t{PS}\n')
        with self.assertRaises(SystemExit):
            split.manual_pairs()


if __name__ == '__main__':
    unittest.main()
