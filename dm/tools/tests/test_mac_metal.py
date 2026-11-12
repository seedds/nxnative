"""The Mac compile of a game's shaders, dmsh_chain.MAC_METAL: one command, in the chain module that imports without running a build, which dmsh_build.py and the tests take
instead of copies of its flags. Its flags in order, the deployment target from dm/mac/min_os.mk, and dmsh_build.py's two uses of it. No compiler needed."""
from pathlib import Path
import re
import sys
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import dmsh_chain  # noqa: E402


class MacMetal(unittest.TestCase):
    def test_command(self):
        mac_min = re.search(r'^MACOS_MIN := (\S+)', (REPO / 'dm/mac/min_os.mk').read_text(), re.M)
        assert mac_min is not None
        self.assertEqual(dmsh_chain.MAC_METAL, ['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', f'-mmacosx-version-min={mac_min.group(1)}', '-fpreserve-invariance', '-fwrapv'])

    def test_build_uses_it(self):   # dmsh_build.py has no copy: its record names this command, and its direct run drops the xcrun prefix (the first four words)
        src = (REPO / 'dm/gfx/tools/dmsh_build.py').read_text()
        self.assertNotRegex(src, r'(?m)^MAC_METAL\s*=')
        self.assertIn("MAC_RECORD = ' '.join(dmsh_chain.MAC_METAL)", src)
        self.assertIn('*dmsh_chain.MAC_METAL[4:]]', src)
        self.assertEqual(dmsh_chain.MAC_METAL[:4], ['xcrun', '-sdk', 'macosx', 'metal'])

    def test_chain_hashes_its_inputs(self):   # the command and its deployment target are in DMSH_CHAIN's files
        self.assertIn('tools/dmsh_chain.py', dmsh_chain.FILES)
        self.assertIn('../mac/min_os.mk', dmsh_chain.FILES)


if __name__ == '__main__':
    unittest.main()
