#!/usr/bin/env python3
"""devkitA64 from the pinned devkitpro/devkita64 image without pulling the image: for a machine without the disk room for it (about 3 GB
unpacked; this takes about 190 MB). Reads the image index the digest names from Docker Hub, its linux/amd64 manifest, and the layer that
holds /opt/devkitpro; checks every digest on the way; and extracts devkitA64 (without the C++ compiler, LTO, gdb and newlib, which the
corpus does not use) and switch-tools' elf2nro into <dir>/opt/devkitpro. build_corpus.sh takes it with DKP_ROOT=<dir>/opt/devkitpro and
runs the same binaries the image has, in the nxnative-aot image.

  python3 dm/tools/aot/corpus/fetch_devkita64.py <dir>
"""
import hashlib
import io
import json
import os
import sys
import tarfile
import urllib.request

REPO = 'devkitpro/devkita64'
INDEX = 'sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282'  # devkitpro/devkita64:20260219, as build_corpus.sh pins
LAYER = 'sha256:da5be4661c9ac4e389f9e5f939e58bff7d32bc60569fa7c14e849d5246881691'  # its last layer: dkp-pacman's switch-dev and devkitA64
KEEP = ('opt/devkitpro/devkitA64/', 'opt/devkitpro/tools/bin/elf2nro')
SKIP = ('/cc1plus', '/lto1', 'gdb', 'lto-dump')  # parts of names
SKIP_TREES = ('opt/devkitpro/devkitA64/aarch64-none-elf/lib', 'opt/devkitpro/devkitA64/share')  # folders, themselves included


def wanted(name):
    if not name.startswith(KEEP) or any(s in name for s in SKIP):
        return False
    return not any(name == t or name.startswith(t + '/') for t in SKIP_TREES)


def get(url, token, accept=None):
    req = urllib.request.Request(url, headers={'Authorization': f'Bearer {token}'})
    if accept:
        req.add_header('Accept', accept)
    with urllib.request.urlopen(req) as r:
        return r.read()


def checked(data, digest, what):
    got = 'sha256:' + hashlib.sha256(data).hexdigest()
    if got != digest:
        raise SystemExit(f'fetch_devkita64.py: {what} has digest {got}, expected {digest}')
    return data


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    dest = sys.argv[1]
    token = json.loads(urllib.request.urlopen(
        f'https://auth.docker.io/token?service=registry.docker.io&scope=repository:{REPO}:pull').read())['token']
    base = f'https://registry-1.docker.io/v2/{REPO}'
    index = json.loads(checked(get(f'{base}/manifests/{INDEX}', token, 'application/vnd.oci.image.index.v1+json'), INDEX, 'the image index'))
    amd64 = [m for m in index['manifests'] if m.get('platform', {}).get('architecture') == 'amd64' and m['platform'].get('os') == 'linux']
    if len(amd64) != 1:
        raise SystemExit('fetch_devkita64.py: the index has no single linux/amd64 manifest')
    manifest = json.loads(checked(get(f'{base}/manifests/{amd64[0]["digest"]}', token, amd64[0]['mediaType']), amd64[0]['digest'], 'the manifest'))
    if LAYER not in [l['digest'] for l in manifest['layers']]:
        raise SystemExit(f'fetch_devkita64.py: the manifest has no layer {LAYER}')
    print(f'fetch_devkita64.py: downloading layer {LAYER[:19]}... of {REPO}@{INDEX[:19]}...', flush=True)
    blob = checked(get(f'{base}/blobs/{LAYER}', token), LAYER, 'the layer')
    n = 0
    with tarfile.open(fileobj=io.BytesIO(blob), mode='r:gz') as t:
        members = [m for m in t.getmembers() if wanted(m.name)]
        for m in members:
            if m.name.startswith('/') or '..' in m.name.split('/'):
                raise SystemExit(f'fetch_devkita64.py: unsafe path {m.name} in the layer')
        t.extractall(dest, members=members, filter='tar' if hasattr(tarfile, 'tar_filter') else None)
        n = len(members)
    print(f'fetch_devkita64.py: {n} entries in {os.path.join(dest, "opt", "devkitpro")}; use DKP_ROOT={os.path.join(dest, "opt", "devkitpro")}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
