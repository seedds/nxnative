"""Run main.m's DLC choice from boot_env (per-game games/<id>/aoc, then aoc_locate) on macOS/Foundation with private Documents and App folders, once per data layout (no device or game files)."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]


@unittest.skipUnless(sys.platform == 'darwin', 'Foundation required')
class AocLayouts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tools = tempfile.TemporaryDirectory(); cls.addClassCleanup(cls.tools.cleanup)
        root = Path(cls.tools.name)
        main = (REPO / 'dm/ios/Sources/main.m').read_text()
        cls.main = main
        locator = main[main.index('static void aoc_locate'):main.index('// Boot prerequisites')]
        choice = main[main.index('NSString *per_game_aoc'):main.index('aoc_locate();') + len('aoc_locate();')]
        source = root / 'layouts.m'
        source.write_text('#import <Foundation/Foundation.h>\n#include <stdio.h>\n#include <stdlib.h>\n'
                          'static NSString *fixture_doc, *fixture_bundle;\n'
                          '@interface FixtureBundle : NSObject\n+ (instancetype)mainBundle;\n- (NSString *)resourcePath;\n@end\n'
                          '@implementation FixtureBundle\n+ (instancetype)mainBundle { return [self new]; }\n'
                          '- (NSString *)resourcePath { return fixture_bundle; }\n@end\n#define NSBundle FixtureBundle\n'
                          '#define NSSearchPathForDirectoriesInDomains(a,b,c) @[fixture_doc]\n' + locator +
                          # argv: Documents, App, the romfs path romfs_locate would have answered
                          '\nint main(int argc, char **argv) { @autoreleasepool { if (argc < 4) return 2;\n'
                          'fixture_doc = @(argv[1]); fixture_bundle = @(argv[2]); NSString *romfs = @(argv[3]);\n'
                          'NSFileManager *fm = NSFileManager.defaultManager;\n'
                          'NSString *gdir = [[fixture_doc stringByAppendingPathComponent:@"games"] stringByAppendingPathComponent:@"0100TEST"];\n'
                          'BOOL per_game = [romfs hasPrefix:gdir];\n' + choice +
                          '\nconst char *e = getenv("DM_AOC_DIR"); puts(e ? e : "(none)"); return 0; }}\n')
        cls.probe = root / 'layouts'
        subprocess.run(['clang', '-fobjc-arc', '-Wall', '-Werror', str(source), '-framework', 'Foundation', '-o', str(cls.probe)], check=True)

    def choose(self, doc, app, romfs, env=None):
        e = {k: v for k, v in os.environ.items() if k != 'DM_AOC_DIR'}; e.update(env or {})
        r = subprocess.run([str(self.probe), str(doc), str(app), str(romfs)], capture_output=True, text=True, env=e)
        self.assertEqual(r.returncode, 0, r.stderr); return r.stdout.strip()

    def folders(self, t):
        doc = Path(t) / 'Documents'; app = Path(t) / 'App'; game = doc / 'games' / '0100TEST'
        game.mkdir(parents=True); app.mkdir(); return doc, app, game

    def test_the_layout_test_reads_the_real_per_game_decision(self):
        self.assertIn('BOOL per_game = [romfs hasPrefix:gdir];', self.main)

    def test_root_layout_finds_documents_aoc(self):
        """Documents/romfs.img with Documents/aoc/2.img beside it: issue #316 logged 'reporting 0 add-on contents (no DM_AOC_DIR)'."""
        with tempfile.TemporaryDirectory() as t:
            doc, app, game = self.folders(t)
            (doc / 'aoc').mkdir(); (doc / 'aoc' / '2.img').write_bytes(b'x'); (doc / 'romfs.img').write_bytes(b'x')
            self.assertEqual(self.choose(doc, app, doc / 'romfs.img'), str(doc / 'aoc'))

    def test_root_layout_ignores_a_per_game_aoc_it_is_not_using(self):
        with tempfile.TemporaryDirectory() as t:
            doc, app, game = self.folders(t)
            (doc / 'aoc').mkdir(); (game / 'aoc').mkdir()
            self.assertEqual(self.choose(doc, app, doc / 'romfs.img'), str(doc / 'aoc'))

    def test_root_layout_falls_back_to_the_bundled_aoc_then_none(self):
        with tempfile.TemporaryDirectory() as t:
            doc, app, game = self.folders(t)
            self.assertEqual(self.choose(doc, app, doc / 'romfs.img'), '(none)')
            (app / 'aoc').mkdir()
            self.assertEqual(self.choose(doc, app, doc / 'romfs.img'), str(app / 'aoc'))

    def test_per_game_layout_takes_its_own_aoc_before_documents_aoc(self):
        with tempfile.TemporaryDirectory() as t:
            doc, app, game = self.folders(t)
            (doc / 'aoc').mkdir(); (game / 'aoc').mkdir(); (app / 'aoc').mkdir()
            self.assertEqual(self.choose(doc, app, game / 'romfs.img'), str(game / 'aoc'))

    def test_per_game_layout_without_its_own_aoc_uses_documents_aoc(self):
        with tempfile.TemporaryDirectory() as t:
            doc, app, game = self.folders(t)
            (doc / 'aoc').mkdir()
            self.assertEqual(self.choose(doc, app, game / 'romfs.img'), str(doc / 'aoc'))

    def test_explicit_dm_aoc_dir_wins_in_both_layouts(self):
        """env.txt sets DM_AOC_DIR (setenv without overwrite) before this runs, so a variable already in the environment stands for it."""
        with tempfile.TemporaryDirectory() as t:
            doc, app, game = self.folders(t)
            (doc / 'aoc').mkdir(); (game / 'aoc').mkdir(); (app / 'aoc').mkdir()
            for romfs in (doc / 'romfs.img', game / 'romfs.img'):
                self.assertEqual(self.choose(doc, app, romfs, {'DM_AOC_DIR': '/elsewhere'}), '/elsewhere')


if __name__ == '__main__': unittest.main()
