"""shader_split: UbiArt IPK archives (Rayman Legends: Definitive Edition's bundle_nn.ipk). The .fxb effect files are read, zlib or stored, and nothing else; shaders_dev/ is left
out; a damaged archive is named by its layout, not an IndexError. Synthetic bytes only."""
import contextlib
import io
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz
ss = fz.ss

PROGRAM = fz.glslc([fz.VS, fz.PS])


def ipk(entries, version=7):
    """A version 7 IPK: the 0x38-byte header (magic, version, platform, base offset, file count), then per entry u32 offset count, size, compressed size (0: stored),
    u64 time, u64 offset from the base, u32 + directory, u32 + name, u32 CRC, u32 0; the data from the base. entries: (directory, name, data, compress)"""
    table, data = b'', b''
    for d, n, body, comp in entries:
        packed = zlib.compress(body) if comp else body
        table += struct.pack('>IIIQQ', 1, len(body), len(packed) if comp else 0, 0, len(data))
        table += struct.pack('>I', len(d)) + d.encode() + struct.pack('>I', len(n)) + n.encode() + struct.pack('>II', 0, 0)
        data += packed
    base = 0x38 + len(table)
    head = ss.UBIART_IPK + struct.pack('>IIII', version, 0x10, base, len(entries))
    return head + bytes(0x38 - len(head)) + table + data


FXB = 'enginedata/shaders/compiled/nx/'


class Ipk(unittest.TestCase):
    def scan(self, data):
        with tempfile.TemporaryDirectory() as t, contextlib.redirect_stdout(io.StringIO()) as log:
            path = Path(t) / 'bundle_nn.ipk'; path.write_bytes(data)
            result = ss.scan_file((str(path), 'bundle_nn.ipk'))
        return result, log.getvalue()

    def test_fxb_entries_are_read(self):
        for comp in (True, False):
            with self.subTest(compressed=comp):
                (rel, keys, pairs, extra), log = self.scan(ipk([(FXB, 'renderpct.fx.fxb', PROGRAM, comp)]))
                self.assertEqual(log, ''); self.assertEqual(rel, 'bundle_nn.ipk')
                self.assertEqual(sorted(v[1] for v in keys.values()), ['ps', 'vs'])
                self.assertEqual({v[0] for v in keys.values()}, {'/bundle_nn.ipk/' + FXB + 'renderpct.fx.fxb#GLSLC@0x0#var'}); self.assertEqual(len(pairs), 1)

    def test_other_entries_are_left_out(self):
        (_, keys, pairs, _), log = self.scan(ipk([('enginedata/shaders_dev/compiled/nx/', 'renderpct.fx.fxb', PROGRAM, True), ('world/', 'level.isc.ckd', PROGRAM, True),
                                                 (FXB, 'font.fx.fxb', b'no shader here', True)]))
        self.assertEqual((keys, pairs, log), ({}, set(), ''))

    def test_damage_is_named(self):
        good = ipk([(FXB, 'a.fxb', PROGRAM, True)])
        bad_stream = bytearray(good); bad_stream[-len(zlib.compress(PROGRAM)):] = bytes(len(zlib.compress(PROGRAM)))
        cut_table = bytearray(good); struct.pack_into('>I', cut_table, 0x10, 2)   # two entries announced, one in the table
        for label, data, why in (('version', ipk([(FXB, 'a.fxb', PROGRAM, True)], version=6), 'UbiArt IPK version 6'), ('zlib', bytes(bad_stream), 'invalid zlib stream'),
                                 ('table', bytes(cut_table), 'runs past the IPK table'), ('data', good[:-8], 'runs past the end of the archive'),
                                 ('header', good[:0x20], 'shorter than a UbiArt IPK header')):
            with self.subTest(damage=label):
                (_, keys, pairs, extra), log = self.scan(data)
                self.assertEqual((keys, pairs), ({}, set()))
                self.assertEqual(extra[0], 'unreadable'); self.assertIn(why, extra[1])
                self.assertTrue(log.startswith('[RED] /bundle_nn.ipk'), log)


if __name__ == '__main__': unittest.main()
