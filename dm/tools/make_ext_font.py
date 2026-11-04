#!/usr/bin/env python3
# Generate a stand-in for the "Nintendo extension" shared font (nintendo_ext.ttf): controller-button icons at the original private-use code points (A/B/X/Y, L/R/ZL/ZR/SL/SR, +/−, D-pad, stick direction arrows).
# The host has no original system fonts; button icons in game text (e.g. Mario's GameMsg/Key.msbt: U+E0E0 = A button) render as "?" when the glyph is missing. Letters come from macOS's Arial Rounded Bold, knocked out of a solid plate.
# --style playstation / xbox draws the same code points as that controller's buttons, where they sit on the pad (the host maps every pad by position, pad_apple.h:
# Switch A = the right button = Circle / Xbox B, B = bottom = Cross / A, X = top = Triangle / Y, Y = left = Square / X; L R ZL ZR = L1 R1 L2 R2 / LB RB LT RT; + - = Options Create / Menu View).
# The host serves nintendo_ext_<style>.ttf beside nintendo_ext.ttf when DM_BUTTON_ICONS picks that style (hle_sdk16.c). The glyph names are the same in every style, and button_icons.py
# draws a game's own icon font from them by name (dpad_updown and dpad_leftright have no code point here; they are there for it).
# Usage: make_ext_font.py out.ttf [letter-font.ttf] [--style switch|playstation|xbox]; needs fontTools (pip install fonttools)
import math, sys
STYLES = ('switch', 'playstation', 'xbox')
from fontTools.ttLib import TTFont
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.reverseContourPen import ReverseContourPen
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.recordingPen import RecordingPen

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

def tri(cx, cy, r):   # an equilateral triangle, point up, circumradius r, centred on its centroid
    return [(round(cx + r * math.cos(math.radians(90 + 120 * i))), round(cy + r * math.sin(math.radians(90 + 120 * i))), 1) for i in range(3)]

