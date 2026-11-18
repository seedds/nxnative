#!/usr/bin/env python3
# Button icons for a game whose prompts are pictures in its own textures (Pokémon Pokopia's UI sheet): edits each texture as the game uploads it and writes the host's replacement files
# (DM_TEX_REPLACE / DM_TEX_REPLACE_KBD, nvn_mtl.m: <width>x<height>_<NVN format>_<levels>_<hash>.bin, the upload's layout, here one BC7 level), one folder per set:
#   pad_label     an Xbox controller's labels, for pads mapped by label (DM_PAD_MAP=label: Xbox A is the Switch's A)
#   keyboard      the Mac keyboard's keys (front_mac.m: J K U I for A B X Y, Q E Z C for L R ZL ZR, F G for the stick presses, Return and Tab for + and −)
#   pad_position  the game's own textures unchanged, the set a controller shows when pads map by position, so that the host can go back to it from the keyboard's
# The Mac launcher hands the app's dm/textures/<set> folders to the host; it swaps between a controller's set and the keyboard's as the player changes input.
# The edits are the profile's button_icons.tsv (DM_GAME), one row per icon: the set, the texture's name (the stem the host logs), the edit, its place in level 0, its value:
#   face   x0,y0,x1,y1  A|B|X|Y   an Xbox face button: the disc inside the outline in the button's colour, its letter white
#   label  x0,y0,x1,y1  text      the marks inside the outline erased, the text drawn in the icon's dark ink (LB, RB, LT, RT for L, R, ZL, ZR; a key's letter)
#   menu / view / return  x0,y0,x1,y1   + and − as Xbox's Menu (three lines) and View (two windows); + as the Return key's arrow
#   key    x0,y0,x1,y1  text      a round icon redrawn as a keyboard key that fits inside its disc, the text on it
#   transpose  x0,y0,x1,y1        a face-button diagram mirrored across its diagonal: right <-> bottom, top <-> left, the Switch's A/B/X/Y places as an Xbox pad's mapped by label
#   disc   cx,cy,r  A|B|X|Y       a white disc with its letter cut out (a "Press A" in a title picture) in the button's colour, the letter white
#   disckey cx,cy,r  text         that disc as a keyboard key
#   paste  sx0,sy0,sx1,sy1,dx0,dy0,dx1,dy1  [flip]   the game's own pixels of the first box, resized into the second (mirrored left-right with flip)
#   clear  x0,y0,x1,y1            the box made transparent (the rest of that bubble, which another pane draws instead)
#   mirror x0,y0,x1,y1  cx        the box's columns right of cx become the mirror image of those left of it (Pokopia's prompt cloud: its right lobe, under the bubble's left end)
# Input: the game's own content of each texture, from a run with DM_TEX_REPLACE_LOG=<width>x<height> DM_TEX_REPLACE_DUMP=<folder> (<name>.raw); only the 4x4 blocks an edit changes are
# encoded again (BC7 mode 6), the rest stay the game's.
# Usage: DM_GAME=<id> button_icons.py <folder of .raw> <output folder> [--preview <folder>]; needs numpy and Pillow (the tools venv)
import sys
from pathlib import Path
import numpy as np   # pyright: ignore[reportMissingImports]  # in the tools venv (.github/requirements/tools.txt), not in the lint job's
from PIL import Image, ImageDraw, ImageFont   # pyright: ignore[reportMissingImports]  # the same

# ---- BC7 (mode 6) encoding
W = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64])

def _interp(e0, e1):   # (4,), (4,) 8-bit endpoints -> (16, 4) palette
    return ((64 - W)[:, None] * e0[None, :] + W[:, None] * e1[None, :] + 32) >> 6

def _quant(e):   # an 8-bit endpoint -> best (7-bit values, p-bit) and its 8-bit value
    best = None
    for p in (0, 1):
        v = np.clip(np.round((e - p) / 2.0), 0, 127).astype(int)
        q = (v << 1) | p
        err = ((q - e) ** 2).sum()
        if best is None or err < best[0]: best = (err, v, p, q)
    return best[1], best[2], best[3]

