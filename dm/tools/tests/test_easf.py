"""Synthetic EA archive fixtures: no game bytes or keys."""
import hashlib
import io
import os
import json
import subprocess
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT / 'dm/tools'), str(ROOT / 'dm/gfx/tools')]
from easf import reader, keyfind, fxo, chunkzip
from Crypto.Cipher import AES
import lz4.block

KEY = bytes(range(16))
ID = b'fixture1'


def encrypted(payload, key=KEY, name=ID):
    padded = payload + bytes(-len(payload) % 16)
    return b'EASF' + struct.pack('>I', len(payload)) + name + hashlib.sha256(payload[:32]).digest() + AES.new(key, AES.MODE_CBC, iv=key).encrypt(padded)


def module(record=ID + KEY, compress=False):
    h = bytearray(0x100); h[:4] = b'NSO0'; h[0x40:0x60] = bytes(range(64, 96))
    blobs = [bytes(0x1000), record + bytes(0x1000 - len(record)), bytes(0x1000)]
    body = bytearray()
    for i, blob in enumerate(blobs):
        packed = lz4.block.compress(blob, store_size=False) if compress else blob
        struct.pack_into('<III', h, 0x10 + i * 16, 0x100 + len(body), i * 0x1000, len(blob))
        struct.pack_into('<I', h, 0x60 + i * 4, len(packed)); body += packed
    struct.pack_into('<I', h, 0xc, 7 if compress else 0)
    return h + body


def big(name, payload):
    table = struct.pack('>II', 16 + 8 + len(name) + 1, len(payload)) + name + b'\0'
    return b'BIG4' + struct.pack('<I', 16 + len(table) + len(payload)) + struct.pack('>II', 1, 16 + len(table)) + table + payload


def compute():
    ctl = bytearray(0x880); ctl[:4] = fxo.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl)); struct.pack_into('<I', ctl, 0x6f0, 0x900)
    struct.pack_into('<I', ctl, 0x704, 16); struct.pack_into('<4I', ctl, 0x748, 8, 4, 1, 17)
    body = ctl + bytes(0x80) + bytes(range(16))
    return b'NXSHADER' + bytes(8) + b'GLSL' + struct.pack('<I', 8 + len(body)) + body


