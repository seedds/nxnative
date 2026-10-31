"""Area labels for pull requests (dm/tools/pr_areas.py): paths give their areas, a profile's folder its game label with the game's name, and a profile main lacks the
"new game" label. Runs against a temporary main with a small games table; no network or GitHub needed."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import pr_areas   # noqa: E402

BOTW = '01007EF00011E000'


class Labels(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.root = Path(self.tmp.name)
        (self.root / 'dm/games' / BOTW).mkdir(parents=True)
        (self.root / 'dm/games' / BOTW / 'game.tsv').write_text('')
        (self.root / 'dm/games/README.md').write_text(f'| [The Legend of Zelda: Breath of the Wild]({BOTW}/README.md) | `{BOTW}` |\n')

    def tearDown(self):
        self.tmp.cleanup()

    def names(self, *paths):
        return set(pr_areas.labels(list(paths), self.root))

    def test_areas(self):
        self.assertEqual(self.names('dm/host/hle_fs.c'), {'host'})
        self.assertEqual(self.names('dm/host/front_mac.m'), {'host', 'mac'})
        self.assertEqual(self.names('dm/gfx/tools/dmsh_build.py'), {'shader chain'})
        self.assertEqual(self.names('dm/gfx/nvn/dmsh_chain.h'), {'nvn', 'shader chain'})
        self.assertEqual(self.names('scripts/build_ipa_from_nsp.py'), {'packing', 'tools'})
        self.assertEqual(self.names('README.md', '.github/workflows/tests.yml'), {'docs', 'ci'})

    def test_game(self):
        out = pr_areas.labels([f'dm/games/{BOTW}/README.md'], self.root)
        self.assertEqual(set(out), {f'game: {BOTW}', 'docs'})
        self.assertEqual(out[f'game: {BOTW}'][1], 'The Legend of Zelda: Breath of the Wild')

    def test_new_game(self):
        self.assertEqual(self.names('dm/games/0100ACD0163D0000/game.tsv'), {'game: 0100ACD0163D0000', 'new game'})

    def test_patches_are_rewriter(self):
        self.assertIn('rewriter', self.names(f'dm/games/{BOTW}/patches.py'))


if __name__ == '__main__':
    unittest.main()
