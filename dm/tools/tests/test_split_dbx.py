"""shader_split: DrinkBox's SHADERS container (Guacamelee! STCE's and Guacamelee! 2's shaders.dat.nx) is read block by block, each code area taken from
before the control section that describes it; a control section whose code area is not there is named in a red line, not read. No game files needed."""
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010015100B514000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def shader(stage):
    ctl, code = bytearray(0x838), bytearray(0x100)   # STCE's own control length
    ctl[:4] = ss.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl))      # the control's own length
    struct.pack_into('<I', ctl, 0x704, len(code))    # the length of the code before it
    code[:4] = ss.GFX_MAGIC
    struct.pack_into('<I', code, 0x30, (ss.STAGES.index(stage) + 1) << 10 | 1)
    if stage == 'ps': struct.pack_into('<I', code, 0x48, 1)
    return bytes(ctl), bytes(code)


def container(*stages):
    d = bytearray(ss.DBX + struct.pack('<IHH', 1, 0, len(stages)))
    for i, s in enumerate(stages):
        d += f'{s}_{i}.{"vsh" if s == "vs" else "psh"}\0main\0'.encode()
    d += b'\0' * 0x40   # uniform metadata: never read
    for s in stages:
        ctl, code = shader(s)
        d += code + ctl + b'\0' * 0x18   # one record between the control and the next block
    return bytes(d)


class DbxTests(unittest.TestCase):
    def test_blocks_are_read_in_order_with_their_stages(self):
        found = ss.dbx_shaders(container('vs', 'ps', 'gs'), '/shaders.dat.nx')
        self.assertEqual([st for _, st, _, _ in found], ['vs', 'ps', 'gs'])
        self.assertEqual([(ctl, code) for _, _, ctl, code in found], [shader('vs'), shader('ps'), shader('gs')])
        self.assertTrue(all(not ss.check(w, st, ctl, code) for w, st, ctl, code in found))
        self.assertEqual(len(ss.uniq_keys(found)), 3)

    def test_the_file_scanner_dispatches_on_the_magic(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'shaders.dat.nx'
            path.write_bytes(container('vs', 'ps'))
            _, keys, _, extra = ss.scan_file((str(path), path.name))
            self.assertEqual(extra, {})
            self.assertEqual(sorted(k for k, v in keys.items() if v[1] == 'ps'), [ss.skey(*shader('ps'))])

    def test_a_control_section_without_its_code_before_it_stops_the_reader(self):
        for offset, value in [(0x704, 0x20000), (0x704, 0x20)]:
            d = bytearray(container('vs')); at = d.find(ss.CTL_MAGIC)
            struct.pack_into('<I', d, at + offset, value)
            with self.subTest(offset=offset), self.assertRaises(SystemExit):
                ss.dbx_shaders(bytes(d), '/bad')

    def test_a_stray_control_magic_with_no_code_header_before_it_stops_the_reader(self):
        with self.assertRaisesRegex(SystemExit, 'no code header'):
            ss.dbx_shaders(container('vs') + bytes(0x100) + ss.CTL_MAGIC + bytes(0x800), '/bad')


if __name__ == '__main__':
    unittest.main()
