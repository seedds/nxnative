"""dm/tools/make_shared_fonts.py: each type's cut holds its characters only, carries no hinting (the font engine's interpreter memory grows with it) and no layout tables but kerning,
and takes a name of its own; the file names are the ones the host reads (hle_sdk16.c font_file). A small hinted source font is made here; with macOS's fonts the whole set is made too."""
import array
import os
from pathlib import Path
import re
import sys
import tempfile
from typing import Any
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
try:
    import fontTools   # noqa: F401
except ImportError:   # fontTools missing: the tool cannot run either
    raise unittest.SkipTest('fontTools is not installed') from None
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables import ttProgram
import make_shared_fonts as msf


def cmap(f): return f.getBestCmap() or {}

# One character per set: Latin, kana, a GB 2312 hanzi (also cp932 and Big5), a GBK-only hanzi (also Big5), a Hangul syllable, and one in no set
CHARS = {'A': 0x41, 'a_kana': 0x3042, 'zhong': 0x4E2D, 'diu': 0x4E1F, 'ga': 0xAC00, 'arabic': 0x0627}

def source_font(path):   # hinted: fpgm, prep, cvt, a large stack and instructions on every glyph, as Arial Unicode has
    names = ['.notdef'] + list(CHARS)
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(names); fb.setupCharacterMap({cp: n for n, cp in CHARS.items()})
    glyphs = {}
    for n in names:
        pen = TTGlyphPen(None); pen.moveTo((100, 0)); pen.lineTo((100, 700)); pen.lineTo((500, 700)); pen.closePath()
        g = pen.glyph(); g.program = ttProgram.Program(); g.program.fromAssembly(['PUSHB[ ]', '0', 'POP[ ]']); glyphs[n] = g
    fb.setupGlyf(glyphs); fb.setupHorizontalMetrics({n: (600, 100) for n in names}); fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({'familyName': 'Source', 'styleName': 'Regular', 'fullName': 'Source', 'psName': 'Source'})
    fb.setupOS2(); fb.setupPost(); fb.setupMaxp()
    f = fb.font
    for tag, asm in (('fpgm', ['PUSHB[ ]', '0', 'FDEF[ ]', 'ENDF[ ]']), ('prep', ['PUSHB[ ]', '0', 'POP[ ]'])):   # fontTools' tables carry their fields dynamically: Any
        t: Any = newTable(tag); t.program = ttProgram.Program(); t.program.fromAssembly(asm); f[tag] = t
    cvt: Any = newTable('cvt '); cvt.values = array.array('h', [0, 10, 20]); f['cvt '] = cvt
    mx: Any = f['maxp']; mx.maxStackElements = 4139; mx.maxFunctionDefs = 1; mx.maxSizeOfInstructions = 3
    f.save(path)


class Cuts(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.dir = Path(self.tmp.name)
        self.src = self.dir / 'source.ttf'; source_font(self.src)

    def tearDown(self): self.tmp.cleanup()

    def cut(self, t):
        name = dict((ty, nm) for ty, _, nm in msf.TYPES)[t]
        out = self.dir / f'{t}.ttf'
        msf.cut(str(self.src), str(out), msf.charsets()[t], name)
        return TTFont(out)

    def test_each_type_holds_its_characters_only(self):
        # GB 2312, Big5 and KS X 1001 hold kana too, and KS X 1001 hanja; the extension only what GBK adds to GB 2312; Arabic is in none
        want = {0: {'A', 'a_kana', 'zhong'}, 1: {'A', 'a_kana', 'zhong'}, 2: {'A', 'diu'}, 3: {'A', 'a_kana', 'zhong', 'diu'}, 4: {'A', 'a_kana', 'zhong', 'ga'}}
        for t, names in want.items():
            with self.subTest(type=t):
                have = cmap(self.cut(t))
                self.assertEqual({n for n, cp in CHARS.items() if cp in have}, names)

    def test_no_hinting_and_no_layout_tables_but_kerning(self):
        f = self.cut(0)
        for tag in ('fpgm', 'prep', 'cvt ', 'hdmx', 'VDMX', 'LTSH'): self.assertNotIn(tag, f)
        mx: Any = f['maxp']
        self.assertEqual((mx.maxStackElements, mx.maxSizeOfInstructions), (0, 0))
        self.assertFalse(any(getattr(f['glyf'][g], 'program', None) and f['glyf'][g].program.getBytecode() for g in f.getGlyphOrder()))

    def test_every_type_has_a_name_of_its_own(self):
        seen = set()
        for t, _, name in msf.TYPES:
            n = self.cut(t)['name']
            self.assertEqual((n.getDebugName(1), n.getDebugName(4), n.getDebugName(6)), (name, name, name.replace(' ', '-')))
            seen.add(name)
        self.assertEqual(len(seen), len(msf.TYPES))

    def test_file_names_are_the_hosts(self):
        src = (REPO / 'dm/host/hle_sdk16.c').read_text()
        m = re.search(r'font_file\[FONT_TYPES\] = \{([^}]*)\}', src)
        assert m, 'hle_sdk16.c font_file'
        self.assertEqual(re.findall(r'"([^"]+)"', m.group(1)), [fn for _, fn, _ in msf.TYPES] + [msf.EXT])


@unittest.skipIf(not (os.path.isfile(msf.SOURCE) and os.path.isfile(msf.LETTERS)), "macOS's Arial Unicode and Arial Rounded Bold")
class WholeSet(unittest.TestCase):
    def test_the_set_from_macos_fonts(self):
        with tempfile.TemporaryDirectory() as d:
            msf.build(d)
            names = set()
            for _, fn, _ in msf.TYPES + [(5, msf.EXT, '')]:
                f = TTFont(os.path.join(d, fn))
                self.assertNotIn('fpgm', f); self.assertNotIn('prep', f)
                names.add(f['name'].getDebugName(4))
            self.assertEqual(len(names), 6, 'six fonts, six full names (the name the font engine reports)')
            std = cmap(TTFont(os.path.join(d, 'standard.ttf')))
            for cp in (0x41, 0xE9, 0x416, 0x3042, 0x30A2, 0x6F22, 0xFF01): self.assertIn(cp, std)
            self.assertNotIn(0xAC00, std)
            self.assertIn(0xD7A3, cmap(TTFont(os.path.join(d, 'korean.ttf'))))


if __name__ == '__main__':
    unittest.main()
