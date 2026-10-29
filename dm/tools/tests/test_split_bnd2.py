"""shader_split: Criterion Bundle 2 files ('bnd2', Need for Speed: Hot Pursuit Remastered's SHADERS.BNDL) are read resource by resource, each block inflated
when the bundle is compressed (bnd2_resources), and a layout the reader does not know is named, not read. No game files needed."""
import os
import struct
import sys
import unittest
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010029B0118E8000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def bundle(resources, compressed=True, version=3):
    """resources: [(id, type, [block 0 bytes, block 1 bytes, …])] → a bnd2 file: header, 0x50-byte entries, then each block's data area"""
    n, eoff = len(resources), 0x30
    areas = [b'', b'', b'', b'']; entries = []
    for rid, typ, blocks in resources:
        sizes, offs = [0] * 4, [0] * 4
        for k, raw in enumerate(blocks):
            if not raw: continue
            data = zlib.compress(raw) if compressed else raw
            offs[k] = len(areas[k]); sizes[k] = len(data) | 0x30000000   # the alignment in the top nibble
            areas[k] += data
        entries.append(struct.pack('<QQ4I4I4IIIHBB', rid, 0, *[len(b) for b in blocks] + [0] * (4 - len(blocks)), *sizes, *offs, 0, typ, 0, 0, 0) + bytes(4))   # 0x4c bytes of fields, padded to 0x50
    base = eoff + n * 0x50; datoff = []
    for a in areas: datoff.append(base); base += len(a)
    head = struct.pack('<4sIIIII4II', b'bnd2', version, 1, base, n, eoff, *datoff, 1 if compressed else 0) + bytes(4)
    return head + b''.join(entries) + b''.join(areas)


class Bnd2(unittest.TestCase):
    def test_resources_are_inflated_by_block(self):
        d = bundle([(0x888893, 4, [b'program']), (0xb281e6, 8, [b'header', b'gpu code' * 40])])
        got = ss.bnd2_resources(d, '/SHADERS.BNDL')
        self.assertEqual(got, [(0x888893, 4, 0, b'program'), (0xb281e6, 8, 0, b'header'), (0xb281e6, 8, 1, b'gpu code' * 40)])

    def test_stored_bundle_is_read_as_is(self):
        d = bundle([(1, 8, [b'', b'raw block'])], compressed=False)
        self.assertEqual(ss.bnd2_resources(d, '/A.BNDL'), [(1, 8, 1, b'raw block')])

    def test_unknown_version_is_named(self):
        with self.assertRaises(SystemExit) as e: ss.bnd2_resources(bundle([(1, 8, [b'x'])], version=9), '/B.BNDL')
        self.assertIn('bnd2 version 9', str(e.exception.code))

    def test_no_glslc_output_finds_nothing(self):
        self.assertEqual(ss.bnd2_shaders(bundle([(1, 8, [b'', b'no shader here'])]), '/C.BNDL'), [])

    def test_truncated_header_is_named(self):
        with self.assertRaisesRegex(SystemExit, 'truncated bnd2 header'):
            ss.bnd2_resources(b'bnd2', '/bad')

    def test_stored_range_cannot_be_silently_truncated(self):
        d = bundle([(1, 8, [b'raw block'])], compressed=False)
        with self.assertRaisesRegex(SystemExit, 'stored range is outside'):
            ss.bnd2_resources(d[:-1], '/bad')

    def test_offsets_cannot_point_into_the_table(self):
        d = bytearray(bundle([(1, 8, [b'x'])], compressed=False))
        struct.pack_into('<I', d, 0x18, 0x30)
        with self.assertRaisesRegex(SystemExit, 'stored range is outside'):
            ss.bnd2_resources(d, '/bad')

    def test_inflation_stops_at_declared_size(self):
        d = bytearray(bundle([(1, 8, [b'x' * 10000])]))
        struct.pack_into('<I', d, 0x30 + 0x10, 1)
        with self.assertRaisesRegex(SystemExit, 'decompressed size differs'):
            ss.bnd2_resources(d, '/bad')

    def test_incomplete_zlib_stream_is_rejected(self):
        d = bytearray(bundle([(1, 8, [b'x' * 100])]))
        n = struct.unpack_from('<I', d, 0x30 + 0x20)[0]
        struct.pack_into('<I', d, 0x30 + 0x20, n - 1)
        with self.assertRaisesRegex(SystemExit, 'incomplete zlib stream'):
            ss.bnd2_resources(d, '/bad')

    def test_unpacked_size_alignment_nibble_is_not_length(self):
        d = bytearray(bundle([(1, 8, [b'content'])]))
        struct.pack_into('<I', d, 0x30 + 0x10, 0x40000007)
        self.assertEqual(ss.bnd2_resources(d, '/ok'), [(1, 8, 0, b'content')])

    def test_uncompressed_size_must_match(self):
        d = bytearray(bundle([(1, 8, [b'x'])], compressed=False))
        struct.pack_into('<I', d, 0x30 + 0x10, 2)
        with self.assertRaisesRegex(SystemExit, 'stored size differs'):
            ss.bnd2_resources(d, '/bad')


if __name__ == '__main__':
    unittest.main()
