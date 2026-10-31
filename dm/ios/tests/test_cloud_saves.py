"""Run the launcher's real cloud-saves folder check (CloudSaves.m) on macOS/Foundation: a folder inside the app's own storage, or one the app cannot write to, is refused with a sentence, and a real one is kept (no device or iCloud account)."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]

HARNESS = '''#import <Foundation/Foundation.h>
#include <stdio.h>
#include "CloudSaves.h"

// Own folders first (Documents, Inbox, temp), then one "<w|r>,<path>" per folder to judge: w = the folder a
// player just picked (the host writes there, so it must be writable), r = a stored folder read at boot.
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 5) return 2;
        NSArray<NSString *> *own = @[ @(argv[1]), @(argv[2]), @(argv[3]) ];
        for (int i = 4; i < argc; i++) {
            NSString *a = @(argv[i]);
            NSRange comma = [a rangeOfString:@","];
            if (comma.location == NSNotFound) return 2;
            NSString *why = dm_cloud_saves_problem([a substringFromIndex:comma.location + 1], own,
                                                   [[a substringToIndex:comma.location] isEqualToString:@"w"]);
            printf("%s\\t%s\\n", why ? "REFUSED" : "OK", why ? why.UTF8String : "usable");
        }
    }
    return 0;
}
'''


@unittest.skipUnless(sys.platform == 'darwin' and subprocess.run(['/usr/bin/xcrun', '--find', 'clang'], capture_output=True).returncode == 0,
                     'needs the macOS Objective-C toolchain')
class CloudSavesFolderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tools = tempfile.TemporaryDirectory(); cls.addClassCleanup(cls.tools.cleanup)
        root = Path(cls.tools.name)
        (root / 'check.m').write_text(HARNESS)
        cls.exe = root / 'cloud_saves_check'
        made = subprocess.run(['clang', '-fobjc-arc', '-Wall', '-Werror', '-I', str(REPO / 'dm/ios/Sources'),
                               str(root / 'check.m'), str(REPO / 'dm/ios/Sources/CloudSaves.m'),
                               '-framework', 'Foundation', '-o', str(cls.exe)], capture_output=True, text=True)
        assert made.returncode == 0, made.stdout + made.stderr

    def verdicts(self, documents, inbox, temp, *folders):
        run = subprocess.run([str(self.exe), str(documents), str(inbox), str(temp)] + [f'{kind},{path}' for kind, path in folders],
                             capture_output=True, text=True, timeout=60)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        out = []
        for line in run.stdout.strip().split('\n'):
            verdict, why = line.split('\t', 1)
            out.append((verdict, why))
        self.assertEqual(len(out), len(folders))
        return out

    def layout(self):
        root = Path(tempfile.mkdtemp())

        def clean():   # a read-only folder refuses the removal: make it writable again first
            subprocess.run(['chmod', '-R', 'u+w', str(root)], capture_output=True)
            subprocess.run(['rm', '-rf', str(root)], capture_output=True)
        self.addCleanup(clean)
        container = root / 'container'; documents = container / 'Documents'; inbox = container / 'Inbox'
        temp = container / 'tmp'; documents.mkdir(parents=True); inbox.mkdir(); temp.mkdir()
        return root, documents, inbox, temp

    def test_a_folder_inside_the_app_is_refused_at_a_pick_and_at_boot(self):
        root, documents, inbox, temp = self.layout()
        cloud = root / 'iCloud Drive'
        inside = [documents / 'Saves', inbox / 'Saves', temp / 'app-Inbox' / 'Saves', documents]
        for folder in inside:
            folder.mkdir(parents=True, exist_ok=True)
        picks = [(kind, folder) for kind in ('w', 'r') for folder in inside]
        got = self.verdicts(documents, inbox, temp, *picks)
        for (kind, path), (verdict, why) in zip(picks, got):
            self.assertEqual(verdict, 'REFUSED', f'{kind} {path}')
            self.assertIn("inside this app's own storage", why)
            self.assertIn('iCloud Drive', why)
        cloud.mkdir()
        self.assertEqual(self.verdicts(documents, inbox, temp, ('w', cloud))[0][0], 'OK')

    def test_a_link_into_the_app_is_refused_as_the_folder_it_points_at(self):
        root, documents, inbox, temp = self.layout()
        (inbox / 'Saves').mkdir()
        link = documents / 'link'
        os.symlink(inbox / 'Saves', link)
        (verdict, why) = self.verdicts(documents, inbox, temp, ('w', link))[0]
        self.assertEqual(verdict, 'REFUSED', why)
        self.assertIn("inside this app's own storage", why)

    @unittest.skipIf(os.geteuid() == 0, 'root writes into a read-only folder')
    def test_a_folder_the_app_cannot_write_to_is_refused_at_a_pick_and_kept_at_boot(self):
        root, documents, inbox, temp = self.layout()
        cloud = root / 'cloud'; cloud.mkdir()
        (verdict, why) = self.verdicts(documents, inbox, temp, ('w', cloud))[0]
        self.assertEqual(verdict, 'OK', why)
        self.assertEqual(list(cloud.iterdir()), [])   # the .nosync check file is gone again
        cloud.chmod(0o555)
        (verdict, why) = self.verdicts(documents, inbox, temp, ('w', cloud))[0]
        self.assertEqual(verdict, 'REFUSED', why)
        self.assertIn('cannot be written to', why)
        # At boot a written file failing is a moment's problem (no room, an offline iCloud Drive), not a reason to ask for a new pick
        self.assertEqual(self.verdicts(documents, inbox, temp, ('r', cloud))[0][0], 'OK')

    def test_a_missing_folder_or_a_file_is_not_a_folder(self):
        root, documents, inbox, temp = self.layout()
        cloud = root / 'cloud'; cloud.mkdir(); file = cloud / 'save.dmsync'; file.write_text('x')
        for verdict, why in self.verdicts(documents, inbox, temp, ('w', root / 'gone'), ('w', file)):
            self.assertEqual(verdict, 'REFUSED', why)
            self.assertIn('not there any more', why)


if __name__ == '__main__':
    unittest.main()
