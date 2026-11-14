"""RE Engine PAKs (KPKA) and SDF shader definitions, without game data: Monster Hunter Rise's layouts, PRAGMATA's (block table, 0x110-byte SDF entries) and Resident Evil 4's (0x60-byte stage record headers)."""
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as ss


def nvn2(stage_byte, sph_type):
    """An NVN2 control section (magic, version 0xc9, code length at +0x44, stage byte at +0x56) and a 0x80-byte code area whose SPH word 0 names sph_type"""
    code = struct.pack('<I', sph_type << 10 | 1).ljust(0x80, b'\0')
    ctl = bytearray(0x60); ctl[:4] = ss.CTL_MAGIC; ctl[4:8] = b'\xc9\0\0\0'
    struct.pack_into('<I', ctl, 0x44, len(code)); ctl[0x56] = stage_byte
    return bytes(ctl), code


def sdf(layout, stages):
    """One SDF entry with stages {slot: (ctl, code)} in layout 0 (Monster Hunter Rise), 1 (PRAGMATA) or 2 (Resident Evil 4)"""
    base, size, lens, at = ss.SDF_LAYOUTS[layout]
    head = b'SDF\0' + struct.pack('<HH', 1, 1) + bytes(base - 8)
    body, offs, o = b'', [0] * 6, base + size
    for slot, (ctl, code) in stages.items():
        rec = bytes(lens) + struct.pack('<II', len(ctl), len(code)) + bytes(at - lens - 8) + ctl + code
        offs[slot] = o + len(body); body += rec
    entry = bytearray(size); struct.pack_into('<6Q', entry, 0x10, *offs)
    return head + bytes(entry) + body


class SdfTests(unittest.TestCase):
    def test_every_layout(self):
        vs, ps = nvn2(0, 1), nvn2(0, 5)
        for layout in range(len(ss.SDF_LAYOUTS)):
            with self.subTest(layout=layout):
                got = ss.sdf_shaders(sdf(layout, {0: vs, 4: ps}), 'x')
                self.assertEqual([(st, c, k) for _, st, c, k in got], [('vs', *vs), ('ps', *ps)])
                self.assertEqual(ss.sdf_layout(sdf(layout, {0: vs}), 1, 'x'), ss.SDF_LAYOUTS[layout])

    def test_mesh_shader_in_the_hull_slot(self):   # PRAGMATA keeps mesh shaders in the hull slot
        got = ss.sdf_shaders(sdf(1, {1: nvn2(7, 3), 4: nvn2(0, 5)}), 'x')
        self.assertEqual([st for _, st, _, _ in got], ['ms', 'ps'])

    def test_wrong_slot_is_rejected(self):
        with self.assertRaisesRegex(SystemExit, r'the stage \(ps\) does not match'):
            ss.sdf_shaders(sdf(1, {0: nvn2(0, 5)}), 'x')


class OcioTests(unittest.TestCase):
    def ocio(self, *stages, head=0x58):   # an OCIO file: a header, then stage records (a 0x58-byte header, or Resident Evil 4's 0x60, lengths at +8) among other data, as the colour pipelines hold them
        d = bytearray(b'OCIO' + bytes(0xc4) + b'names\0' + bytes(2))
        for ctl, code in stages:
            d += bytes(8) + struct.pack('<II', len(ctl), len(code)) + bytes(head - 0x10) + ctl + code + bytes(8)
        return bytes(d)

    def test_resident_evil_4_records_packed_to_one_byte(self):   # 3472b3eda9a8c309: 0x60-byte record headers, control sections at odd offsets
        ps, cs = nvn2(0, 5), nvn2(5, 0)
        d = self.ocio(ps, head=0x60) + bytes(3) + self.ocio(cs, head=0x60)[0xd0:]
        self.assertEqual([m.start() & 3 for m in re.finditer(re.escape(ss.CTL_MAGIC), d)][1], 3)
        got = ss.ocio_shaders(d, 'x')
        self.assertEqual([(st, c, k) for _, st, c, k in got], [('ps', *ps), ('cs', *cs)])

    def test_records_are_read(self):
        ps, cs = nvn2(0, 5), nvn2(5, 0)
        got = ss.ocio_shaders(self.ocio(ps, cs), 'x')
        self.assertEqual([(st, c, k) for _, st, c, k in got], [('ps', *ps), ('cs', *cs)])

    def test_records_packed_to_four_bytes(self):   # the larger OCIO files (8ae6c7ee83bc0563): a record whose control section starts 4 bytes off an 8-byte boundary
        ps, cs = nvn2(0, 5), nvn2(5, 0)
        d = self.ocio(ps) + bytes(4) + self.ocio(cs)[0xd0:]
        self.assertTrue([m.start() & 7 for m in re.finditer(re.escape(ss.CTL_MAGIC), d)][1])
        self.assertEqual([st for _, st, _, _ in ss.ocio_shaders(d, 'x')], ['ps', 'cs'])

    def test_a_section_whose_record_names_another_length_is_left_out(self):
        ctl, code = nvn2(0, 5)
        d = bytearray(self.ocio((ctl, code))); at = d.index(ss.CTL_MAGIC)
        struct.pack_into('<I', d, at - 0x4c, len(code) + 0x10)   # the record's code length disagrees with the file's bytes after it
        self.assertEqual(ss.ocio_shaders(bytes(d), 'x'), [])


class KpkaTests(unittest.TestCase):
    def test_block_table(self):
        zstd = ss.zstd_mod()
        bl = 0x100
        plain = b'SDF\0' + bytes(12)
        big = bytes(range(256)) + b'tail' * 16   # two blocks: one stored, one zstd frame of a padded block
        b0, b1 = big[:bl], zstd.compress(big[bl:].ljust(bl, b'\0'))
        n = 2; table_end = 16 + 0x30 * n
        blocks_at = table_end + 8; data_at = blocks_at + 16
        p_off = data_at; b0_off = p_off + len(zp := zstd.compress(plain)); b1_off = b0_off + len(b0)
        ents = struct.pack('<Qqqqqq', 0x1111, p_off, len(zp), len(plain), 2, 0) + struct.pack('<Qqqqqq', 0x2222, 0, len(b0) + len(b1), len(big), ss.KPKA_IN_BLOCKS | 0x400, 0)
        blocks = struct.pack('<II', bl, 2) + struct.pack('<QQ', b0_off | len(b0) << 42, b1_off | len(b1) << 42)
        pak = b'KPKA' + bytes([4, 2]) + struct.pack('<HII', ss.KPKA_BLOCKS, n, 0) + ents + blocks + zp + b0 + b1
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 're_chunk_000.pak'; path.write_bytes(pak)
            es = ss.kpka_entries(str(path)); bt = ss.kpka_blocks(str(path))
            self.assertEqual([e[0] for e in es], [0x1111, 0x2222])
            self.assertEqual(bt, (bl, [(b0_off, len(b0)), (b1_off, len(b1))]))
            self.assertEqual(ss.kpka_data(pak, es[0], blocks=bt), plain)
            self.assertEqual(ss.kpka_data(pak, es[1], blocks=bt), big)
            self.assertEqual(ss.kpka_data(pak, es[1], 4, bt), big[:4])

    def test_unknown_feature_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'x.pak'; path.write_bytes(b'KPKA' + bytes([4, 0]) + struct.pack('<HII', 0x8, 0, 0))
            with self.assertRaisesRegex(SystemExit, 'features 0x8'):
                ss.kpka_entries(str(path))


if __name__ == '__main__':
    unittest.main()
