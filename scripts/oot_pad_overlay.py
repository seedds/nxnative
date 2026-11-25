#!/usr/bin/env python3
"""Generate PlayStation (DualSense) and Xbox Series RomFS overlays for The Legend of Zelda: Ocarina of Time (0400486024DEC000).

Reads the player's own RomFS image or folder (never committed to this repository) and contributor controller diagrams in
dm/games/0400486024DEC000/, patches Font2/Scalable/External.Nin_Ounce_NVN2.bfotf.zs and UI/Layout2Archive/Common.Product.*.bntx.zs,
and writes overlay_playstation/ and overlay_xbox/ under <out_root>.

Usage: oot_pad_overlay.py <RomFS.img or dir> <out_root> [--cache-dir DIR]
"""
import argparse, hashlib, io, math, os, shutil, struct, subprocess, sys, tempfile, zlib
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(REPO / 'dm/tools'))
import romfs as rfs

from fontTools.cffLib.specializer import specializeProgram
from fontTools.pens.areaPen import AreaPen
from fontTools.pens.recordingPen import RecordingPen
from fontTools.pens.reverseContourPen import ReverseContourPen
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.ttLib import TTFont

BFOTF_PATH = 'Font2/Scalable/External.Nin_Ounce_NVN2.bfotf.zs'
ZSDIC_PATH = 'Pack/ZsDic.pack.zs'
BFOTF_KEY = bytes((0xA6, 0x01, 0x85, 0x02))
WANT_TEXTURES = {
    'ActionGuidePlus_00',
    'KeyTexFontA_00', 'KeyTexFontB_00', 'KeyTexFontX_00', 'KeyTexFontY_00',
    'OcarinaButtonA_00', 'OcarinaButtonB_00', 'OcarinaButtonX_00', 'OcarinaButtonY_00',
    'OcarinaButtonL_00', 'OcarinaButtonR_00',
    *(f'Controller_{fam}' for fam in ('Pro', 'Grip', 'Console')),
    *(f'Controller_{fam}1' for fam in ('Pro', 'Grip', 'Console')),
    *(f'ControllerSh_{fam}1' for fam in ('Pro', 'Grip', 'Console')),
    *(f'ControllerGuide_{fam}_{i:02d}' for fam in ('Pro', 'Grip', 'Console') for i in range(5)),
}


class ZstdHelper:
    """Decompress/compress zstd payloads with an optional dictionary across Python 3.14, backports.zstd, zstandard, or zstd CLI."""

    def __init__(self, work: Path):
        self._work = work
        self._dict_file: Path | None = None
        self._mod: Any = None
        try:
            from compression import zstd  # type: ignore[import-not-found]
            self._kind, self._mod = 'stdlib', zstd
        except ImportError:
            try:
                from backports import zstd  # type: ignore[import-not-found]
                self._kind, self._mod = 'stdlib', zstd
            except ImportError:
                try:
                    import zstandard  # type: ignore[import-not-found]
                    self._kind, self._mod = 'zstandard', zstandard
                except ImportError:
                    exe = shutil.which('zstd')
                    if not exe:
                        sys.exit('[RED] oot_pad_overlay: need Python 3.14+ compression.zstd, zstandard, or the zstd CLI')
                    self._kind, self._mod = 'cli', exe

    def _dict_path(self, zsdic: bytes) -> Path:
        if self._dict_file is None:
            self._dict_file = self._work / 'zs.zsdic'
            self._dict_file.write_bytes(zsdic)
        return self._dict_file

    def decompress(self, data: bytes, zsdic: bytes | None = None, max_size: int = 256 << 20) -> bytes:
        if self._kind == 'stdlib':
            kw = {'zstd_dict': self._mod.ZstdDict(zsdic)} if zsdic else {}
            return self._mod.decompress(data, **kw)
        if self._kind == 'zstandard':
            dd = self._mod.ZstdCompressionDict(zsdic) if zsdic else None
            return self._mod.ZstdDecompressor(dict_data=dd).decompress(data, max_output_size=max_size)
        cmd = [self._mod, '-dc', '-q'] + (['-D', str(self._dict_path(zsdic))] if zsdic else [])
        return subprocess.run(cmd, input=data, capture_output=True, check=True).stdout

    def compress(self, data: bytes, zsdic: bytes | None = None, level: int = 19) -> bytes:
        if self._kind == 'stdlib':
            kw = {'zstd_dict': self._mod.ZstdDict(zsdic)} if zsdic else {}
            return self._mod.compress(data, level=level, **kw)
        if self._kind == 'zstandard':
            dd = self._mod.ZstdCompressionDict(zsdic) if zsdic else None
            return self._mod.ZstdCompressor(level=level, dict_data=dd).compress(data)
        cmd = [self._mod, f'-{level}', '-c', '-q'] + (['-D', str(self._dict_path(zsdic))] if zsdic else [])
        return subprocess.run(cmd, input=data, capture_output=True, check=True).stdout


