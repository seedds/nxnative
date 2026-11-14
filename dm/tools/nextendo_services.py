"""From RobiiAragon/NX-Port-Studio dm/tools/nextendo_services.py @ bd13a49b (MIT): the public Nextendo service profile for one game.

Writes routes.tsv (DM_NEXTENDO_ROUTES, dm/host/hle_nextendo_routes.c), provider-ca.der, the pinned server certificates its routes name
(<SHA-256>.der, from dm/net/nextendo-pins) and manifest.json into a folder.

Only public service facts, the provider's authenticated public CA and certificates Nextendo's servers present to any client are used.
No Nintendo files, account tokens, game patches or system trust changes.
"""
from pathlib import Path
import base64
import hashlib
import ipaddress
import json
import re
import urllib.request


def contract(repository):
    value = json.loads((Path(repository) / 'dm/net/nextendo-services.json').read_text())
    if value.get('formatVersion') != 1: raise ValueError('Unsupported Nextendo service contract')
    for name in ('server', 'nat'):
        address = ipaddress.IPv4Address(value['provider'][name])
        if address.is_unspecified or address.is_multicast or address.is_loopback: raise ValueError('Invalid public Nextendo service address')
    if not re.fullmatch('[0-9a-f]{64}', value['provider']['caSHA256']): raise ValueError('Invalid provider CA digest')
    return value


def provider_ca(value):
    cache = Path.home() / 'Library/Application Support/nxnative/nextendo/public-provider-ca.der'
    if cache.is_file() and not cache.is_symlink() and cache.stat().st_size <= 65536:
        data = cache.read_bytes()
        if hashlib.sha256(data).hexdigest() == value['provider']['caSHA256']: return data
    url = value['provider']['caURL']
    # Nextendo's own Git server, where Prelude-Nextendo moved from GitHub (the old raw.githubusercontent.com address is 404); the SHA-256 below is the trust, not the host
    if not url.startswith('https://git.nextendo.network/NextendoNetwork/Prelude-Nextendo/raw/commit/'):
        raise ValueError('The provider CA must come from its official source')
    request = urllib.request.Request(url, headers={'User-Agent': 'nxnative'})
    with urllib.request.urlopen(request, timeout=15) as response: encoded = response.read(65537)
    if len(encoded) > 65536: raise ValueError('Provider CA exceeds its bounds')
    match = re.fullmatch(rb'\s*-----BEGIN CERTIFICATE-----\s*([A-Za-z0-9+/=\r\n]+)\s*-----END CERTIFICATE-----\s*', encoded)
    if not match: raise ValueError('Expected exactly one public provider certificate')
    der = base64.b64decode(re.sub(rb'\s', b'', match[1]), validate=True)
    if hashlib.sha256(der).hexdigest() != value['provider']['caSHA256']:
        raise ValueError('The public provider CA integrity check failed')
    cache.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    if not cache.is_symlink():
        import tempfile
        with tempfile.NamedTemporaryFile(dir=cache.parent, prefix='.ca-', delete=False) as out:
            temporary = Path(out.name)
            try: out.write(der); out.close(); temporary.chmod(0o600); temporary.replace(cache)
            finally: temporary.unlink(missing_ok=True)
    return der


PINS = 'dm/net/nextendo-pins'   # <SHA-256>.der: a server certificate a pinned-exact route accepts, byte for byte (hle_nextendo_x509.c dm_nextendo_x509_verify_pin)


