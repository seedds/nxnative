"""Firaxis's CIVBSF shader file (Civilization VII), without game data."""
from pathlib import Path
import contextlib
import io
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


def shader(stage, own=0x850):
    ctl, code = bytearray(own), bytearray(0x100)
    ctl[:4] = split.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    if stage == 'cs':
        code[:4] = split.CS_MAGIC
        struct.pack_into('<3I', ctl, 0x748, 8, 1, 1)
    else:
        code[:4] = split.GFX_MAGIC
        struct.pack_into('<I', code, 0x30, (split.STAGES.index(stage) + 1) << 10 | 1)
        if stage == 'ps': struct.pack_into('<I', code, 0x48, 1)
    return bytes(ctl), bytes(code)


def record(stage, name, trailing=0):
    """A name, then the record: 00 ff 00 ff, u32 code length, u32 stored control length, u32 flags, the code area, the control section (and bytes past its own length)"""
    ctl, code = shader(stage)
    return name + b'\0' + bytes.fromhex('00ff00ff') + struct.pack('<III', len(code), len(ctl) + trailing, 0) + code + ctl + b'\xee' * trailing


def civbsf(records, count=None):
    head = bytearray(0x90)
    head[:8] = split.CIVBSF
    struct.pack_into('<I', head, 8, 0x1218)
    struct.pack_into('<Q', head, 0x18, 7)
    struct.pack_into('<I', head, 0x68, len(records) if count is None else count)
    d = head + b'entry table and names' + b''.join(records)
    struct.pack_into('<I', d, 0xc, len(d))
    return d


class CivbsfTests(unittest.TestCase):
    def test_records_of_every_stage_unaligned(self):
        d = civbsf([record('vs', b'VS_ScreenTri'), record('ps', b'PS_Copy'), record('cs', b'CS_Blur')])
        found = split.civbsf_shaders(d, '/ShaderAutoGen.bs')
        self.assertEqual([(st, ctl, code) for _, st, ctl, code in found], [('vs', *shader('vs')), ('ps', *shader('ps')), ('cs', *shader('cs'))])
        self.assertTrue(all('#CIVBSF@' in where for where, *_ in found))
        self.assertTrue(all(not split.check(w, s, c, k) for w, s, c, k in found))

    def test_stored_control_longer_than_its_own_length(self):
        d = civbsf([record('vs', b'VS_A', trailing=0x80), record('ps', b'PS_B')])
        found = split.civbsf_shaders(d, '/s.bs')
        self.assertEqual([(st, ctl, code) for _, st, ctl, code in found], [('vs', *shader('vs')), ('ps', *shader('ps'))])

    def test_programs_are_single_stage(self):
        found = split.civbsf_shaders(civbsf([record('vs', b'VS_A'), record('ps', b'PS_B')]), '/s.bs')
        pairs, cover = split.separable_pairs(split.uniq_keys(found))
        self.assertEqual(cover, [split.skey(*shader('ps'))])
        self.assertEqual(pairs, {(split.skey(*shader('vs')), cover[0])})

    def test_file_scanner_reads_it_by_its_magic(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'ShaderAutoGen_Linux_NVN_FinalRelease.bs'
            path.write_bytes(civbsf([record('vs', b'VS_A'), record('cs', b'CS_B')]))
            _, keys, _, extra = split.scan_file((str(path), path.name))
            self.assertEqual(extra, {})
            self.assertEqual(sorted(v[1] for v in keys.values()), ['cs', 'vs'])

    def test_file_length_count_and_record_lengths_are_checked(self):
        good = [record('vs', b'VS_A'), record('ps', b'PS_B')]
        cut = civbsf(good)[:-1]
        bad_count = civbsf(good, count=3)
        bad_code_len = bytearray(civbsf(good)); o = bad_code_len.index(bytes.fromhex('00ff00ff')); struct.pack_into('<I', bad_code_len, o + 4, 0x80)
        bad_ctl_len = bytearray(civbsf(good)); struct.pack_into('<I', bad_ctl_len, o + 8, 0x800)
        for name, d in [('cut', cut), ('count', bad_count), ('code length', bad_code_len), ('control length', bad_ctl_len)]:
            with self.subTest(name), self.assertRaises(SystemExit): split.civbsf_shaders(bytes(d), '/bad.bs')

    def test_unreadable_file_is_left_out(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'bad.bs'; path.write_bytes(civbsf([record('vs', b'VS_A')], count=2))
            with contextlib.redirect_stdout(io.StringIO()): result = split.scan_file((os.fspath(path), path.name))
            self.assertEqual(result[1], {})
            self.assertEqual(result[3][0], 'unreadable')


if __name__ == '__main__': unittest.main()
