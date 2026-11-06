"""Snowdrop's SDF slices of BERG version 23 (Starlink: Battle for Atlas): chained LZ4 entries found through the part lists, without game data."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split
from test_snowdrop import shader

HEAD = ('  CompSize       Size Compressed/Uncompressed                              \n'
        '         ^          ^ ^ SDF File Index(InstallPart)                           \n'
        '         |          | |  ^ File Part Index                         \n'
        '         |          | |  | ^     Offset   PartOffs Block/Read-ahead\n')


def literals(raw):
    """raw as literal-only LZ4 sequences, one per 64 KB piece, as the game's pieces are cut"""
    out = bytearray()
    for i in range(0, len(raw), 0x10000):
        p = raw[i:i + 0x10000]; n = len(p)
        if n < 15: out.append(n << 4)
        else:
            out.append(0xF0); n -= 15
            while n >= 255: out.append(255); n -= 255
            out.append(n)
        out += p
    return bytes(out)


def row(stored, raw, comp, idx, off, blk, kind, path, ident=None):
    return f'{stored:10d} {raw:10d} {comp} {idx}(1) 0 {off:10d} 0 {blk:3d}(0) ' + (f'0x{ident:016x} ' if ident is not None else '') + f'{kind:20s} {path}\n'


class SdfLz4Tests(unittest.TestCase):
    def test_matches_reach_back_into_the_block_and_a_last_match_is_cut(self):
        first = split.sdf_lz4(bytes([0x80]) + b'abcdefgh', 8, b'', 't')
        self.assertEqual(first, b'abcdefgh')
        self.assertEqual(split.sdf_lz4(bytes([0x04, 8, 0]), 6, first, 't'), b'abcdef')   # a match of 8 cut at the entry's 6 bytes
        with self.assertRaisesRegex(SystemExit, 'before the block'): split.sdf_lz4(bytes([0x04, 8, 0]), 6, b'', 't')

    def test_pieces_of_64_kb(self):
        raw = bytes(range(256)) * 0x110 + b'tail!'
        self.assertEqual(split.sdf_lz4(literals(raw), len(raw), b'', 't'), raw)
        with self.assertRaisesRegex(SystemExit, 'stored bytes left'): split.sdf_lz4(literals(raw) + b'\0', len(raw), b'', 't')
        with self.assertRaisesRegex(SystemExit, 'ends before'): split.sdf_lz4(literals(raw)[:-1], len(raw), b'', 't')

    def layout(self, t, update):
        """data/ holds the slices (rewritten by an update in place), diff005/ the update's list. → the slice's stages"""
        ctl, code = shader('ps')
        obj = b'\x91\xd9\xcd\xbb' + bytes(0x51) + ctl + code
        other = b'class Thing\r\n{\r\n}\r\n'
        e1, e2 = literals(other), literals(obj)
        blob = b'BERG\x17\0\0\0massive\0' + bytes(0x28)
        o1 = len(blob); blob += e1; o2 = len(blob); blob += e2
        (t / 'data').mkdir(); (t / 'data/sdf-A-0001.sdfdata').write_bytes(blob)
        stale = HEAD + row(10, 10, 'C', 1, 56, 0, 'mcache', 'onward/shadercache/nvn/old.mcache')
        good = HEAD + row(len(e1), len(other), 'C', 1, o1, 0, 'fruit', 'a.fruit') + row(len(e2), len(obj), 'C', 1, o2, 0, 'mcache', 'onward/shadercache/nvn/1.mcache')
        if update:
            (t / 'data/_DataPartList.txt').write_text(stale)
            (t / 'diff005').mkdir(); (t / 'diff005/_DataPartList.txt').write_text(good.replace(' fruit', ' 0x0000000000000001 fruit').replace(' mcache', ' 0x0000000000000002 mcache'))
        else: (t / 'data/_DataPartList.txt').write_text(good)
        files = sorted((str(p), str(p.relative_to(t))) for p in t.rglob('*') if p.is_file())
        jobs, _ = split.sdf_list_jobs(files)
        self.assertEqual(len(jobs), 1)
        rel, keys, _, extra = split.scan_file(jobs[0])
        self.assertEqual(extra, {})
        return rel, [v[1] for v in keys.values()]

    def test_part_list_finds_shader_objects(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertEqual(self.layout(Path(t), update=False), ('data/sdf-A-0001.sdfdata', ['ps']))

    def test_newest_list_describes_a_rewritten_slice(self):
        with tempfile.TemporaryDirectory() as t:   # the base list's stale row would not decode
            self.assertEqual(self.layout(Path(t), update=True), ('data/sdf-A-0001.sdfdata', ['ps']))

    def test_a_list_that_does_not_match_its_slice_is_a_red_line(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t); (t / 'data').mkdir()
            (t / 'data/sdf-A-0001.sdfdata').write_bytes(b'BERG' + bytes(60) + literals(b'x' * 40))
            (t / 'data/_DataPartList.txt').write_text(HEAD + row(42, 30, 'C', 1, 64, 0, 'mcache', 'a.mcache'))
            jobs, _ = split.sdf_list_jobs([(str(p), str(p.relative_to(t))) for p in t.rglob('*') if p.is_file()])
            rel, keys, _, extra = split.scan_file(jobs[0])
            self.assertEqual((keys, extra[0]), ({}, 'unreadable'))
            self.assertIn('literals past the 30 bytes', extra[1])

    def test_no_list_no_jobs(self):
        self.assertEqual(split.sdf_list_jobs([('/x/_DataPartList.txt.bak', 'x')]), ([], set()))


if __name__ == '__main__':
    unittest.main()
