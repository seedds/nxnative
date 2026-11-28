"""shader_split: bahs_shaders finds an agl BAHS file's program entries (code offset, 0, control length, control distance) by scanning its table area, and a control section's own
words can read as one. An NVN2 section's +0x38..+0x47 (instruction length, c1 start and end, code length, here 0x100, 0, 0x100, 0x100) is such an entry: its control distance lands on
the next program's control section, its offset on that program's code, and the shader it made paired one program's control with whatever code lay there. Ocarina of Time's
agl_common.sharcb@0x357f0 gave a compute control and a vertex shader's code, whose SPH the translator read as an instruction ("Ampere instruction 481 at 0x0"); six such entries in
its agl and terrain files, none ever loaded. The terrain file's lowest one also kept bahs_variations from finding the binary array, so tera_common gave no variation pairs. No game files."""
import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz   # shader_split with a profile
ss = fz.ss

REGION, CODE = 0x400, 0x100   # the code region's start; each program's code area length


def nvn2_ctl(stage):
    """An NVN2 control section (0xe0 bytes): magic, version 0xc9, +0x38 instruction length, +0x40 / +0x44 c1 start and end (= the code area's length), +0x56 the stage byte"""
    c = bytearray(0xe0); c[:4] = ss.CTL_MAGIC; c[4:8] = b'\xc9\0\0\0'
    struct.pack_into('<4I', c, 0x38, CODE, 0, CODE, CODE); c[0x56] = stage
    return c


def bahs():
    """Two vertex programs: entries at 0x100 and 0x238, each with its control 0x68 after it (0x168, 0x2a0), code at the region's +0 and +0x100. A's control words at
    0x1a0 read (0x100, 0, 0x100, 0x100): an entry whose control is B's (0x1a0 + 0x100) and whose code is B's"""
    d = bytearray(REGION + 2 * CODE)
    struct.pack_into('<II', d, 0x18, 2 * CODE, REGION)   # code region length, its start
    for entry, off in ((0x100, 0), (0x238, CODE)):
        struct.pack_into('<IIIi', d, entry, off, 0, 0xe0, 0x68)
        d[entry + 0x68:entry + 0x68 + 0xe0] = nvn2_ctl(0)
        code = bytearray(CODE); struct.pack_into('<I', code, 0, 0x000c0481 + off)   # an SPH, vertex (word 0: type 1, stage 1 in bits 10..13), told apart by its offset
        d[REGION + off:REGION + off + CODE] = code
    assert struct.unpack_from('<IIIi', d, 0x1a0) == (CODE, 0, CODE, CODE) and 0x1a0 + CODE == 0x2a0
    return bytes(d)


def bahs_with_payload_entry():
    """Two binary records and descriptor-shaped payload outside their entry slots."""
    d = bytearray(0x700)
    struct.pack_into('<II', d, 0x18, 0x200, 0x500)
    struct.pack_into('<II', d, 0x40, 0x408, 2)
    for rec, off in ((0x48, 0), (0x248, 0x100)):
        struct.pack_into('<II', d, rec, 0x200, 0)
        ent, ctl = rec + 0x10, rec + 0x78
        struct.pack_into('<IIIi', d, ent, off, 0, 0xe0, ctl - ent)
        d[ctl:ctl + 0xe0] = nvn2_ctl(0)
        struct.pack_into('<I', d, 0x500 + off, 0x000c0481)
    struct.pack_into('<IIIi', d, 0x1c0, 0x100, 0, 0xe0, 0x100)
    return d


class ControlWordsAreNoEntry(unittest.TestCase):
    def test_descriptor_shaped_payload_outside_the_binary_records(self):
        got = ss.bahs_shaders(bahs_with_payload_entry(), 'payload.sharcb')
        self.assertEqual([w for w, *_ in got], ['payload.sharcb#BAHS@0x58', 'payload.sharcb#BAHS@0x258'])

    def test_unrecognized_binary_array_keeps_the_scan_candidates(self):
        # Filtering is safe only after the entire size-delimited array validates.
        for field, value in ((0x40, 0), (0x40, 0xffff), (0x44, 0), (0x48, 0x10), (0x248, 0x1fc)):
            with self.subTest(field=hex(field), value=value):
                d = bahs_with_payload_entry()
                struct.pack_into('<I', d, field, value)
                got = ss.bahs_shaders(d, 'unknown.sharcb')
                self.assertEqual([w for w, *_ in got], ['unknown.sharcb#BAHS@0x58', 'unknown.sharcb#BAHS@0x1c0', 'unknown.sharcb#BAHS@0x258'])

    def test_empty_binary_array_keeps_the_only_scan_candidate(self):
        d = bahs_with_payload_entry()
        struct.pack_into('<II', d, 0x40, 8, 0)
        d[0x1c0:0x1d0] = bytes(16)
        d[0x258:0x268] = bytes(16)
        got = ss.bahs_shaders(d, 'empty.sharcb')
        self.assertEqual([w for w, *_ in got], ['empty.sharcb#BAHS@0x58'])

    def test_binary_array_overlapping_code_keeps_the_scan_candidates(self):
        d = bahs_with_payload_entry()
        struct.pack_into('<I', d, 0x40, 0x4c8)   # array ends at 0x508, code starts at 0x500
        struct.pack_into('<I', d, 0x248, 0x2c0)  # the final binary record ends with the array
        got = ss.bahs_shaders(d, 'overlap.sharcb')
        self.assertEqual([w for w, *_ in got], ['overlap.sharcb#BAHS@0x58', 'overlap.sharcb#BAHS@0x1c0', 'overlap.sharcb#BAHS@0x258'])

    def test_only_the_two_programs(self):
        got = ss.bahs_shaders(bahs(), 'f.sharcb')
        self.assertEqual([w for w, *_ in got], ['f.sharcb#BAHS@0x100', 'f.sharcb#BAHS@0x238'])
        self.assertEqual([st for _, st, _, _ in got], ['vs', 'vs'])

    def test_the_false_entry_is_a_candidate_without_the_rule(self):   # the scan's own checks take it: the rule is what leaves it out
        d = bahs()
        off, z, csz, rel = struct.unpack_from('<IIIi', d, 0x1a0)
        c = 0x1a0 + rel
        self.assertTrue(ss.nvn2_ctl(d[c:c + csz]) and not z and off % 0x100 == 0 and ss.check('f', 'vs', d[c:c + csz], d[REGION + off:REGION + off + CODE]) is None)


class FallbackArchive(unittest.TestCase):
    def test_reflection_archive_has_no_nvn_programs(self):
        reflection = b'common\0aPosition\0aTexCoord\0'
        fallback = struct.pack('<4s4IH', b'BAHS', 9, 0x16 + len(reflection), 5, 0, 0x1200) + reflection
        self.assertEqual(ss.bahs_shaders(fallback, 'common.sharcfb'), [])
        with self.assertRaisesRegex(SystemExit, 'BAHS code region out of bounds'):
            ss.bahs_shaders(fallback[:-1], 'common.sharcfb')

    def test_nvn_control_section_keeps_the_binary_reader(self):
        binary = bytearray(bahs())
        struct.pack_into('<4sII', binary, 0, b'BAHS', 9, len(binary))
        self.assertEqual(len(ss.bahs_shaders(bytes(binary), 'common.sharcfb')), 2)


if __name__ == '__main__':
    unittest.main()
