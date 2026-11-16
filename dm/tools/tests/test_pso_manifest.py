"""The prewarm seed lists shipped in the game profiles (dm/games/<id>/pso_manifest.tsv, read by the host's warm_parse): every row is one the host parses (S shader key and
stage, R render pipeline key, C compute shader key, M task shader key or - and a native task / mesh pipeline key; a pipeline key is pso_skey as hex), a list has at most one
#@after_first_frame line, and 0400486024DEC000's list: no row twice, its shader keys all in its shader map, the mark present, and every M row before it (task / mesh pipelines have
no stand-in while they build, so they are prepared before the first frame). No game files needed."""
from pathlib import Path
import re
import unittest

REPO = Path(__file__).resolve().parents[3]
GAMES = REPO / 'dm/games'
MARK = '#@after_first_frame'
KEY = re.compile(r'[0-9a-f]{16}_[a-z0-9]+')
HEX = re.compile(r'(?:[0-9a-f]{2})+')


def row_ok(row):
    f = row.split('\t')
    if f[0] == 'S': return len(f) == 3 and KEY.fullmatch(f[1]) is not None and f[2].isdigit()
    if f[0] == 'C': return len(f) == 2 and KEY.fullmatch(f[1]) is not None
    if f[0] == 'R': return len(f) == 2 and HEX.fullmatch(f[1]) is not None
    if f[0] == 'M': return len(f) == 3 and (f[1] == '-' or KEY.fullmatch(f[1]) is not None) and HEX.fullmatch(f[2]) is not None
    return False


def row_keys(row):
    """The 16-hex shader keys a row names: its own, and those inside a pipeline key (pso_skey holds them as text)"""
    out = []
    for x in row.split('\t')[1:]:
        if KEY.fullmatch(x): out.append(x[:16])
        elif len(x) > 32 and HEX.fullmatch(x): out += [k[:16] for k in KEY.findall(bytes.fromhex(x).decode('latin1'))]
    return out


class Manifests(unittest.TestCase):
    def test_rows_parse(self):
        for f in sorted(GAMES.glob('*/pso_manifest.tsv')):
            lines = f.read_text(errors='replace').split('\n')
            bad = [l[:60] for l in lines if l and not l.startswith('#') and not row_ok(l)]
            self.assertEqual(bad, [], f)
            self.assertLessEqual(sum(l.strip() == MARK for l in lines), 1, f)


class Profile0400486024DEC000(unittest.TestCase):
    D = GAMES / '0400486024DEC000'

    def setUp(self):
        self.lines = (self.D / 'pso_manifest.tsv').read_text().split('\n')
        self.rows = [l for l in self.lines if l and not l.startswith('#')]

    def test_no_row_twice(self):
        self.assertEqual(len(self.rows), len(set(self.rows)))

    def test_keys_in_shader_map(self):
        keys = {l.split('\t')[3] for l in (self.D / 'shader_map.tsv').read_text().split('\n') if l and not l.startswith('#') and l.count('\t') >= 4}
        missing = sorted({k for r in self.rows for k in row_keys(r)} - keys)
        self.assertEqual(missing, [])
        self.assertTrue(all(row_keys(r) for r in self.rows))

    def test_mark_after_every_m_row(self):
        self.assertIn(MARK, self.lines)
        m = self.lines.index(MARK)
        self.assertGreater(sum(1 for l in self.lines[:m] if l.startswith('M\t')), 0)
        self.assertEqual([l[:40] for l in self.lines[m + 1:] if l.startswith('M\t')], [])
        self.assertGreater(sum(1 for l in self.lines[m + 1:] if l and not l.startswith('#')), 0)

    def test_header_counts(self):
        """The header's 'The last N rows' line names rows after the mark, S / R / C only"""
        rows = self.rows
        last = [int(m.group(1).replace(',', '')) for l in self.lines if l.startswith('#') for m in [re.match(r'# The last ([\d,]+) rows', l)] if m]
        self.assertEqual(len(last), 1)
        tail = rows[-last[0]:]
        self.assertTrue(all(r[0] in 'SRC' for r in tail))
        m = self.lines.index(MARK)
        self.assertTrue(set(tail) <= set(self.lines[m + 1:]))


if __name__ == '__main__':
    unittest.main()
