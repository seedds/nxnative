"""Button icons in a PlayStation or Xbox style (dm/tools/make_ext_font.py --style, dm/tools/button_icons.py), on synthetic files, no game files: the three styles of the
extension font, Yaz0 (a round trip, a splice that keeps the groups around the change), BC4 and the block-linear sheet, and a whole run of `make` on a made-up BFFNT
(BNTX sheet, CMAP table, CWDH) in a Yaz0 SARC: the profile's glyphs are redrawn, the others and the rest of the archive stay as they were, the font keeps its size, a
wider glyph widens its CWDH entry, and `survey` finds the font."""
import random
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import button_icons as bi

LETTERS = Path('/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf')
CW = CH = 50; COLS = 5; W, H = 256, 256   # cells on a 51-pixel pitch, as Breath of the Wild's External_00.bffnt


def bffnt(cells):
    """A BFFNT with one BC4 sheet (a BNTX, block height 16, stored upside down) holding a filled square in each cell of cells ({code point: glyph index}), a table CMAP and
    a CWDH of (1, 30, 32) per glyph."""
    n = max(cells.values()) + 1
    px = bytearray(W * H)
    for g in range(n):
        r, c = divmod(g, COLS); x0, y0 = c * (CW + 1), r * (CH + 1)
        for y in range(y0 + 10, y0 + 40):
            for x in range(x0 + 2, x0 + 32): px[(H - 1 - y) * W + x] = 255   # stored upside down
    head = 0x14; tglp = head + 0x20; sheet_off = 0x1000; bntx_pix = 0x1000
    f = type('F', (), dict(w=W, h=H, bh=16))
    addr = bi.block_map(f); data = bytearray(max(addr) + 8)   # a 256-row sheet in 16-GOB blocks is padded to 512 rows
    for by in range(H // 4):
        for bx in range(W // 4):
            o = addr[by * (W // 4) + bx]; data[o:o + 8] = bi.bc4_encode([px[(by * 4 + i // 4) * W + bx * 4 + i % 4] for i in range(16)])
    bntx = bytearray(bntx_pix) + data
    bntx[0:4] = b'BNTX'; struct.pack_into('<Q', bntx, 0x28, 0x198); struct.pack_into('<Q', bntx, 0x198, 0x1E8)
    bntx[0x1E8:0x1EC] = b'BRTI'; struct.pack_into('<I', bntx, 0x1E8 + 0x34, 4); struct.pack_into('<Q', bntx, 0x1E8 + 0x70, 0x488); struct.pack_into('<Q', bntx, 0x488, bntx_pix)
    lo, hi = min(cells), max(cells); table = [0xFFFF] * (hi - lo + 1)
    for cp, g in cells.items(): table[cp - lo] = g
    cmap = b'CMAP' + struct.pack('<IIIHHI', 24 + 2 * len(table), lo, hi, 1, 0, 0) + struct.pack(f'<{len(table)}H', *table)
    cwdh = b'CWDH' + struct.pack('<IHHI', 16 + 3 * n, 0, n - 1, 0) + bytes([1, 30, 32]) * n
    tglp_sec = b'TGLP' + struct.pack('<IBBBBIHHHHHHI', sheet_off + len(bntx) - tglp, CW, CH, 1, 48, len(bntx), 36, 12, COLS, H // (CH + 1), W, H, sheet_off)
    out = bytearray(b'FFNT\xff\xfe' + struct.pack('<H', head) + bytes(head - 8))
    out += b'FINF' + struct.pack('<I', 0x20) + bytes(0x18)
    out += tglp_sec; out += bytes(sheet_off - len(out)); out += bntx; out += cwdh; out += cmap
    return bytes(out)


def sarc(files):
    """A little-endian SARC of files ({name: bytes}), members on 0x100 boundaries."""
    names = sorted(files); pool = b''; offs = []
    for n in names: offs.append(len(pool) // 4); pool += n.encode() + b'\0'; pool += bytes(-len(pool) % 4)
    nodes = b''; body = b''; starts = []
    for n in names: body += bytes(-len(body) % 0x100); starts.append(len(body)); body += files[n]
    for k, n in enumerate(names): nodes += struct.pack('<IIII', k, 0x01000000 | offs[k], starts[k], starts[k] + len(files[n]))
    sfat = b'SFAT' + struct.pack('<HHI', 12, len(names), 0x65) + nodes; sfnt = b'SFNT' + struct.pack('<HH', 8, 0) + pool
    data = 0x14 + len(sfat) + len(sfnt); data += -data % 0x100
    head = b'SARC' + struct.pack('<HHIII', 0x14, 0xFEFF, data + len(body), data, 0x100)
    return head + sfat + sfnt + bytes(data - 0x14 - len(sfat) - len(sfnt)) + body


def yaz0(data):
    return b'Yaz0' + struct.pack('>III', len(data), 0x80, 0) + bi.yaz0_encode(data, 0, len(data), whole=False)


@unittest.skipUnless(LETTERS.is_file(), 'needs macOS\'s Arial Rounded Bold')
class Styles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from fontTools.ttLib import TTFont
        cls.tmp = tempfile.TemporaryDirectory(); cls.fonts = {}
        for style in ('switch', 'playstation', 'xbox'):
            out = Path(cls.tmp.name) / (f'nintendo_ext_{style}.ttf' if style != 'switch' else 'nintendo_ext.ttf')
            subprocess.run([sys.executable, TOOLS / 'make_ext_font.py', out, '--style', style], check=True, capture_output=True)
            cls.fonts[style] = TTFont(str(out))
    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def test_same_code_points_and_names(self):
        cmaps = {s: f.getBestCmap() for s, f in self.fonts.items()}
        self.assertEqual(cmaps['switch'], cmaps['playstation']); self.assertEqual(cmaps['switch'], cmaps['xbox'])
        for f in self.fonts.values(): self.assertTrue({'dpad_updown', 'dpad_leftright', 'btnA', 'btnZR', 'btnPlus'} <= set(f.getGlyphOrder()))

    def test_styles_draw_their_buttons(self):
        def coords(style, name): g = self.fonts[style]['glyf'][name]; return g.getCoordinates(self.fonts[style]['glyf'])[0]
        for name in ('btnA', 'btnL', 'btnZR', 'btnPlus', 'btnMinus'):
            self.assertNotEqual(list(coords('switch', name)), list(coords('playstation', name)), name)
            if name != 'btnPlus': self.assertNotEqual(list(coords('playstation', name)), list(coords('xbox', name)), name)   # Options and Menu are both three bars
        for name in ('dpad', 'dpad_up', 'stickL', 'arrow_right', 'btnSL'):   # buttons every pad shares keep the Switch drawing
            self.assertEqual(list(coords('switch', name)), list(coords('playstation', name)), name)

    def test_xbox_letters_follow_the_positions(self):   # Switch A (right) is Xbox B: the same glyph as the Switch style's B
        def coords(style, name): return list(self.fonts[style]['glyf'][name].getCoordinates(self.fonts[style]['glyf'])[0])
        for switch, xbox in (('btnA', 'btnB'), ('btnB', 'btnA'), ('btnX', 'btnY'), ('btnY', 'btnX')): self.assertEqual(coords('xbox', switch), coords('switch', xbox))

    def test_unknown_style(self):
        r = subprocess.run([sys.executable, TOOLS / 'make_ext_font.py', Path(self.tmp.name) / 'x.ttf', '--style', 'sega'], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0); self.assertIn('--style is one of', r.stderr)

    def test_make_redraws_the_profile_glyphs(self):
        cells = {0xE040: 0, 0xE041: 1, 0xE044: 2, 0xE050: 3}
        font = bffnt(cells); other = bytes(random.Random(1).randrange(256) for _ in range(9000)) * 3
        arc = sarc({'Other.bin': other, 'External_00.bffnt': font}); comp = yaz0(arc)
        rom = Path(self.tmp.name) / 'rom'; (rom / 'Font').mkdir(parents=True, exist_ok=True)
        for region in ('US', 'JP'): (rom / f'Font/Font_{region}.sbfarc').write_bytes(comp)
        bi.PROFILES['0100000000TEST00'] = dict(name='test', archives='Font/Font_*.sbfarc', member='External_00.bffnt', flip=True, glyphs={0xE040: 'btnA', 0xE041: 'btnB', 0xE044: 'btnL'})
        try:
            out = Path(self.tmp.name) / 'icons'
            bi.make(rom, out, '0100000000test00', self.tmp.name, ['playstation'])
        finally: del bi.PROFILES['0100000000TEST00']
        for region in ('US', 'JP'):
            new = bi.yaz0_ops((out / f'playstation/Font/Font_{region}.sbfarc').read_bytes())[0]
            self.assertEqual(len(new), len(arc))
            lo, hi = bi.sarc_member(new, 'External_00.bffnt'); olo, ohi = bi.sarc_member(arc, 'External_00.bffnt')
            self.assertEqual((lo, hi), (olo, ohi)); self.assertEqual(new[:lo], arc[:lo]); self.assertEqual(new[hi:], arc[hi:])
            f, g = bi.Font(arc[lo:hi]), bi.Font(new[lo:hi]); a, b = bi.Sheet(f, True), bi.Sheet(g, True)
            def cell(s, i): x, y = f.cell(i); return bytes(s.px[(y + j) * W + x + k] for j in range(CH) for k in range(CW))
            for i in (0, 1, 2): self.assertNotEqual(cell(a, i), cell(b, i), f'glyph {i} redrawn')
            self.assertEqual(cell(a, 3), cell(b, 3), 'a glyph outside the profile is left as it was')
            self.assertEqual(g.data[g.widths(0):g.widths(0) + 3], bytes([1, 30, 32]), 'a round glyph keeps its widths')
            left, gw, adv = g.data[g.widths(2):g.widths(2) + 3]
            self.assertEqual(left, 1); self.assertGreater(gw, 30); self.assertEqual(adv - gw, 2, 'the wider L1 plate widens its glyph and advance alike')

    def test_survey_finds_the_icon_font(self):
        rom = Path(self.tmp.name) / 'survey_rom'; (rom / 'Font').mkdir(parents=True, exist_ok=True)
        (rom / 'Font/Font_US.sbfarc').write_bytes(yaz0(sarc({'External_00.bffnt': bffnt({0xE040: 0, 0xE041: 1})})))
        out = Path(self.tmp.name) / 'survey'; bi.survey(rom, out)
        tsv = next(out.glob('*.tsv')).read_text()
        self.assertIn('U+E040\t0\t0\t0', tsv); self.assertIn('U+E041\t1\t51\t0', tsv)
        self.assertEqual(len(list(out.glob('*.png'))), 2)


class Codecs(unittest.TestCase):
    def test_yaz0_round_trip(self):
        rng = random.Random(7)
        for data in (b'', b'a', bytes(rng.randrange(4) for _ in range(5000)), b'xyz' * 3000, bytes(rng.randrange(256) for _ in range(3000))):
            self.assertEqual(bi.yaz0_ops(yaz0(data))[0], data)

    def test_yaz0_splice_keeps_the_rest(self):
        rng = random.Random(3); data = bytearray(rng.randrange(8) for _ in range(40000)); src = yaz0(bytes(data))
        old, groups = bi.yaz0_ops(src); new = bytearray(old)
        for i in range(20000, 20300): new[i] = rng.randrange(256)
        out = bi.yaz0_splice(src, groups, bytes(new), 20000, 20300)
        self.assertEqual(bi.yaz0_ops(out)[0], bytes(new))
        self.assertEqual(out[:groups[0][0] + 100], src[:groups[0][0] + 100], 'the groups before the change are copied')
        self.assertEqual(out[-200:], src[-200:], 'the groups 4 KiB past the change are copied')
        tail = bi.yaz0_splice(src, groups, old[:-5] + b'ZZZZZ', len(old) - 5, len(old))   # a change in the last group
        self.assertEqual(bi.yaz0_ops(tail)[0], old[:-5] + b'ZZZZZ')

    def test_bc4(self):
        for block in ([0] * 16, [255] * 16, [i * 17 for i in range(16)], [0, 255] * 8):
            got = bi.bc4_decode(bi.bc4_encode(block))
            self.assertLessEqual(max(abs(a - b) for a, b in zip(got, block)), 19, block)

    def test_block_map_is_a_permutation(self):
        f = type('F', (), dict(w=256, h=1024, bh=16)); m = bi.block_map(f)
        self.assertEqual(sorted(m), list(range(0, 256 * 1024 // 2, 8)))


if __name__ == '__main__':
    unittest.main()
