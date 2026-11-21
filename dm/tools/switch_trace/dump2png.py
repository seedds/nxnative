#!/usr/bin/env python3
"""Turn a switch_trace dump capture into PNG images: dumpN.tsv and its dumpN_<n>.bin files, from the console's sd:/switch-trace folder.

usage: dump2png.py <folder with dumpN.tsv> [--capture N] [--out DIR] [--exposure 1.0]

Decodes the NVN formats render targets use (8-bit RGBA / BGRA / RGBX, sRGB or not, R8, RG8, RGBA16F, RG16F, R16F, RGBA32F, R32F, R11G11B10F, RGB10A2, and depth: DEPTH16,
DEPTH24, DEPTH24_STENCIL8, DEPTH32F, DEPTH32F_STENCIL8). Float colours are clamped to 0..1 after --exposure; depth is stretched from its minimum to its maximum. Other
formats, and buffer dumps, stay as .bin. Only the standard library: the PNGs are written with zlib."""
import argparse, csv, math, os, struct, sys, zlib

# NVN format -> (decoder, channels); from dm/gfx/nvn/include/nvn.h
U8 = {0x1: 1, 0xD: 2, 0x25: 4, 0x37: 4, 0x38: 4, 0x6D: 4}          # R8, RG8, RGBA8, RGBX8_SRGB, RGBA8_SRGB, RGBX8
BGR8 = {0x99: 4, 0x9A: 4, 0x9B: 4, 0x9C: 4}                         # BGRX8, BGRA8 and their sRGB forms
F16 = {0x5: 1, 0x11: 2, 0x29: 4, 0x71: 4}                           # R16F, RG16F, RGBA16F, RGBX16F
F32 = {0xA: 1, 0x16: 2, 0x2E: 4, 0x76: 4}                           # R32F, RG32F, RGBA32F, RGBX32F
DEPTH = {0x32: 'H', 0x33: 'I24', 0x34: 'f', 0x35: 'I24', 0x36: 'f8'}   # DEPTH16, DEPTH24, DEPTH32F, DEPTH24_STENCIL8, DEPTH32F_STENCIL8


def png(path, w, h, rgba):
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def to8(v, exposure=1.0):
    if v != v: return 0
    return max(0, min(255, int(round(min(1.0, max(0.0, v * exposure)) * 255))))


def r11g11b10(u):
    def f(m, e, mbits):
        if e == 0: return m / (1 << mbits) * 2 ** -14
        if e == 31: return 65504.0
        return (1 + m / (1 << mbits)) * 2 ** (e - 15)
    return f(u & 0x3f, (u >> 6) & 0x1f, 6), f((u >> 11) & 0x3f, (u >> 17) & 0x1f, 6), f((u >> 22) & 0x1f, (u >> 27) & 0x1f, 5)


def decode(fmt, w, h, data, exposure=1.0):
    """RGBA8 bytes for a w x h dump of NVN format fmt, or None when the format is not decoded"""
    n = w * h; out = bytearray(n * 4)
    if fmt in U8 or fmt in BGR8:
        c = U8.get(fmt) or BGR8[fmt]
        no_alpha = fmt in (0x37, 0x6D, 0x99, 0x9B)   # the X formats
        for i in range(n):
            px = data[i * c:(i + 1) * c]
            r = px[0]; g = px[1] if c > 1 else px[0]; b = px[2] if c > 2 else (0 if c == 2 else px[0]); a = px[3] if c == 4 and not no_alpha else 255
            if fmt in BGR8: r, b = b, r
            out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
        return bytes(out)
    if fmt in F16 or fmt in F32:
        c = F16.get(fmt) or F32[fmt]; sz = 2 if fmt in F16 else 4; code = 'e' if sz == 2 else 'f'
        vals = struct.unpack(f'<{n * c}{code}', data[:n * c * sz])
        for i in range(n):
            px = vals[i * c:(i + 1) * c]
            r, g, b = (px[0], px[1] if c > 1 else px[0], px[2] if c > 2 else (0.0 if c == 2 else px[0]))
            out[i * 4:i * 4 + 4] = bytes((to8(r, exposure), to8(g, exposure), to8(b, exposure), to8(px[3]) if c == 4 and fmt not in (0x71, 0x76) else 255))
        return bytes(out)
    if fmt == 0x3F or fmt == 0x3D:   # R11G11B10F, RGB10A2
        for i, (u,) in enumerate(struct.iter_unpack('<I', data[:n * 4])):
            if fmt == 0x3F: r, g, b = r11g11b10(u); px = (to8(r, exposure), to8(g, exposure), to8(b, exposure), 255)
            else: px = ((u & 0x3ff) * 255 // 1023, (u >> 10 & 0x3ff) * 255 // 1023, (u >> 20 & 0x3ff) * 255 // 1023, (u >> 30) * 85)
            out[i * 4:i * 4 + 4] = bytes(px)
        return bytes(out)
    if fmt in DEPTH:
        k = DEPTH[fmt]
        if k == 'H': vals = [v / 65535 for (v,) in struct.iter_unpack('<H', data[:n * 2])]
        elif k == 'I24': vals = [(v & 0xffffff) / 0xffffff for (v,) in struct.iter_unpack('<I', data[:n * 4])]
        elif k == 'f': vals = [v for (v,) in struct.iter_unpack('<f', data[:n * 4])]
        else: vals = [v for (v, _) in struct.iter_unpack('<fI', data[:n * 8])]
        fin = [v for v in vals if math.isfinite(v)]; lo, hi = (min(fin), max(fin)) if fin else (0.0, 1.0); span = (hi - lo) or 1.0
        for i, v in enumerate(vals):
            g = to8((v - lo) / span if math.isfinite(v) else 0.0); out[i * 4:i * 4 + 4] = bytes((g, g, g, 255))
        return bytes(out)
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('folder'); ap.add_argument('--capture', type=int, default=1); ap.add_argument('--out'); ap.add_argument('--exposure', type=float, default=1.0)
    a = ap.parse_args(argv)
    index = os.path.join(a.folder, f'dump{a.capture}.tsv')
    with open(index) as f:
        rows = [r for r in csv.DictReader((l for l in f if not l.startswith('#')), delimiter='\t')]
    out = a.out or a.folder; os.makedirs(out, exist_ok=True); done = 0
    for r in rows:
        w, h, fmt = int(r['w']), int(r['h']), int(r['nvn_format'])
        if r['kind'] == 'buffer' or not w or not h: continue
        data = open(os.path.join(a.folder, r['file']), 'rb').read()
        rgba = decode(fmt, w, h, data, a.exposure)
        if rgba is None: print(f"dump {r['n']}: NVN format {fmt:#x} is not decoded; left as {r['file']}"); continue
        name = os.path.join(out, f"dump{a.capture}_{r['n']}_f{r['frame']}_{r['kind']}_{w}x{h}.png"); png(name, w, h, rgba); done += 1
    print(f'{done} PNG files in {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
