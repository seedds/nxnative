"""The forced takeover list (HLE_FORCE): force_init in dm/host/loader.c aborts every game at load on a symbol listed twice, and two stubs in different files
compile and link without a word, as each is static. This reads the host's and the graphics layer's sources (dm/host, dm/gfx; not their tests, which are
programs of their own), so a duplicate fails here instead of at a game's boot."""
from collections import defaultdict
from pathlib import Path
import re
import unittest

REPO = Path(__file__).resolve().parents[3]
ENTRY = re.compile(r'^[ \t]*HLE_FORCE\("(\w+)"', re.M)


def sources():
    yield from sorted((REPO / 'dm/host').glob('*.[cm]'))
    yield from sorted(f for f in (REPO / 'dm/gfx').rglob('*.[cm]') if 'tests' not in f.relative_to(REPO / 'dm/gfx').parts)


class Duplicates(unittest.TestCase):
    def test_no_symbol_is_taken_over_twice(self):
        files = defaultdict(list)
        for f in sources():
            for sym in ENTRY.findall(f.read_text(encoding='utf-8', errors='replace')): files[sym].append(str(f.relative_to(REPO)))
        self.assertGreater(len(files), 100)
        self.assertEqual({s: fs for s, fs in files.items() if len(fs) > 1}, {})


if __name__ == '__main__':
    unittest.main()
