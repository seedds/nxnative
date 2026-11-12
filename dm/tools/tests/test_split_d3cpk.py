"""Diablo III's console packages (d3cpk.py) give the shaders of their zlib streams to the split, whatever byte ranges the jobs cut them into, and name a control section whose code
area is not in its stream. Synthetic bytes only."""
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
import d3cpk  # noqa: E402

CTL_LEN = 0x850   # 2.7.7's control sections


def stage(sph_type):
    ctl, code = fz.stage(sph_type)
    ctl = bytearray(ctl[:CTL_LEN]); struct.pack_into('<I', ctl, 0x20, CTL_LEN)
    return bytes(ctl), code


VS, PS = stage(0x0461), stage(0x1462)


def sno(*stages, pad=0x24):
    """An SNO asset: its 0xdeadbeef header, other data, then each stage's control section directly followed by its code area"""
    body = bytearray(struct.pack('<4I', 0xdeadbeef, 37, 1, 0) + bytes(pad))
    for ctl, code in stages: body += ctl + code + bytes(0x10)
    return bytes(body)


def record(data, stored_len=None):
    z = zlib.compress(data, 9)
    return struct.pack('<IH', len(data) if stored_len is None else stored_len, len(z)) + z


def package(*parts):
    head = bytearray(0x48); head[:4] = d3cpk.MAGIC; struct.pack_into('<I', head, 4, 7)
    return bytes(head) + b''.join(parts)


class D3Package(unittest.TestCase):
    def scan(self, data, ranges=None):
        with tempfile.TemporaryDirectory() as t, contextlib.redirect_stdout(io.StringIO()) as log:
            path = Path(t) / 'ClientCommon.cpk'; path.write_bytes(data)
            jobs, paths = ss.d3cpk_jobs([(str(path), 'CPKs/ClientCommon.cpk')])
            self.assertEqual(paths, {str(path)})
            if ranges is not None: jobs = [(str(path), 'CPKs/ClientCommon.cpk', (ss.D3CPK, a, b)) for a, b in ranges]
            keys = {}
            for job in jobs:
                rel, found, pairs, extra = ss.scan_file(job)
                self.assertEqual(extra, {})
                keys.update(found)
        return keys, log.getvalue()

    def test_shaders_in_streams(self):
        stored = b'\x00' * 6 + b'\x78\xda' + bytes(range(256)) * 4   # stored data with a zlib header in it: not a stream
        data = package(stored, record(sno(VS, PS)), bytes(0x3000), record(b'texture' * 400), record(sno(PS, pad=0x200)))
        keys, log = self.scan(data)
        self.assertEqual(sorted(v[1] for v in keys.values()), ['ps', 'vs'])   # the second PS is the same shader: one key
        self.assertEqual(log, '')
        self.assertTrue(all('#D3@' in v[0] for v in keys.values()))

    def test_ranges_cut_anywhere(self):
        data = package(record(sno(VS)), bytes(0x100), record(sno(PS)), record(sno(stage(0x1462 | 0x10000))))
        whole, _ = self.scan(data, [(0, len(data))])
        for cut in range(0x48, len(data), 97):
            with self.subTest(cut=cut):
                parts, log = self.scan(data, [(0, cut), (cut, len(data))])
                self.assertEqual(set(parts), set(whole)); self.assertEqual(log, '')
        self.assertEqual(len(whole), 3)

    def test_cut_code_area_named(self):
        ctl, code = VS
        data = package(record(sno(PS)), record(struct.pack('<4I', 0xdeadbeef, 37, 1, 0) + ctl + code[:0x40]))
        keys, log = self.scan(data)
        self.assertEqual([v[1] for v in keys.values()], ['ps'])
        self.assertIn('[RED] /CPKs/ClientCommon.cpk', log); self.assertIn('1 control section(s) not followed by their code area', log)

    def test_wrong_length_not_a_stream(self):
        good = sno(VS)
        data = package(record(good, stored_len=len(good) + 1), record(good, stored_len=len(good) - 1))
        keys, log = self.scan(data)
        self.assertEqual(keys, {}); self.assertEqual(log, '')

    def test_not_a_package(self):
        self.assertTrue(d3cpk.is_d3cpk(package()[:8]))
        self.assertFalse(d3cpk.is_d3cpk(b'CPK \0\0\0\0'))
        self.assertFalse(d3cpk.is_d3cpk(d3cpk.MAGIC + struct.pack('<I', 8)))


if __name__ == '__main__':
    unittest.main()