def parse_sarc(data: bytes) -> dict[str, bytes]:
    _, _, _, data_off = struct.unpack_from('<HHII', data, 4)
    _, sfat_sz, n_nodes, _ = struct.unpack_from('<4sHHI', data, 0x14)
    pos = 0x14 + sfat_sz
    str_base = pos + n_nodes * 16 + 8
    out: dict[str, bytes] = {}
    for i in range(n_nodes):
        _, flags_noff, s_off, e_off = struct.unpack_from('<IIII', data, pos + i * 16)
        sp = str_base + (flags_noff & 0xFFFFFF) * 4
        ep = data.index(b'\0', sp)
        out[data[sp:ep].decode('utf-8', 'replace')] = data[data_off + s_off:data_off + e_off]
    return out


def find_romfs_file(romfs: Path, rel_dir: str, prefix: str, suffix: str) -> str:
    d = rfs.Path(f'{romfs}/{rel_dir}')
    if not d.is_dir():
        sys.exit(f'[RED] oot_pad_overlay: missing {rel_dir} in {romfs}')
    matches = sorted(p.name for p in d.iterdir() if p.name.startswith(prefix) and p.name.endswith(suffix))
    if not matches:
        sys.exit(f'[RED] oot_pad_overlay: no {prefix}*{suffix} in {romfs}/{rel_dir}')
    return f'{rel_dir}/{matches[-1]}'


def rstb_limit_for(romfs: Path, zs: ZstdHelper, rel_plain: str, default: int) -> int:
    try:
        rstb_rel = find_romfs_file(romfs, 'System/Resource', 'ResourceSizeTable.', '.rsizetable.zs')
    except SystemExit:
        return default
    raw = zs.decompress(rfs.open(f'{romfs}/{rstb_rel}', 'rb').read())
    if raw[:4] != b'REST':
        return default
    key = struct.pack('<I', zlib.crc32(rel_plain.encode()))
    idx = raw.find(key)
    if idx < 0:
        return default
    val = struct.unpack_from('<I', raw, idx + 4)[0]
    return val if 1024 < val < (1 << 20) else default


# ---- BFOTF scalable shared font patching ----

def _split_contours(glyph) -> list[list[tuple[str, tuple]]]:
    rec = RecordingPen()
    glyph.draw(rec)
    contours: list[list[tuple[str, tuple]]] = []
    cur: list[tuple[str, tuple]] = []
    for op, args in rec.value:
        cur.append((op, args))
        if op == 'closePath':
            contours.append(cur)
            cur = []
    return contours


def _replay_ops(ops, pen) -> None:
    for op, args in ops:
        getattr(pen, op)(*args)


def _contour_area(ops) -> float:
    ap = AreaPen()
    _replay_ops(ops, ap)
    return float(ap.value)


def _transform_ops(ops, sx: float, sy: float, dx: float, dy: float):
    return [(op, tuple((x * sx + dx, y * sy + dy) for x, y in args)) for op, args in ops]


def _ensure_winding(ops, ccw: bool = True):
    if (_contour_area(ops) > 0) == ccw:
        return ops
    rec = RecordingPen()
    _replay_ops(ops, ReverseContourPen(rec))
    return rec.value


def _circle_ops(cx: float, cy: float, r: float, ccw: bool = True):
    k = 0.5522847498 * r
    pts_ccw = [
        ('moveTo', ((cx, cy + r),)),
        ('curveTo', ((cx - k, cy + r), (cx - r, cy + k), (cx - r, cy))),
        ('curveTo', ((cx - r, cy - k), (cx - k, cy - r), (cx, cy - r))),
        ('curveTo', ((cx + k, cy - r), (cx + r, cy - k), (cx + r, cy))),
        ('curveTo', ((cx + r, cy + k), (cx + k, cy + r), (cx, cy + r))),
        ('closePath', ()),
    ]
    return _ensure_winding(pts_ccw, ccw)


