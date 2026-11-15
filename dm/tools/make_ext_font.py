#!/usr/bin/env python3
# Generate a stand-in for the "Nintendo extension" shared font (nintendo_ext.ttf): controller-button icons at the original private-use code points (A/B/X/Y, L/R/ZL/ZR/SL/SR, +/−, D-pad, stick direction arrows).
# The host has no original system fonts; button icons in game text (e.g. Mario's GameMsg/Key.msbt: U+E0E0 = A button) render as "?" when the glyph is missing. The letters are a font's
# of this system (letter_font), knocked out of a solid plate: macOS's Arial Rounded Bold on a Mac; on Windows Arial Bold (every Windows has it), then Segoe UI or Tahoma Bold; on
# Linux the bold of the families the port's host takes the standard shared font from (dm/host/hle_sdk16.c font_system: DejaVu Sans, Liberation Sans, Noto Sans, FreeSans, in
# the XDG data folders' fonts/ at the paths the distributions install them to), then their regular weight.
# Usage: make_ext_font.py out.ttf [letter-font.ttf]; needs fontTools (pip install fonttools; the port's bundles carry it in their own Python). Exit 2 when no letter font is found.
import math, os, sys

# The letter fonts by platform, the first that exists taken (letter_font): paths, or names under the platform's font folders (font_dirs)
LETTER_FONTS = {
    'darwin': ['/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf'],
    'win32': ['arialbd.ttf', 'segoeuib.ttf', 'tahomabd.ttf', 'arial.ttf', 'segoeui.ttf', 'tahoma.ttf'],
    'linux': [f'{d}/{f}' for w in ('Bold', '') for d, f in (
        ('truetype/dejavu', f'DejaVuSans{"-" + w if w else ""}.ttf'), ('dejavu-sans-fonts', f'DejaVuSans{"-" + w if w else ""}.ttf'),
        ('dejavu', f'DejaVuSans{"-" + w if w else ""}.ttf'), ('TTF', f'DejaVuSans{"-" + w if w else ""}.ttf'),
        ('truetype/liberation', f'LiberationSans-{w or "Regular"}.ttf'), ('liberation-sans', f'LiberationSans-{w or "Regular"}.ttf'),
        ('liberation', f'LiberationSans-{w or "Regular"}.ttf'), ('TTF', f'LiberationSans-{w or "Regular"}.ttf'),
        ('truetype/noto', f'NotoSans-{w or "Regular"}.ttf'), ('noto', f'NotoSans-{w or "Regular"}.ttf'), ('google-noto', f'NotoSans-{w or "Regular"}.ttf'),
        ('TTF', f'NotoSans-{w or "Regular"}.ttf'), ('truetype/freefont', f'FreeSans{w}.ttf'), ('gnu-free', f'FreeSans{w}.ttf'))],
}


def font_dirs(platform=None, environ=None):
    """The folders a letter font's name is looked up in: Windows' %SystemRoot%\\Fonts; on Linux the XDG data folders' fonts/ ($XDG_DATA_HOME, default ~/.local/share,
    then $XDG_DATA_DIRS, default /usr/local/share:/usr/share), as the port's host looks for the standard font; none on a Mac (its entry is a path)"""
    platform, environ = platform or sys.platform, os.environ if environ is None else environ
    if platform == 'win32': return [os.path.join(environ.get('SystemRoot') or 'C:\\Windows', 'Fonts')]
    if platform != 'linux': return []
    home = environ.get('XDG_DATA_HOME') or (os.path.join(environ['HOME'], '.local/share') if environ.get('HOME') else '')
    return [os.path.join(d, 'fonts') for d in [home] + (environ.get('XDG_DATA_DIRS') or '/usr/local/share:/usr/share').split(':') if d]


def letter_font(platform=None, environ=None):
    """The letter font of this system (LETTER_FONTS, the first that exists), or None"""
    platform = platform or sys.platform
    names, dirs = LETTER_FONTS.get(platform, []), font_dirs(platform, environ)
    for n in names:
        for p in [n] if os.path.isabs(n) or not dirs else [os.path.join(d, n) for d in dirs]:
            if os.path.isfile(p): return p
    return None


def where(platform=None, environ=None):   # the places letter_font looks, for a message
    platform = platform or sys.platform
    dirs = font_dirs(platform, environ)
    return ', '.join(LETTER_FONTS.get(platform, [])[:4]) + (' ... in ' + ', '.join(dirs) if dirs else '')

EM, ASC, DESC = 1000, 880, -120
CY, R = 350, 450          # icon centre height and circle radius: -100 to 800, sitting on the baseline