class EA(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / 'main'

    def ring(self, data=None):
        self.path.write_bytes(module() if data is None else data)
        return keyfind.Keyring(modules=[self.path])

    def test_discovery(self):
        data = encrypted(bytes(range(64))); ring = self.ring()
        self.assertEqual(ring.resolve(data)[0], KEY)
        self.assertNotIn(KEY.hex(), repr(ring.notes))
        self.assertEqual(ring.fingerprints['fixture1'], keyfind.fingerprint(KEY))

    def test_compressed_nso(self):
        self.assertEqual(self.ring(module(compress=True)).resolve(encrypted(bytes(64)))[0], KEY)

    def test_shifted_registry(self):
        self.assertEqual(self.ring(module(bytes(37) + ID + KEY)).resolve(encrypted(bytes(64)))[0], KEY)

    def test_private_override_and_wrong_override_fallback(self):
        self.ring()
        p = Path(self.tmp.name) / 'private.json'
        data = encrypted(bytes(64))
        p.write_text(json.dumps({'fixture1': KEY.hex()}))
        self.assertEqual(keyfind.Keyring(key_file=p).resolve(data)[0], KEY)
        p.write_text(json.dumps({'fixture1': bytes(16).hex()}))
        self.assertEqual(keyfind.Keyring(modules=[self.path], key_file=p).resolve(data)[0], KEY)

    def test_bounded_scan(self):
        self.ring(module(b'other-id' + KEY))
        ring = keyfind.Keyring(modules=[self.path], bounded=True)
        self.assertEqual(ring.resolve(encrypted(bytes(64)))[0], KEY)

    def test_wrong_id(self):
        self.assertIsNone(self.ring().resolve(encrypted(bytes(64), name=b'unknown1'))[0])

    def test_wrong_candidate(self):
        self.assertIsNone(self.ring(module(ID + bytes(16))).resolve(encrypted(bytes(64)))[0])

    def test_wrong_digest(self):
        d = bytearray(encrypted(bytes(64))); d[16] ^= 1
        self.assertIsNone(self.ring().resolve(d)[0])

    def test_truncated_nso(self):
        with self.assertRaises(ValueError): self.ring(module()[:80]).resolve(encrypted(bytes(64)))

    def test_invalid_lz4(self):
        d = module(compress=True); d[0x100:0x104] = b'\xff' * 4
        with self.assertRaises(ValueError): self.ring(d).resolve(encrypted(bytes(64)))

    def test_truncated_easf(self):
        for n in (0, 48, 64, 79):
            with self.subTest(n=n), self.assertRaises(ValueError): keyfind.parse_header(encrypted(bytes(64))[:n])

    def test_big4_bounds(self):
        d = big(b'a.fxo', compute())
        self.assertEqual(len(reader.entries(io.BytesIO(d), len(d))), 1)
        with self.assertRaises(ValueError): reader.entries(io.BytesIO(d), len(d) - 1)
        with self.assertRaises(ValueError): reader.entries(io.BytesIO(b'BIG4'), 4)

    def test_compute_offset_and_facts(self):
        d = compute(); records = fxo.programs(d)
        self.assertEqual(records[0][0], 'cs'); self.assertEqual(records[0][3], bytes(range(16)))
        ctl = records[0][2]
        self.assertEqual(struct.unpack_from('<4I', ctl, 0x748), (8, 4, 1, 17))

    def test_invalid_fxo(self):
        for d in (b'', b'NXSHADER', compute()[:-1]):
            with self.subTest(size=len(d)), self.assertRaises(ValueError): fxo.programs(d)

    def test_chunkzip(self):
        payload = bytes(range(64)); z = zlib.compressobj(wbits=-15); compressed = z.compress(payload) + z.flush()
        h = bytearray(48); h[:8] = b'chunkzip'; struct.pack_into('>5I', h, 8, 2, 64, 64, 1, 16)
        self.assertEqual(chunkzip.decode(h + compressed)[1], payload)
        with self.assertRaises(ValueError): chunkzip.decode(h + compressed[:-1])
        struct.pack_into('>I', h, 16, 0)
        with self.assertRaises(ValueError): chunkzip.decode(h + compressed)

    def test_non_shader_resource(self):
        self.assertEqual(reader.archive(encrypted(bytes(64)), 'image', self.ring()), [])

    def test_end_to_end(self):
        d = encrypted(big(b'fixture.cs.fxo', compute()))
        found = reader.archive(d, 'synthetic', self.ring())
        self.assertEqual(len(found), 1); self.assertEqual(found[0][1], 'cs')

    def test_pipeline_exports_package_and_compute_query(self):
        self.ring()
        root = Path(self.tmp.name); rom = root / 'RomFS'; rom.mkdir()
        (rom / 'data.big').write_bytes(big(b'shader.big', encrypted(big(b'a.fxo', compute()))))
        env = dict(os.environ, DM_GAME='010015100B514000', J='1')
        out = subprocess.run([sys.executable, ROOT / 'dm/gfx/tools/shader_split.py', rom, self.path,
                              root / 'pkg', root / 'map.tsv', root / 'facts.tsv', root / 'pairs.tsv'],
                             env=env, capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        rows = [l for l in (root / 'facts.tsv').read_text().splitlines() if l and not l.startswith('#')]
        fact = json.loads(rows[0].split('\t')[1])
        self.assertEqual(fact, {'local': [8, 4, 1], 'shared': 32})
        self.assertEqual(len(list((root / 'pkg').glob('*.bin'))), 1)

    def test_reader_through_outer_big(self):
        self.ring()
        p = Path(self.tmp.name) / 'outer.big'
        p.write_bytes(big(b'arbitrary.resource', encrypted(big(b'a.fxo', compute()))))
        found = reader.scan(p, self.path)
        assert found is not None
        self.assertEqual(len(found), 1)


if __name__ == '__main__': unittest.main()
