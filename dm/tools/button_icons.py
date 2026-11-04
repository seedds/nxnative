#!/usr/bin/env python3
# PlayStation / Xbox button icons for a game's own icon font, as RomFS overlay folders the host serves when DM_BUTTON_ICONS picks that style (hle_fs.c, hle_sdk16.c).
# Two places hold a Switch game's button icons:
#   - the Nintendo extension shared font (system font type 5): the host serves make_ext_font.py's stand-in, nintendo_ext_<style>.ttf for a style, so every game that draws its
#     prompts with it changes with no work here;
#   - a font in the game's own RomFS. A profile below names the archives, the font in them and which of its code points is which button; this tool redraws those glyphs
#     from the same styled stand-in (by glyph name: btnA, btnL, dpad_up, ...), so one drawing serves both places.
# Supported font: BFFNT (NX, version 4) with BC4 sheets in a Yaz0 SARC (Breath of the Wild's Font/Font_<region>.sbfarc -> External_00.bffnt). Each glyph is drawn into the box
# the original icon's ink filled, so prompts keep their size and spacing; only the BC4 blocks that changed are encoded again, so the font keeps its size and the game's resource
# size table needs no change. The archive is compressed again only around the font (the Yaz0 groups before it and from 4 KiB past it are copied), so it stays about as large as
# the game's own.
# `survey` writes every BFFNT icon sheet it finds in a RomFS as a PNG, with a TSV of each code point's cell, for writing a new game's profile.
# Usage: button_icons.py make <RomFS folder or image> <out dir> --title-id <id> --font-dir <dir with nintendo_ext_<style>.ttf> [--styles playstation,xbox]
#        button_icons.py survey <RomFS folder or image> <out dir>
# make writes <out dir>/<style>/<RomFS path>; a title without a profile writes nothing (its prompts still change if it uses the shared font). Needs fontTools.
import argparse, fnmatch, math, struct, sys, zlib
from pathlib import Path as _Path

sys.path.insert(0, str(_Path(__file__).resolve().parent))
import romfs  # noqa: E402

# A profile: the archives (glob in the RomFS), the member font, whether its sheet is stored upside down (checked on the game's own sheet: survey writes both), and
# {code point: stand-in glyph name} of the glyphs to redraw. Glyphs not named here (stick directions, arrows, the console, Home, the power button) keep the game's own icons.
PROFILES = {
    '01007EF00011E000': dict(name='The Legend of Zelda: Breath of the Wild', archives='Font/Font_*.sbfarc', member='External_00.bffnt', flip=True,
                             glyphs={0xE040: 'btnA', 0xE041: 'btnB', 0xE042: 'btnX', 0xE043: 'btnY', 0xE044: 'btnL', 0xE045: 'btnR', 0xE046: 'btnZL', 0xE047: 'btnZR',
                                     0xE049: 'dpad', 0xE04B: 'btnPlus', 0xE04C: 'btnMinus', 0xE060: 'dpad_up', 0xE061: 'dpad_down', 0xE062: 'dpad_left',
                                     0xE063: 'dpad_right', 0xE064: 'dpad_updown', 0xE065: 'dpad_leftright'}),
}
STYLES = ('playstation', 'xbox')

# ---- Yaz0 ----
def yaz0_ops(src):
    """(decompressed bytes, [(compressed offset, decompressed offset) of each group]) of a Yaz0 file."""
    assert src[:4] == b'Yaz0', 'not Yaz0'
    n = struct.unpack_from('>I', src, 4)[0]; out = bytearray(); i = 16; groups = []
    while len(out) < n:
        groups.append((i, len(out))); flags = src[i]; i += 1
        for b in range(8):
            if len(out) >= n: break
            if flags & (0x80 >> b): out.append(src[i]); i += 1; continue
            b1, b2 = src[i], src[i + 1]; i += 2; dist = ((b1 & 0xF) << 8 | b2) + 1; cnt = b1 >> 4
            if cnt == 0: cnt = src[i] + 0x12; i += 1
            else: cnt += 2
            s = len(out) - dist
            if dist >= cnt: out += out[s:s + cnt]
            else:
                for k in range(cnt): out.append(out[s + k])
    groups.append((i, len(out)))
    return bytes(out), groups