def _poly_ops(pts, ccw: bool = True):
    ops = [('moveTo', (pts[0],))] + [('lineTo', (p,)) for p in pts[1:]] + [('closePath', ())]
    return _ensure_winding(ops, ccw)


def _rrect_ops(x0: float, y0: float, x1: float, y1: float, r: float, ccw: bool = True):
    r = min(r, (x1 - x0) / 2.0, (y1 - y0) / 2.0)
    if r <= 0:
        return _poly_ops([(x0, y1), (x0, y0), (x1, y0), (x1, y1)], ccw)
    k = 0.5522847498 * r
    ops = [
        ('moveTo', ((x0 + r, y1),)),
        ('curveTo', ((x0 + r - k, y1), (x0, y1 - r + k), (x0, y1 - r))),
        ('lineTo', ((x0, y0 + r),)),
        ('curveTo', ((x0, y0 + r - k), (x0 + r - k, y0), (x0 + r, y0))),
        ('lineTo', ((x1 - r, y0),)),
        ('curveTo', ((x1 - r + k, y0), (x1, y0 + r - k), (x1, y0 + r))),
        ('lineTo', ((x1, y1 - r),)),
        ('curveTo', ((x1, y1 - r + k), (x1 - r + k, y1), (x1 - r, y1))),
        ('closePath', ()),
    ]
    return _ensure_winding(ops, ccw)


def _rounded_polygon_ops(verts, rad: float, ccw: bool = True):
    n = len(verts)
    pts_in = []
    for i in range(n):
        p_prev, p_cur, p_next = verts[(i - 1) % n], verts[i], verts[(i + 1) % n]
        v1 = (p_prev[0] - p_cur[0], p_prev[1] - p_cur[1])
        v2 = (p_next[0] - p_cur[0], p_next[1] - p_cur[1])
        l1, l2 = math.hypot(*v1), math.hypot(*v2)
        u1, u2 = (v1[0] / l1, v1[1] / l1), (v2[0] / l2, v2[1] / l2)
        dot = max(-1.0, min(1.0, u1[0] * u2[0] + u1[1] * u2[1]))
        half_ang = math.acos(dot) / 2.0
        d = min(rad / math.tan(half_ang), l1 * 0.45, l2 * 0.45)
        r_eff = d * math.tan(half_ang)
        a = (p_cur[0] + u1[0] * d, p_cur[1] + u1[1] * d)
        b = (p_cur[0] + u2[0] * d, p_cur[1] + u2[1] * d)
        arc_ang = math.pi - 2 * half_ang
        kappa = (4.0 / 3.0) * math.tan(arc_ang / 4.0) * r_eff
        pts_in.append((a, (a[0] - u1[0] * kappa, a[1] - u1[1] * kappa), (b[0] - u2[0] * kappa, b[1] - u2[1] * kappa), b))
    ops: list[tuple[str, tuple[Any, ...]]] = [('moveTo', (pts_in[0][0],))]
    for i in range(n):
        a, c1, c2, b = pts_in[i]
        if i > 0:
            ops.append(('lineTo', (a,)))
        ops.append(('curveTo', (c1, c2, b)))
    ops.append(('closePath', ()))
    return _ensure_winding(ops, ccw)


def _ps_cross_ops(cx: float, cy: float, half: float, arm: float):
    a = arm / 2.0
    ang = math.radians(45)
    ca, sa = math.cos(ang), math.sin(ang)
    b = 9.0
    raw = [
        (-a + b, half), (-a, half - b), (-a, a), (-half + b, a), (-half, a - b),
        (-half, -a + b), (-half + b, -a), (-a, -a), (-a, -half + b), (-a + b, -half),
        (a - b, -half), (a, -half + b), (a, -a), (half - b, -a), (half, -a + b),
        (half, a - b), (half - b, a), (a, a), (a, half - b), (a - b, half),
    ]
    return _poly_ops([(cx + x * ca - y * sa, cy + x * sa + y * ca) for x, y in raw], ccw=False)


