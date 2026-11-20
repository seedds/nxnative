"""An offline seed is a retry recipe, never a successful delivery cache."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import nextendo_bcat as bcat

class CalendarSeeds(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(); self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = bcat.recipe(self.root / 'source', '0100f8f0000a2000', '5.5.2')
        self.repo = Path(__file__).resolve().parents[3]

    def test_missing_crypto_prefix_never_builds_or_downloads_a_helper(self):
        with mock.patch.dict(bcat.os.environ, {}, clear=True), mock.patch.object(bcat.subprocess, 'check_output') as compiler:
            with self.assertRaisesRegex(ValueError, 'DM_OPENSSL_ROOT'):
                bcat.helper(self.repo)
            compiler.assert_not_called()

    def test_missing_snapshot_is_an_error_in_strict_staging(self):
        with mock.patch.object(bcat, 'helper', return_value=Path('/original-test-verifier')), \
             mock.patch.object(bcat.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1)):
            with self.assertRaises(ValueError): bcat.stage(self.repo, self.source, self.root / 'strict', '0100f8f0000a2000', '5.5.2')
        self.assertFalse((self.root / 'strict').exists())

    def test_explicit_offline_seed_preserves_only_recipe_and_drops_corrupt_payloads(self):
        (self.source / 'current.json').write_text('{"snapshot":"../unverified"}')
        (self.source / 'unverified.data').write_text('original fixture, not a delivered resource')
        with mock.patch.object(bcat, 'helper', return_value=Path('/original-test-verifier')), \
             mock.patch.object(bcat.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1)):
            output = bcat.stage(self.repo, self.source, self.root / 'retry', '0100f8f0000a2000', '5.5.2', allow_missing=True)
        self.assertEqual({p.name for p in output.iterdir()}, {'setup.json'})
        self.assertEqual(json.loads((output / 'setup.json').read_text()), {'formatVersion': 1, 'titleID': '0100f8f0000a2000', 'version': '5.5.2'})

    def test_identity_or_version_cannot_be_promoted_to_another_recipe(self):
        for title, version in (('01003bc0000a0000','5.5.2'),('0100f8f0000a2000','5.5.1')):
            with self.subTest(title=title, version=version), mock.patch.object(bcat,'helper') as helper:
                with self.assertRaises(ValueError): bcat.stage(self.repo,self.source,self.root/'wrong',title,version,allow_missing=True)
                helper.assert_not_called()

    def test_recipe_refuses_symlink_ancestors(self):
        link = self.root / 'link'; link.symlink_to(self.source,target_is_directory=True)
        with self.assertRaises(ValueError): bcat.recipe(link/'child','0100f8f0000a2000','5.5.2')
        self.assertFalse((self.source/'child').exists())

if __name__ == '__main__': unittest.main()
