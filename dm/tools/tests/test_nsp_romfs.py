"""NSP script --romfs bundle|separate (scripts/build_ipa_from_nsp.py): the option and setting, and the push_romfs.sh command printed for a separate build. No game files or Xcode needed."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
assert spec is not None and spec.loader is not None
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)


class RomfsOption(unittest.TestCase):
    def test_option_and_setting(self):
        self.assertEqual(nsp.ROMFS, 'bundle')   # the default build is the one that always was
        self.assertIsNone(nsp.parse_args([]).romfs)
        self.assertEqual(nsp.parse_args(['--romfs', 'separate']).romfs, 'separate')
        with self.assertRaises(SystemExit), mock.patch('sys.stderr'): nsp.parse_args(['--romfs', 'maybe'])

    def test_separate_cannot_be_a_mac_build(self):
        with tempfile.TemporaryDirectory() as t, mock.patch.object(nsp, 'apple_silicon', return_value=True):
            with self.assertRaises(nsp.BuildError) as e: nsp.main(nsp.parse_args(['--platform', 'mac', '--romfs', 'separate', '-k', t, t]))
            self.assertIn('--romfs separate', str(e.exception))

    def test_push_command_names_the_image(self):
        said = []
        with tempfile.TemporaryDirectory() as t, mock.patch.object(nsp.PROG, 'say', side_effect=said.append):
            image = Path(t) / 'RomFS.img'; image.touch()
            nsp.say_push_romfs(image, 'org.nxnative.01007ef00011e000')
            text = '\n'.join(said)
            self.assertIn(f'scripts/push_romfs.sh <device UDID> org.nxnative.01007ef00011e000 "{image}"', text)
            self.assertIn('needed again only when the image changed', text)
            self.assertNotIn('[RED]', text)
            said.clear(); nsp.say_push_romfs(Path(t), 'org.x'); self.assertIn('[RED]', '\n'.join(said))   # a folder has no identity


if __name__ == '__main__':
    unittest.main()