def area(c): return sum(c[i][0] * c[i - 1][1] - c[i - 1][0] * c[i][1] for i in range(len(c))) / 2
def contour(pen, pts, hole=False):
    # solid clockwise, hole counter-clockwise (TrueType convention, nonzero winding)
    if (area(pts) > 0) != hole: pts = pts[::-1]
    k = next(i for i, p in enumerate(pts) if p[2]); pts = pts[k:] + pts[:k]
    pen.moveTo(pts[0][:2]); offs = []
    for x, y, on in pts[1:]:
        if not on: offs.append((x, y)); continue
        if offs: pen.qCurveTo(*offs, (x, y)); offs = []
        else: pen.lineTo((x, y))
    if offs: pen.qCurveTo(*offs, pts[0][:2])
    pen.closePath()
def circle(cx, cy, r, n=8):
    pts = []
    for i in range(n):
        a = 2 * math.pi * i / n; b = a + math.pi / n; rr = r / math.cos(math.pi / n)
        pts += [(round(cx + r * math.cos(a)), round(cy + r * math.sin(a)), 1), (round(cx + rr * math.cos(b)), round(cy + rr * math.sin(b)), 0)]
    return pts
def rrect(x0, y0, x1, y1, rad):
    rad = min(rad, (x1 - x0) / 2, (y1 - y0) / 2)
    return [(x0 + rad, y1, 1), (x1 - rad, y1, 1), (x1, y1, 0), (x1, y1 - rad, 1), (x1, y0 + rad, 1), (x1, y0, 0), (x1 - rad, y0, 1),
            (x0 + rad, y0, 1), (x0, y0, 0), (x0, y0 + rad, 1), (x0, y1 - rad, 1), (x0, y1, 0)]
def rect(x0, y0, x1, y1): return [(x0, y1, 1), (x1, y1, 1), (x1, y0, 1), (x0, y0, 1)]
def rot(pts, cx, cy, deg):
    a = math.radians(deg); c, s = math.cos(a), math.sin(a)
    return [(round(cx + (x - cx) * c - (y - cy) * s), round(cy + (x - cx) * s + (y - cy) * c), on) for x, y, on in pts]

def _fonttools():   # fontTools, imported when a font is built: letter_font is imported by the packer (build_ipa_from_nsp.py port_font), whose Python may lack it
    global TTFont, FontBuilder, TTGlyphPen, TransformPen, ReverseContourPen, BoundsPen, RecordingPen
    from fontTools.ttLib import TTFont
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.pens.transformPen import TransformPen
    from fontTools.pens.reverseContourPen import ReverseContourPen
    from fontTools.pens.boundsPen import BoundsPen
    from fontTools.pens.recordingPen import RecordingPen

class Letters:
    def __init__(self, path):
        self.f = TTFont(path); self.gs = self.f.getGlyphSet(); self.cmap = self.f.getBestCmap()
    def knock(self, pen, text, cx, cy, h):
        # knock text out of the plate, centred at (cx, cy) with height h
        names = [self.cmap[ord(ch)] for ch in text]; x = 0; parts = []
        for nm in names: parts.append((nm, x)); x += self.gs[nm].width
        bp = BoundsPen(self.gs)
        for nm, dx in parts: self.gs[nm].draw(TransformPen(bp, (1, 0, 0, 1, dx, 0)))
        x0, y0, x1, y1 = bp.bounds; s = h / (y1 - y0)
        ox, oy = cx - (x0 + x1) / 2 * s, cy - (y0 + y1) / 2 * s
        # Knocked out = opposite direction to the plate. Fonts differ in outer-contour direction (TrueType clockwise, converted fonts often counter-clockwise); flip based on the largest contour's direction
        rec = RecordingPen()
        for nm, dx in parts: self.gs[nm].draw(TransformPen(rec, (s, 0, 0, s, ox + dx * s, oy)))
        rings, cur = [], []
        for op, args in rec.value:
            if op == 'moveTo': cur = [args[0]]; rings.append(cur)
            elif op in ('lineTo', 'qCurveTo', 'curveTo'): cur.extend(a for a in args if a is not None)
        big = max(rings, key=lambda c: abs(area(c)))
        rec.replay(ReverseContourPen(pen) if area(big) < 0 else pen)

def cross(cx, cy, half, arm):   # D-pad outline (one closed ring)
    a, h = arm / 2, half
    return [(cx - a, cy + h, 1), (cx + a, cy + h, 1), (cx + a, cy + a, 1), (cx + h, cy + a, 1), (cx + h, cy - a, 1), (cx + a, cy - a, 1),
            (cx + a, cy - h, 1), (cx - a, cy - h, 1), (cx - a, cy - a, 1), (cx - h, cy - a, 1), (cx - h, cy + a, 1), (cx - a, cy + a, 1)]