def encode_block(px):   # px: (16, 4) uint8 RGBA, row-major 4x4 -> 16 bytes
    px = px.astype(np.float64)
    mean = px.mean(0); c = px - mean
    cov = c.T @ c
    if np.allclose(cov, 0): axis = np.array([1.0, 1, 1, 1]) / 2
    else:
        w, v = np.linalg.eigh(cov); axis = v[:, -1]
    t = c @ axis
    e0 = np.clip(mean + axis * t.min(), 0, 255); e1 = np.clip(mean + axis * t.max(), 0, 255)
    best = None
    starts = [(e0, e1)] + [(px[i], px[j]) for i in range(16) for j in range(i + 1, 16)]
    for s0, s1 in starts:
        v0, p0, q0 = _quant(s0); v1, p1, q1 = _quant(s1)
        d = ((px[:, None, :] - _interp(q0, q1)[None, :, :]) ** 2).sum(2); err = d.min(1).sum()
        if best is None or err < best[0]: best = (err, v0, p0, v1, p1, d.argmin(1)); e0, e1 = s0, s1
    for it in range(3):
        v0, p0, q0 = _quant(e0); v1, p1, q1 = _quant(e1)
        pal = _interp(q0, q1)
        d = ((px[:, None, :] - pal[None, :, :]) ** 2).sum(2)
        idx = d.argmin(1); err = d.min(1).sum()
        if best is None or err < best[0]: best = (err, v0, p0, v1, p1, idx.copy())
        # least-squares refinement of the endpoints for these indices
        wt = W[idx] / 64.0; a = 1 - wt
        A = np.array([[(a * a).sum(), (a * wt).sum()], [(a * wt).sum(), (wt * wt).sum()]])
        if abs(np.linalg.det(A)) < 1e-9: break
        B = np.stack([(a[:, None] * px).sum(0), (wt[:, None] * px).sum(0)])
        sol = np.linalg.solve(A, B); e0 = np.clip(sol[0], 0, 255); e1 = np.clip(sol[1], 0, 255)
    assert best is not None   # the first try always sets it
    _, v0, p0, v1, p1, idx = best
    if idx[0] >= 8: v0, v1, p0, p1, idx = v1, v0, p1, p0, 15 - idx   # the anchor index's top bit is implied 0
    bits = [(0b1000000, 7)]
    for ch in range(4): bits += [(int(v0[ch]), 7), (int(v1[ch]), 7)]
    bits += [(p0, 1), (p1, 1), (int(idx[0]), 3)] + [(int(i), 4) for i in idx[1:]]
    n = 0; pos = 0
    for val, nb in bits: n |= val << pos; pos += nb
    assert pos == 128
    return n.to_bytes(16, 'little')

def encode_region(rgba, blocks, orig):   # rgba: (H, W, 4) uint8; blocks: iterable of (bx, by); orig: bytearray of the level's BC7 blocks (row-major) updated in place
    bw = rgba.shape[1] // 4
    for bx, by in blocks:
        b = rgba[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4].reshape(16, 4)
        o = (by * bw + bx) * 16
        orig[o:o + 16] = encode_block(b)

# ---- the edits
FONT = '/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf'
DARK = (51, 51, 51, 191); WHITE = (255, 255, 255, 255)
XBOX = {'A': (16, 124, 16), 'B': (210, 40, 36), 'X': (24, 104, 214), 'Y': (214, 156, 0)}
SS = 4   # supersampling

def is_dark(p): return p[3] > 100 and max(p[:3]) < 140
def components(mask):
    h, w = mask.shape; lab = np.zeros((h, w), int); n = 0; out = []
    for y in range(h):
        for x in range(w):
            if mask[y, x] and not lab[y, x]:
                n += 1; st = [(y, x)]; lab[y, x] = n; pts = []
                while st:
                    cy, cx = st.pop(); pts.append((cy, cx))
                    for ny, nx in ((cy+1, cx), (cy-1, cx), (cy, cx+1), (cy, cx-1)):
                        if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not lab[ny, nx]: lab[ny, nx] = n; st.append((ny, nx))
                out.append(pts)
    return out
def dilate(m, r):
    o = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1): o |= np.roll(np.roll(m, dy, 0), dx, 1)
    return o

def erase_inner(sp):   # sp: (h, w, 4) array of a sprite: paints the dark marks inside its outline white; returns the interior mask
    h, w = sp.shape[:2]
    dark = np.array([[is_dark(sp[y, x]) for x in range(w)] for y in range(h)])
    comps = components(dark)
    outline = max(comps, key=len)
    inner = np.zeros((h, w), bool)
    for c in comps:
        if c is outline: continue
        for y, x in c: inner[y, x] = True
    om = np.zeros((h, w), bool)
    for y, x in outline: om[y, x] = True
    # the interior: opaque light pixels plus the marks, not the outline
    light = (sp[:, :, 3] > 200) & (sp[:, :, :3].min(2) > 140)
    interior = (light | dilate(inner, 2)) & ~dilate(om, 1)
    paint = dilate(inner, 2) & ~om & (sp[:, :, 3] > 0)
    # keep the outline's own anti-aliased edge: only pixels farther than 1 from the outline
    sp[paint & ~dilate(om, 1)] = WHITE
    return interior, om

