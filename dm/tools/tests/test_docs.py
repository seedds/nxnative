"""The repository's own Markdown stays navigable: every relative link and #anchor resolves (GitHub's heading anchors), every game profile has its note
(dm/games/<id>/README.md) and a row in the games table of dm/games/README.md, the symptom index (docs/agents/symptoms.md) is the one dm/tools/docs_symptoms.py writes from
docs/reference, and the entry points stay short: AGENTS.md at most 1,500 words, each numbered guide in docs/agents at most 6,000 (a new finding goes into a game note or a
reference, not into these). Third-party trees are left out. No game files needed."""
from pathlib import Path
import re
import subprocess
import sys
import unittest
from urllib.parse import unquote

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import docs_symptoms   # noqa: E402

SKIP = ('third_party', 'node_modules')
AGENTS_WORDS, GUIDE_WORDS = 1500, 6000


def md_files():
    # tracked and new (not ignored) Markdown files; without git, a walk of the tree
    try:
        out = subprocess.run(['git', 'ls-files', '-co', '--exclude-standard', '*.md'], cwd=REPO, capture_output=True, text=True, check=True).stdout.split('\n')
        files = [REPO / p for p in out if p]
    except (OSError, subprocess.CalledProcessError):
        files = [p for p in REPO.rglob('*.md') if '.git' not in p.relative_to(REPO).parts]
    return sorted(p for p in files if p.is_file() and not any(s in p.relative_to(REPO).parts for s in SKIP))


ANCHORS = {}


def anchors(path):   # the anchors GitHub gives the headings of a Markdown file (repeated headings get -1, -2, …)
    if path not in ANCHORS:
        seen, out, fence = {}, set(), False
        for line in path.read_text(errors='replace').split('\n'):
            if line.startswith('```'): fence = not fence
            if fence: continue
            m = re.match(r'#{1,6}\s+(.*?)\s*#*$', line)
            if not m: continue
            s = docs_symptoms.slug(m.group(1)); n = seen.get(s, 0); seen[s] = n + 1
            out.add(s if n == 0 else f'{s}-{n}')
        ANCHORS[path] = out
    return ANCHORS[path]


def links(path):   # (line number, target) of every inline link outside code
    text = re.sub(r'```.*?```', lambda m: '\n' * m.group(0).count('\n'), path.read_text(errors='replace'), flags=re.S)
    for n, line in enumerate(text.split('\n'), 1):
        line = re.sub(r'`[^`]*`', '', line)
        for m in re.finditer(r'\]\(([^)\s]+)(?:\s+"[^"]*")?\)', line):
            yield n, m.group(1).strip('<>')


class Links(unittest.TestCase):
    def test_relative_links_and_anchors_resolve(self):
        bad = []
        for p in md_files():
            for n, t in links(p):
                if re.match(r'[a-z][a-z0-9+.-]*:', t, re.I): continue   # http:, https:, mailto:
                file, _, anchor = t.partition('#')
                dest = (p.parent / unquote(file)).resolve() if file else p
                where = f'{p.relative_to(REPO)}:{n}: {t}'
                if not dest.exists(): bad.append(f'{where} (no such file)'); continue
                if anchor and dest.suffix == '.md' and unquote(anchor).lower() not in anchors(dest): bad.append(f'{where} (no such heading)')
        self.assertEqual(bad, [], '\n'.join(bad))


class GameNotes(unittest.TestCase):
    def test_every_profile_has_a_note_and_a_row(self):
        games = REPO / 'dm/games'
        profiles = sorted(d.name for d in games.iterdir() if (d / 'game.tsv').is_file())
        rows = set(re.findall(r'^\| \[[^\]]+\]\(([0-9A-F]{16})/README\.md\) \| `([0-9A-F]{16})`', (games / 'README.md').read_text(), re.M))
        self.assertTrue(profiles)
        for g in profiles:
            self.assertTrue((games / g / 'README.md').is_file(), f'dm/games/{g} has no README.md (its game note)')
            self.assertIn((g, g), rows, f'dm/games/{g} has no row in the games table of dm/games/README.md')
        for g, _ in rows:
            self.assertIn(g, profiles, f'the games table lists {g}, which has no profile')


class SymptomIndex(unittest.TestCase):
    def test_index_is_current(self):
        self.assertEqual(docs_symptoms.OUT.read_text(), docs_symptoms.index(), 'docs/agents/symptoms.md is out of date: run dm/tools/docs_symptoms.py')


class Length(unittest.TestCase):
    def test_entry_points_stay_short(self):
        words = lambda p: len(p.read_text().split())
        self.assertLessEqual(words(REPO / 'AGENTS.md'), AGENTS_WORDS, 'AGENTS.md: put game facts in the game note, mechanisms in docs/reference')
        for p in sorted((REPO / 'docs/agents').glob('0*.md')):
            self.assertLessEqual(words(p), GUIDE_WORDS, f'{p.relative_to(REPO)}: put findings in docs/reference or the game notes')

    def test_agents_states_no_game_count(self):   # a count there was edited by most commits that touched the file, and conflicted between pull requests; the games table is the count
        count = re.search(r'\b(\d+|(?:twenty|thirty|forty|fifty|sixty|seventy|eighty|ninety|one hundred)(?:-\w+)?)\s+(?:\w+\s+)?games\b', (REPO / 'AGENTS.md').read_text(), re.I)
        self.assertIsNone(count, f'AGENTS.md: {count and count.group(0)!r}: say "the games in the games table" instead of a number')


if __name__ == '__main__':
    unittest.main()
