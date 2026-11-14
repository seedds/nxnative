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
            pins = {row.split('\t')[4] for row in routes.decode().splitlines()[1:] if row.startswith('pinned-exact\t')}
            expected = {'routes.tsv': hashlib.sha256(routes).hexdigest(), 'provider-ca.der': hashlib.sha256(ca).hexdigest()}
            expected.update({name: hashlib.sha256((out / name).read_bytes()).hexdigest() for name in pins})
            self.assertEqual(manifest['files'], expected)

    def test_pinned_routes_carry_their_certificate(self):
        # Mario Kart 8 Deluxe's NEX server presents a self-signed certificate: its routes accept exactly that one, and its address too
        mk8 = '0100152000022000'; value = n.contract(REPO); rows = [line.split('\t') for line in n.rules(value, mk8).splitlines()[1:]]
        pins = [row for row in rows if row[0].startswith('pinned-')]
        self.assertEqual({row[0] for row in pins}, {'pinned-exact', 'pinned-address'})
        for row in pins:
            self.assertEqual(row[4], row[5] + '.der')
            self.assertEqual(hashlib.sha256((REPO / n.PINS / row[4]).read_bytes()).hexdigest(), row[5])
            self.assertEqual(row[3], row[1] if row[0] == 'pinned-exact' else 'g2b309e01-lp1.s.n.srv.nintendo.net')
        with tempfile.TemporaryDirectory() as folder:
            ca = b'synthetic public CA bytes'; value['provider']['caSHA256'] = hashlib.sha256(ca).hexdigest()
            with patch.object(n, 'contract', return_value=value):
                out = n.prepare(REPO, mk8, Path(folder) / 'profile', ca=ca)
            self.assertTrue((out / (pins[0][5] + '.der')).is_file())
        broken = n.contract(REPO); broken['base'] = [dict(broken['base'][-1], tls=False)]
        with self.assertRaises(ValueError): n.rules(broken, mk8)


if __name__ == '__main__': unittest.main()