def text_layer(size, text, box, h_px, colour, max_w=None):   # an RGBA layer of size with text centred in box, cap height h_px (smaller if it is wider than max_w)
    W, H = size; L = Image.new('RGBA', (W * SS, H * SS), (0, 0, 0, 0)); d = ImageDraw.Draw(L)
    f = ImageFont.truetype(FONT, 100); bb = d.textbbox((0, 0), 'H', font=f); cap = bb[3] - bb[1]
    f = ImageFont.truetype(FONT, int(100 * h_px * SS / cap))
    bb = d.textbbox((0, 0), text, font=f); tw, th = bb[2] - bb[0], bb[3] - bb[1]
    if max_w and tw > max_w * SS: f = ImageFont.truetype(FONT, int(100 * h_px * SS / cap * max_w * SS / tw)); bb = d.textbbox((0, 0), text, font=f); tw, th = bb[2] - bb[0], bb[3] - bb[1]
    cx, cy = (box[0] + box[2]) / 2 * SS, (box[1] + box[3]) / 2 * SS
    d.text((cx - tw / 2 - bb[0], cy - th / 2 - bb[1]), text, font=f, fill=colour)
    return L.resize((W, H), Image.Resampling.LANCZOS)

def shapes_layer(size, rects, colour):   # rects in sprite pixels (floats), drawn as rounded bars
    W, H = size; L = Image.new('RGBA', (W * SS, H * SS), (0, 0, 0, 0)); d = ImageDraw.Draw(L)
    for kind, r in rects:
        r = [v * SS for v in r]
        if kind == 'bar': d.rounded_rectangle(r, radius=min(r[2]-r[0], r[3]-r[1]) / 2, fill=colour)
        elif kind == 'box': d.rounded_rectangle(r, radius=SS * 1.5, outline=colour, width=int(SS * 2.6))
        elif kind == 'boxfill': d.rounded_rectangle(r, radius=SS * 1.5, fill=colour)
    return L.resize((W, H), Image.Resampling.LANCZOS)

def over(sp, layer, mask=None):
    base = Image.fromarray(sp); base.alpha_composite(layer); out = np.array(base)
    if mask is not None: out[~mask] = sp[~mask]
    return out

def mark_box(interior):   # the bounding box of the interior
    ys, xs = np.nonzero(interior); return (xs.min(), ys.min(), xs.max() + 1, ys.max() + 1)

def face(sp, letter):   # an Xbox face button: the disc in its colour, the letter white. Each pixel is a mix of the dark ink and white (the outline's anti-aliased edge too), so
    # it takes the same mix of the ink and the colour, which keeps the edge smooth
    interior, om = erase_inner(sp)
    col = np.array(XBOX[letter], float); ink = np.array(DARK[:3], float)
    rgb = sp[:, :, :3].astype(float); a = sp[:, :, 3]
    w = np.clip((rgb.min(2) - ink.min()) / (255 - ink.min()), 0, 1)[:, :, None]
    sp[:, :, :3] = np.where((a > 0)[:, :, None], np.round(ink + w * (col - ink)), rgb).astype(np.uint8)
    h, w_ = sp.shape[:2]
    box = mark_box(interior)
    return over(sp, text_layer((w_, h), letter, box, (box[3] - box[1]) * 0.52, WHITE), interior)

def relabel(sp, text):
    interior, om = erase_inner(sp)
    h, w = sp.shape[:2]; box = mark_box(interior)
    scale = 0.6 if len(text) == 1 else 0.5
    return over(sp, text_layer((w, h), text, box, (box[3] - box[1]) * scale, DARK, max_w=(box[2] - box[0]) * 0.72), interior)

def menu(sp):
    interior, om = erase_inner(sp); h, w = sp.shape[:2]; x0, y0, x1, y1 = mark_box(interior)
    cx, cy, s = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0)
    bars = [('bar', (cx - s * 0.27, cy + dy * s * 0.19 - s * 0.045, cx + s * 0.27, cy + dy * s * 0.19 + s * 0.045)) for dy in (-1, 0, 1)]
    return over(sp, shapes_layer((w, h), bars, DARK), interior)

