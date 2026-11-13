"""shader_split merge_keys: a shader found in both the game and an add-on (/aoc/<index>/) is the game's row whatever the scan order, so the game's rows are the same with or without
any DLC (issue #2439: PRAGMATA's re_dlc_000.pak holds 45 of the game's shaders, and aoc/ sorts before re_chunk_000.pak). Otherwise the first copy is kept, and two copies of one key
that differ are still red. No game files needed."""
import os
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '01007BB017812000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had

CTL, CODE = bytes(0x800), b'\x01' * 64


def result(rel, where, key='aaaaaaaaaaaaaaaa', ctl=CTL, code=CODE):   # one scanned file's result as merge_keys reads it: (rel, {key: (where, stage, ctl, code)}, pairs, extra)
    return (rel, {key: (where, 'vs', ctl, code)}, set(), None)


GAME = result('re_chunk_000.pak', '/re_chunk_000.pak#0bed8d3cabfc7f02#SDF#var619')
DLC = result('aoc/1/re_dlc_000.pak', '/aoc/1/re_dlc_000.pak#df6d586eb9b7747e#SDF#var619')


class SharedKeys(unittest.TestCase):
    def test_game_copy_wins_in_either_order(self):
        for name, results in (('add-on scanned first', [DLC, GAME]), ('game scanned first', [GAME, DLC])):
            with self.subTest(name):
                keys, _, per_src = ss.merge_keys(results)
                self.assertEqual(keys['aaaaaaaaaaaaaaaa'][0], GAME[1]['aaaaaaaaaaaaaaaa'][0])
                self.assertEqual(sum(per_src.values()), 1)
                self.assertEqual(+per_src, ss.collections.Counter({ss.group(GAME[1]['aaaaaaaaaaaaaaaa'][0]): 1}))

    def test_without_the_addon_the_same_row(self):
        with_dlc, _, _ = ss.merge_keys([DLC, GAME]); without, _, _ = ss.merge_keys([GAME])
        self.assertEqual(with_dlc, without)

    def test_first_copy_kept_otherwise(self):
        a, b = result('a.pak', '/a.pak#1'), result('b.pak', '/b.pak#2')
        self.assertEqual(ss.merge_keys([a, b])[0]['aaaaaaaaaaaaaaaa'][0], '/a.pak#1')
        c, d = result('aoc/1/x.pak', '/aoc/1/x.pak#1'), result('aoc/2/y.pak', '/aoc/2/y.pak#2')
        self.assertEqual(ss.merge_keys([c, d])[0]['aaaaaaaaaaaaaaaa'][0], '/aoc/1/x.pak#1')

    def test_differing_copies_still_red(self):
        other = result('re_chunk_000.pak', '/re_chunk_000.pak#x', code=b'\x02' * 64)
        with self.assertRaisesRegex(SystemExit, r'control section or code area differs'):
            ss.merge_keys([DLC, other])

    def test_pairs_are_joined(self):
        r1 = ('a', {}, {('k1', 'k2')}, None); r2 = ('b', {}, {('k3', 'k4')}, None)
        self.assertEqual(ss.merge_keys([r1, r2])[1], {('k1', 'k2'), ('k3', 'k4')})


if __name__ == '__main__':
    unittest.main()
