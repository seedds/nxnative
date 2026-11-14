"""Blizzard's CASC as Diablo II: Resurrected ships it (casc.py) gives the split the shaders of its SHAD containers, whatever the files' encodings; the records of the same form in main are
found by the reference to their start. Synthetic bytes only."""
import contextlib
import io
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

from dm.tools.tests.test_split_gzip import REPO, fz, ss

sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import casc  # noqa: E402

KEY = '079013ef272cc109bebfc8b62fbd2a16'
HEADER = 'FileId!STRING:0|KeyLayout!DEC:4|Chunk!DEC:8|Archive!DEC:8|CKey!HEX:16|CSize!DEC:4|EKey!HEX:16|ESize!DEC:4|Encoding!STRING:0|Path!STRING:0|Flags!STRING:0'


def cs():
    """A compute stage as D2R's: no code header, the thread group at control +0x748"""
    ctl, _ = fz.VS
    code = bytes.fromhex('0f00000000000000') * 16
    ctl = bytearray(ctl); struct.pack_into('<I', ctl, 0x704, len(code)); struct.pack_into('<3I', ctl, 0x748, 64, 1, 1)
    return bytes(ctl), code


def record(ctl, code):
    return struct.pack('<5I', 5, len(ctl), len(code) + 0xc, len(code), 4) + code + ctl


def chunk(tag, body, cnt=0, tag2=None, table=b''):
    hl = 0x1c
    return struct.pack('<4s4sIIIII', tag, tag2 or tag, cnt, 0xffffffff, hl, hl + len(table), hl + len(table) + len(body)) + table + body


def btcd(ctl, code):
    body = struct.pack('<2I', 0, 0) + record(ctl, code)   # the BTCD header is 0x24 bytes
    return struct.pack('<4s4sIIIII', b'BTCD', b'BTCD', 1, len(body) - 8, 0x24, 0x24, 0x1c + len(body)) + body


def shad(*stages):
    progs = [btcd(*s) for s in stages]
    table, at = b'', 0
    for p in progs: table += struct.pack('<I', at); at += len(p)
    prm = chunk(b'PRM[', b''.join(progs), len(progs), table=table)
    return chunk(b'SHAD', chunk(b'TXT[', b'') + chunk(b'SYM[', b'') + chunk(b'SHMD', prm, 1, tag2=b'/\0\0\0'), 3)


def blte(data, size=0x40):
    parts = [b'N' + data[i:i + size] for i in range(0, len(data), size)]
    table = b''.join(struct.pack('>II', len(p), len(p) - 1) + bytes(16) for p in parts)
    return b'BLTE' + struct.pack('>I', 12 + len(table)) + b'\x0f' + len(parts).to_bytes(3, 'big') + table + b''.join(parts)


def storage(folder, files, key=KEY, ekey_archive=None):
    """files: [(file ID, bytes, encoding)] → data/00-00000001.data, the manifest and build_key.txt"""
    arch, lines = bytearray(b'\0' * 0x20), ['## config-key = ' + key, HEADER]
    for fid, data, enc in files:
        stored = zlib.compress(data) if enc == 'z' else blte(data) if enc.startswith('b:') else data
        off = len(arch); arch += stored
        ekey = bytes(9) + bytes([0, 1 if ekey_archive is None else ekey_archive]) + off.to_bytes(5, 'big')
        lines.append(f'{fid}|0|0|1|{"0" * 32}|{len(data)}|{ekey.hex()}|{len(stored)}|{enc}|{fid}|table')
    (folder / '00-00000001.data').write_bytes(arch)
    (folder / 'build_manifest.psv').write_text('\n'.join(lines) + '\n')
    (folder / 'build_key.txt').write_text(KEY)


