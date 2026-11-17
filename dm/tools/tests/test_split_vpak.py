"""VPAK graphics records use unaligned NVN code followed by its control section; no game assets needed."""
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '0100199023B2E000'
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def record(stage):
    code, ctl = bytearray(0x100), bytearray(0x880)
    code[:4], ctl[:4] = ss.GFX_MAGIC, ss.CTL_MAGIC
    struct.pack_into('<I', code, 0x30, (stage << 10) | 0x61)
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    return struct.pack('<II', len(code), len(ctl)) + code + ctl


class Vpak(unittest.TestCase):
    def test_embedded_records_require_matching_lengths(self):
        blob = b'unaligned' + record(1) + record(5)
        self.assertEqual([r[1] for r in ss.phyre_shaders(blob, 'main', prefixed=True)], ['vs', 'ps'])
        self.assertEqual(ss.phyre_shaders(record(1)[8:], 'main', prefixed=True), [])
        damaged = bytearray(record(1)); struct.pack_into('<I', damaged, 0, 0x200)
        self.assertEqual(ss.phyre_shaders(damaged, 'main', prefixed=True), [])

    def test_file_scanner_reaches_vpak_reader(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'shaders.pak'
            path.write_bytes(b'VPAK\x03\x00\x01\x00' + b'padding' + record(1) + record(5))
            rel, found, pairs, extra = ss.scan_file((str(path), path.name))
        self.assertEqual(rel, 'shaders.pak')
        self.assertEqual(sorted(r[1] for r in found.values()), ['ps', 'vs'])
        self.assertEqual(extra, {})

    def test_unaligned_vertex_and_pixel_records(self):
        out = []
        ss.scan_blob(b'VPAK\x03\x00\x01\x00' + b'padding' + record(1) + record(5), '/shaders.pak', out)
        self.assertEqual([stage for _, stage, _, _ in out], ['vs', 'ps'])
        self.assertTrue(all(ss.check(where, stage, ctl, code) is None for where, stage, ctl, code in out))

    def test_truncated_record_is_not_accepted(self):
        out = []
        ss.scan_blob(b'VPAK\x03\x00\x01\x00' + record(1)[:-1], '/truncated.pak', out)
        self.assertEqual(out, [])

    def test_unknown_version_is_not_assumed(self):
        out = []
        ss.scan_blob(b'VPAK\x04\x00\x01\x00' + record(1), '/future.pak', out)
        self.assertEqual(out, [])


if __name__ == '__main__':
    unittest.main()
