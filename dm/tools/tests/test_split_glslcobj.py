"""shader_split: Game Atelier's .glslcobj programs are read by their layout (glslcobj_shaders): the stage records follow the name at 8-byte alignment, each stage's code area follows
its record, and a record's control length may add padding after the control section's own length. Other files are left alone. No game files needed."""
import os
import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010029B0118E8000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def control(code_len, own, pad=0):
    """A control section of `own` bytes (+0x20 its length, +0x704 the code area's), then `pad` bytes the record counts but the section does not"""
    c = bytearray(own)
    c[:4] = ss.CTL_MAGIC
    struct.pack_into('<I', c, 0x20, own)
    struct.pack_into('<I', c, 0x704, code_len)
    return bytes(c) + b'\xee' * pad


def code(stage, n=0x100):
    return ss.GFX_MAGIC + bytes([stage]) + b'\0' * (n - 5)


def glslcobj(name, stages):
    """stages: [(stage byte, code area, control section's own length, padding)] → the file: u32 count, u32 name length (with its NUL), the u64 record offsets, the name,
    then per stage {u32 stage, u32 code length, u32 control length (with padding), u32 control offset} + code area + control section"""
    n, nm = len(stages), name.encode() + b'\0'
    o = (8 + 8 * n + len(nm) + 7) & ~7
    offs, body = [], b''
    for st, k, own, pad in stages:
        offs.append(o + len(body))
        ctl = control(len(k), own, pad)
        rec = struct.pack('<4I', st, len(k), len(ctl), o + len(body) + 0x10 + len(k))
        body += rec + k + ctl
    head = struct.pack('<2I', n, len(nm)) + struct.pack(f'<{n}Q', *offs) + nm
    return head + b'\0' * (o - len(head)) + body


class Glslcobj(unittest.TestCase):
    def setUp(self):
        self.real = ss.glslc_stage, ss.check
        ss.glslc_stage = lambda ctl, k: {1: 'ps', 0: 'vs'}[k[4]]   # the stage from the synthetic code area's fifth byte; the real one reads the SPH
        ss.check = lambda where, st, ctl, k: None

    def tearDown(self): ss.glslc_stage, ss.check = self.real

    def test_both_stages_are_read_and_paired(self):
        for name in 'PGRM_BLACKISH_LIGHT', 'PGRM_BLUR_MIX', 'PGRM_BLUR_VSM_HORIZONTAL':   # names ending at 0x2c, 0x26, 0x31: records at 0x30, 0x28, 0x38
            d = glslcobj(name, [(1, code(1, 0x200), 0x9f8, 0), (0, code(0, 0x200), 0xab8, 0)])
            got = ss.glslcobj_shaders(d, f'/glsl/compiled/{name}.glslcobj')
            assert got is not None, name
            self.assertEqual([st for _, st, _, _ in got], ['ps', 'vs'])
            self.assertEqual([(len(c), len(k)) for _, _, c, k in got], [(0x9f8, 0x200), (0xab8, 0x200)])
            self.assertEqual(len(ss.var_pairs(got)), 1)

    def test_padding_after_the_control_section_is_left_out(self):
        d = glslcobj('PGRM_OLD_TV', [(1, code(1, 0x2300), 0x43c0, 0x80), (0, code(0), 0x970, 0)])
        got = ss.glslcobj_shaders(d, '/glsl/compiled/PGRM_OLD_TV.glslcobj')
        assert got is not None
        self.assertEqual(len(got[0][2]), 0x43c0)
        self.assertNotIn(b'\xee', got[0][2])

    def test_other_files_are_not_glslcobj(self):
        good = glslcobj('PGRM_PLAIN', [(1, code(1), 0x880, 0), (0, code(0), 0x880, 0)])
        moved = bytearray(good); struct.pack_into('<Q', moved, 8, struct.unpack_from('<Q', good, 8)[0] + 8)   # the first record not right after the name
        no_nul = bytearray(good); no_nul[8 + 16 + len('PGRM_PLAIN')] = 0x41
        for d in (b'', bytes(0x100), b'SARC' + bytes(0x100), bytes(moved), bytes(no_nul), good[:0x60]):
            self.assertIsNone(ss.glslcobj_shaders(d, '/x'))

    def test_a_code_length_that_disagrees_stops(self):
        d = bytearray(glslcobj('PGRM_PLAIN', [(1, code(1), 0x880, 0), (0, code(0), 0x880, 0)]))
        c = struct.unpack_from('<I', d, struct.unpack_from('<Q', d, 8)[0] + 0xc)[0]   # the first stage's control offset
        struct.pack_into('<I', d, c + 0x704, 0x80)
        with self.assertRaises(SystemExit) as e: ss.glslcobj_shaders(bytes(d), '/x.glslcobj')
        self.assertIn('disagrees', str(e.exception.code))


if __name__ == '__main__':
    unittest.main()
