"""Double Eleven's .pkdswitch resource packs read through their .pkiswitch index, without game data."""
from pathlib import Path
import contextlib
import io
import os
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
_had = os.environ.get('DM_GAME')
os.environ['DM_GAME'] = '01007820196A6000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as split
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def stage_pair(stage, salt=0):
    """a synthetic (control section, code area) pair for one stage"""
    ctl, code = bytearray(0x880), bytearray(0x100)
    ctl[:4] = split.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    code[:4] = split.GFX_MAGIC
    struct.pack_into('<I', code, 0x30, (split.STAGES.index(stage) + 1) << 10 | 1)
    if stage == 'ps': struct.pack_into('<I', code, 0x48, 1)
    struct.pack_into('<Q', code, len(code) - 16, 0x0f00 + salt)
    return bytes(ctl), bytes(code)


def nvnbin(stage, salt=0):
    """a loose NVNsh shader record, which scan_blob finds"""
    ctl, code = stage_pair(stage, salt)
    d = bytearray(0x34 + len(ctl) + len(code))
    d[:8] = split.NVNSH + struct.pack('<H', 1)
    struct.pack_into('<I', d, 8, len(d))
    struct.pack_into('<I', d, 28, split.STAGES.index(stage))
    struct.pack_into('<4I', d, 32, 0x34, 0x34 + len(ctl), len(code), len(ctl))
    d[0x34:0x34 + len(ctl)], d[0x34 + len(ctl):] = ctl, code
    return bytes(d)


def glslc_stage(stage, salt=0):
    """a single-stage GLSLC output (the real .pkdswitch shader payload), which glslc_shaders finds"""
    ctl, code = stage_pair(stage, salt)
    d = bytearray(0x90 * 2)
    d[:4] = b'\x91\x68\x86\x19'
    struct.pack_into('<I', d, 0x4c, 1)
    at = len(d)
    d += ctl + code
    struct.pack_into('<III', d, 0x90, len(ctl) + len(code), at, 0)
    struct.pack_into('<4I', d, 0x90 + 0x30, 0, len(ctl), len(code), len(ctl))
    struct.pack_into('<I', d, 0x44, len(d))
    return bytes(d)


def pack(tmp, members, reorder=None):
    """writes x.pkdswitch and x.pkiswitch from (size, stored, payload, hash) or (size, stored, payload, hash, off) members;
    returns the data file's path. Unlike a compressed entry (stored = len(payload)), a raw member uses stored = 0 with len(payload) == size."""
    data, rows = bytearray(), []
    for m in members:
        if len(m) == 4:
            size, stored, payload, h = m
            off = len(data)
            data += payload
        else:
            size, stored, payload, h, off = m
            if off + len(payload) > len(data): data.extend(bytes(off + len(payload) - len(data)))
            data[off:off + len(payload)] = payload
        rows.append((size, stored, off, h))
    if reorder is not None: rows = [rows[i] for i in reorder]
    idx = bytearray(struct.pack('<3I', 3, 0, len(rows)))
    for row in rows: idx += struct.pack('<4I', *row)
    path = Path(tmp) / 'x.pkdswitch'
    path.write_bytes(data)
    path.with_suffix('.pkiswitch').write_bytes(idx)
    return str(path)


