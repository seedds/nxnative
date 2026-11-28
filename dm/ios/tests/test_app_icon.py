"""The app icon step of build_ipa.sh: a readable image becomes the asset catalog's 1024x1024 icon; an empty or non-image file stops the build naming the file.
Only that step runs (sips, no Xcode or game files)."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

REPO = Path(__file__).resolve().parents[3]
SCRIPT = (REPO / 'dm/ios/tools/build_ipa.sh').read_text()
ICON_STEP = SCRIPT[SCRIPT.index('# App icon: the game'):SCRIPT.index('# A prepared Xcode project is signed')]
PREAMBLE = 'set -u\nt() { echo "$*"; }; die() { echo "FAIL: $*" >&2; exit 1; }\n'
APP_ICON = 'Gen/Assets.xcassets/AppIcon.appiconset/icon.png'


def png(width, height):   # an opaque grey picture, enough for sips to read
    def chunk(kind, data): return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + b'\x80' * width * 3 for _ in range(height))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')


@unittest.skipUnless(shutil.which('sips'), 'needs macOS sips')
class AppIconTests(unittest.TestCase):
    def icon_step(self, name, data):
        """Runs the step with DM_ICON naming a file of these bytes (None: no such file); returns the result, whether the app icon is set, and the icon's path"""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'Gen').mkdir()
            icon = root / name
            if data is not None: icon.write_bytes(data)
            env = {k: v for k, v in os.environ.items() if not k.startswith('DM_')}
            env.update(DM_ICON=str(icon), ROMFS=str(root / 'romfs'))
            result = subprocess.run(['/bin/bash', '-c', PREAMBLE + ICON_STEP], cwd=root, env=env, capture_output=True, text=True)
            xcconfig = root / 'Gen/dm.xcconfig'
            set_icon = xcconfig.exists() and 'DM_APPICON = AppIcon' in xcconfig.read_text()
            size = subprocess.run(['sips', '-g', 'pixelWidth', '-g', 'pixelHeight', str(root / APP_ICON)], capture_output=True, text=True).stdout if (root / APP_ICON).exists() else ''
            return result, set_icon, str(icon), size

    def test_an_image_becomes_the_1024_icon(self):
        result, set_icon, _, size = self.icon_step('icon.png', png(256, 256))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(set_icon)
        self.assertIn('pixelWidth: 1024', size); self.assertIn('pixelHeight: 1024', size)

    def test_no_icon_file_leaves_the_icon_blank(self):
        result, set_icon, _, _ = self.icon_step('icon.jpg', None)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(set_icon)

    def test_an_empty_or_non_image_file_stops_naming_it(self):
        for name, data in (('icon.jpg', b''), ('icon.jpg', b'not an image\n'), ('icon.png', b'not an image\n')):
            with self.subTest(name=name, data=data):
                result, set_icon, icon, _ = self.icon_step(name, data)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(icon, result.stderr)
                self.assertIn('JPEG or PNG', result.stderr)
                self.assertFalse(set_icon)


if __name__ == '__main__':
    unittest.main()
