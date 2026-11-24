"""shader_split: the shadercache.nx reader (BioShock Remastered, Blind Squirrel's Unreal Engine 3 port). The RomFS file is {u32 version, u32 block count} then per block
{u32 stored length, one zlib stream} of 32 KiB; the NVN records are laid out {16-byte header: u32 control length, u32 code length}, code area, control section, and 2,622 of
the 20,421 records cross a block boundary. A per-block scan finds 18,041 distinct keys; joining the blocks first finds 20,419. No game files needed: the fixture puts one
record across the 32 KiB boundary on purpose.
"""
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz   # its stage() builder and shader_split with a profile
ss = fz.ss

BLOCK = 32768


def rec(ctl, code):
    """One record: the container's 16-byte header (control length, code length, 0, 0), the code area, the control section"""
    return struct.pack('<4I', len(ctl), len(code), 0, 0) + code + ctl


def container(stream):
    """The file a stream writes as: the version and block count, then one zlib stream per 32 KiB block, back to back to the end of the file"""
    blocks = [zlib.compress(stream[i:i + BLOCK]) for i in range(0, len(stream), BLOCK)]
    return struct.pack('<II', 13, len(blocks)) + b''.join(struct.pack('<I', len(b)) + b for b in blocks)


def container_v28(stream):
    """The same stream as version 28 writes it: each block keeps its stored and raw lengths, and the raw length varies
    (BioShock 2 Remastered has 32 KiB blocks in shadercache.nx and one 30,178-byte block in startupshadercache.nx)"""
    blocks = [zlib.compress(stream[i:i + BLOCK]) for i in range(0, len(stream), BLOCK)]
    return struct.pack('<II', 28, len(blocks)) + b''.join(struct.pack('<II', len(b), min(BLOCK, len(stream) - i)) + b
                                                          for i, b in zip(range(0, len(stream), BLOCK), blocks))


VS, PS, GS = fz.stage(0x0461), fz.stage(0x1462), fz.stage(0x1061)   # vertex, pixel, geometry (SPH word 0: type in bits 10..13)
A, B, C = rec(*VS), rec(*PS), rec(*GS)
STREAM = bytes(BLOCK - 200) + A + B + C + B   # A starts 200 bytes before the boundary and ends past it; B is repeated to make one key from two records
FILE = container(STREAM)


class ShadercacheNx(unittest.TestCase):
    def test_header_and_block_table_alone_identify_the_file(self):
        self.assertTrue(ss.is_shadercache_nx(FILE))
        self.assertFalse(ss.is_shadercache_nx(FILE[:-1]))   # the table must end at the file's end
        self.assertFalse(ss.is_shadercache_nx(b'\x28\xb5\x2f\xfd' + bytes(64)))   # a zstd frame
        self.assertFalse(ss.is_shadercache_nx(bytes(64)))
        # a 0x78 byte that fails the RFC 1950 zlib header check (0x7800 % 31 != 0) is rejected
        bogus = struct.pack('<III', 13, 1, 16) + b'\x78\x00' + bytes(14)
        self.assertFalse(ss.is_shadercache_nx(bogus))

    def test_the_record_before_the_first_boundary_is_found_only_after_joining(self):
        ctl_off = len(STREAM) - (len(B) + len(C) + len(B)) - len(VS[0])
        self.assertNotEqual(ctl_off // BLOCK, (ctl_off - len(VS[1])) // BLOCK)   # A's control is in the second block, its code in the first
        found = ss.shadercache_nx(FILE, '/shadercache.nx')
        self.assertEqual(len(found), 4)
        self.assertEqual(found[0][0], f'/shadercache.nx#NXBLK@0x{ctl_off:x}')
        self.assertTrue(all(w.startswith('/shadercache.nx#NXBLK@0x') for w, *_ in found))
        keys = ss.uniq_keys(found)
        self.assertEqual(len(keys), 3)   # A, B, C: B's copy is one key
        self.assertEqual({v[1] for v in keys.values()}, {'vs', 'ps', 'gs'})

    def test_scan_file_dispatches_on_the_container(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'shadercache.nx'
            p.write_bytes(FILE)
            rel, keys, pairs, trees = ss.scan_file_in((str(p), 'shadercache.nx'))
            self.assertEqual((rel, len(keys), pairs, trees), ('shadercache.nx', 3, set(), {}))
            self.assertTrue(all(w.startswith('/shadercache.nx#NXBLK@0x') for w, *_ in keys.values()))
            sub = Path(tmp) / 'sub' / 'shadercache.nx'
            sub.parent.mkdir(); sub.write_bytes(FILE)
            _, sub_keys, _, _ = ss.scan_file_in((str(sub), 'sub/shadercache.nx'))
            self.assertTrue(all(w.startswith('/sub/shadercache.nx#NXBLK@0x') for w, *_ in sub_keys.values()))
            # an unrelated chunked-zlib file with no NVN records returns empty instead of failing [RED]
            non_shader = Path(tmp) / 'data.bin'
            non_shader.write_bytes(container(b'not a shader cache' * 64))
            self.assertEqual(ss.scan_file_in((str(non_shader), 'data.bin')), ('data.bin', {}, set(), {}))
            q = Path(tmp) / 'other.nx'
            q.write_bytes(bytes(4096))
            self.assertEqual(ss.scan_file_in((str(q), 'other.nx')), ('other.nx', {}, set(), {}))

    def test_the_version_28_framing_reads_the_same_records(self):
        # BioShock 2 Remastered's cache: each block carries its stored and raw length; the records are the same stream
        wide = container_v28(STREAM)
        self.assertEqual(ss.shadercache_nx_framing(FILE), 4)
        self.assertEqual(ss.shadercache_nx_framing(wide), 8)
        self.assertTrue(ss.is_shadercache_nx(wide))
        self.assertFalse(ss.is_shadercache_nx(wide[:-1]))   # still has to end at the file's end
        narrow, broad = ss.shadercache_nx(FILE, '/shadercache.nx'), ss.shadercache_nx(wide, '/shadercache.nx')
        self.assertEqual([r[0] for r in narrow], [r[0] for r in broad])
        self.assertEqual(len(ss.uniq_keys(broad)), 3)
        # v28 rejects zero raw length in framing and mismatched inflated length in shadercache_nx
        b = zlib.compress(b'hello' * 20)
        self.assertIsNone(ss.shadercache_nx_framing(struct.pack('<IIII', 28, 1, len(b), 0) + b))
        with self.assertRaisesRegex(SystemExit, 'header says 99'):
            ss.shadercache_nx(struct.pack('<IIII', 28, 1, len(b), 99) + b, '/shadercache.nx')

    def test_a_code_area_with_no_record_header_is_left_out(self):
        # a control magic whose 16 bytes before the code area are not a header: skipped, not read as a record
        code, ctl = VS
        bad = bytes(200) + code + ctl   # no header, so control - code length - 16 reads zeros
        with self.assertRaisesRegex(SystemExit, 'no NVN record'): ss.shadercache_nx(container(bad), '/shadercache.nx')


if __name__ == '__main__':
    unittest.main()
