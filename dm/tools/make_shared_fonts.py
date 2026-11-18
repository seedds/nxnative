#!/usr/bin/env python3
"""Make the host's stand-ins for the six system shared fonts in one folder (hle_sdk16.c reads them from DM_FONT_DIR):
types 0-4 (standard, Simplified Chinese, its extension, Traditional Chinese, Korean) as unhinted cuts of one source font, type 5 (Nintendo extension: button icons) from make_ext_font.py.
A game sizes each nn::fontll font engine's work memory for Nintendo's font of that type, and what FSS_load_font needs grows with the font's hinting program and its outlines, not with its glyph count:
Mario Tennis Fever's 50 KB engine needs 56-60 KB for macOS's Arial Unicode as it is (LoadFont 201, then a crash), 44-48 KB for it unhinted, at most 32 KB for an unhinted Japanese cut
(docs/reference/services.md, "Shared font stand-ins sized for the game's font engines"). So every cut drops the hinting (fpgm, prep, cvt and each glyph's instructions) and the layout tables but kerning, and
holds the characters of its type only, as Nintendo's set splits them; each takes a name of its own, as a game can load several types into one engine.
Usage: make_shared_fonts.py out_dir [source.ttf]; needs fontTools (pip install fonttools)"""
import os, sys
from fontTools import subset
from fontTools.ttLib import TTFont

SOURCE = '/System/Library/Fonts/Supplemental/Arial Unicode.ttf'   # macOS's; STD_FONT / the second argument names another (build_ipa.sh, the NSP script)
LETTERS = '/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf'   # the button icons' letters (make_ext_font.py)
# Type → file name (hle_sdk16.c font_file) and the name the font engine reports for it (LoadFont's name; short, as Nintendo's are)
TYPES = [(0, 'standard.ttf', 'DM Standard'), (1, 'chinese_simplified.ttf', 'DM Chinese Simplified'), (2, 'chinese_simplified_ext.ttf', 'DM Chinese Simplified Ext'),
         (3, 'chinese_traditional.ttf', 'DM Chinese Traditional'), (4, 'korean.ttf', 'DM Korean')]
EXT = 'nintendo_ext.ttf'

def rng(*pairs): return {c for a, b in pairs for c in range(a, b + 1)}

def legacy(codec, lead, trail=None):   # every character of a double-byte legacy set: the lead and trail byte ranges, decoded
    out, (lo, hi) = set(), trail or lead
    for a in range(lead[0], lead[1] + 1):
        for b in range(lo, hi + 1):
            try: s = bytes([a, b]).decode(codec)
            except UnicodeDecodeError: continue
            if len(s) == 1: out.add(ord(s))
    return out

# What every type holds besides its script, so a game that puts a Chinese or Korean font first still has digits, Latin and the CJK punctuation from it
COMMON = rng((0x20, 0x7e), (0xa0, 0xff), (0x2000, 0x206f), (0x3000, 0x303f), (0xff00, 0xffef))

def charsets():
    gb2312 = legacy('gb2312', (0xa1, 0xfe))
    gbk_han = {c for c in legacy('gbk', (0x81, 0xfe), (0x40, 0xff)) if 0x4e00 <= c <= 0x9fff} - gb2312
    return {
        # Japanese with Latin, Greek and Cyrillic, as Nintendo's standard font (nintendo_udsg-r_std): JIS X 0208 with the NEC and IBM additions (cp932), and the symbols games use
        0: COMMON | rng((0x100, 0x24f), (0x370, 0x3ff), (0x400, 0x4ff), (0x20a0, 0x20cf), (0x2100, 0x22ff), (0x2300, 0x23ff), (0x2460, 0x27bf), (0x3040, 0x30ff), (0x31f0, 0x31ff))
           | legacy('cp932', (0x81, 0xfc), (0x40, 0xfd)),
        1: COMMON | gb2312,                                                       # GB 2312 (nintendo_udsg-r_org_zh-cn)
        2: COMMON | gbk_han,                                                      # the other GBK hanzi (nintendo_udsg-r_ext_zh-cn)
        3: COMMON | legacy('big5', (0xa1, 0xf9), (0x40, 0xff)),                   # Big5 (nintendo_udjxh-db_zh-tw)
        4: COMMON | rng((0x1100, 0x11ff), (0x3130, 0x318f), (0xac00, 0xd7a3)) | legacy('euc_kr', (0xa1, 0xfe)),   # every modern Hangul syllable and KS X 1001 (nintendo_udsg-r_ko)
    }

def cut(src, out, chars, name):
    f = TTFont(src)
    have = sorted(c for c in chars if c in (f.getBestCmap() or {}))
    # No hinting: its program sizes the engine's interpreter memory (Arial Unicode: 4,139 stack elements, a 9 KB prep). Of the layout tables only kerning: GSUB's forms grow the tables and the glyph count
    o = subset.Options(hinting=False, layout_features=['kern'], name_IDs=['*'], name_languages=['*'], notdef_outline=True, recalc_bounds=True)
    s = subset.Subsetter(o); s.populate(unicodes=have); s.subset(f)
    ps = name.replace(' ', '-')
    for rec in f['name'].names:       # family, unique, full and PostScript names (and the typographic family): one font each, as the engine knows fonts by name
        if rec.nameID in (1, 4, 16): rec.string = name
        elif rec.nameID in (3, 6): rec.string = ps
    f.save(out)
    return len(have), f['maxp'].numGlyphs

def build(out_dir, src=SOURCE, letters=LETTERS):
    os.makedirs(out_dir, exist_ok=True)
    sets = charsets()
    for t, fn, name in TYPES:
        chars, glyphs = cut(src, os.path.join(out_dir, fn), sets[t], name)
        print('%s: type %d, %d characters, %d glyphs, %d bytes' % (fn, t, chars, glyphs, os.path.getsize(os.path.join(out_dir, fn))))
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import make_ext_font
    make_ext_font.build(os.path.join(out_dir, EXT), letters)

if __name__ == '__main__':
    if len(sys.argv) < 2: sys.exit(__doc__ or 'usage: make_shared_fonts.py out_dir [source.ttf]')
    build(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else SOURCE)
