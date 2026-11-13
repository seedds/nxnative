# The build folder of each program of a multi-program game (build_ipa_from_nsp.py build_folder): under --work-dir the programs packed with the application's first get folders
# of their own, so one program's extract/<version>/RomFS.img does not replace another's (docs/reference/packing.md)
import importlib.util
from pathlib import Path
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
assert spec and spec.loader
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
NSP = Path('/games/All-Stars')


def args(work_dir=None, **kw):
    return types.SimpleNamespace(work_dir=work_dir, **kw)


class BuildFolder(unittest.TestCase):
    def setUp(self):   # the Spyder settings block's WORK_DIR stays out of it
        patch = mock.patch.object(nsp, 'WORK_DIR', ''); patch.start(); self.addCleanup(patch.stop)

    def test_default_is_one_folder_per_profile(self):
        self.assertEqual(nsp.build_folder(args(), NSP, '010049900F546000'), NSP / '010049900F546000_build')
        self.assertEqual(nsp.build_folder(args(in_programs={}), NSP, '010049900F546001'), NSP / '010049900F546001_build')

    def test_work_dir_is_used_as_given_for_the_first_program(self):
        self.assertEqual(nsp.build_folder(args('/b/w'), NSP, '010049900F546000'), Path('/b/w').resolve())
        self.assertEqual(nsp.build_folder(args('/b/w', program=1), NSP, '010049900F546001'), Path('/b/w').resolve())   # --program N alone: the folder named

    def test_programs_under_work_dir_do_not_share_a_folder(self):
        first = nsp.build_folder(args('/b/w'), NSP, '010049900F546000')
        others = [nsp.build_folder(args('/b/w', program=n, in_programs={}), NSP, f'010049900F54600{n}') for n in (1, 2)]
        self.assertEqual(others, [first / 'programs' / '010049900F546001_build', first / 'programs' / '010049900F546002_build'])
        self.assertEqual(len({first, *others}), 3)


if __name__ == '__main__':
    unittest.main()