def build(out, letter_font):
    _fonttools()
    L = Letters(letter_font); glyphs = {}; adv = {}; cmap = {}
    def glyph(name, codes, width, draw):
        pen = TTGlyphPen(None); draw(pen, width / 2); glyphs[name] = pen.glyph(); adv[name] = width
        for c in codes: cmap[c] = name
    def round_btn(t):
        def d(pen, cx): contour(pen, circle(cx, CY, R)); L.knock(pen, t, cx, CY, 470)
        return d
    def plate(t, w, h=620, th=400):
        def d(pen, cx): contour(pen, rrect(cx - w / 2, CY - h / 2, cx + w / 2, CY + h / 2, 220)); L.knock(pen, t, cx, CY, th)
        return d
    def sign(minus):
        def d(pen, cx):
            contour(pen, circle(cx, CY, R)); t, l = 70, 250
            if minus: contour(pen, rect(cx - l, CY - t, cx + l, CY + t), hole=True)
            else: contour(pen, cross(cx, CY, l, 2 * t), hole=True)
        return d
    def dpad(arm_dir):   # None = all solid; otherwise only one arm filled
        def d(pen, cx):
            contour(pen, cross(cx, CY, R, 320))
            if arm_dir is None: return
            contour(pen, cross(cx, CY, R - 55, 320 - 110), hole=True)
            arm = rect(cx - 105, CY + 105, cx + 105, CY + R - 55)
            contour(pen, rot(arm, cx, CY, {'up': 0, 'left': 90, 'down': 180, 'right': 270}[arm_dir]))
        return d
    def arrow(deg):
        def d(pen, cx):
            p = [(-380, 105, 1), (60, 105, 1), (60, 310, 1), (420, 0, 1), (60, -310, 1), (60, -105, 1), (-380, -105, 1)]
            contour(pen, rot([(cx + x, CY + y, on) for x, y, on in p], cx, CY, deg))
        return d
    def stick(t):
        def d(pen, cx):
            contour(pen, circle(cx, CY, R)); contour(pen, circle(cx, CY, R - 70), hole=True); contour(pen, circle(cx, CY, 300))
            if t: L.knock(pen, t, cx, CY, 300)
        return d

    pen = TTGlyphPen(None); glyphs['.notdef'] = pen.glyph(); adv['.notdef'] = 500
    for i, t in enumerate(['A', 'B', 'X', 'Y']): glyph('btn' + t, [0xE0A0 + i, 0xE0E0 + i], 1000, round_btn(t))
    for i, (t, w) in enumerate([('L', 900), ('R', 900), ('ZL', 1200), ('ZR', 1200), ('SL', 1200), ('SR', 1200)]):
        glyph('btn' + t, [0xE0A4 + i, 0xE0E4 + i], w + 100, plate(t, w))
    glyph('btnPlus', [0xE0B5, 0xE0EF], 1000, sign(False)); glyph('btnMinus', [0xE0B6, 0xE0F0], 1000, sign(True))
    glyph('dpad', [0xE0EA], 1000, dpad(None))
    for c, dr in [(0xE0AB, 'right'), (0xE0AC, 'down'), (0xE0AD, 'up'), (0xE0AE, 'left')]: glyph('dpad_' + dr, [c, c + 0x40], 1000, dpad(dr))
    for c, (nm, deg) in zip(range(0xE090, 0xE098), [('right', 0), ('left', 180), ('up', 90), ('down', 270), ('rightup', 45), ('rightdown', 315), ('leftdown', 225), ('leftup', 135)]):
        glyph('arrow_' + nm, [c], 1000, arrow(deg))
    glyph('stick', [0xE100], 1000, stick(None)); glyph('stickL', [0xE101, 0xE0C1], 1000, stick('L')); glyph('stickR', [0xE102, 0xE0C2], 1000, stick('R'))

    order = ['.notdef'] + sorted(n for n in glyphs if n != '.notdef')
    fb = FontBuilder(EM, isTTF=True)
    fb.setupGlyphOrder(order); fb.setupCharacterMap(cmap); fb.setupGlyf(glyphs)
    glyf = fb.font['glyf']
    for n in order: glyf[n].recalcBounds(glyf)
    fb.setupHorizontalMetrics({n: (adv[n], getattr(glyf[n], 'xMin', 0)) for n in order})
    fb.setupHorizontalHeader(ascent=ASC, descent=DESC)
    fb.setupNameTable({'familyName': 'DM Nintendo Extension Stand-in', 'styleName': 'Regular'})
    fb.setupOS2(sTypoAscender=ASC, sTypoDescender=DESC, sTypoLineGap=0, usWinAscent=ASC, usWinDescent=-DESC)
    fb.setupPost(); fb.save(out)
    print('%s: %d glyphs, %d code points, letters from %s' % (out, len(order), len(cmap), letter_font))

if __name__ == '__main__':
    if len(sys.argv) < 2: sys.exit(__doc__ or 'usage: make_ext_font.py out.ttf [letter-font.ttf]')
    letters = sys.argv[2] if len(sys.argv) > 2 else letter_font()
    if not letters:
        print(f'[RED] make_ext_font.py: no letter font on this system (looked for {where()}): give one as the second argument', file=sys.stderr); sys.exit(2)
    build(sys.argv[1], letters)