def yaz0_encode(data, start, end, whole=True):
    """Yaz0 groups for data[start:end] (back references may reach 4 KiB before start). whole: complete groups, for a stream that goes on after them: the op count is made a
    multiple of 8 by splitting long matches; the stream's last group may be partial."""
    ops = []; heads = {}; i = start
    for p in range(max(0, start - 0x1000), start): heads.setdefault(data[p:p + 3], []).append(p)
    while i < end:
        best, bd = 0, 0; cand = heads.get(data[i:i + 3], ())
        for p in reversed(cand[-32:]):
            if i - p > 0x1000: break
            m = 0; lim = min(0x111, end - i)
            while m < lim and data[p + m] == data[i + m]: m += 1
            if m > best: best, bd = m, i - p
            if m == lim: break
        step = best if best >= 3 else 1
        ops.append((bd, best) if best >= 3 else (0, data[i]))
        for p in range(i, i + step): heads.setdefault(data[p:p + 3], []).append(p)
        i += step
    k = len(ops) - 1
    while whole and len(ops) % 8:   # split the last long matches (each split adds one op) until the groups are whole
        while k >= 0 and not (ops[k][0] and ops[k][1] >= 6): k -= 1
        if k < 0: raise ValueError('cannot make whole Yaz0 groups')
        d, n = ops[k]; ops[k:k + 1] = [(d, 3), (d, n - 3)]
    out = bytearray()
    for g in range(0, len(ops), 8):
        flags = 0; body = bytearray()
        for b, (d, v) in enumerate(ops[g:g + 8]):
            if not d: flags |= 0x80 >> b; body.append(v); continue
            if v < 0x12: body += bytes([(v - 2) << 4 | (d - 1) >> 8, (d - 1) & 0xFF])
            else: body += bytes([(d - 1) >> 8, (d - 1) & 0xFF, v - 0x12])
        out.append(flags); out += body
    return bytes(out)

def yaz0_splice(src, groups, new, lo, hi):
    """src (Yaz0 of the old data) for new, which differs from it only in [lo, hi): the groups before lo and from 4 KiB + one match past hi are kept."""
    a = max(k for k, (_, o) in enumerate(groups) if o <= lo)
    c = next((k for k, (_, o) in enumerate(groups) if o >= hi + 0x1000 + 0x111), len(groups) - 1)
    (ia, oa), (ic, oc) = groups[a], groups[c]
    if c == len(groups) - 1: oc = len(new)   # the last group may be partial: encode to the end
    mid = yaz0_encode(new, oa, oc, whole=oc < len(new))
    return src[:16] + src[16:ia] + mid + (src[ic:] if oc < len(new) else b'')

# ---- SARC ----
def sarc_member(sarc, name):
    """(start, end) of a member of a SARC (either byte order)."""
    assert sarc[:4] == b'SARC', 'not a SARC'
    bo = '<' if sarc[6:8] == b'\xff\xfe' else '>'; hl = struct.unpack_from(bo + 'H', sarc, 4)[0]; data = struct.unpack_from(bo + 'I', sarc, 0x0C)[0]
    nodes = struct.unpack_from(bo + 'H', sarc, hl + 6)[0]; names = hl + 12 + nodes * 16 + 8
    for k in range(nodes):
        _, attr, a, b = struct.unpack_from(bo + 'IIII', sarc, hl + 12 + k * 16)
        at = names + (attr & 0xFFFFFF) * 4
        if attr >> 24 and sarc[at:sarc.index(b'\0', at)].decode() == name: return data + a, data + b
    raise KeyError(name)

def sarc_names(sarc):
    bo = '<' if sarc[6:8] == b'\xff\xfe' else '>'; hl = struct.unpack_from(bo + 'H', sarc, 4)[0]; nodes = struct.unpack_from(bo + 'H', sarc, hl + 6)[0]
    names = hl + 12 + nodes * 16 + 8; out = []
    for k in range(nodes):
        attr = struct.unpack_from(bo + 'I', sarc, hl + 12 + k * 16 + 4)[0]; at = names + (attr & 0xFFFFFF) * 4
        if attr >> 24: out.append(sarc[at:sarc.index(b'\0', at)].decode())
    return out

