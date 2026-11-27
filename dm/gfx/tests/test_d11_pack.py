"""Double Eleven's .pkdswitch resource packs read through their .pkiswitch index, without game data."""
from pathlib import Path
import contextlib
import io
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


def nvnbin(stage):
    """a loose NVNsh shader record (as in test_sumo.py), which the member scanner finds"""
    ctl, code = bytearray(0x850), bytearray(0x100)
    ctl[:4] = split.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    code[:4] = split.GFX_MAGIC
    struct.pack_into('<I', code, 0x30, (split.STAGES.index(stage) + 1) << 10 | 1)
    if stage == 'ps': struct.pack_into('<I', code, 0x48, 1)
    d = bytearray(0x34 + len(ctl) + len(code))
    d[:8] = split.NVNSH + struct.pack('<H', 1)
    struct.pack_into('<I', d, 8, len(d))
    struct.pack_into('<I', d, 28, split.STAGES.index(stage))
    struct.pack_into('<4I', d, 32, 0x34, 0x34 + len(ctl), len(code), len(ctl))
    d[0x34:0x34 + len(ctl)], d[0x34 + len(ctl):] = ctl, code
    return bytes(d)


def pack(tmp, members):
    """writes x.pkdswitch and x.pkiswitch from (length, stored bytes, name hash) members; returns the data file's path"""
    data, idx = b'', struct.pack('<3I', 3, 0, len(members))
    for size, stored, h in members:
        idx += struct.pack('<4I', size, len(stored), len(data), h)
        data += stored
    path = Path(tmp) / 'x.pkdswitch'
    path.write_bytes(data)
    path.with_suffix('.pkiswitch').write_bytes(idx)
    return str(path)


class D11PackTests(unittest.TestCase):
    def test_plain_and_zlib_members(self):
        vs, ps = nvnbin('vs'), nvnbin('ps')
        with tempfile.TemporaryDirectory() as tmp:
            out = []
            split.d11_pack_shaders(pack(tmp, [(len(vs), vs, 0x11), (len(ps), zlib.compress(ps), 0x22)]), '/x.pkdswitch', out)
        self.assertEqual([(lb.split('#')[1], st) for lb, st, _, _ in out], [('00000011', 'vs'), ('00000022', 'ps')])

    def test_member_with_nothing_stored_does_not_end_the_pack(self):
        # LEGO Harry Potter Collection's launcher.pkdswitch (issue #3382): member 90 of 158 lists 8 bytes and stores none; read as a zlib stream, it stopped the pack
        vs, ps = nvnbin('vs'), nvnbin('ps')
        with tempfile.TemporaryDirectory() as tmp:
            out = []
            split.d11_pack_shaders(pack(tmp, [(len(vs), vs, 0x11), (8, b'', 0xbd7144e6), (len(ps), zlib.compress(ps), 0x22)]), '/x.pkdswitch', out)
        self.assertEqual([(lb.split('#')[1], st) for lb, st, _, _ in out], [('00000011', 'vs'), ('00000022', 'ps')])

    def test_long_member_with_nothing_stored_is_reported(self):
        # 8 bytes cannot hold a shader and pass quietly; a member as long as a control section could, so it is named, and the pack still goes on
        ps = nvnbin('ps')
        for size, red in [(8, False), (0x75f, False), (0x760, True)]:
            with self.subTest(size=size), tempfile.TemporaryDirectory() as tmp:
                out, log = [], io.StringIO()
                with contextlib.redirect_stdout(log): split.d11_pack_shaders(pack(tmp, [(size, b'', 0x33), (len(ps), ps, 0x22)]), '/x.pkdswitch', out)
                self.assertEqual([st for _, st, _, _ in out], ['ps'])
                self.assertEqual('[RED] /x.pkdswitch: member 0 (00000033)' in log.getvalue(), red)

    def test_whole_file_scan_keeps_every_member(self):
        # through scan_file, as the split reads it: a red line there leaves out the whole pack, so on main not even member 0's shader reached the map
        vs, ps = nvnbin('vs'), nvnbin('ps')
        with tempfile.TemporaryDirectory() as tmp:
            path = pack(tmp, [(len(vs), vs, 0x11), (8, b'', 0xbd7144e6), (len(ps), zlib.compress(ps), 0x22)])
            with contextlib.redirect_stdout(io.StringIO()): _, keys, _, extra = split.scan_file((path, 'x.pkdswitch'))
        self.assertEqual(extra, {})
        self.assertEqual(sorted(w.split('#')[1] for w, _, _, _ in keys.values()), ['00000011', '00000022'])

    def test_damaged_members_still_stop_the_pack(self):
        ps = nvnbin('ps')
        for members, msg in [([(len(ps), b'\x78\x9c\x01', 0x22)], r'member 0 \(00000022\) is no zlib stream'),
                             ([(len(ps) + 1, zlib.compress(ps), 0x22)], r'member 0 \(00000022\) inflates to'),
                             ([(4, b'', 0x33), (8, b'1', 0x44)], r'member 1 \(00000044\) is no zlib stream')]:
            with self.subTest(msg=msg), tempfile.TemporaryDirectory() as tmp:
                with self.assertRaisesRegex(SystemExit, msg): split.d11_pack_shaders(pack(tmp, members), '/x.pkdswitch', [])


if __name__ == '__main__': unittest.main()