def _char_contours(orig_gs, ch: str, x0: float, y0: float, scale: float):
    out = []
    if ch == 'L':
        c = _transform_ops(_split_contours(orig_gs['uniE044'])[0], 1.0, 1.0, -340.0, -172.0)
        out.append(_ensure_winding(c, ccw=False))
        adv = 148
    elif ch == 'R':
        c_in = _transform_ops(_split_contours(orig_gs['uniE045'])[0], 1.0, 1.0, -274.0, -169.0)
        c_out = _transform_ops(_split_contours(orig_gs['uniE045'])[1], 1.0, 1.0, -274.0, -169.0)
        out.append(_ensure_winding(c_out, ccw=False))
        out.append(_ensure_winding(c_in, ccw=True))
        adv = 192
    elif ch == '1':
        out.append(_poly_ops([(46, 250), (88, 250), (88, 0), (44, 0), (44, 200), (6, 174), (6, 214)], ccw=False))
        adv = 112
    elif ch == '2':
        ops = [
            ('moveTo', ((14, 180),)),
            ('curveTo', ((16, 226), (48, 252), (88, 252))),
            ('curveTo', ((132, 252), (160, 224), (160, 182))),
            ('curveTo', ((160, 146), (138, 122), (80, 70))),
            ('lineTo', ((56, 48),)), ('lineTo', ((56, 40),)), ('lineTo', ((162, 40),)),
            ('lineTo', ((162, 0),)), ('lineTo', ((6, 0),)), ('lineTo', ((6, 36),)),
            ('lineTo', ((68, 94),)),
            ('curveTo', ((104, 128), (116, 146), (116, 176))),
            ('curveTo', ((116, 198), (104, 212), (84, 212))),
            ('curveTo', ((64, 212), (52, 196), (52, 172))),
            ('lineTo', ((14, 172),)), ('closePath', ()),
        ]
        out.append(_ensure_winding(ops, ccw=False))
        adv = 172
    elif ch == 'B':
        outer = [
            ('moveTo', ((0, 250),)), ('lineTo', ((98, 250),)),
            ('curveTo', ((146, 250), (168, 224), (168, 186))),
            ('curveTo', ((168, 158), (154, 140), (130, 132))),
            ('curveTo', ((158, 122), (174, 100), (174, 68))),
            ('curveTo', ((174, 24), (148, 0), (96, 0))),
            ('lineTo', ((0, 0),)), ('closePath', ()),
        ]
        top_hole = [
            ('moveTo', ((44, 212),)), ('lineTo', ((44, 148),)), ('lineTo', ((92, 148),)),
            ('curveTo', ((116, 148), (126, 160), (126, 180))),
            ('curveTo', ((126, 200), (116, 212), (92, 212))), ('closePath', ()),
        ]
        bot_hole = [
            ('moveTo', ((44, 112),)), ('lineTo', ((44, 38),)), ('lineTo', ((94, 38),)),
            ('curveTo', ((120, 38), (132, 50), (132, 74))),
            ('curveTo', ((132, 98), (120, 112), (94, 112))), ('closePath', ()),
        ]
        out.extend((_ensure_winding(outer, ccw=False), _ensure_winding(top_hole, ccw=True), _ensure_winding(bot_hole, ccw=True)))
        adv = 186
    elif ch == 'T':
        out.append(_poly_ops([(0, 250), (166, 250), (166, 210), (105, 210), (105, 0), (61, 0), (61, 210), (0, 210)], ccw=False))
        adv = 166
    else:
        raise ValueError(ch)
    return [_transform_ops(c, scale, scale, x0, y0) for c in out], adv * scale


def _label_ops(orig_gs, text: str, cx: float, cy: float, scale: float):
    cur_x = 0.0
    items = []
    for i, ch in enumerate(text):
        cs, adv = _char_contours(orig_gs, ch, cur_x, 0.0, scale)
        items.extend(cs)
        cur_x += adv - (8.0 * scale if i + 1 < len(text) else 0.0)
    xs = [p[0] for c in items for _, args in c for p in args]
    ys = [p[1] for c in items for _, args in c for p in args]
    dx, dy = cx - (min(xs) + max(xs)) / 2.0, cy - (min(ys) + max(ys)) / 2.0
    all_c = []
    for c in items:
        all_c.extend(_transform_ops(c, 1.0, 1.0, dx, dy))
    return all_c


