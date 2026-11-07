# Adapted from ohxiaoguang/Touchline23, MIT; original authorship retained.
"""Find the EA EASF key of a title inside the title's own program module.

An EASF header (0x30 bytes) is `EASF`, the plaintext length (big endian), the
eight-character key ID, and SHA-256 of the plaintext's first 32 bytes. The
engine's key registry stores one record per ID, laid out as `{char id[8];
uint8 key[16]}`, so the 16 bytes after the ID are the AES-128 key for it.

A candidate is never trusted on its shape: it must decrypt the container's
first two blocks to the digest in the header (32 bytes),
which is the same check easf/prepare.py makes after decrypting. No offset, no
build ID, no external key table and no network are used, so the same code runs
against a base game, an update, or any other title that keeps this registry.

The key is never printed, logged or returned in an exception message: callers
see `fingerprint(key)`, the first 16 hex digits of its SHA-256.
"""
import hashlib
import json
from pathlib import Path
import struct

HEADER = 48          # 'EASF' + size + key ID + digest
PROBE = 32           # bytes the digest covers

# A bounded fallback scan walks the read-only segment for ID-shaped records
# when the container's ID is not present verbatim (another title may encode it
# differently). It is off by default: the direct search finds the record in one
# step, and the wide scan costs about a minute on a large module.
SCAN_LIMIT = 200000


def fingerprint(key):
    """-> a stable, non-secret identifier for a key, for logs and manifests."""
    return hashlib.sha256(key).hexdigest()[:16]


def parse_header(data):
    """-> {'size', 'key_id', 'digest'} of an EASF container; raises otherwise."""
    data = bytes(data)
    if not data.startswith(b'EASF'):
        raise ValueError('not an EASF container')
    if len(data) < HEADER + PROBE or (len(data) - HEADER) % 16:
        raise ValueError('invalid EASF length')
    key_id = data[8:16]
    if any(not 0x20 <= c < 0x7f for c in key_id):
        raise ValueError('EASF key ID is not printable text')
    return {'size': struct.unpack_from('>I', data, 4)[0], 'key_id': key_id,
            'digest': data[16:48]}


def confirms(data, key):
    """Whether `key` decrypts the container's first blocks to the header digest."""
    if len(key) != 16 or len(data) < HEADER + PROBE:
        return False
    from Crypto.Cipher import AES
    plain = AES.new(key, AES.MODE_CBC, iv=key).decrypt(bytes(data[HEADER:HEADER + PROBE]))
    return hashlib.sha256(plain).digest() == bytes(data[16:48])


def module_image(path):
    """-> the loaded image (text, rodata, data) of an NSO0 module.

    The three segments are placed at their memory offsets, so a search offset is
    also the virtual address the module runs at. A file that is not an NSO0 is
    rejected: an unpacked blob is not a module this can read reliably.
    """
    import nso
    module = nso.read(path)
    return bytes(module.image)


def rodata(path):
    import nso
    module = nso.read(path)
    start, size = module.segments[1]
    return bytes(module.image), start, start + size


def records(image, key_id):
    """-> [(offset, 16-byte key)] for every occurrence of the ID: the record layout."""
    found, at = [], image.find(key_id)
    while at != -1:
        found.append((at, image[at + len(key_id):at + len(key_id) + 16]))
        at = image.find(key_id, at + 1)
    return found


def shaped(image, start, end, limit=SCAN_LIMIT):
    """-> [(offset, key)] for ID-shaped records in [start, end), bounded by `limit`.

    A record is eight printable bytes followed by sixteen that are not all zero;
    this is a guess about a layout, so every result is still confirmed by digest.
    """
    out, printable = [], bytes(range(0x20, 0x7f))
    for at in range(max(start + 8, 8), min(end, len(image)) - 16 + 1):
        head = image[at - 8:at]
        if not all(c in printable for c in head):
            continue
        key = image[at:at + 16]
        if key == bytes(16):
            continue
        out.append((at - 8, key))
        if len(out) >= limit:
            break
    return out


class Keyring:
    """Resolves EASF keys: a private key file first, then the given modules.

    `key_file` is the user's explicit private source (a JSON object of key ID to
    hex key) and is consulted first, so an explicit choice is never overridden.
    `modules` are NSO0 paths searched for the compiled-in registry. Every key,
    from either source, is only returned after it reproduces the container's
    header digest, so a wrong entry in a key file falls through to discovery
    instead of failing the build.
    """

    def __init__(self, modules=(), key_file=None, bounded=False):
        self.modules = [Path(m) for m in modules]
        self.key_file = Path(key_file) if key_file else None
        self.bounded = bounded
        self.notes = []          # human-readable, key-free, in the order they happened
        self.fingerprints = {}   # key ID -> fingerprint() of the key that was used
        self._private = None
        self._images = {}

    def private(self):
        """-> {key ID: bytes} from the private file, read once; empty without one."""
        if self._private is None:
            self._private = {}
            if self.key_file:
                values = json.loads(self.key_file.read_text())
                for name, value in values.items():
                    try:
                        key = bytes.fromhex(value)
                    except (TypeError, ValueError):
                        continue
                    if len(key) == 16:
                        self._private[name] = key
        return self._private

    def _image(self, path):
        if path not in self._images:
            self._images[path] = module_image(path)
        return self._images[path]

    def _build_id(self, path):
        """-> the module's build ID from its header, for the log (not a key)."""
        with open(path, 'rb') as f:
            head = f.read(0x50)
        return head[0x40:0x50].hex() if len(head) >= 0x50 else ''

    def _took(self, name, key, source, note):
        """Record a confirmed key: its fingerprint and a key-free account of it."""
        self.fingerprints[name] = fingerprint(key)
        self.notes.append(f'{note}; key sha256[:16]={fingerprint(key)}')
        return key, source

    def resolve(self, data):
        """-> (key, source) for this container, both verified, or (None, None).

        `source` is a key-free label such as 'easf key file' or 'main@0x45ee6a5'
        naming where the key came from. Every source is tried; the notes say why
        one did not answer, so a build can explain a fallback without a key.
        """
        header = parse_header(data)
        key_id = header['key_id']
        name = key_id.decode('ascii', 'replace')
        candidate = self.private().get(name)
        if candidate is not None:
            if confirms(data, candidate):
                return self._took(name, candidate, 'easf key file',
                                  f'{name!r} taken from the private key file')
            self.notes.append(f'the key file entry for {name!r} does not verify; '
                              'discovery was tried instead')
        for path in self.modules:
            image = self._image(path)
            found = records(image, key_id)
            for offset, key in found:
                if confirms(data, key):
                    return self._took(name, key, f'{path.name}@{offset:#x}',
                                      f'{name!r} found in {path.name} (build {self._build_id(path)}), '
                                      f'{len(found)} candidate(s) checked')
            if found:
                self.notes.append(f'{path.name}: {len(found)} record(s) named {name!r}, none verifies')
            elif self.bounded:
                image, start, end = rodata(path)
                for offset, key in shaped(image, start, end):
                    if confirms(data, key):
                        return self._took(name, key, f'{path.name}@{offset:#x} (bounded scan)',
                                          f'{name!r} not named in {path.name}; a bounded scan of its '
                                          'read-only segment confirmed it')
                self.notes.append(f'{path.name}: no record named {name!r} '
                                  f'and the bounded scan did not confirm one')
            else:
                self.notes.append(f'{path.name}: no record named {name!r}')
        return None, None
