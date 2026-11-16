"""The survey reads a game's sdk module only when the game ships one: an SDK linked into main, or
main only loading an NRO payload (wine-nx ports), means no sdk NSO in the ExeFS and no exports.
"""
import tempfile
import unittest
from pathlib import Path
from unittest import mock
import sys

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import nso_syms


class Exports(unittest.TestCase):
    def test_no_sdk_module_means_no_exports(self):
        with tempfile.TemporaryDirectory() as d:   # an ExeFS holding only main and main.npdm: nothing to read
            unread = mock.Mock(side_effect=AssertionError('read a module that is not there'))
            with mock.patch.object(nso_syms, 'load', unread):
                self.assertEqual(nso_syms.exports(d), set())

    def test_exports_are_the_sdk_module_globally_defined_symbols(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'sdk').write_bytes(b'')   # the file's existence is what exports() checks; dynsyms reads through the mock
            syms = (('nnOsQueryEvent', True, 2), ('weakLocal', True, 0), ('imported', False, 1))
            with mock.patch.object(nso_syms, 'load', lambda p: 'img'), mock.patch.object(nso_syms, 'dynsyms', lambda img: syms):
                self.assertEqual(nso_syms.exports(d), {'nnOsQueryEvent'})


if __name__ == '__main__':
    unittest.main()
