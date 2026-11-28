"""SHN! resource blocks and embedded SHDROUNC programs, without game data."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

import lz4.block

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


def shader(stage, tag=0):
    ctl, code = bytearray(0xe0), bytearray(0x100)
    ctl[:8] = split.CTL_MAGIC + b'\xc9\0\0\0'
    struct.pack_into('<I', ctl, 0x30, len(ctl))
    struct.pack_into('<I', ctl, 0x44, len(code))
    if stage == 'cs': ctl[0x56] = 5
    else: struct.pack_into('<I', code, 0, (1 if stage == 'vs' else 5) << 10 | 1)
    code[-1] = tag
    return bytes(ctl), bytes(code)


def metadata(*programs):
    """Programs are lists of variants, each {stage: (control, code)}. Two programs model embedded scene shaders; pointers are absolute to the metadata, with code in another block."""
    meta, data = bytearray(b'SCENOUNC' + bytes(24)), bytearray(bytes(0x20))
    for variants in programs:
        h = len(meta); meta += b'SHDROUNC' + bytes(0x58)
        table = len(meta); meta += bytes(len(variants) * 9 * 0x50)
        struct.pack_into('<I', meta, h + 0xc, len(variants))
        struct.pack_into('<Q', meta, h + 0x30, table)
        for v, stages in enumerate(variants):
            for st, (ctl, code) in stages.items():
                r = table + (v * 9 + {'vs': 0, 'ps': 1, 'cs': 2}[st]) * 0x50
                struct.pack_into('<QQ', meta, r, len(data), len(meta))
                struct.pack_into('<II', meta, r + 0x40, len(code), len(ctl))
                meta += ctl
                data += code + bytes(0x30)   # code areas need not be adjacent or in control-section order
    return bytes(meta), bytes(data)


def resource(meta, data, compressed=True):
    out = bytearray(b'SHN!' + bytes(0x40))
    for at, raw in ((4, meta), (0x18, b'relocation table'), (0x24, data)):
        packed = lz4.block.compress(raw, store_size=False) if compressed else raw
        # Incompressible blocks are stored raw, distinguished by equal lengths.
        if len(packed) >= len(raw): packed = raw
        struct.pack_into('<III', out, at, len(out), len(packed), len(raw))
        out += packed
    return bytes(out)


class ShnTests(unittest.TestCase):
    def test_embedded_programs_variants_and_code_offsets(self):
        vs, ps, cs = shader('vs'), shader('ps'), shader('cs')
        meta, code = metadata([{'vs': vs, 'ps': ps}, {'vs': vs, 'ps': shader('ps', 1)}], [{'cs': cs}])
        for compressed in (False, True):
            with self.subTest(compressed=compressed):
                found = []
                split.scan_blob(resource(meta, code, compressed), '/scene', found)
                self.assertEqual([(st, c, k) for _, st, c, k in found], [('vs', *vs), ('ps', *ps), ('vs', *vs), ('ps', *shader('ps', 1)), ('cs', *cs)])
                self.assertEqual(split.var_pairs(found), {(split.skey(*vs), split.skey(*ps)), (split.skey(*vs), split.skey(*shader('ps', 1)))})

    def test_loose_file_is_dispatched_by_magic(self):
        vs, ps = shader('vs'), shader('ps')
        meta, code = metadata([{'vs': vs, 'ps': ps}])
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / 'any-extension.bin'; p.write_bytes(resource(meta, code))
            _, keys, pairs, extra = split.scan_file_in((str(p), p.name))
        self.assertEqual(set(keys), {split.skey(*vs), split.skey(*ps)})
        self.assertEqual(pairs, {(split.skey(*vs), split.skey(*ps))})
        self.assertEqual(extra, {})

    def test_texture_payload_is_not_decoded(self):
        d = bytearray(resource(b'TEX OUNC' + bytes(64), b'pixels'))
        struct.pack_into('<III', d, 0x24, len(d) + 100, 10, 100)
        self.assertEqual(split.shn_shaders(bytes(d), '/texture'), [])

    def test_bad_block_bounds_and_lz4_are_named(self):
        meta, code = metadata([{'vs': shader('vs')}])
        for at, values, reason in ((4, (0, 10, 100), 'overlaps the header'), (4, (0x44, 0xffffffff, 100), 'outside'),
                                   (4, (0x44, 1, 0xffffffff), 'impossible LZ4 length'), (4, (0x44, 1, 100), 'invalid LZ4 data'),
                                   (0x24, (0x44, 0xffffffff, 100), 'outside')):
            with self.subTest(values=values):
                d = bytearray(resource(meta, code)); struct.pack_into('<III', d, at, *values)
                with self.assertRaisesRegex(SystemExit, reason): split.shn_shaders(bytes(d), '/bad')

    def test_truncated_header(self):
        with self.assertRaisesRegex(SystemExit, 'SHN! header.*outside'):
            split.scan_blob(b'SHN!' + bytes(8), '/cut', [])

    def test_stage_table_and_section_ranges_are_checked(self):
        meta, code = metadata([{'vs': shader('vs')}])
        table = struct.unpack_from('<Q', meta, 0x20 + 0x30)[0]
        for at, fmt, value in ((0x20 + 0xc, '<I', 0xffffffff), (0x20 + 0x30, '<Q', len(meta)),
                               (table, '<Q', len(code)), (table + 8, '<Q', len(meta))):
            with self.subTest(at=at):
                bad = bytearray(meta); struct.pack_into(fmt, bad, at, value)
                with self.assertRaisesRegex(SystemExit, 'outside'):
                    split.shn_shaders(resource(bytes(bad), code), '/bad')

    def test_control_and_code_lengths_must_agree(self):
        ctl, code = shader('vs'); ctl = bytearray(ctl)
        struct.pack_into('<I', ctl, 0x44, len(code) - 16)
        meta, data = metadata([{'vs': (bytes(ctl), code)}])
        with self.assertRaisesRegex(SystemExit, 'code area length'):
            split.shn_shaders(resource(meta, data), '/bad')


if __name__ == '__main__':
    unittest.main()