def _set_glyph_ops(otf: TTFont, glyph_name: str, ops) -> None:
    cff_tbl: object = otf['CFF ']
    cff = cff_tbl.cff  # type: ignore[attr-defined]
    top = cff.topDictIndex[0]
    hmtx_tbl: object = otf['hmtx']
    width = hmtx_tbl[glyph_name][0]  # type: ignore[index]
    pen: Any = T2CharStringPen(width, None)
    rp = lambda pt: (int(round(pt[0])), int(round(pt[1])))
    for op, args in ops:
        if op == 'moveTo':
            pen.moveTo(rp(args[0]))
        elif op == 'lineTo':
            pen.lineTo(rp(args[0]))
        elif op == 'curveTo':
            pen.curveTo(*(rp(a) for a in args))
        elif op == 'closePath':
            pen.closePath()
    cs = pen.getCharString(private=top.Private, globalSubrs=cff.GlobalSubrs)
    cs.decompile()
    cs.program = specializeProgram(cs.program)
    top.CharStrings[glyph_name] = cs


def patch_bfotf(orig_otf_bytes: bytes, rstb_limit: int, layout: str) -> bytes:
    orig_otf = TTFont(io.BytesIO(orig_otf_bytes))
    orig_gs = orig_otf.getGlyphSet()
    otf = TTFont(io.BytesIO(orig_otf_bytes))
    gs = otf.getGlyphSet()

    if layout == 'playstation':
        outer_face = _ensure_winding(_split_contours(gs['uniE040'])[-1], ccw=True)
        cx, cy = 320.0, 301.0
        _set_glyph_ops(otf, 'uniE040', list(outer_face) + _ps_cross_ops(cx, cy, half=152, arm=68))
        _set_glyph_ops(otf, 'uniE041', list(outer_face) + _circle_ops(cx, cy, 152, ccw=False) + _circle_ops(cx, cy, 90, ccw=True))
        _set_glyph_ops(otf, 'uniE042', list(outer_face) + _rrect_ops(cx - 134, cy - 134, cx + 134, cy + 134, 24, ccw=False) + _rrect_ops(cx - 74, cy - 74, cx + 74, cy + 74, 8, ccw=True))
        ty = cy - 16.0
        tri_out = [(cx, ty + 162.0), (cx - 152.0, ty - 102.0), (cx + 152.0, ty - 102.0)]
        tri_in = [(cx, ty + 74.0), (cx - 70.0, ty - 48.0), (cx + 70.0, ty - 48.0)]
        _set_glyph_ops(otf, 'uniE043', list(outer_face) + _rounded_polygon_ops(tri_out, 16.0, ccw=False) + _rounded_polygon_ops(tri_in, 6.0, ccw=True))

    lbls = ('L1', 'R1', 'L2', 'R2') if layout == 'playstation' else ('LB', 'RB', 'LT', 'RT')
    for gname, lbl, cx, cy, sc in (
        ('uniE044', lbls[0], 396.0, 297.0, 0.92),
        ('uniE045', lbls[1], 344.0, 297.0, 0.92),
        ('uniE046', lbls[2], 378.0, 286.0, 0.86),
        ('uniE047', lbls[3], 342.0, 286.0, 0.86),
    ):
        plate = _ensure_winding(_split_contours(gs[gname])[-1], ccw=True)
        _set_glyph_ops(otf, gname, list(plate) + _label_ops(orig_gs, lbl, cx, cy, sc))

    circle_plus = _ensure_winding(_split_contours(gs['uniE04B'])[0], ccw=True)
    px, py = 300.0, 300.0
    ops_plus = list(circle_plus)
    for dy in (90.0, 0.0, -90.0):
        ops_plus.extend(_rrect_ops(px - 130.0, py + dy - 24.0, px + 130.0, py + dy + 24.0, 18.0, ccw=False))
    _set_glyph_ops(otf, 'uniE04B', ops_plus)

    ops_minus = list(circle_plus)
    ops_minus.extend(_rrect_ops(px - 44.0, py - 116.0, px + 126.0, py + 54.0, 14.0, ccw=False))
    ops_minus.extend(_rrect_ops(px - 10.0, py - 82.0, px + 92.0, py + 20.0, 4.0, ccw=True))
    ops_minus.extend(_poly_ops([
        (px - 126.0, py + 116.0), (px - 126.0, py - 44.0), (px - 92.0, py - 44.0),
        (px - 92.0, py + 82.0), (px + 44.0, py + 82.0), (px + 44.0, py + 116.0),
    ], ccw=False))
    _set_glyph_ops(otf, 'uniE04C', ops_minus)

    # Keep the encrypted BFOTF inside the game's ResourceSizeTable entry without rewriting RSTB.
    name_tbl: object = otf['name']
    name_tbl.names = [r for r in name_tbl.names if r.nameID in (1, 2, 4, 6) and r.platformID == 3]  # type: ignore[attr-defined]
    if 'DSIG' in otf:
        del otf['DSIG']
    buf = io.BytesIO()
    otf.save(buf)
    new_otf = buf.getvalue()
    hdr = b'\x7f\x9a\x02\x18' + struct.pack('>I', len(new_otf))
    plain = hdr + new_otf
    if len(plain) > rstb_limit:
        sys.exit(f'[RED] oot_pad_overlay: {layout} BFOTF size {len(plain)} exceeds RSTB limit {rstb_limit}')
    return bytes(b ^ BFOTF_KEY[i % 4] for i, b in enumerate(plain))