def build(out, letter_font, style='switch'):
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
    def dpad(*arm_dirs):   # none = all solid; otherwise only those arms filled
        def d(pen, cx):
            contour(pen, cross(cx, CY, R, 320))
            if not arm_dirs: return
            contour(pen, cross(cx, CY, R - 55, 320 - 110), hole=True)
            arm = rect(cx - 105, CY + 105, cx + 105, CY + R - 55)
            for arm_dir in arm_dirs: contour(pen, rot(arm, cx, CY, {'up': 0, 'left': 90, 'down': 180, 'right': 270}[arm_dir]))
        return d
    T = 75   # stroke of the PlayStation symbols and the Menu / View marks
    def ps_face(sym):   # a disc with the symbol's outline knocked out
        def d(pen, cx):
            contour(pen, circle(cx, CY, R))
            if sym == 'circle': contour(pen, circle(cx, CY, 250, 16), hole=True); contour(pen, circle(cx, CY, 250 - T, 16))
            elif sym == 'square': contour(pen, rect(cx - 205, CY - 205, cx + 205, CY + 205), hole=True); contour(pen, rect(cx - 205 + T, CY - 205 + T, cx + 205 - T, CY + 205 - T))
            elif sym == 'triangle': contour(pen, tri(cx, CY - 25, 300), hole=True); contour(pen, tri(cx, CY - 25, 300 - 2 * T))
            else: contour(pen, rot(cross(cx, CY, 280, T), cx, CY, 45), hole=True)
        return d
    def bars(n, w):   # Options / Menu: n bars knocked out of a disc
        def d(pen, cx):
            contour(pen, circle(cx, CY, R))
            for i in range(n): y = CY + (i - (n - 1) / 2) * 2 * T; contour(pen, rect(cx - w / 2, y - T / 2, cx + w / 2, y + T / 2), hole=True)
        return d
    def create(pen, cx):   # the Create button's three strokes, rising to the right
        contour(pen, circle(cx, CY, R))
        for i, h in enumerate((150, 250, 350)):
            x = cx + (i - 1) * 2 * T; contour(pen, rect(x - T / 2, CY - 175, x + T / 2, CY - 175 + h), hole=True)
    def view(pen, cx):   # Xbox View: two overlapping windows, the back one (up and right) showing where the front one leaves it; holes must not overlap (nonzero winding)
        def ring(x0, y0, x1, y1): return [(x0, y1 - T, x1, y1), (x0, y0, x1, y0 + T), (x0, y0 + T, x0 + T, y1 - T), (x1 - T, y0 + T, x1, y1 - T)]
        def minus(r, c):   # rectangle r without rectangle c, as up to four rectangles
            x0, y0, x1, y1 = r; a0, b0, a1, b1 = c
            if a0 >= x1 or a1 <= x0 or b0 >= y1 or b1 <= y0: return [r]
            out = [(x0, y0, x1, b0)] if b0 > y0 else []
            if b1 < y1: out.append((x0, b1, x1, y1))
            if a0 > x0: out.append((x0, max(y0, b0), a0, min(y1, b1)))
            if a1 < x1: out.append((a1, max(y0, b0), x1, min(y1, b1)))
            return out
        contour(pen, circle(cx, CY, R)); h, o = 150, 65
        front = (cx - o - h, CY - o - h, cx - o + h, CY - o + h)
        for r in ring(*front) + [q for e in ring(cx + o - h, CY + o - h, cx + o + h, CY + o + h) for q in minus(e, front)]: contour(pen, rect(*r), hole=True)

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
    # Each style's face buttons in Switch order (A B X Y = right, bottom, top, left), shoulders (L R ZL ZR) and + / -
    face = {'switch': [round_btn(t) for t in 'ABXY'], 'playstation': [ps_face(t) for t in ('circle', 'cross', 'triangle', 'square')],
            'xbox': [round_btn(t) for t in 'BAYX']}[style]
    shoulders = {'switch': ['L', 'R', 'ZL', 'ZR'], 'playstation': ['L1', 'R1', 'L2', 'R2'], 'xbox': ['LB', 'RB', 'LT', 'RT']}[style]
    plus_minus = {'switch': [sign(False), sign(True)], 'playstation': [bars(3, 380), create], 'xbox': [bars(3, 380), view]}[style]
    for i, t in enumerate(['A', 'B', 'X', 'Y']): glyph('btn' + t, [0xE0A0 + i, 0xE0E0 + i], 1000, face[i])
    for i, (t, w) in enumerate([('L', 900), ('R', 900), ('ZL', 1200), ('ZR', 1200), ('SL', 1200), ('SR', 1200)]):
        label = shoulders[i] if i < 4 else t
        glyph('btn' + t, [0xE0A4 + i, 0xE0E4 + i], w + 100, plate(label, w if len(label) == len(t) else 1200))
    glyph('btnPlus', [0xE0B5, 0xE0EF], 1000, plus_minus[0]); glyph('btnMinus', [0xE0B6, 0xE0F0], 1000, plus_minus[1])
    glyph('dpad', [0xE0EA], 1000, dpad())
    for c, dr in [(0xE0AB, 'right'), (0xE0AC, 'down'), (0xE0AD, 'up'), (0xE0AE, 'left')]: glyph('dpad_' + dr, [c, c + 0x40], 1000, dpad(dr))
    glyph('dpad_updown', [], 1000, dpad('up', 'down')); glyph('dpad_leftright', [], 1000, dpad('left', 'right'))
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
    fb.setupNameTable({'familyName': 'DM Nintendo Extension Stand-in', 'styleName': 'Regular' if style == 'switch' else style.capitalize()})
    fb.setupOS2(sTypoAscender=ASC, sTypoDescender=DESC, sTypoLineGap=0, usWinAscent=ASC, usWinDescent=-DESC)
    fb.setupPost(); fb.save(out)
    print('%s: %d glyphs, %d code points' % (out, len(order), len(cmap)))

if __name__ == '__main__':
    args, style = sys.argv[1:], 'switch'
    if '--style' in args:
        k = args.index('--style'); style = args[k + 1] if k + 1 < len(args) else ''; del args[k:k + 2]
        if style not in STYLES: sys.exit('make_ext_font.py: --style is one of ' + ', '.join(STYLES))
    if not args: sys.exit(__doc__ or 'usage: make_ext_font.py out.ttf [letter-font.ttf] [--style switch|playstation|xbox]')
    build(args[0], args[1] if len(args) > 1 else '/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf', style)
