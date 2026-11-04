"""Compressed, verified intermediate shader results, independent of downstream rewrites and Metal flags.
An entry is published atomically only after its stage succeeds. Corrupt or incomplete entries are misses.
The caller supplies the stage's compiler identity and every semantic input; temporary paths are not inputs.
"""
import collections
import hashlib
import json
import os
import struct
import threading
import zlib

MAGIC = b'DMSHSTAGE1\0'


def digest(identity, inputs, binary=b''):
    record = json.dumps([identity, inputs], sort_keys=True, separators=(',', ':')).encode()
    return hashlib.sha256(struct.pack('<Q', len(record)) + record + binary).hexdigest()


class Cache:
    def __init__(self, root):
        self.root = os.path.join(root, 'stages-v1')
        self.stats, self.lock = collections.Counter(), threading.Lock()

    def count(self, stage, result):
        with self.lock: self.stats[stage, result] += 1

    def path(self, stage, key):
        if stage not in ('xlate', 'msl') or len(key) != 64 or any(c not in '0123456789abcdef' for c in key): raise ValueError('invalid shader stage cache key')
        return os.path.join(self.root, stage, key[:2], key + '.z')

    def get(self, stage, key):
        try:
            with open(self.path(stage, key), 'rb') as f: stored = f.read()
            if not stored.startswith(MAGIC): raise ValueError('format')
            checksum, packed = stored[len(MAGIC):len(MAGIC) + 32], stored[len(MAGIC) + 32:]
            raw = zlib.decompress(packed)
            if hashlib.sha256(raw).digest() != checksum: raise ValueError('checksum')
            size = struct.unpack_from('<I', raw)[0]
            entries = json.loads(raw[4:4 + size]); at = 4 + size; parts = {}
            for name, length in entries:
                if not isinstance(name, str) or name in parts or not isinstance(length, int) or length < 0 or at + length > len(raw): raise ValueError('entry')
                parts[name] = raw[at:at + length]; at += length
            if at != len(raw): raise ValueError('length')
        except (OSError, ValueError, TypeError, KeyError, struct.error, zlib.error):
            self.count(stage, 'miss'); return None
        self.count(stage, 'hit'); return parts

    def put(self, stage, key, parts):
        entries = sorted(parts.items())
        header = json.dumps([(name, len(data)) for name, data in entries], separators=(',', ':')).encode()
        raw = struct.pack('<I', len(header)) + header + b''.join(data for _, data in entries)
        stored = MAGIC + hashlib.sha256(raw).digest() + zlib.compress(raw, 1)
        path = self.path(stage, key); os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + f'.tmp-{os.getpid()}-{threading.get_ident()}'
        try:
            with open(tmp, 'wb') as f: f.write(stored)
            os.replace(tmp, path)
        finally:
            try: os.unlink(tmp)
            except FileNotFoundError: pass
        self.count(stage, 'stored')

    def summary(self):
        return 'intermediate cache: ' + '; '.join(f'{stage} {self.stats[stage, "hit"]} reused, {self.stats[stage, "stored"]} stored' for stage in ('xlate', 'msl'))
