"""Area labels for issues (dm/tools/issue_areas.py): title IDs and names give the game labels pull requests get, title words the areas, the bug form's answer the platform, the form or the title's start the kind. Runs against a
temporary main with a small games table; no network or GitHub needed."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import issue_areas   # noqa: E402
import pr_areas   # noqa: E402

TOTK = '0100F2C0115B6000'
HADES = '0100535012974000'
HADES2 = '0100A00019DE0000'
OTHER = '0100C3F0181C8000'
TOOL = Path(__file__).resolve().parents[1] / 'issue_areas.py'
TEMPLATES = Path(__file__).resolve().parents[3] / '.github/ISSUE_TEMPLATE'
TABLE = f'''| [The Legend of Zelda: Tears of the Kingdom]({TOTK}/README.md) | x |
| [Hades]({HADES}/README.md) | x |
| [Hades II]({HADES2}/README.md) | x |
| [DARK SOULS: REMASTERED](01004AB00A260000/README.md) | x |
| [Xenoblade Chronicles: Definitive Edition](0100FF500E34A000/README.md) | x |
| [FINAL FANTASY RESONANCE](0100606026F12000/README.md) | x |
| [FINAL FANTASY RESONANCE, Asian release](0100606026F13000/README.md) | x |
'''


class Labels(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.root = Path(self.tmp.name)
        (self.root / 'dm/games').mkdir(parents=True)
        (self.root / 'dm/games/README.md').write_text(TABLE)

    def tearDown(self):
        self.tmp.cleanup()

    def names(self, title, body=''):
        return set(issue_areas.labels(title, body, self.root))

    def test_title_id(self):
        out = issue_areas.labels(f'Crash ({TOTK})', '', self.root)
        self.assertEqual(set(out), {f'game: {TOTK}'})
        self.assertEqual(out[f'game: {TOTK}'][1], 'The Legend of Zelda: Tears of the Kingdom')
        self.assertEqual(issue_areas.labels(f'Crash ({OTHER})', '', self.root)[f'game: {OTHER}'][1], 'A game')   # no profile in main

    def test_update_and_dlc_name_the_base(self):
        self.assertEqual(issue_areas.base_id('010097F018538800'), '010097F018538000')
        self.assertEqual(issue_areas.base_id('01008f6008c5f001'), '01008F6008C5E000')
        self.assertEqual(issue_areas.base_id('010049900F546001'), '010049900F546001')   # a program of a multi-program title

    def test_body_id_only_without_one_in_the_title_or_form(self):
        self.assertEqual(self.names('It crashes', f'Seen on {TOTK}.'), {f'game: {TOTK}'})
        self.assertEqual(self.names(f'Crash ({TOTK})', f'Also like {OTHER}.'), {f'game: {TOTK}'})
        form = f'### Game, title ID and version\n\nThe game, {TOTK}, 1.4.3\n\n### Other\n\nLike {OTHER}.'
        self.assertEqual(self.names('It crashes', form), {f'game: {TOTK}'})

    def test_a_list_of_ids_names_no_game(self):
        ids = ' '.join(f'0100000000{i:02X}0000' for i in range(1, 7))
        self.assertEqual(self.names('Many games', ids), set())
        self.assertEqual(self.names(f'Many games, one {TOTK}', ids), {f'game: {TOTK}'})

    def test_name_in_the_title(self):
        self.assertEqual(self.names('Tears of the Kingdom 1.4.3: stall'), {f'game: {TOTK}'})
        self.assertEqual(self.names('Artifacts in Dark Souls Remastered'), {'game: 01004AB00A260000'})
        self.assertEqual(self.names('Hades II black screen'), {f'game: {HADES2}'})
        self.assertEqual(self.names('Hades 2 black screen'), set())
        self.assertEqual(self.names('Final Fantasy Resonance black'), {'game: 0100606026F12000'})
        self.assertEqual(self.names('Xenoblade Chronicles X Definitive Edition crash'), set())
        self.assertEqual(self.names('It crashes', 'Unlike Hades.'), set())   # the title only
        self.assertEqual(self.names(f'Hades ({OTHER})'), {f'game: {OTHER}'})   # an ID beats a name

    def test_title_words(self):
        self.assertEqual(self.names('patch_nso: a jump table'), {'rewriter'})
        self.assertEqual(self.names('shader_split.py stops'), {'packing', 'tools'})
        self.assertEqual(self.names('feat(mac): local players'), {'mac', 'enhancement'})
        self.assertEqual(self.names('[iOS] Cloud Saves'), {'ios'})
        self.assertEqual(self.names('Support in PokemonZA在iPhoneAir；'), {'ios'})
        self.assertEqual(self.names('SIGSEGV in h_nvnBufferGetAddress'), {'nvn'})
        self.assertEqual(self.names('Vulkan on the Mac host'), {'vulkan', 'mac', 'host'})
        self.assertEqual(self.names('DMSH_CHAIN after an update'), {'shader chain'})
        self.assertEqual(self.names('AGENTS.md rule'), {'docs'})
        self.assertEqual(self.names('Machine gun hosting cinema'), set())   # whole words

    def test_body_words_are_not_areas(self):
        self.assertEqual(self.names('Black screen', 'Built with scripts/build_ipa_from_nsp.py on a Mac, Vulkan.'), set())

    def test_form_platform(self):
        form = '### How it was built and run\n\n%s\n\n### Device and OS\n\nMacBook Pro'
        self.assertEqual(self.names('Black screen', form % 'Mac app (--platform mac-app)'), {'mac'})
        self.assertEqual(self.names('Black screen', form % 'iPhone or iPad, unsigned IPA from build_ipa_from_nsp.py'), {'ios'})
        self.assertEqual(self.names('Black screen', form % 'The NSP script failed before anything ran'), set())

    def test_kind_from_the_form(self):
        self.assertEqual(self.names('Black screen', '### What happened\n\nIt is black.\n'), {'bug'})
        self.assertEqual(self.names('More', '### What is missing\n\nA thing.\n'), {'enhancement'})
        self.assertEqual(self.names('A game', '### Survey and what happened\n\n_No response_\n'), {'new game'})
        self.assertEqual(self.names('Black screen', 'What happened: ### What happened in the text'), set())
        self.assertEqual(self.names('Proposal: more', '### What happened\n\nIt is black.\n'), {'bug'})   # the form beats the title

    def test_kind_from_the_title(self):
        for title in ('Proposal: an Android host', 'Scope proposal: amiibo', 'Feature request: a thing', 'Suggestion: controls', 'Design: capture', 'Roadmap: green', 'feat(ui): a thing'):
            self.assertIn('enhancement', self.names(title), title)
        for title in ('Request: Skyrim', '[Patch] Support a game', '[PATCH] Support a game', '[Bring-up] A game', '[Bringup] A game'):
            self.assertIn('new game', self.names(title), title)
        for title in ('Patch design: bind every patch', 'Bring-up: a game', 'A design: of sorts', 'Crash Bandicoot crashes', 'Requests are slow'):
            self.assertEqual(self.names(title), set(), title)

    def test_forms_have_their_headings(self):
        for form, heading, name in (('bug_report.yml', 'What happened', 'bug'), ('feature_request.yml', 'What is missing', 'enhancement'), ('game_request.yml', 'Survey and what happened', 'new game')):
            self.assertIn(f'label: {heading}\n', (TEMPLATES / form).read_text(encoding='utf-8'))
            self.assertEqual(issue_areas.FORMS[heading], name)
        others = {f.name: f.read_text(encoding='utf-8') for f in TEMPLATES.glob('*_*.yml')}
        for heading in issue_areas.FORMS:
            self.assertEqual(sum(f'label: {heading}\n' in t for t in others.values()), 1, heading)   # the heading tells the forms apart

    def test_every_word_rule_is_a_pull_request_label(self):
        self.assertEqual(set(issue_areas.WORDS) - {a[0] for a in pr_areas.AREAS}, {pr_areas.CHAIN[0]})
        self.assertTrue(all(pr_areas.automatic(n) for n in issue_areas.WORDS))
        self.assertTrue(pr_areas.automatic(f'game: {TOTK}'))
        self.assertTrue(all(pr_areas.automatic(n) for n in issue_areas.KIND_LABELS))
        self.assertFalse(pr_areas.automatic('wontfix'))

    def test_command_line(self):
        issue = json.dumps({'title': f'Mac: crash ({TOTK})', 'body': None})
        out = subprocess.run([sys.executable, TOOL, '--root', self.root], input=issue, capture_output=True, text=True, check=True).stdout
        self.assertEqual(out.splitlines(), ['mac\tc5def5\tThe Mac front end and app (dm/mac, front_mac.m); on an issue, seen on a Mac',
                                            f'game: {TOTK}\tf9d0c4\tThe Legend of Zelda: Tears of the Kingdom'])


if __name__ == '__main__':
    unittest.main()
