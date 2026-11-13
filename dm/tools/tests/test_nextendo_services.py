"""Public Nextendo service profiles (dm/tools/nextendo_services.py), after NX-Port-Studio's test (MIT); no account state, no network."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('nextendo_services', REPO / 'dm/tools/nextendo_services.py')
assert spec is not None and spec.loader is not None
n = importlib.util.module_from_spec(spec); spec.loader.exec_module(n)
WONDER = '010015100b514000'


class NextendoServicesTests(unittest.TestCase):
    def test_shared_families_and_title_scoped_services(self):
        value = n.contract(REPO)
        self.assertIn('suffix\tdemonware.net\t', n.rules(value, '01001b300b9be000'))
        self.assertIn('api.epicgames.dev', n.rules(value, '0100ab401f136000'))
        self.assertNotIn('api.epicgames.dev', n.rules(value, WONDER))
        self.assertIn('label\tnncs2-*.', n.rules(value, WONDER))
        for title in ('0' * 16, 'bad', '../0000000000000'):
            with self.assertRaises(ValueError): n.rules(value, title)

    def test_profile_files_match_their_manifest(self):
        with tempfile.TemporaryDirectory() as folder:
            ca = b'synthetic public CA bytes'; value = n.contract(REPO)
            value['provider']['caSHA256'] = hashlib.sha256(ca).hexdigest()
            with patch.object(n, 'contract', return_value=value):
                out = n.prepare(REPO, WONDER, Path(folder) / 'profile', ca=ca)
            manifest = json.loads((out / 'manifest.json').read_text())
            self.assertEqual(manifest['identity'], {'account': 'pid', 'friends': 'baas'})   # hle_nextendo_api_apple.m reads it
            routes = (out / 'routes.tsv').read_bytes()
            self.assertTrue(routes.startswith(b'NXNEXTENDO_ROUTES_2\n'))
            self.assertEqual(manifest['files'], {'routes.tsv': hashlib.sha256(routes).hexdigest(), 'provider-ca.der': hashlib.sha256(ca).hexdigest()})


if __name__ == '__main__': unittest.main()
