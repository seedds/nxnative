"""Run the actual iOS RomFS locator on macOS/Foundation with private Documents/temp folders (no device or game files)."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('romfs_image', REPO / 'dm/tools/romfs_image.py')
assert spec is not None and spec.loader is not None
image = importlib.util.module_from_spec(spec); spec.loader.exec_module(image)


@unittest.skipUnless(sys.platform == 'darwin', 'Foundation required')
class Startup(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tools = tempfile.TemporaryDirectory(); cls.addClassCleanup(cls.tools.cleanup)
        root = Path(cls.tools.name)
        main = (REPO / 'dm/ios/Sources/main.m').read_text()
        locator = main[main.index('static int romfs_image_complete'):main.index('// Boot prerequisites')]
        source = root / 'locator.m'
        source.write_text('#import <Foundation/Foundation.h>\n#include <stdio.h>\n#include "romfs_image.h"\n'
                          'static NSString *fixture_doc, *fixture_tmp, *fixture_bundle;\n#define DM_GAME_ROMFS_MARKER "marker"\n'
                          '@interface FixtureBundle : NSObject\n+ (instancetype)mainBundle;\n- (NSString *)resourcePath;\n@end\n'
                          '@implementation FixtureBundle\n+ (instancetype)mainBundle { return [self new]; }\n'
                          '- (NSString *)resourcePath { return fixture_bundle; }\n@end\n#define NSBundle FixtureBundle\n'
                          '#define NSSearchPathForDirectoriesInDomains(a,b,c) @[fixture_doc]\n#define NSTemporaryDirectory() fixture_tmp\n' + locator +
                          '\nint main(int argc, char **argv) { @autoreleasepool { if (argc < 3) return 2;\n'
                          'fixture_doc = @(argv[1]); fixture_tmp = @(argv[2]); fixture_bundle = argc > 3 ? @(argv[3]) : @"/nonexistent-fixture"; NSString *p = romfs_locate();\n'
                          'if (!p) return 1; puts(p.fileSystemRepresentation); return 0; }}\n')
        cls.locator = root / 'locator'
        subprocess.run(['clang', '-fobjc-arc', '-Wall', '-Werror', '-I', str(REPO / 'dm/host'),
                        str(source), '-framework', 'Foundation', '-o', str(cls.locator)], check=True)

    def test_invalid_image_preserves_working_folder_and_valid_image_replaces_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); doc = root / 'Documents'; folder = doc / 'romfs'; folder.mkdir(parents=True)
            (folder / 'marker').write_text('working files')
            tmp = root / 'temp'; tmp.mkdir()
            img = doc / 'romfs.img'
            img.write_bytes(image.HEAD.pack(image.MAGIC, 32, 0, 0))
            result = subprocess.run([str(self.locator), str(doc), str(tmp)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), str(folder))
            self.assertTrue((folder / 'marker').is_file()); self.assertEqual(list(tmp.iterdir()), [])
            image.pack(folder, img)
            result = subprocess.run([str(self.locator), str(doc), str(tmp)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), str(img)); self.assertFalse(folder.exists())

    def test_bundled_image_is_used_in_place_and_documents_can_override_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); doc = root / 'Documents'; doc.mkdir()
            tmp = root / 'temp'; tmp.mkdir(); bundle = root / 'App'; bundle.mkdir()
            source = root / 'source'; source.mkdir(); (source / 'marker').write_text('resource')
            image.pack(source, bundle / 'romfs.img')
            def locate():
                result = subprocess.run([str(self.locator), str(doc), str(tmp), str(bundle)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr); return result.stdout.strip()
            self.assertEqual(locate(), str(bundle / 'romfs.img'))
            self.assertEqual(list(doc.iterdir()), []); self.assertFalse((bundle / 'romfs').exists())
            folder = doc / 'romfs'; folder.mkdir(); (folder / 'marker').touch()
            self.assertEqual(locate(), str(folder))
            image.pack(source, doc / 'romfs.img'); self.assertEqual(locate(), str(doc / 'romfs.img'))


if __name__ == '__main__': unittest.main()
