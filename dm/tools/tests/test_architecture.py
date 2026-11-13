"""The host stays general (AGENTS.md: per-game values in the profile, not in code; issue #635: no per-game branch in the host): its sources name no game by its title ID in code.
A comment may cite one, like hle_fpslock.c's path to an FPSLocker config. A game build's ID has its own check: docs_patches.py stops on a host row it cannot classify (test_docs.py).
No game files needed."""
from pathlib import Path
import re
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import docs_patches   # noqa: E402

# A game's title ID: Switch (01) or Switch 2 (04), a base game (000), its update (800) or another program of the game (001), as digits or an integer, whose leading zero C does not need.
# 0100000000000xxx is firmware content (hle_fs.c's MiiModel), not a game; shader keys are also 16 digits, but they do not take this form unless by chance
TITLE_ID = re.compile(r'(?<![0-9A-Za-z_])(?:0[xX]0?|0)(?![14]0{11})[14][0-9A-Fa-f]{11}[08]0[0-9A-Fa-f](?![0-9A-Fa-f])')


def code_lines(p):   # (line number, the line without its // or /* */ comment)
    text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), p.read_text(errors='replace'), flags=re.S)
    return [(n, re.sub(r'//.*', '', line)) for n, line in enumerate(text.splitlines(), 1)]


def profile_ids():   # every title and program ID the profiles name: {ID: where}
    out = {}
    for f in sorted((REPO / 'dm/games').glob('*/game.tsv')):
        out[f.parent.name] = f
        for line in f.read_text().splitlines():
            key, _, value = line.partition('\t')
            if key in ('title_id', 'program_id'): out[value.strip()] = f
            elif key == 'programs': out.update((v.partition('=')[2], f) for v in value.split())
    return out


class TitleIds(unittest.TestCase):
    def test_no_title_ids_in_the_host(self):
        hits = [f'{p.relative_to(REPO)}:{n}' for p in docs_patches.host_files() for n, line in code_lines(p) if TITLE_ID.search(line)]
        self.assertEqual(hits, [], "a title ID in the host: put the game's value in its profile (dm/games/<id>/game.tsv) and read it as a DM_GAME_ macro")

    def test_the_pattern_knows_every_profile(self):   # a new family of IDs fails here instead of passing the check above unseen
        ids = profile_ids()
        self.assertGreater(len(ids), 100)
        self.assertEqual({i: str(f) for i, f in ids.items() if not TITLE_ID.fullmatch(i)}, {})

    def test_the_pattern(self):
        for s in ('0x0100F2C0115B6000ull', '0x0100F2C0115B6800', '0x100F2C0115B6000', '"0100F2C0115B6000"', '0x0400D4201C6EA000', '0x010049900F546001', 'x == 0100f2c0115b6000'):
            self.assertIsNotNone(TITLE_ID.search(s), s)
        for s in ('0x0100000000000802ull', '0x0100000000000000', '"d0f769bb916bbba9"', '0x0100F2C0115B6000F', 'k0100F2C0115B6000', '0x100F2C0115B600'):
            self.assertIsNone(TITLE_ID.search(s), s)

    def test_the_scan_reaches_the_host(self):   # an empty or narrowed scan would pass the check above
        files = {str(p.relative_to(REPO)) for p in docs_patches.host_files()}
        self.assertLessEqual({'dm/host/main.c', 'dm/gfx/nvn/nvn_mtl.m', 'dm/gfx/nvn/nvn_capture.inc'}, files)
        self.assertTrue(any(f.startswith('dm/ios/') and f.endswith('.swift') for f in files))
        self.assertFalse([f for f in files if '/tests/' in f])

    def test_comments_are_left_out(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d, 'a.c')
            p.write_text('// 0100F2C0115B6000\n/* 0100F2C0115B6000\n 0100F2C0115B6000 */ int a;\nint b = 0x0100F2C0115B6000ull; // why\n')
            self.assertEqual([n for n, line in code_lines(p) if TITLE_ID.search(line)], [4])


if __name__ == '__main__':
    unittest.main()