def extract_bntx_list(bntx: bytes) -> list[str]:
    if bntx[:4] != b'BNTX':
        sys.exit('[RED] oot_pad_overlay: Common BNTX missing BNTX header')
    cnt = struct.unpack_from('<I', bntx, 0x24)[0]
    ptrs_off = struct.unpack_from('<Q', bntx, 0x28)[0]
    rows: list[str] = []
    for i in range(cnt):
        o = struct.unpack_from('<Q', bntx, ptrs_off + i * 8)[0]
        noff = struct.unpack_from('<Q', bntx, o + 0x60)[0]
        slen = struct.unpack_from('<H', bntx, noff)[0]
        name = bntx[noff + 2:noff + 2 + slen].decode('utf-8', 'replace')
        if name not in WANT_TEXTURES:
            continue
        bfmt, _, w, h = struct.unpack_from('<IIII', bntx, o + 0x1c)
        layout = struct.unpack_from('<I', bntx, o + 0x34)[0]
        sz = struct.unpack_from('<I', bntx, o + 0x50)[0]
        s0, s1, s2, s3 = struct.unpack_from('<BBBB', bntx, o + 0x58)
        mip0 = struct.unpack_from('<Q', bntx, struct.unpack_from('<Q', bntx, o + 0x70)[0])[0]
        rows.append(f'{name} {w} {h} {bfmt:x} {layout} {sz} {mip0} {s0} {s1} {s2} {s3}\n')
    if len(rows) < len(WANT_TEXTURES):
        sys.exit(f'[RED] oot_pad_overlay: expected {len(WANT_TEXTURES)} textures in Common BNTX, found {len(rows)}')
    return rows


def compile_bntx_tool(cache_dir: Path) -> Path:
    srcs = [
        REPO / 'dm/tools/oot_pad_overlay.m',
        REPO / 'dm/gfx/tex/nvn_deswizzle.c',
        REPO / 'dm/gfx/nvn/nvn_tex.c',
    ]
    h = hashlib.sha256()
    for s in srcs + [REPO / 'dm/gfx/tex/nvn_deswizzle.h', REPO / 'dm/gfx/nvn/nvn_tex.h']:
        h.update(s.read_bytes())
    key = h.hexdigest()[:16]
    exe = cache_dir / 'oot_pad_overlay'
    stamp = cache_dir / 'oot_pad_overlay.key'
    if exe.is_file() and stamp.is_file() and stamp.read_text().strip() == key:
        return exe
    cache_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        'xcrun', 'clang', '-fobjc-arc', '-O2', '-I', str(REPO),
        '-framework', 'Foundation', '-framework', 'CoreGraphics', '-framework', 'CoreText',
        '-framework', 'ImageIO', '-framework', 'Metal',
        *(str(s) for s in srcs), '-o', str(exe),
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'[RED] oot_pad_overlay: clang failed ({r.returncode}): {(r.stderr or r.stdout).strip()[-1500:]}')
    stamp.write_text(f'{key}\n')
    return exe


