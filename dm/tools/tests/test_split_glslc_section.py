"""A GLSLC output of one stage is read by its section header first (shader_split.glslc_shaders, glslc_section), and by the control section's own length
(+0x20) only when the header gives no usable section. SDK 21's GLSLC (Trails in the Sky 2nd Chapter 1.0.4, the compute shader at main+0x7774a8) keeps
its assembly listing after the control section's own length and puts the code area further on, where the section header says; the +0x20 rule read
listing text as a compute code area with no header, which passed the check under a key the game never uses. Synthetic outputs, no game files needed."""
import os
import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '01007BB017812000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had

GLSLC_MAGIC = bytes.fromhex('91688619')
SEC = 0x2d0                                  # the GPU code section's data, from the output's start


def output(own, code_off, ncode=0x200, header=True):
    """A one-stage compute output: the control section at SEC (its own length `own` at +0x20, threads at +0x748), assembly listing text up to
    code_off, then the code area (CS_MAGIC header, ncode bytes). header: write the section table (+0x4c count, the section header at +0x90)."""
    d = bytearray(SEC + code_off + ncode)
    d[:4] = GLSLC_MAGIC
    struct.pack_into('<I', d, 0x44, len(d))
    c = SEC
    d[c:c + 4] = ss.CTL_MAGIC
    struct.pack_into('<I', d, c + 0x20, own)
    struct.pack_into('<I', d, c + 0x704, ncode)
    struct.pack_into('<3I', d, c + 0x748, 512, 1, 1)
    listing = b'!!NVcp5.0\nOPTION NV_internal;\nMOV.F R0.x, R1.x;\n' * 64
    d[c + 0x878:c + code_off] = (listing * ((code_off - 0x878) // len(listing) + 1))[:code_off - 0x878]
    k = c + code_off
    d[k:k + 4] = ss.CS_MAGIC
    d[k + 0x100:k + ncode] = bytes(range(256)) * ((ncode - 0x100) // 256)
    if header:
        struct.pack_into('<I', d, 0x4c, 1)
        struct.pack_into('<3I', d, 0x90, code_off + ncode, SEC, 0)                 # {size, data offset, type 0 = GPU code}
        struct.pack_into('<4I', d, 0x90 + 0x30, 0, code_off, ncode, code_off)      # control offset, code offset, code length, control length
    return bytes(d)


class GlslcSectionTest(unittest.TestCase):
    def test_code_area_past_the_control_length(self):
        own, code_off = 0x1000, 0x1180            # the code area 0x180 bytes past the control section's own length, as in the game
        d = output(own, code_off)
        (key, st, ctl, code), = ss.glslc_shaders(d, 'm', need=False)
        self.assertEqual((key, st), ('m#GLSLC@0x0', 'cs'))
        self.assertEqual(code, d[SEC + code_off:])
        self.assertEqual(code[:4], ss.CS_MAGIC)
        self.assertEqual(ss.skey(ctl, code), ss.skey(d[SEC:SEC + 0x880], d[SEC + code_off:]))   # the key takes the control section's first 0x880 bytes
        # what the +0x20 rule alone read: listing text taken as a compute code area with no header, which the check lets through under another key
        early = d[SEC + own:SEC + own + 0x200]
        self.assertIsNone(ss.check('m', ss.glslc_stage(d[SEC:SEC + own], early), d[SEC:SEC + own], early))
        self.assertNotEqual(ss.skey(ctl, code), ss.skey(d[SEC:SEC + own], early))

    def test_rule_without_a_section_header(self):
        own = 0x880                               # the code area right after the control section, and no section table
        d = output(own, own, header=False)
        (key, st, ctl, code), = ss.glslc_shaders(d, 'm', need=False)
        self.assertEqual(st, 'cs')
        self.assertEqual(code, d[SEC + own:])
        self.assertEqual(ctl, d[SEC:SEC + own])

    def test_header_and_rule_agree(self):
        own = 0x880                               # where both readings find the same code area, the key is the same either way
        a, = ss.glslc_shaders(output(own, own), 'm', need=False)
        b, = ss.glslc_shaders(output(own, own, header=False), 'm', need=False)
        self.assertEqual(ss.skey(a[2], a[3]), ss.skey(b[2], b[3]))


if __name__ == '__main__':
    unittest.main()
