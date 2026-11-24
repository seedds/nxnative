"""Public Nextendo service profiles (dm/tools/nextendo_services.py), after NX-Port-Studio's test (MIT); no account state, no network."""
import base64
from email.message import Message
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('nextendo_services', REPO / 'dm/tools/nextendo_services.py')
assert spec is not None and spec.loader is not None
n = importlib.util.module_from_spec(spec); spec.loader.exec_module(n)
WONDER = '010015100b514000'


class NextendoServicesTests(unittest.TestCase):
    def test_empty_cache_uses_the_pinned_raw_api(self):
        value = n.contract(REPO)
        self.assertEqual(value['provider']['caSHA256'], 'a654c0eff8aa20a72280ea0ef2b5c2a17392922dd78ffbb113862e1ad2c66d49')
        url = ('https://git.nextendo.network/api/v1/repos/NextendoNetwork/Prelude-Nextendo/raw/'
               'romfs/sd/rootCA.pem?ref=482c182917228d69ebbbabfea44fffa4a44051be')
        der = b'synthetic public CA bytes'
        encoded = b'-----BEGIN CERTIFICATE-----\n' + base64.b64encode(der) + b'\n-----END CERTIFICATE-----\n'

        def fetch(request, timeout):
            if request.full_url != url:
                with HTTPError(request.full_url, 403, 'Forbidden', Message(), io.BytesIO()) as error:
                    raise error
            self.assertEqual(timeout, 15)
            return io.BytesIO(encoded)

        with tempfile.TemporaryDirectory() as folder, patch.object(n.Path, 'home', return_value=Path(folder)):
            with (patch.object(n.urllib.request, 'urlopen', side_effect=fetch),
                  patch.object(n.urllib.request.OpenerDirector, 'open', side_effect=fetch) as opened):
                with self.assertRaisesRegex(ValueError, 'integrity check failed'): n.provider_ca(value)
                value['provider']['caSHA256'] = hashlib.sha256(der).hexdigest()
                self.assertEqual(n.provider_ca(value), der)
                self.assertEqual(n.provider_ca(value), der)
                self.assertEqual(opened.call_count, 2)

    def test_ca_redirect_is_not_followed(self):
        value = n.contract(REPO)
        headers = Message()
        headers['Location'] = 'https://outside.invalid/other.pem'
        with HTTPError(value['provider']['caURL'], 302, 'Found', headers, io.BytesIO()) as redirect:
            with (tempfile.TemporaryDirectory() as folder, patch.object(n.Path, 'home', return_value=Path(folder)),
                  patch.object(n.urllib.request.HTTPSHandler, 'https_open', return_value=redirect) as opened,
                  self.assertRaises(HTTPError) as failure):
                n.provider_ca(value)
            self.assertEqual(failure.exception.code, 302)
            failure.exception.close()
            self.assertEqual(opened.call_count, 1)

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
