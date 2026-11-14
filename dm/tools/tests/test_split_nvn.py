"""Vengeance Hunters' loose .nvn files: one NVN program each, a 16-byte header {control length, control offset, code length, code offset} naming the
control section and the code area that follow it. Synthetic bytes only."""
import contextlib
import io
import struct
import tempfile
import unittest
from pathlib import Path

from dm.tools.tests.test_split_gzip import ss
import dm.tools.fuzz.fuzz_shader_split as fz


def nvn_file(ctl, code, co=0x10, pad=0):
    """A .nvn file holding one program: the four-word header, the control section at its offset, the code area right after it"""
    ko = co + len(ctl) + pad
    return struct.pack('<4I', len(ctl), co, len(code), ko) + bytes(co - 0x10) + ctl + bytes(pad) + code


class LooseNvn(unittest.TestCase):
    def scan(self, data, rel='textured_colored_vs.nvn'):
        with tempfile.TemporaryDirectory() as t, contextlib.redirect_stdout(io.StringIO()) as log:
            path = Path(t) / rel; path.write_bytes(data)
            result = ss.scan_file((str(path), rel))
        return result, log.getvalue()

    def test_vs_and_fs(self):
        for st, (ctl, code) in (('vs', fz.VS), ('ps', fz.PS)):
            with self.subTest(stage=st):
                (rel, keys, pairs, extra), log = self.scan(nvn_file(ctl, code))
                self.assertEqual(log, '')
                self.assertEqual({v[1] for v in keys.values()}, {st})
                self.assertEqual(len(keys), 1); self.assertEqual(pairs, set()); self.assertEqual(extra, {})

    def test_offsets_need_not_be_minimal(self):
        ctl, code = fz.VS
        (rel, keys, pairs, extra), log = self.scan(nvn_file(ctl, code, co=0x40, pad=0x30))
        self.assertEqual(log, ''); self.assertEqual(len(keys), 1)

    def test_rejects(self):
        ctl, code = fz.VS
        good = nvn_file(ctl, code)
        # no control magic at the control offset
        bad = bytearray(good); bad[0x10:0x14] = b'NONE'
        # no code magic at the code offset
        bad2 = bytearray(good); bad2[0x890:0x894] = b'NONE'
        # a code offset past the file
        bad3 = bytearray(good); struct.pack_into('<I', bad3, 0xc, len(good) + 0x10)
        # not even the header
        for i, data in enumerate((bytes(bad), bytes(bad2), bytes(bad3), b'\x88\x08\0\0')):
            with self.subTest(case=i):
                (rel, keys, pairs, extra), log = self.scan(data)
                self.assertEqual(keys, {}); self.assertEqual(pairs, set())


if __name__ == '__main__':
    unittest.main()