def rules(value, title_id, protocol=None):
    """routes.tsv. A pinned-exact route is for a TLS service whose certificate no CA signs (Nextendo's NEX server for Mario Kart 8 Deluxe
    serves a self-signed one with no host name): it accepts exactly the certificate named by certificateSHA256, from PINS. Its address
    also gets a pinned-address route with the same certificate, as a NEX secure server can be handed to the game by address"""
    title_id = title_id.lower()
    if not re.fullmatch('[0-9a-f]{16}', title_id) or int(title_id, 16) == 0: raise ValueError('Invalid game title identity')
    families = value.get('titleFamilies', {})
    family = protocol or families.get(title_id, 'nex')
    items = list(value['base']) + value.get('families', {}).get(family, []) + value.get('titles', {}).get(title_id, [])
    seen, output, aliases = set(), [], {}
    for item in items:
        kind, host = item['match'], item['host'].lower()
        if kind not in ('exact', 'suffix', 'label', 'pinned-exact') or len(host) > 253:
            raise ValueError('Invalid service route pattern')
        pin = item.get('certificateSHA256')
        if (kind == 'pinned-exact') != (pin is not None) or (pin is not None and (not item['tls'] or not re.fullmatch('[0-9a-f]{64}', pin))):
            raise ValueError('A pinned route needs TLS and exactly one certificate digest')
        pattern = host.replace('*', 'x')
        if not re.fullmatch(r'[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?', pattern) or '..' in pattern:
            raise ValueError('Invalid service route hostname')
        if (kind == 'label' and host.count('*') != 1) or (kind != 'label' and '*' in host):
            raise ValueError('Invalid service label wildcard')
        if (kind, host) in seen: continue
        seen.add((kind, host))
        address = value['provider'][item['server']]
        if pin:   # the routes loader takes a pin only for its own host name
            output.append('\t'.join([kind, host, address, host, pin + '.der', pin]))
            if aliases.setdefault(address, (host, pin))[1] != pin: raise ValueError('One address with two pinned certificates')
            continue
        tail = ['@host', 'provider-ca.der', value['provider']['caSHA256']] if item['tls'] else ['-', '-', '-']
        output.append('\t'.join([kind, host, address] + tail))
    for address, (host, pin) in aliases.items(): output.append('\t'.join(['pinned-address', address, address, host, pin + '.der', pin]))
    if not 0 < len(output) <= 128: raise ValueError('Invalid service route count')
    return 'NXNEXTENDO_ROUTES_2\n' + '\n'.join(output) + '\n'


def prepare(repository, title_id, destination, protocol=None, ca=None):
    value = contract(repository); ca = provider_ca(value) if ca is None else ca
    if hashlib.sha256(ca).hexdigest() != value['provider']['caSHA256']: raise ValueError('Invalid provider CA')
    text = rules(value, title_id, protocol)
    files = {'routes.tsv': text.encode(), 'provider-ca.der': ca}
    for line in text.splitlines()[1:]:
        fields = line.split('\t')
        if fields[0] != 'pinned-exact': continue
        data = (Path(repository) / PINS / fields[4]).read_bytes()
        if hashlib.sha256(data).hexdigest() != fields[5]: raise ValueError('A pinned server certificate does not match its digest')
        files[fields[4]] = data
    manifest = {'formatVersion': 1, 'titleID': title_id.lower(), 'protocol': protocol,
                'identity': value.get('identities', {}).get(title_id.lower(), {'account':'pid','friends':'pid'}),
                'routeCount': len(text.splitlines()) - 1,
                'files': {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}}
    files['manifest.json'] = (json.dumps(manifest, sort_keys=True) + '\n').encode()
    destination = Path(destination)
    if destination.is_symlink(): raise ValueError('The service profile must not be a symbolic link')
    destination.mkdir(parents=True, exist_ok=True, mode=0o700)
    for name, data in files.items():
        path = destination / name
        if path.is_symlink(): raise ValueError('A service profile file must not be a symbolic link')
        temporary = destination / ('.' + name + '.partial')
        if temporary.exists() or temporary.is_symlink(): raise ValueError('A service profile preparation is already in progress')
        try:
            with temporary.open('xb') as output: output.write(data)
            temporary.chmod(0o600); temporary.replace(path)
        finally: temporary.unlink(missing_ok=True)
    return destination


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('repository', type=Path); parser.add_argument('title_id'); parser.add_argument('destination', type=Path)
    parser.add_argument('--protocol')
    args = parser.parse_args()
    try: print(prepare(args.repository, args.title_id, args.destination, args.protocol))
    except (OSError, ValueError) as error: raise SystemExit('Nextendo services could not be prepared: ' + str(error)) from None