def view(sp):
    interior, om = erase_inner(sp); h, w = sp.shape[:2]; x0, y0, x1, y1 = mark_box(interior)
    cx, cy, s = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0)
    a = s * 0.2
    back = ('box', (cx - a * 1.45, cy - a * 1.45, cx + a * 0.55, cy + a * 0.55))
    front_fill = ('boxfill', (cx - a * 0.55, cy - a * 0.55, cx + a * 1.45, cy + a * 1.45))
    front_clear = shapes_layer((w, h), [front_fill], WHITE)
    out = over(sp, shapes_layer((w, h), [back], DARK), interior)
    out = over(out, front_clear, interior)
    return over(out, shapes_layer((w, h), [('box', front_fill[1])], DARK), interior)

def disc_letter(img, cx, cy, r, colour):   # a white disc with its letter cut out (the title's "PRESS (A) BUTTON") -> a disc of colour with the letter white
    y0, y1, x0, x1 = int(cy - r - 2), int(cy + r + 3), int(cx - r - 2), int(cx + r + 3)
    for y in range(y0, y1):
        for x in range(x0, x1):
            d = ((x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2) ** 0.5
            if d > r + 1: continue
            p = img[y, x].astype(float); t = min(max((p[3] - 110) / 120, 0), 1)
            if d <= r - 1.5:   # inside: the cut-out letter (transparent) becomes white, the disc the colour
                img[y, x] = [round(colour[k] * t + 255 * (1 - t)) for k in range(3)] + [255]
            elif p[3] > 0: img[y, x] = list(colour) + [int(p[3])]   # the rim keeps its edge

def keycap(sp, text, fit=False):   # the sprite redrawn as a keyboard key: a white rounded square with the dark outline, the text dark. fit: the square inside the round
    # button's disc (its corners 0.62 of its side from the centre, so a side of 0.8 of the disc), which keeps it off what the disc sits in (Pokopia's prompt bubble); else the sprite's square
    h, w = sp.shape[:2]; m = 3
    if fit: m = (w - 0.8 * (w - 2 * m)) / 2
    L = Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0)); d = ImageDraw.Draw(L)
    box = [round(m * SS), round(m * SS), round((w - m) * SS), round((h - m) * SS)]
    d.rounded_rectangle(box, radius=(w - 2 * m) * SS * 0.22, fill=WHITE, outline=DARK, width=int((w - 2 * m) * SS * 0.075))
    L = L.resize((w, h), Image.Resampling.LANCZOS)
    out = np.zeros_like(sp); out = over(out, L)
    inner = ((w - 2 * m) * 0.62)
    scale = (0.46 if fit else 0.42) if len(text) == 1 else 0.3 if len(text) <= 3 else 0.22
    return over(out, text_layer((w, h), text, (m, m, w - m, h - m), (h - 2 * m) * scale, DARK, max_w=inner))

def ret_arrow(sp):   # + as the Return key: the marks erased, a down-and-left arrow drawn
    interior, om = erase_inner(sp); h, w = sp.shape[:2]; x0, y0, x1, y1 = mark_box(interior)
    cx, cy, s = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0)
    W = Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0)); d = ImageDraw.Draw(W); t = s * 0.09 * SS
    px = lambda x, y: (x * SS, y * SS)
    d.line([px(cx + s * 0.22, cy - s * 0.25), px(cx + s * 0.22, cy + s * 0.08), px(cx - s * 0.2, cy + s * 0.08)], fill=DARK, width=int(t), joint='curve')
    d.polygon([px(cx - s * 0.3, cy + s * 0.08), px(cx - s * 0.12, cy - s * 0.06), px(cx - s * 0.12, cy + s * 0.22)], fill=DARK)
    return over(sp, W.resize((w, h), Image.Resampling.LANCZOS), interior)

def disc_key(img, cx, cy, r, text):   # a title's disc as a keyboard key of the same size
    x0, y0, x1, y1 = int(cx - r - 2), int(cy - r - 2), int(cx + r + 3), int(cy + r + 3)
    sp = img[y0:y1, x0:x1].copy(); h, w = sp.shape[:2]
    keep = np.array([[((x + 0.5 - (cx - x0)) ** 2 + (y + 0.5 - (cy - y0)) ** 2) ** 0.5 > r + 1 for x in range(w)] for y in range(h)])
    k = keycap(np.zeros_like(sp), text)
    sp = np.where(keep[:, :, None], sp, 0).astype(np.uint8); sp = over(sp, Image.fromarray(k))
    img[y0:y1, x0:x1] = sp

def transpose_at(img, box):   # transposes the square around a sprite's centre (a face-button diagram: right <-> bottom, top <-> left)
    x0, y0, x1, y1 = (int(v) for v in box); cx, cy = (x0 + x1) // 2, (y0 + y1) // 2; r = (max(x1 - x0, y1 - y0) + 1) // 2
    sq = img[cy - r:cy + r, cx - r:cx + r].copy()
    img[cy - r:cy + r, cx - r:cx + r] = sq.transpose(1, 0, 2)