# ---- BFFNT ----
class Font:
    """An NX BFFNT: its one sheet, cell geometry and {code point: glyph index}."""
    def __init__(self, data):
        assert data[:4] == b'FFNT' and data[4:6] == b'\xff\xfe', 'not a little-endian BFFNT'
        self.data = data; self.cmap = {}; self.cwdh = []; k = struct.unpack_from('<H', data, 6)[0]; self.tglp = None
        while k + 8 <= len(data):
            magic, size = data[k:k + 4], struct.unpack_from('<I', data, k + 4)[0]
            if magic == b'TGLP': self.tglp = k
            elif magic == b'CMAP': self._cmap(k)
            elif magic == b'CWDH': self.cwdh.append(k)
            elif magic not in (b'FINF', b'KRNG'): break
            if size < 8: break
            k += size
        assert self.tglp is not None, 'no TGLP'
        (self.cw, self.ch, self.sheets, _, self.sheet_size, _, self.fmt, self.cols, self.rows_, self.w, self.h,
         self.off) = struct.unpack_from('<BBBBIHHHHHHI', data, self.tglp + 8)
        self.bh, self.pixels = 16, self.off   # an NX sheet is a whole BNTX: its BRTI gives the block height and where the pixels start
        if data[self.off:self.off + 4] == b'BNTX':
            bntx = self.off; brti = bntx + struct.unpack_from('<Q', data, bntx + struct.unpack_from('<Q', data, bntx + 0x28)[0])[0]
            assert data[brti:brti + 4] == b'BRTI', 'no BRTI in the sheet'
            self.bh = 1 << struct.unpack_from('<I', data, brti + 0x34)[0]
            self.pixels = bntx + struct.unpack_from('<Q', data, bntx + struct.unpack_from('<Q', data, brti + 0x70)[0])[0]
    def _cmap(self, k):
        lo, hi, method = struct.unpack_from('<IIH', self.data, k + 8); d = k + 24
        if method == 0: first = struct.unpack_from('<H', self.data, d)[0]; self.cmap.update({cp: first + cp - lo for cp in range(lo, hi + 1)})
        elif method == 1:
            for cp in range(lo, hi + 1):
                g = struct.unpack_from('<H', self.data, d + (cp - lo) * 2)[0]
                if g != 0xFFFF: self.cmap[cp] = g
        else:
            n = struct.unpack_from('<I', self.data, d)[0]
            for j in range(n): cp, g = struct.unpack_from('<IH', self.data, d + 4 + j * 8); self.cmap[cp] = g
    def widths(self, g):   # offset of glyph g's (left, glyph width, advance) in CWDH
        for k in self.cwdh:
            lo, hi = struct.unpack_from('<HH', self.data, k + 8)
            if lo <= g <= hi: return k + 16 + (g - lo) * 3
        raise KeyError(g)
    def cell(self, g):   # (x, y) of glyph g's cell in the upright sheet; cells sit on a pitch one pixel larger than the cell
        r, c = divmod(g, self.cols); return c * (self.cw + 1), r * (self.ch + 1)