class Casc(unittest.TestCase):
    def scan(self, files, **kw):
        with tempfile.TemporaryDirectory() as t, contextlib.redirect_stdout(io.StringIO()) as log:
            d = Path(t) / 'data'; d.mkdir(); storage(d, files, **kw)
            listed = [(str(p), 'data/' + p.name) for p in sorted(d.iterdir())]
            jobs, taken = ss.casc_jobs(listed)
            keys = {}
            for job in jobs:
                rel, found, pairs, extra = ss.scan_file(job)
                keys.update(found)
        return keys, jobs, taken, log.getvalue()

    def test_shaders_in_every_encoding(self):
        files = [('data:data/hd/shaders/vs_ui.bin', shad(fz.VS), 'z'), ('data:data/hd/shaders/ps_ui.bin', shad(fz.PS, fz.PS), 'n'),
                 ('data:data/hd/shaders/cs_lut.bin', shad(cs()), 'b:64K*=n'), ('data:data/global/music.opus', b'opus' * 100, 'b:64K*=n')]
        keys, jobs, taken, log = self.scan(files)
        self.assertEqual(log, '')
        self.assertEqual(sorted(v[1] for v in keys.values()), ['cs', 'ps', 'vs'])   # the second PS is the same shader: one key
        self.assertTrue(all('#SHAD@' in v[0] and v[0].startswith('/data/data:data/hd/shaders/') for v in keys.values()))
        self.assertEqual({Path(p).name for p in taken}, {'00-00000001.data'})

    def test_other_build_key_not_read(self):
        keys, jobs, taken, log = self.scan([('a', shad(fz.VS), 'z')], key='00' * 16)
        self.assertEqual((keys, jobs, taken), ({}, [], set()))
        self.assertIn('no manifest', log); self.assertIn('has the config key ' + KEY, log)

    def test_key_names_another_archive(self):
        keys, jobs, taken, log = self.scan([('a', shad(fz.VS), 'z')], ekey_archive=2)
        self.assertEqual(jobs, []); self.assertIn('does not end in chunk 0, archive 1', log)

    def test_bad_record_named(self):
        bad = bytearray(shad(fz.VS)); i = bad.find(b'BTCD') + 0x24; struct.pack_into('<I', bad, i + 0xc, 0x40)   # code length no longer adds up
        keys, jobs, taken, log = self.scan([('data:a.bin', bytes(bad), 'z'), ('data:b.bin', shad(fz.PS), 'z')])
        self.assertIn('does not hold a record of its length', log)

    def test_blte_lengths_checked(self):
        b = bytearray(blte(b'x' * 0x100)); struct.pack_into('>I', b, 16, 0x41)   # chunk 0 claims 0x41 decoded bytes
        with self.assertRaises(casc.Error): casc.blte(bytes(b))
        self.assertEqual(casc.blte(blte(b'x' * 0x100)), b'x' * 0x100)


def nso_image(records):
    """A module image with the MOD0 header, a dynamic section with one RELA relocation per record whose 'rel' is set, and adrp + add to the start of each whose 'adrp' is set"""
    img = bytearray(0x2000)
    struct.pack_into('<I', img, 4, 0x10); img[0x10:0x14] = b'MOD0'; struct.pack_into('<i', img, 0x14, 0x100 - 0x10)
    starts, relas = [], []
    for ctl, code, how in records:
        img += bytes(-len(img) & 0xff); at = len(img); img += record(ctl, code); starts.append((at, how))
        if how == 'rel': relas.append(at)
    rela = len(img); img += b''.join(struct.pack('<QQq', 0x1800 + 8 * i, 0x403, t) for i, t in enumerate(relas))
    struct.pack_into('<4q', img, 0x100, 7, rela, 8, 24 * len(relas)); struct.pack_into('<q', img, 0x120, 0)
    pc = 0x400
    for at, how in starts:
        if how != 'adrp': continue
        page = (at >> 12) - (pc >> 12)
        struct.pack_into('<II', img, pc, 0x90000000 | (page & 3) << 29 | (page >> 2 & 0x7ffff) << 5 | 3, 0x91000000 | (at & 0xfff) << 10 | 3 << 5 | 3); pc += 8
    return bytes(img)


class MainRecords(unittest.TestCase):
    def test_referenced_records_taken(self):
        ps2 = fz.stage(0x1462 | 0x10000)
        img = nso_image([(*fz.PS, 'rel'), (*cs(), 'adrp'), (*ps2, None)])
        with contextlib.redirect_stdout(io.StringIO()) as log: out = ss.raw_shaders(img, 'main')
        self.assertEqual(sorted(st for _, st, _, _ in out), ['cs', 'ps'])
        self.assertTrue(all('#RAW@' in w for w, *_ in out))
        self.assertEqual(log.getvalue().count('[RED] main control section'), 1)   # the record nothing points at


if __name__ == '__main__':
    unittest.main()