OPS = {'face': face, 'label': relabel, 'menu': lambda s, v: menu(s), 'view': lambda s, v: view(s), 'return': lambda s, v: ret_arrow(s),
       'key': lambda s, v: keycap(s, v, fit=True)}

def edit(raw, rows, preview=None):   # raw: the texture's content (BC7, level 0 first); rows: (op, numbers, value) -> the replacement's bytes
    name = raw.stem; w, h = map(int, name.split('_')[0].split('x'))
    data = raw.read_bytes(); level = bytearray(data[:w * h])   # BC7: one byte per texel
    img = np.array(Image.frombytes('RGBA', (w, h), bytes(level), 'bcn', 7)); src = img.copy()
    for op, nums, val in rows:
        if op == 'disc': cx, cy, r = nums; disc_letter(img, cx, cy, r, XBOX[val]); continue
        if op == 'disckey': cx, cy, r = nums; disc_key(img, cx, cy, r, val); continue
        if op == 'transpose': transpose_at(img, nums); continue
        if op == 'paste':
            sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1 = (int(v) for v in nums)
            part = Image.fromarray(src[sy0:sy1, sx0:sx1]).resize((dx1 - dx0, dy1 - dy0), Image.Resampling.LANCZOS)
            if val == 'flip': part = part.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
            img[dy0:dy1, dx0:dx1] = np.array(part)
            continue
        if op == 'clear':
            x0, y0, x1, y1 = (int(v) for v in nums); img[y0:y1, x0:x1] = 0
            continue
        if op == 'mirror':
            x0, y0, x1, y1 = (int(v) for v in nums); cx = float(val)
            for x in range(int(cx) + 1, x1): img[y0:y1, x] = img[y0:y1, int(round(2 * cx - x))]
            continue
        x0, y0, x1, y1 = (int(v) for v in nums); x0, y0, x1, y1 = x0 - 2, y0 - 2, x1 + 2, y1 + 2
        img[y0:y1, x0:x1] = OPS[op](img[y0:y1, x0:x1].copy(), val)
    blocks = sorted({(x // 4, y // 4) for y, x in zip(*np.nonzero((img != src).any(2)))})
    encode_region(img, blocks, level)
    if preview:
        out = Image.frombytes('RGBA', (w, h), bytes(level), 'bcn', 7); bg = Image.new('RGBA', (w, h), (60, 70, 140, 255)); bg.alpha_composite(out); bg.convert('RGB').save(preview)
    print(f'{name}: {len(rows)} edits, {len(blocks)} blocks encoded again')
    return bytes(level) + data[w * h:]

def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description='button icon sets for the host\'s texture replacement (DM_TEX_REPLACE)')
    ap.add_argument('dumps'); ap.add_argument('out'); ap.add_argument('--preview')
    a = ap.parse_args(argv)
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'games')); import game
    g = game.load(); table = g.table('button_icons.tsv')
    if not table.is_file(): raise SystemExit(f'{table}: the profile has no button icons to edit')
    by = {}
    for line in table.read_text(encoding='utf-8').splitlines():
        if not line.strip() or line.startswith('#'): continue
        c = line.split('\t'); by.setdefault((c[0], c[1]), []).append((c[2], [float(v) for v in c[3].split(',')], c[4] if len(c) > 4 else ''))
    names = sorted({n for _, n in by}); missing = [n for n in names if not (Path(a.dumps) / f'{n}.raw').is_file()]
    if missing: raise SystemExit(f'[RED] no dump of {", ".join(missing)} in {a.dumps}: run the game with DM_TEX_REPLACE_LOG and DM_TEX_REPLACE_DUMP where they are on screen (another build of the game has other textures)')
    out = Path(a.out)
    for n in names:   # pad_position: the game's own, to go back to from the keyboard's
        (out / 'pad_position').mkdir(parents=True, exist_ok=True); (out / 'pad_position' / f'{n}.bin').write_bytes((Path(a.dumps) / f'{n}.raw').read_bytes())
    for (st, n), rows in sorted(by.items()):
        (out / st).mkdir(parents=True, exist_ok=True)
        prev = Path(a.preview) / f'{st}_{n}.png' if a.preview else None
        if prev: prev.parent.mkdir(parents=True, exist_ok=True)
        print(st, end=': '); (out / st / f'{n}.bin').write_bytes(edit(Path(a.dumps) / f'{n}.raw', rows, prev))

if __name__ == '__main__': main(sys.argv[1:])