def build(romfs: Path, out_root: Path, cache_dir: Path | None) -> None:
    game_dir = REPO / 'dm/games/0400486024DEC000'
    ps_art = game_dir / 'controller_dualsense.png'
    xb_art = game_dir / 'controller_xbox.png'
    for p in (ps_art, xb_art):
        if not p.is_file():
            sys.exit(f'[RED] oot_pad_overlay: missing contributor artwork {p}')

    with tempfile.TemporaryDirectory() as tmp_str:
        tmp = Path(tmp_str)
        zs = ZstdHelper(tmp)
        sarc = zs.decompress(rfs.open(f'{romfs}/{ZSDIC_PATH}', 'rb').read(), max_size=32 << 20)
        zsdic = parse_sarc(sarc)['zs.zsdic']

        orig_enc = zs.decompress(rfs.open(f'{romfs}/{BFOTF_PATH}', 'rb').read(), zsdic=zsdic, max_size=4 << 20)
        orig_dec = bytes(b ^ BFOTF_KEY[i % 4] for i, b in enumerate(orig_enc))
        if orig_dec[:4] != b'\x7f\x9a\x02\x18':
            sys.exit('[RED] oot_pad_overlay: unexpected External.bfotf header')
        rstb_limit = rstb_limit_for(romfs, zs, BFOTF_PATH[:-3], max(14856, len(orig_enc)))
        for layout in ('playstation', 'xbox'):
            enc = patch_bfotf(orig_dec[8:], rstb_limit, layout)
            out_f = out_root / f'overlay_{layout}' / BFOTF_PATH
            out_f.parent.mkdir(parents=True, exist_ok=True)
            out_f.write_bytes(zs.compress(enc, zsdic=zsdic, level=19))

        bntx_rel = find_romfs_file(romfs, 'UI/Layout2Archive', 'Common.Product.', '.bntx.zs')
        bntx_raw = zs.decompress(rfs.open(f'{romfs}/{bntx_rel}', 'rb').read(), zsdic=zsdic, max_size=512 << 20)
        orig_bntx_path = tmp / 'common_orig.bntx'
        list_path = tmp / 'list.tsv'
        ps_bntx_path = tmp / 'common_playstation.bntx'
        xb_bntx_path = tmp / 'common_xbox.bntx'
        orig_bntx_path.write_bytes(bntx_raw)
        list_path.write_text(''.join(extract_bntx_list(bntx_raw)))

        exe = compile_bntx_tool(cache_dir or tmp)
        r = subprocess.run([str(exe), str(orig_bntx_path), str(list_path), str(ps_art), str(xb_art),
                            str(ps_bntx_path), str(xb_bntx_path)], capture_output=True, text=True)
        if r.returncode:
            sys.exit(f'[RED] oot_pad_overlay: texture patch failed ({r.returncode}): {(r.stderr or r.stdout).strip()[-1500:]}')

        for layout, path in (('playstation', ps_bntx_path), ('xbox', xb_bntx_path)):
            out_b = out_root / f'overlay_{layout}' / bntx_rel
            out_b.parent.mkdir(parents=True, exist_ok=True)
            out_b.write_bytes(zs.compress(path.read_bytes(), zsdic=zsdic, level=19))

    print(f'oot_pad_overlay: wrote overlay_playstation and overlay_xbox in {out_root} ({len(WANT_TEXTURES)} textures + External.bfotf)')


def main(argv: list[str] | None = None) -> None:
    ap = argparse.ArgumentParser(description='Generate PlayStation and Xbox RomFS icon overlays for Ocarina of Time')
    ap.add_argument('romfs', help='RomFS image or extracted folder')
    ap.add_argument('out_root', help='output folder receiving overlay_playstation/ and overlay_xbox/')
    ap.add_argument('--cache-dir', help='directory caching the compiled oot_pad_overlay binary')
    a = ap.parse_args(argv)
    build(Path(os.path.expanduser(a.romfs)).resolve(), Path(os.path.expanduser(a.out_root)).resolve(),
          Path(os.path.expanduser(a.cache_dir)).resolve() if a.cache_dir else None)


if __name__ == '__main__':
    main()
