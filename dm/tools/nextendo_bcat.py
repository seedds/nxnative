"""Prepare and stage private public-calendar snapshots with the native verifier.

I keep provider ZIPs, configuration files and extracted calendars outside Git.
The shared Mac/iOS backend verifies catalog signatures, paths, CRCs and formats.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import uuid
import warnings

TITLES = {'0100f8f0000a2000', '01003bc0000a0000', '01003c700009c800'}


# The titles hle_bcat_apple.m supports and the one with no DebugUnderPilot files (Splatoon 2's regions): the host takes them from the build, not its source
TITLE_FLAGS = ('-DDM_BCAT_TITLES=@"0100f8f0000a2000",@"01003bc0000a0000",@"01003c700009c800"', '-DDM_BCAT_PLAIN_TITLE=@"01003c700009c800"')
def helper(repository):
    repository = Path(repository)
    override = os.environ.get('DM_OPENSSL_ROOT')
    if not override:
        raise ValueError('Set DM_OPENSSL_ROOT to the reviewed static OpenSSL prefix before preparing calendars')
    root = Path(override)
    sources = [repository / 'dm/tools/bcat_prepare.m', repository / 'dm/host/hle_bcat_apple.m', repository / 'dm/host/nextendo_bcat.h']
    compiler = subprocess.check_output(['xcrun', 'clang', '--version'])
    identity = hashlib.sha256(b''.join(p.read_bytes() for p in sources) + compiler + os.fsencode(root)).hexdigest()[:24]
    cache = Path.home() / 'Library/Application Support/nxnative/BCAT/tools'
    if cache.is_symlink(): raise ValueError('Unsafe calendar tool cache')
    cache.mkdir(parents=True, exist_ok=True, mode=0o700)
    executable = cache / ('bcat-' + identity)
    if not executable.is_file() or executable.is_symlink():
        with tempfile.TemporaryDirectory(dir=cache) as temporary:
            output = Path(temporary) / 'bcat'
            subprocess.run(['xcrun', 'clang', '-O1', '-fobjc-arc', '-Wall', '-Wextra', '-Wno-unused-parameter', '-DDM_OPENSSL_X509', *TITLE_FLAGS,
                            '-I' + str(root / 'include'), str(sources[0]), str(sources[1]), str(root / 'lib/libcrypto.a'),
                            '-lz', '-framework', 'Foundation', '-o', str(output)], check=True, capture_output=True)
            output.chmod(0o700); output.replace(executable)
    return executable


def private_directory(path):
    path = Path(path).absolute()
    current = Path(path.anchor)
    for part in path.parts[1:]:
        current /= part
        if current.is_symlink() or (current.exists() and not current.is_dir()): raise ValueError('Unsafe calendar directory component')
        current.mkdir(exist_ok=True, mode=0o700)
    return path


def recipe(source, title_id, version):
    """A recipe descriptor is not a successful or populated delivery cache."""
    if title_id.lower() not in TITLES or version != '5.5.2': raise ValueError('Unsupported calendar recipe version')
    source = private_directory(source)
    setup = {'formatVersion': 1, 'titleID': title_id.lower(), 'version': version}
    temporary = source / ('.setup-' + uuid.uuid4().hex)
    try:
        with temporary.open('xb') as stream: stream.write((json.dumps(setup, sort_keys=True) + '\n').encode())
        temporary.chmod(0o600); temporary.replace(source / 'setup.json')
    finally: temporary.unlink(missing_ok=True)
    return source


def prepare(repository, title_id, version, catalog, destination):
    """No recipe means no synthetic success: return None for unrelated games."""
    title_id = title_id.lower()
    if title_id not in TITLES: return None
    destination = recipe(Path(destination).absolute(), title_id, version)
    result = subprocess.run([str(helper(repository)), title_id, version, str(catalog), str(destination)], capture_output=True, text=True)
    if result.returncode:
        retained = subprocess.run([str(helper(repository)), 'validate', title_id, version, str(destination)], capture_output=True)
        if retained.returncode: raise ValueError(result.stderr.strip() or 'Calendar preparation failed')
        warnings.warn('Calendar refresh failed; retaining the previous validated snapshot for this exact game/version', RuntimeWarning, stacklevel=2)
    return destination


def stage(repository, source, destination, title_id, version=None, allow_missing=False):
    """Copy only one complete, revalidated immutable snapshot, never loose files."""
    source, destination = Path(source).absolute(), Path(destination).absolute()
    if source.is_symlink() or not source.is_dir() or destination.exists() or destination.is_symlink():
        raise ValueError('Unsafe calendar staging location')
    setup_path = source / 'setup.json'
    if setup_path.is_symlink() or not setup_path.is_file() or setup_path.stat().st_size > 4096: raise ValueError('Invalid calendar setup')
    setup = json.loads(setup_path.read_bytes())
    if type(setup.get('formatVersion')) is not int or setup['formatVersion'] != 1 or setup.get('titleID') != title_id.lower() or setup.get('version') != (version or setup.get('version')):
        raise ValueError('Calendar belongs to another game/version')
    version = setup['version']
    if title_id.lower() not in TITLES or version != '5.5.2': raise ValueError('Unsupported calendar recipe')
    validated = subprocess.run([str(helper(repository)), 'validate', title_id.lower(), version, str(source)], capture_output=True).returncode == 0
    if not validated and not allow_missing: raise ValueError('No complete validated calendar snapshot')
    pointer: str | None = None
    if validated:
        candidate = json.loads((source / 'current.json').read_bytes())['snapshot']
        if not isinstance(candidate, str) or not re.fullmatch('[a-f0-9]{64}', candidate): raise ValueError('Invalid calendar pointer')
        pointer = candidate
    private_directory(destination.parent)
    with tempfile.TemporaryDirectory(prefix='.bcat-stage-', dir=destination.parent) as temporary:
        temporary = Path(temporary) / 'data'; temporary.mkdir()
        shutil.copyfile(source / 'setup.json', temporary / 'setup.json')
        if pointer is not None:
            (temporary / 'snapshots').mkdir()
            # The verifier rejects symlinks and unexpected files in this snapshot.
            shutil.copytree(source / 'snapshots' / pointer, temporary / 'snapshots' / pointer, symlinks=True)
            shutil.copyfile(source / 'current.json', temporary / 'current.json')
            subprocess.run([str(helper(repository)), 'validate', title_id.lower(), version, str(temporary)], check=True, capture_output=True)
        # A recipe-only seed is explicitly incomplete. The runtime cannot mount
        # it, but can retry the authenticated public download after startup.

        temporary.replace(destination)
    return destination