# ---- sheet: Tegra block-linear swizzle, BC4 ----
def gob_addr(x, y, wgobs, bh):   # x in bytes and y in rows of the block-linear surface -> byte offset
    block = (y // (8 * bh)) * wgobs + x // 64
    return block * 512 * bh + (y // 8 % bh) * 512 + (x % 64) // 32 * 256 + (y % 8) // 2 * 64 + (x % 32) // 16 * 32 + (y % 2) * 16 + x % 16

def block_map(f):
    """[(byte offset of block (bx, by) of the stored sheet)] in row-major order of the upright sheet (by, bx), for BC4 (8 bytes a block)."""
    wb, hb = f.w // 4, f.h // 4; pitch = wb * 8; wgobs = (pitch + 63) // 64; out = [0] * (wb * hb)
    for by in range(hb):
        for bx in range(wb): out[by * wb + bx] = gob_addr(bx * 8, by, wgobs, f.bh)
    return out

def stored_row(f, flip, y): return f.h - 1 - y if flip else y   # the stored row of upright row y

def bc4_decode(b):
    r0, r1 = b[0], b[1]; bits = int.from_bytes(b[2:8], 'little')
    pal = [r0, r1] + ([((7 - i) * r0 + i * r1) // 7 for i in range(1, 7)] if r0 > r1 else [((5 - i) * r0 + i * r1) // 5 for i in range(1, 5)] + [0, 255])
    return [pal[bits >> 3 * i & 7] for i in range(16)]

def bc4_encode(px):
    hi, lo = max(px), min(px)
    if hi == lo: return bytes([hi, lo]) + bytes(6)
    pal = [hi, lo] + [((7 - i) * hi + i * lo) // 7 for i in range(1, 7)]; bits = 0
    for i, v in enumerate(px): bits |= min(range(8), key=lambda k: abs(pal[k] - v)) << 3 * i
    return bytes([hi, lo]) + bits.to_bytes(6, 'little')

class Sheet:
    """The upright 8-bit picture of a BC4 sheet; write() puts the changed blocks back."""
    def __init__(self, f, flip):
        assert f.fmt == 12 and f.sheets == 1, f'sheet format {f.fmt} x{f.sheets}: only one BC4 sheet is supported'
        assert len(f.data) >= f.pixels + max(block_map(f)) + 8, 'the sheet runs past the font'
        self.f, self.flip = f, flip; self.addr = block_map(f); self.raw = f.data[f.pixels:f.pixels + max(self.addr) + 8]   # with the block-linear padding
        stored = bytearray(f.w * f.h); wb = f.w // 4
        for by in range(f.h // 4):
            for bx in range(wb):
                o = self.addr[by * wb + bx]; v = bc4_decode(self.raw[o:o + 8])
                for i in range(16): stored[(by * 4 + i // 4) * f.w + bx * 4 + i % 4] = v[i]
        self.px = bytearray(f.w * f.h)
        for y in range(f.h): s = stored_row(f, flip, y); self.px[y * f.w:(y + 1) * f.w] = stored[s * f.w:(s + 1) * f.w]
        self.old = bytes(self.px)
    def write(self):
        """The font's bytes with each changed block encoded again."""
        f = self.f; out = bytearray(f.data); stored = bytearray(f.w * f.h); old = bytearray(f.w * f.h); wb = f.w // 4
        for y in range(f.h):
            s = stored_row(f, self.flip, y); stored[s * f.w:(s + 1) * f.w] = self.px[y * f.w:(y + 1) * f.w]; old[s * f.w:(s + 1) * f.w] = self.old[y * f.w:(y + 1) * f.w]
        for by in range(f.h // 4):
            for bx in range(wb):
                idx = [(by * 4 + i // 4) * f.w + bx * 4 + i % 4 for i in range(16)]
                if any(stored[i] != old[i] for i in idx):
                    o = f.pixels + self.addr[by * wb + bx]; out[o:o + 8] = bc4_encode([stored[i] for i in idx])
        return bytes(out)

# ---- drawing a stand-in glyph into a cell ----
def outline(glyphset, name):
    """The glyph's contours as polygons in font units (curves flattened)."""
    from fontTools.pens.basePen import BasePen
    class Flat(BasePen):
        def __init__(self): super().__init__(glyphset); self.polys = []; self.cur = []
        def _moveTo(self, p): self.cur = [p]; self.polys.append(self.cur)
        def _lineTo(self, p): self.cur.append(p)
        def _curveToOne(self, p1, p2, p3):
            p0 = self.cur[-1]
            for t in (k / 8 for k in range(1, 9)):
                u = 1 - t; self.cur.append(tuple(u**3 * a + 3 * u * u * t * b + 3 * u * t * t * c + t**3 * d for a, b, c, d in zip(p0, p1, p2, p3)))
        def _qCurveToOne(self, p1, p2):
            p0 = self.cur[-1]
            for t in (k / 6 for k in range(1, 7)): u = 1 - t; self.cur.append(tuple(u * u * a + 2 * u * t * b + t * t * c for a, b, c in zip(p0, p1, p2)))
        def _closePath(self): pass
    pen = Flat(); glyphset[name].draw(pen); return [p for p in pen.polys if len(p) > 2]

def rasterize(polys, box, w, h, ss=4):
    """Coverage 0..255 of the polygons (nonzero winding) scaled into box = (x0, y0, x1, y1) of a w x h cell, y down; the glyph keeps its aspect and is centred in the box."""
    xs = [x for p in polys for x, _ in p]; ys = [y for p in polys for _, y in p]
    gx0, gx1, gy0, gy1 = min(xs), max(xs), min(ys), max(ys); bx0, by0, bx1, by1 = box
    s = min((bx1 - bx0) / (gx1 - gx0), (by1 - by0) / (gy1 - gy0)); ox = (bx0 + bx1) / 2 - (gx0 + gx1) / 2 * s; oy = (by0 + by1) / 2 + (gy0 + gy1) / 2 * s
    edges = []
    for p in polys:
        q = [(ox + x * s, oy - y * s) for x, y in p]
        for (xa, ya), (xb, yb) in zip(q, q[1:] + q[:1]):
            if ya != yb: edges.append((xa, ya, xb, yb, 1 if yb > ya else -1))
    cov = [0] * (w * h)
    for sy in range(h * ss):
        y = (sy + 0.5) / ss; xs_ = []
        for xa, ya, xb, yb, d in edges:
            if min(ya, yb) <= y < max(ya, yb): xs_.append((xa + (y - ya) * (xb - xa) / (yb - ya), d))
        xs_.sort(); wind = 0
        for k, (x, d) in enumerate(xs_[:-1]):
            wind += d
            if wind:
                for sx in range(max(0, math.ceil(x * ss - 0.5)), min(w * ss, math.ceil(xs_[k + 1][0] * ss - 0.5))): cov[(sy // ss) * w + sx // ss] += 1
    return [min(255, c * 255 // (ss * ss)) for c in cov]

def ink_box(px, sw, x, y, w, h, level=64):
    pts = [(i, j) for j in range(h) for i in range(w) if px[(y + j) * sw + x + i] >= level]
    if not pts: return None
    return min(i for i, _ in pts), min(j for _, j in pts), max(i for i, _ in pts) + 1, max(j for _, j in pts) + 1

def redraw(font_bytes, profile, glyphset):
    """The font with the profile's glyphs drawn from the stand-in. A glyph wider for its height than the icon it replaces (L1 for L) keeps the icon's height and takes more of
    its cell, and its glyph width and advance in CWDH grow by as much, so the text after it moves over."""
    f = Font(font_bytes); sheet = Sheet(f, profile['flip']); widths = {}
    for cp, name in profile['glyphs'].items():
        if cp not in f.cmap: raise ValueError(f'U+{cp:04X} is not in the font')
        g = f.cmap[cp]; x, y = f.cell(g); box = ink_box(sheet.px, f.w, x, y, f.cw, f.ch)
        if box is None: raise ValueError(f'U+{cp:04X} has an empty cell')
        polys = outline(glyphset, name); xs = [p[0] for q in polys for p in q]; ys = [p[1] for q in polys for p in q]
        x0, y0, x1, y1 = box; want = round((max(xs) - min(xs)) / (max(ys) - min(ys)) * (y1 - y0))
        if want > x1 - x0:
            nw = min(want, f.cw - 1); nx0 = min(x0, f.cw - 1 - nw); box = (nx0, y0, nx0 + nw, y1)
            k = f.widths(g); left, gw, adv = f.data[k:k + 3]; ngw = min(f.cw, max(gw, nx0 + nw + 1)); widths[k] = bytes([left, ngw, adv + ngw - gw])
        cov = rasterize(polys, box, f.cw, f.ch)
        for j in range(f.ch): sheet.px[(y + j) * f.w + x:(y + j) * f.w + x + f.cw] = bytes(cov[j * f.cw:(j + 1) * f.cw])
    out = bytearray(sheet.write())
    for k, v in widths.items(): out[k:k + 3] = v
    return bytes(out)

def make(rom, out, title_id, font_dir, styles):
    profile = PROFILES.get(title_id.upper())
    if not profile: print(f'{title_id}: no button-icon profile (prompts drawn with the shared font change without one)'); return 0
    from fontTools.ttLib import TTFont
    archives = sorted(p for p in romfs.Path(rom).glob(profile['archives']))
    if not archives: raise SystemExit(f'{profile["name"]}: no {profile["archives"]} in {rom}')
    sources = {}
    for a in archives:
        src = a.read_bytes(); sarc, groups = yaz0_ops(src); lo, hi = sarc_member(sarc, profile['member']); sources[a] = (src, sarc, groups, lo, hi)
    n = 0
    for style in styles:
        glyphset = TTFont(str(_Path(font_dir) / f'nintendo_ext_{style}.ttf')).getGlyphSet(); done = {}
        for a, (src, sarc, groups, lo, hi) in sources.items():
            member = sarc[lo:hi]
            if member not in done: done[member] = redraw(member, profile, glyphset)
            new = sarc[:lo] + done[member] + sarc[hi:]; assert len(new) == len(sarc)
            changed = [i for i in range(lo, hi) if new[i] != sarc[i]]; first, last = changed[0], changed[-1] + 1
            dst = _Path(out) / style / a.relative_to(romfs.Path(rom)); dst.parent.mkdir(parents=True, exist_ok=True)
            data = yaz0_splice(src, groups, new, first, last); assert yaz0_ops(data)[0] == new, 'Yaz0 round trip'
            dst.write_bytes(data); n += 1
    print(f'{profile["name"]}: {len(archives)} archives x {len(styles)} styles ({", ".join(styles)}) -> {out}')
    return n

# ---- survey ----
def png(path, px, w, h):
    raw = b''.join(b'\0' + bytes(px[y * w:(y + 1) * w]) for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    _Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

def survey(rom, out):
    """Each BFFNT whose code points include the private use area (U+E000-U+F8FF), loose or in a Yaz0 / plain SARC: its sheet as PNG, as stored and upside down, and its code points."""
    out = _Path(out); out.mkdir(parents=True, exist_ok=True); seen = set(); root = romfs.Path(rom)
    for p in sorted(root.rglob('*')):
        if not p.is_file() or not any(fnmatch.fnmatch(p.name, g) for g in ('*.bffnt', '*arc', '*.sarc', '*.szs', '*.pack', '*.sbfarc', '*.bfarc')): continue
        data = p.read_bytes()
        if data[:4] == b'Yaz0': data = yaz0_ops(data)[0]
        found = [(p.name, data)] if data[:4] == b'FFNT' else [(n, data[slice(*sarc_member(data, n))]) for n in sarc_names(data) if n.endswith('.bffnt')] if data[:4] == b'SARC' else []
        for name, fb in found:
            try: f = Font(fb)
            except AssertionError: continue
            pua = sorted(cp for cp in f.cmap if 0xE000 <= cp <= 0xF8FF)
            if not pua or fb in seen: continue
            seen.add(fb); tag = f'{p.relative_to(root).as_posix().replace("/", "_")}_{name}'
            lines = [f'# {p.relative_to(root)} -> {name}: {f.w}x{f.h} format {f.fmt}, cells {f.cw}x{f.ch}, {f.cols} a row\n', 'code point\tglyph\tcell x\tcell y\n']
            lines += [f'U+{cp:04X}\t{f.cmap[cp]}\t{f.cell(f.cmap[cp])[0]}\t{f.cell(f.cmap[cp])[1]}\n' for cp in pua]
            (out / f'{tag}.tsv').write_text(''.join(lines))
            if f.fmt == 12 and f.sheets == 1:
                for flip in (False, True): png(out / f'{tag}.{"flip" if flip else "stored"}.png', Sheet(f, flip).px, f.w, f.h)
            print(f'{p.relative_to(root)} -> {name}: {len(pua)} private-use code points (format {f.fmt})')

def main():
    ap = argparse.ArgumentParser(description='Button icons in a PlayStation or Xbox style for a game\'s own icon font (RomFS overlay folders)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('make'); m.add_argument('romfs'); m.add_argument('out'); m.add_argument('--title-id', required=True); m.add_argument('--font-dir', required=True)
    m.add_argument('--styles', default=','.join(STYLES))
    s = sub.add_parser('survey'); s.add_argument('romfs'); s.add_argument('out')
    a = ap.parse_args()
    if a.cmd == 'survey': survey(a.romfs, a.out); return
    styles = [x for x in a.styles.split(',') if x]
    bad = [x for x in styles if x not in STYLES]
    if bad: ap.error(f'unknown style {", ".join(bad)} (one of {", ".join(STYLES)})')
    make(a.romfs, a.out, a.title_id, a.font_dir, styles)

if __name__ == '__main__':
    main()
