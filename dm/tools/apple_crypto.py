#!/usr/bin/env python3
"""Prepare a static, scoped X.509 verifier dependency for Apple game hosts.

The source archive is pinned by SHA-256. Nothing is installed system-wide, no
account state is read, and iOS archives never link a macOS Homebrew library.
Only the resulting prefix is printed; build output goes to a private log.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

VERSION = '4.1.0'
SOURCE = f'https://github.com/openssl/openssl/releases/download/openssl-{VERSION}/openssl-{VERSION}.tar.gz'
SHA256 = '50c17baf54cb008f6ce42d50deb4b28050a1aad711636cabd4b241c7e90cc2fd'
MAX_ARCHIVE = 100 * 2**20


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(2**20), b''): h.update(block)
    return h.hexdigest()


def extract_source(archive, destination):
    with tarfile.open(archive, 'r:gz') as tar:
        members = tar.getmembers()
        if len(members) > 30000 or sum(m.size for m in members) > 512 * 2**20:
            raise ValueError('OpenSSL source archive exceeds its bounds')
        for member in members:
            parts = Path(member.name).parts
            if not parts or parts[0] != f'openssl-{VERSION}' or member.name.startswith('/') or '..' in parts:
                raise ValueError('Invalid OpenSSL source path')
            if not member.isfile() and not member.isdir():
                raise ValueError('OpenSSL source contains an unsupported link or file')
        # Manual regular-file extraction also works with Apple's older Python;
        # no archive link, owner, special mode or absolute path is restored.
        for member in members:
            target = destination / member.name
            if member.isdir(): target.mkdir(parents=True, exist_ok=True, mode=0o700)
            else:
                target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
                source = tar.extractfile(member)
                if source is None: raise ValueError('OpenSSL regular file has no data')
                with source, target.open('xb') as output:
                    shutil.copyfileobj(source, output, 2**20)
                target.chmod(0o700 if member.mode & 0o111 else 0o600)


def run(command, **kwargs):
    return subprocess.check_output(command, text=True, **kwargs).strip()


def prepare(platform, cache=None):
    if platform not in ('macos', 'ios'): raise ValueError('Unsupported crypto platform')
    # Build both platforms against their declared deployment targets. A local
    # Homebrew archive may require this Mac's newer OS even when statically linked.
    cache = Path(cache) if cache else Path.home() / 'Library/Application Support/nxnative/Toolchains/OpenSSL'
    if cache.is_symlink(): raise ValueError('The crypto cache must not be a symbolic link')
    cache.mkdir(parents=True, exist_ok=True, mode=0o700)
    sdk = 'iphoneos' if platform == 'ios' else 'macosx'
    sdk_path = run(['xcrun', '--sdk', sdk, '--show-sdk-path'])
    minimum = '26.0' if platform == 'ios' else '14.0'
    options = ['no-shared', 'no-tests', 'no-apps', 'no-module']
    identity = {'format': 2, 'source_sha256': SHA256, 'platform': platform, 'architecture':'arm64',
                'minimum_os':minimum, 'configure_options':options,
                'sdk': sdk_path, 'compiler': run(['xcrun', '--sdk', sdk, 'clang', '--version'])}
    key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:24]
    target = cache / (platform + '-' + key)
    with (cache / (platform + '.lock')).open('a') as lock:
        os.chmod(lock.name, 0o600); fcntl.flock(lock, fcntl.LOCK_EX)
        receipt = target / 'receipt.json'
        if receipt.is_file() and not receipt.is_symlink():
            try:
                record = json.loads(receipt.read_text())
                if record['identity'] == identity and digest(target / 'lib/libcrypto.a') == record['libcrypto_sha256'] and (target / 'include/openssl/x509.h').is_file(): return target
            except (OSError, ValueError, KeyError): pass
        archive = cache / f'openssl-{VERSION}.tar.gz'
        if not archive.is_file() or archive.is_symlink() or digest(archive) != SHA256:
            request = urllib.request.Request(SOURCE, headers={'User-Agent': 'NX-Port-Studio'})
            with tempfile.NamedTemporaryFile(dir=cache, prefix='.download-', delete=False) as out:
                temporary = Path(out.name)
                try:
                    with urllib.request.urlopen(request, timeout=30) as response:
                        size = 0
                        for block in iter(lambda: response.read(2**20), b''):
                            size += len(block)
                            if size > MAX_ARCHIVE: raise ValueError('OpenSSL download exceeds its bounds')
                            out.write(block)
                    out.close()
                    if digest(temporary) != SHA256: raise ValueError('OpenSSL source integrity check failed')
                    temporary.replace(archive)
                finally: temporary.unlink(missing_ok=True)
        # OpenSSL's generated Makefile cannot represent a source path containing
        # spaces. Build in the system temp directory, then publish to Support.
        with tempfile.TemporaryDirectory(prefix='nx-crypto-' + platform + '-') as work:
            work = Path(work); extract_source(archive, work)
            source = work / f'openssl-{VERSION}'; output = work / 'prefix'
            env = dict(os.environ, SDKROOT=sdk_path)
            env['CFLAGS'] = ('-miphoneos-version-min=' if platform == 'ios' else '-mmacosx-version-min=') + minimum
            configuration = 'ios64-xcrun' if platform == 'ios' else 'darwin64-arm64-cc'
            log_path = cache / (platform + '-build.log')
            with log_path.open('w') as log:
                os.chmod(log_path, 0o600)
                for command in ([str(source / 'Configure'), configuration, *options, '--prefix=' + str(output)],
                                ['make', '-j4', 'build_libs'], ['make', 'install_dev']):
                    subprocess.run(command, cwd=source, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
            library = output / 'lib/libcrypto.a'
            if not library.is_file(): raise ValueError('The static crypto library was not produced')
            # A failed attempt never publishes a usable prefix.
            (output / 'receipt.json').write_text(json.dumps({'identity': identity, 'libcrypto_sha256': digest(library)}, sort_keys=True))
            if target.exists(): shutil.rmtree(target)
            output.rename(target)
        return target


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('platform', choices=('macos', 'ios'))
    parser.add_argument('--cache', type=Path)
    options = parser.parse_args()
    try: print(prepare(options.platform, options.cache))
    except (OSError, ValueError, tarfile.TarError, subprocess.CalledProcessError) as error:
        raise SystemExit('Static crypto preparation failed; inspect the private toolchain build log: ' + str(error)) from None
