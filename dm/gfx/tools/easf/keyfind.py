# Adapted from ohxiaoguang/Touchline23, MIT; original authorship retained.
"""Find the EA EASF key of a title inside the title's own program module.

An EASF header (0x30 bytes) is `EASF`, the plaintext length (big endian), the
eight-character key ID, and SHA-256 of the plaintext's first 32 bytes. The
engine's key registry stores one record per ID, laid out as `{char id[8];
uint8 key[16]}`, so the 16 bytes after the ID are the AES-128 key for it.

A candidate is never trusted on its shape: it must decrypt the container's
first two blocks to the digest in the header (32 bytes),
matching the verification after decrypting. No offset, no
build ID, no external key table and no network are used, so the same code runs
against a base game, an update, or any other title that keeps this registry.

The key is never printed, logged or returned in an exception message: callers
see `fingerprint(key)`, the first 16 hex digits of its SHA-256.
"""
import hashlib
from pathlib import Path
import struct

HEADER = 48          # 'EASF' + size + key ID + digest
PROBE = 32           # bytes the digest covers

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
    from aes import decrypt
    plain = decrypt(key, bytes(data[HEADER:HEADER + PROBE]), iv=key)
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




def records(image, key_id):
    """-> [(offset, 16-byte key)] for every occurrence of the ID: the record layout."""
    found, at = [], image.find(key_id)
    while at != -1:
        found.append((at, image[at + len(key_id):at + len(key_id) + 16]))
        at = image.find(key_id, at + 1)
    return found




class Keyring:
    """Resolve verified EASF keys from the matching version's own NSO modules."""

    def __init__(self, modules=()):
        self.modules = [Path(m) for m in modules]
        self.notes = []
        self.fingerprints = {}
        self._images = {}

    def resolve(self, data):
        """Return (key, key-free source label), or (None, None) when no registry record verifies."""
        header = parse_header(data)
        key_id = header['key_id']
        name = key_id.decode('ascii')
        for path in self.modules:
            if path not in self._images:
                self._images[path] = module_image(path)
            found = records(self._images[path], key_id)
            for offset, key in found:
                if confirms(data, key):
                    with path.open('rb') as f:
                        head = f.read(0x50)
                    build = head[0x40:0x50].hex()
                    self.fingerprints[name] = fingerprint(key)
                    self.notes.append(f'{name!r} found in {path.name} (build {build}), '
                                      f'{len(found)} candidate(s); key sha256[:16]={fingerprint(key)}')
                    return key, f'{path.name}@{offset:#x}'
            self.notes.append(f'{path.name}: {len(found)} record(s) named {name!r}, none verifies')
        return None, None