class D11PackTests(unittest.TestCase):
    def test_compressed_raw_and_plain_members_are_all_scanned(self):
        # zlib (stored < size), raw-marker (stored == 0 with size bytes on disk, including a whole shader), and plain (stored == size)
        vs = glslc_stage('vs', 1)
        ps = glslc_stage('ps', 2)
        vs2 = nvnbin('vs')
        with tempfile.TemporaryDirectory() as tmp:
            out = []
            p = pack(tmp, [
                (len(vs), len(zlib.compress(vs)), zlib.compress(vs), 0x11),
                (len(ps), 0, ps, 0x22),
                (len(vs2), len(vs2), vs2, 0x33),
            ])
            split.d11_pack_shaders(p, '/x.pkdswitch', out)
        self.assertEqual([(lb.split('#')[1], st) for lb, st, _, _ in out], [('00000011', 'vs'), ('00000022', 'ps'), ('00000033', 'vs')])

    def test_observed_raw_member_layout_and_whole_file_scan(self):
        # LEGO Harry Potter Collection launcher.pkdswitch (issue #3382): member 90 has size=8, stored=0, off[91] == off[90] + 8;
        # through scan_file, all preceding and subsequent shader members reach the shader map
        vs, ps = glslc_stage('vs', 10), glslc_stage('ps', 11)
        zvs, zps = zlib.compress(vs), zlib.compress(ps)
        with tempfile.TemporaryDirectory() as tmp:
            path = pack(tmp, [
                (len(vs), len(zvs), zvs, 0x11),
                (8, 0, b'raw8byte', 0xbd7144e6),
                (len(ps), len(zps), zps, 0x22),
            ])
            with contextlib.redirect_stdout(io.StringIO()) as log:
                _, keys, _, extra = split.scan_file((path, 'launcher_dll/launcher.pkdswitch'))
        self.assertEqual(log.getvalue(), '')
        self.assertEqual(extra, {})
        self.assertEqual(sorted((w.split('#')[1], st) for w, st, _, _ in keys.values()), [('00000011', 'vs'), ('00000022', 'ps')])

    def test_trailing_raw_empty_dedup_and_unsorted_index_entries(self):
        vs, ps = glslc_stage('vs', 20), glslc_stage('ps', 21)
        zvs = zlib.compress(vs)
        with tempfile.TemporaryDirectory() as tmp:
            # Truly empty member (size=0, stored=0), deduplicated 8-byte raw member, and trailing raw shader, with index entries out of offset order
            p = pack(tmp, [
                (len(vs), len(zvs), zvs, 0x11),
                (0, 0, b'', 0xaa),
                (8, 0, b'12345678', 0xbb),
                (8, 0, b'', 0xcc, len(zvs)),      # exact deduplicated copy of member 2 at the same offset
                (len(ps), 0, ps, 0x22),
            ], reorder=[4, 2, 0, 3, 1])
            out = []
            split.d11_pack_shaders(p, '/x.pkdswitch', out)
        self.assertEqual(sorted((lb.split('#')[1], st) for lb, st, _, _ in out), [('00000011', 'vs'), ('00000022', 'ps')])

    def test_invalid_raw_member_layouts_are_rejected(self):
        ps = glslc_stage('ps', 30)
        zps = zlib.compress(ps)
        # 1. Fabricated zero-byte member (stored=0, size=8, next member starts at the same offset)
        # 2. Partial overlap (stored=0, size=8, only 4 bytes before next member)
        # 3. Preceding compressed member runs into a raw member's offset
        # 4. Raw member runs past the end of the data file
        cases = [
            ('zero-byte placeholder', [(8, 0, b'', 0xbd7144e6), (len(ps), len(zps), zps, 0x22)], r'member 0 \(bd7144e6\) has stored length 0 and length 8, overlapping member 1'),
            ('partial overlap', [(8, 0, b'1234', 0xbd7144e6), (len(ps), len(zps), zps, 0x22)], r'member 0 \(bd7144e6\) has stored length 0 and length 8, overlapping member 1'),
            ('preceding overlap', [(len(ps), len(zps), zps, 0x22, 0), (8, 0, b'', 0xbd7144e6, 4)], r'member 1 \(bd7144e6\) at 0x4 overlaps member 0'),
            ('past eof', [(8, 0, b'1234', 0xbd7144e6)], r'member 0 runs past the end of the data file'),
        ]
        for name, members, msg in cases:
            with self.subTest(case=name), tempfile.TemporaryDirectory() as tmp:
                with self.assertRaisesRegex(SystemExit, msg):
                    split.d11_pack_shaders(pack(tmp, members), '/x.pkdswitch', [])

    def test_damaged_zlib_members_still_stop_the_pack(self):
        ps = glslc_stage('ps', 40)
        zps = zlib.compress(ps)
        cases = [
            ([(len(ps), 3, b'\x78\x9c\x01', 0x22)], r'member 0 \(00000022\) is no zlib stream'),
            ([(len(ps) + 1, len(zps), zps, 0x22)], r'member 0 \(00000022\) inflates to'),
            ([(8, 0, b'raw8byte', 0x33), (8, 1, b'1', 0x44)], r'member 1 \(00000044\) is no zlib stream'),
        ]
        for members, msg in cases:
            with self.subTest(msg=msg), tempfile.TemporaryDirectory() as tmp:
                with self.assertRaisesRegex(SystemExit, msg):
                    split.d11_pack_shaders(pack(tmp, members), '/x.pkdswitch', [])


if __name__ == '__main__': unittest.main()
