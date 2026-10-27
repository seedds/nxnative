"""The host stays general (AGENTS.md: per-game values in the profile, not in code): its C and Objective-C sources (dm/host, dm/gfx; tests and third-party code left out) name no
game by its title ID in code (comments that cite a source may), and a game build's ID appears only in the per-build host tables that key behaviour by build (BUILD_TABLES), which the profiles are to take over (issue
#313). Both hold today; the test keeps them, so a game-specific branch shows up in review as a profile key or a table row, not as a literal in the core. No game files needed."""
from pathlib import Path
import re
import subprocess
import unittest

REPO = Path(__file__).resolve().parents[3]
CORE = ('dm/host/*.c', 'dm/host/*.m', 'dm/host/*.h', 'dm/gfx/*.c', 'dm/gfx/*.m', 'dm/gfx/*.h', 'dm/gfx/*.inc', ':!dm/*/tests/*', ':!dm/third_party/*')
BUILD_TABLES = {'dm/host/hle_fpslock.c', 'dm/host/hle_uemalloc.c'}   # rows keyed by a module's build ID (module_t.build_id), one per game build
TITLE_ID = re.compile(r'(?<![0-9A-Za-z_])(?:0x)?01[0-9A-Fa-f]{11}[08]00(?![0-9A-Fa-f])')   # a base game's or its update's title ID, also with a suffix (ull)
BUILD_ID = re.compile(r'"[0-9A-Fa-f]{32,64}"')   # an NSO build ID as the tables spell it (shader keys are 16 digits)


def code_lines(f):   # (line number, the line without its // or /* */ comment): a title ID in a comment that names a source (an FPSLocker config's path) is no branch
    text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), (REPO / f).read_text(errors='replace'), flags=re.S)
    return [(n, re.sub(r'//.*', '', line)) for n, line in enumerate(text.splitlines(), 1)]


def core_files():
    out = subprocess.run(['git', 'ls-files', '--', *CORE], cwd=REPO, capture_output=True, text=True, check=True).stdout
    return [f for f in out.splitlines() if f]


class Host(unittest.TestCase):
    def test_no_title_ids_in_the_core(self):
        hits = [f'{f}:{n}' for f in core_files() for n, line in code_lines(f) if TITLE_ID.search(line)]
        self.assertEqual(hits, [], 'a title ID in the host: put the game\'s value in its profile (dm/games/<id>/game.tsv) and read it as a DM_GAME_ macro')

    def test_build_ids_only_in_the_build_tables(self):
        files = core_files()
        self.assertTrue(BUILD_TABLES <= set(files), f'a build table moved: update BUILD_TABLES ({", ".join(sorted(BUILD_TABLES - set(files)))})')
        hits = [f'{f}:{n}' for f in files if f not in BUILD_TABLES for n, line in code_lines(f) if BUILD_ID.search(line)]
        self.assertEqual(hits, [], 'a game build\'s ID outside the per-build tables: add a row to one of ' + ', '.join(sorted(BUILD_TABLES))
                         + ', or put the cut in the profile\'s patches.py (dm/tools/patch_nso.py)')


if __name__ == '__main__':
    unittest.main()
