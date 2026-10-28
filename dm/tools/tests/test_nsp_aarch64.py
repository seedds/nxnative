"""NSP script: a 32-bit ARM version (main.npdm with the 64-bit instruction flag clear) stops the build right after extraction, for a game that has a profile too (Mario Kart 8 Deluxe 1.0.0, LNPANDY/NX2Apple#116),
naming the version and the registered versions to pack instead. No game files, keys or Xcode needed (temporary files only)."""
import importlib.util
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games')); sys.path.insert(0, str(REPO / 'dm/tools'))
import profile_guess
sys.path.insert(0, str(REPO / 'scripts'))
nsp = importlib.import_module('build_ipa_from_nsp')
vars(nsp)['pg'] = profile_guess   # main sets it


class Profile:   # the parts of a game profile the check reads
    id = '0100AAAA00000000'
    def builds(self): return {'v1441792': {'display': '4.0.0', 'main': 'ab' * 16}}


def exefs(root, flags):   # an ExeFS with a main.npdm of this flags byte (None: no main.npdm)
    d = root / 'ExeFS'; d.mkdir(parents=True)
    if flags is not None: (d / 'main.npdm').write_bytes(b'META' + bytes(8) + bytes([flags]) + bytes(0x30))
    return d


class Aarch64(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)

    def check(self, flags, log=True):
        d = self.tmp / f'{flags}'; exefs(d, flags)
        logs = self.tmp / 'logs'; logs.mkdir(exist_ok=True)
        if log: (logs / 'extract_v0.log').write_text('display version 1.0.0\n12 files\n')
        nsp.check_versions_aarch64({'v0': d}, logs, Profile(), 'Mario Kart 8 Deluxe')

    def test_32bit_names_the_version_and_the_registered_ones(self):
        with self.assertRaises(nsp.BuildError) as e: self.check(0x04)
        m = str(e.exception)
        self.assertTrue(m.startswith('Mario Kart 8 Deluxe v0 (1.0.0) is a 32-bit ARM program'), m)
        self.assertIn('game profile 0100AAAA00000000 registers v1441792 4.0.0: pack that version (-p / PACKAGE) with its update in the folder', m)
        self.assertNotIn('\n', m)

    def test_without_a_display_version(self):
        with self.assertRaises(nsp.BuildError) as e: self.check(0x04, log=False)
        self.assertTrue(str(e.exception).startswith('Mario Kart 8 Deluxe v0 is a 32-bit ARM program'))

    def test_64bit_passes(self): self.check(0x3f)

    def test_missing_npdm_passes(self): self.check(None)


if __name__ == '__main__': unittest.main()
