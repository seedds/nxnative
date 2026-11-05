#!/usr/bin/env python3
"""A gap opened in the middle of a module's text, for a patch area there: patch_nso.py's middle area (MID), for a text so long that the front and back patch areas leave sites
between them that no branch reaches (Core Keeper's main: 302 MB of text, ro, data and bss after it, so the back area reaches no site below 272 MB and the front none above 111 MB).

The gap opens at a function boundary F: every byte from F on moves G bytes up (G a multiple of 16 KB, so page offsets and the host's 16 KB pages stay as they were), and the
gap [F, F + G) becomes part of the text. f(a) = a for a < F, a + G from F on. What refers to an address across F is fixed so it means the same address after the move:
  · text: B / BL whose target is on the other side of F get the new displacement; one that no longer reaches (|displacement| + G past 128 MB) goes through a veneer in the gap
    (a single B: veneers at the gap's start for targets above it that still reach from there, at its end otherwise). ADRP to a page on the other side gets the page difference.
    B.cond, CBZ / CBNZ, TBZ / TBNZ, ADR and literal loads across F rule that F out (choose_split picks another), as does an ADRP to the page holding F, whose target could be
    on either side of it. The words of text data blocks (nso_code.data_blocks) and the MOD0 offset at +4 are not instructions and are left alone.
  · the image's MOD0 offset (+4) and the MOD0 fields (offsets from MOD0: dynamic, bss, eh_frame_hdr, module object);
  · .dynamic: the tags holding addresses (DT_RELA, DT_SYMTAB, DT_INIT_ARRAY, ...);
  · relocations: RELA / JMPREL offsets, the addends of R_AARCH64_RELATIVE (and of ABS64 without a symbol); RELR entries and the implicit addends at the places they relocate;
  · the dynamic symbol table's defined symbol values;
  · .eh_frame_hdr's eh_frame pointer and search table, and in .eh_frame each FDE's pc begin and LSDA pointer and each CIE's personality pointer (PC-relative ones; an absolute one
    is a relocation's).
Anything else in the module that holds an address relative to another place across F would be wrong after the move. Clang's jump tables are relative to a label in their own
function, which F never splits, and pointers in a position-independent module are relocations; a module with something of another kind (a pointer encoding, a relocation type,
a dynamic tag this script does not know) raises Error, never a silently wrong image.
Usage on its own: nso_shift.py <module.nso> <F> <G> (checks the shifted image against the original: relocations applied to both, function ranges, sample branches)"""
import struct, sys
import nso_code

PAGE, GRAIN = 0x1000, 0x4000
REACH = 0x8000000   # ±128 MB of a B / BL
DT_PTR = {3, 4, 5, 6, 7, 12, 13, 17, 21, 23, 25, 26, 32, 36, 0x6ffffef5, 0x6ffffff0, 0x6ffffffc, 0x6ffffffe}   # PLTGOT HASH STRTAB SYMTAB RELA INIT FINI REL DEBUG JMPREL INIT_ARRAY FINI_ARRAY PREINIT_ARRAY RELR GNU_HASH VERSYM VERDEF VERNEED
DT_VAL = {1, 2, 8, 9, 10, 11, 14, 15, 16, 18, 19, 20, 22, 24, 27, 28, 29, 30, 33, 35, 37, 0x6ffffff9, 0x6ffffffa, 0x6ffffffb, 0x6ffffffd, 0x6fffffff}
R_ABS64, R_GLOB_DAT, R_JUMP_SLOT, R_RELATIVE = 257, 1025, 1026, 1027
R_SYMBOLIC = {R_GLOB_DAT, R_JUMP_SLOT, 1028, 1029, 1030, 1031}   # GLOB_DAT JUMP_SLOT TLS_DTPMOD TLS_DTPREL TLS_TPREL TLSDESC: the addend is no module address


class Error(ValueError):
    pass


def sx(v, bits): return v - (1 << bits) if v >> (bits - 1) & 1 else v


def decode(w, pc):
    """(kind, target) of a PC-relative instruction word at pc, or None. kind: 'b' (B / BL, imm26), 'adrp' (target is the page), 'short' (any other: imm19 / imm14 / ADR)"""
    if w & 0x7C000000 == 0x14000000: return 'b', pc + sx(w & 0x3FFFFFF, 26) * 4
    if w & 0x9F000000 == 0x90000000: return 'adrp', (pc & ~0xFFF) + sx((w >> 5 & 0x7FFFF) << 2 | w >> 29 & 3, 21) * PAGE
    if w & 0x9F000000 == 0x10000000: return 'short', pc + sx((w >> 5 & 0x7FFFF) << 2 | w >> 29 & 3, 21)
    if w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000 or w & 0x3B000000 == 0x18000000: return 'short', pc + sx(w >> 5 & 0x7FFFF, 19) * 4
    if w & 0x7E000000 == 0x36000000: return 'short', pc + sx(w >> 5 & 0x3FFF, 14) * 4
    return None


def b_word(w, pc, to):
    d = to - pc
    if d % 4 or not -REACH <= d < REACH: raise Error(f'branch at {pc:#x} to {to:#x} out of reach')
    return w & 0xFC000000 | (d >> 2) & 0x3FFFFFF


def adrp_word(w, pc, page):
    d = (page - (pc & ~0xFFF)) >> 12
    if not -(1 << 20) <= d < 1 << 20: raise Error(f'adrp at {pc:#x} to {page:#x} out of reach')
    return w & 0x9F00001F | (d & 3) << 29 | (d >> 2 & 0x7FFFF) << 5


def code_ranges(img, text_end, md):
    """[(lo, hi)] of the text words read as instructions: all but the data blocks and the header words (+4: the MOD0 offset)"""
    blocks = nso_code.data_blocks(img, text_end, md)
    out, at = [], 8
    for s, e in blocks + [(text_end, text_end)]:
        if s > at: out.append((at, s))
        at = max(at, e)
    return [(0, 4)] + out


def pcrel_words(img, ranges):
    """[(pc, word, kind, target)] of every PC-relative instruction in ranges"""
    out = []
    for lo, hi in ranges:
        for pc in range(lo, hi & ~3, 4):
            w = struct.unpack_from('<I', img, pc)[0]
            d = decode(w, pc)
            if d: out.append((pc, w, *d))
    return out


def blockers(words, F):
    """Why F cannot be the split, or None: a short-range reference across it, or an adrp to the page that holds it"""
    fp = F & ~0xFFF
    for pc, w, kind, t in words:
        if kind == 'short' and (pc < F) != (t < F): return f'{pc:#x} refers across it to {t:#x} with a short range'
        if kind == 'adrp' and F % PAGE and t == fp: return f'adrp at {pc:#x} to the page holding it'
    return None


def choose_split(img, text_end, lo, hi, words, starts):
    """The function start in [lo, hi] nearest their middle that blockers() allows → F. starts: sorted function starts that follow the end of the function before them"""
    mid = (lo + hi) // 2
    cand = sorted((s for s in starts if lo <= s <= hi), key=lambda s: abs(s - mid))
    by_page, short = {}, []
    for x in words:
        if x[2] == 'adrp': by_page[x[3]] = by_page.get(x[3], 0) + 1
        elif x[2] == 'short' and abs(x[3] - x[0]) > 0: short.append((min(x[0], x[3]), max(x[0], x[3])))
    short.sort()
    import bisect
    for F in cand[:4000]:
        if F % PAGE and by_page.get(F & ~0xFFF): continue
        # a short reference crosses F when its lower end is below F and its upper end at or above it; short ranges are at most 1 MB, so look at those starting within 1 MB below
        i = bisect.bisect_left(short, (F - 0x100001, 0))
        if any(a < F <= b for a, b in short[i:bisect.bisect_left(short, (F, 0))]): continue
        return F
    raise Error(f'no function start in {lo:#x}..{hi:#x} where the text can be split')


def shift(img, segments, F, G, md, words=None):
    """The image with G bytes opened at F → (new image, new segments, counts: far branches, low and high veneers, and the bytes the veneer pools take at the gap's start and end, VL and VH).
    segments: ((offset, size),) of text, ro and data; words: pcrel_words of the image when the caller has them. Raises Error for anything it cannot fix"""
    if G % GRAIN or G <= 0: raise Error(f'gap {G:#x} is not a positive multiple of {GRAIN:#x}')
    text_end = segments[0][1]
    if not 8 <= F < text_end or F % 4: raise Error(f'split {F:#x} not in the text')
    f = lambda a: a + G if a >= F else a
    N = bytearray(img[:F]) + bytearray(G) + bytearray(img[F:])
    def put32(o, v): struct.pack_into('<I', N, f(o), v)
    def put_s32(o, v):
        if not -(1 << 31) <= v < 1 << 31: raise Error(f'{o:#x}: {v:#x} does not fit 32 bits')
        struct.pack_into('<i', N, f(o), v)
    def put64(o, v): struct.pack_into('<Q', N, f(o), v)

    # ---- text: PC-relative instructions across F ----
    if words is None: words = pcrel_words(img, code_ranges(img, text_end, md))
    why = blockers(words, F)
    if why: raise Error(f'split {F:#x}: {why}')
    far = []
    for pc, w, kind, t in words:
        if (pc < F) == (t < F): continue
        if kind == 'adrp': put32(pc, adrp_word(w, f(pc), f(t))); continue
        d = f(t) - f(pc)
        if -REACH <= d < REACH: put32(pc, b_word(w, f(pc), f(t))); continue
        far.append((pc, w, t))
    # Veneers: a single B each, shared by the branches to one target that reach it: the low pool grows up from F, the high pool down from F + G; a branch takes the
    # first of (its target's low veneer, its high veneer, a new low one, a new high one) that both legs reach
    def reach(a, b): return -REACH <= b - a < REACH
    low, high, nl, nh = {}, {}, 0, 0
    for pc, w, t in far:
        v = next((v for v in (low.get(t), high.get(t)) if v is not None and reach(f(pc), v)), None)
        if v is None:
            for pool in ('low', 'high'):
                c = F + 4 * nl if pool == 'low' else F + G - 4 * (nh + 1)
                if reach(f(pc), c) and reach(c, f(t)):
                    v = c
                    if pool == 'low': low[t] = c; nl += 1
                    else: high[t] = c; nh += 1
                    struct.pack_into('<I', N, c, b_word(0x14000000, c, f(t)))
                    break
            else: raise Error(f'branch at {pc:#x} to {t:#x}: no veneer in the gap reaches both')
        put32(pc, b_word(w, f(pc), v))
    VL, VH = (4 * nl + GRAIN - 1) & ~(GRAIN - 1), (4 * nh + GRAIN - 1) & ~(GRAIN - 1)
    if VL + VH > G: raise Error(f'{nl + nh} veneers do not fit a gap of {G:#x}')

    # ---- MOD0 and .dynamic ----
    mod0 = struct.unpack_from('<I', img, 4)[0]
    if img[mod0:mod0 + 4] != b'MOD0': raise Error('no MOD0')
    struct.pack_into('<I', N, 4, f(mod0))
    rel = {}
    for i in range(6):
        r = struct.unpack_from('<i', img, mod0 + 4 + 4 * i)[0]
        rel[i] = mod0 + r
        put_s32(mod0 + 4 + 4 * i, f(mod0 + r) - f(mod0))
    dyn = rel[0]; tags = {}
    o = dyn
    while True:
        tag, v = struct.unpack_from('<qQ', img, o)
        if tag == 0: break
        if tag in DT_PTR:
            if v: put64(o + 8, f(v))
        elif tag not in DT_VAL: raise Error(f'dynamic tag {tag:#x} at {o:#x}: not known to hold an address or not')
        tags.setdefault(tag, v); o += 16

    # ---- relocations ----
    for t_tab, t_sz in ((7, 8), (23, 2)):
        if t_tab not in tags: continue
        base, n = tags[t_tab], tags.get(t_sz, 0) // 24
        for k in range(n):
            e = base + 24 * k
            off, info, add = struct.unpack_from('<QQq', img, e)
            typ, sym = info & 0xFFFFFFFF, info >> 32
            put64(e, f(off))
            if typ == R_RELATIVE or typ == R_ABS64 and sym == 0: struct.pack_into('<q', N, f(e) + 16, f(add))
            elif typ not in R_SYMBOLIC and typ != R_ABS64: raise Error(f'relocation type {typ} at {e:#x}')
    if 36 in tags:
        base, n = tags[36], tags.get(35, 0) // 8
        where = None
        for k in range(n):
            e = struct.unpack_from('<Q', img, base + 8 * k)[0]
            if not e & 1:
                put64(base + 8 * k, f(e)); locs = [e]; where = e + 8
            else:
                if where is None: raise Error('RELR starts with a bitmap')
                locs = [where + 8 * (i - 1) for i in range(1, 64) if e >> i & 1]
                if (where < F) != (where + 62 * 8 < F): raise Error(f'RELR bitmap at {where:#x} spans the split')
                where += 63 * 8
            for L in locs: put64(L, f(struct.unpack_from('<Q', img, L)[0]))

    # ---- symbols ----
    if 6 in tags:
        if 4 in tags: nsym = struct.unpack_from('<I', img, tags[4] + 4)[0]
        else: raise Error('no DT_HASH: the symbol count is unknown')
        for k in range(nsym):
            s = tags[6] + 24 * k
            shndx, val = struct.unpack_from('<H', img, s + 6)[0], struct.unpack_from('<Q', img, s + 8)[0]
            if shndx and shndx != 0xFFF1: put64(s + 8, f(val))

    # ---- eh_frame_hdr and eh_frame ----
    ehs, ehe = rel[3], rel[4]
    if ehe - ehs >= 12 and img[ehs] == 1 and img[ehs + 3] not in (0xFF, 0):
        _, penc, cenc, tenc = img[ehs:ehs + 4]
        def fix_ptr(o, enc, datarel):
            """Rewrites the pointer at o (original offset) of encoding enc → (its target, its size)"""
            fmt, app = enc & 0xF, enc & 0x70
            if enc == 0xFF: return None, 0
            sz = {0x3: 4, 0xB: 4, 0x0: 8, 0x4: 8, 0xC: 8}.get(fmt)
            if sz is None: raise Error(f'pointer encoding {enc:#x} at {o:#x}')
            v = struct.unpack_from({0x3: '<I', 0xB: '<i', 0x0: '<Q', 0x4: '<Q', 0xC: '<q'}[fmt], img, o)[0]
            if app == 0x10: t = v + o; nv = f(t) - f(o)
            elif app == 0x30: t = v + datarel; nv = f(t) - f(datarel)
            elif app == 0x00: return v, sz   # absolute: a relocation's
            else: raise Error(f'pointer application {app:#x} at {o:#x}')
            if fmt == 0xB: put_s32(o, nv)
            elif fmt == 0x3:
                if not 0 <= nv < 1 << 32: raise Error(f'{o:#x}: {nv:#x} does not fit 32 bits')
                put32(o, nv)
            else: struct.pack_into('<q' if fmt == 0xC else '<Q', N, f(o), nv)
            return t, sz
        o = ehs + 4
        eh_frame, s = fix_ptr(o, penc, ehs); o += s
        if eh_frame is None: raise Error('eh_frame_hdr names no eh_frame')
        cnt, s = nso_code._read(img, o, cenc, ehs); o += s
        for _ in range(cnt):
            _, s = fix_ptr(o, tenc, ehs); o += s
            _, s = fix_ptr(o, tenc, ehs); o += s
        # eh_frame: CIEs and FDEs in order up to a zero terminator
        cies = {}
        o, end = eh_frame, segments[2][0] + segments[2][1]
        while o + 4 <= end:
            ln = struct.unpack_from('<I', img, o)[0]
            if ln == 0: break
            if ln == 0xFFFFFFFF: raise Error(f'64-bit eh_frame entry at {o:#x}')
            cid = struct.unpack_from('<I', img, o + 4)[0]
            if cid == 0:
                p = o + 9; aug = img[p:img.index(b'\0', p)]; p += len(aug) + 1
                _, p = nso_code._uleb(img, p)
                while img[p] & 0x80: p += 1
                p += 1
                p = p + 1 if img[o + 8] == 1 else nso_code._uleb(img, p)[1]
                renc, lenc = 0, 0xFF
                if aug.startswith(b'z'):
                    _, p = nso_code._uleb(img, p)
                    for ch in aug[1:]:
                        if ch == ord('R'): renc = img[p]; p += 1
                        elif ch == ord('P'): pe = img[p]; p += 1; p += fix_ptr(p, pe, 0)[1]
                        elif ch == ord('L'): lenc = img[p]; p += 1
                        elif ch in b'S': pass
                        else: raise Error(f'CIE augmentation {aug!r} at {o:#x}')
                cies[o] = (renc, lenc, aug.startswith(b'z'))
            else:
                cie = o + 4 - cid
                if cie not in cies: raise Error(f'FDE at {o:#x}: CIE {cie:#x} not seen before it')
                renc, lenc, z = cies[cie]
                p = o + 8
                _, s = fix_ptr(p, renc, 0); p += s
                p += nso_code._read(img, p, renc & 0xF, 0)[1]
                if z:
                    _, p = nso_code._uleb(img, p)
                    if lenc != 0xFF: fix_ptr(p, lenc, 0)
            o += 4 + ln

    segs = ((0, text_end + G), (segments[1][0] + G, segments[1][1]), (segments[2][0] + G, segments[2][1]))
    return N, segs, {'low': nl, 'high': nh, 'far': len(far), 'VL': VL, 'VH': VH}


def relocated(img, F, G):
    """{place: value} of every relocated pointer of the image (RELATIVE, RELR), as the loader would write them with base 0, in original coordinates when G is given
    (place and value mapped back through f⁻¹): the check that the shift kept every pointer's meaning"""
    finv = (lambda a: a - G if a >= F + G else a) if G else (lambda a: a)
    mod0 = struct.unpack_from('<I', img, 4)[0]
    dyn = mod0 + struct.unpack_from('<i', img, mod0 + 4)[0]
    tags = {}; o = dyn
    while True:
        tag, v = struct.unpack_from('<qQ', img, o)
        if tag == 0: break
        tags.setdefault(tag, v); o += 16
    out = {}
    for t_tab, t_sz in ((7, 8), (23, 2)):
        if t_tab in tags:
            for k in range(tags.get(t_sz, 0) // 24):
                off, info, add = struct.unpack_from('<QQq', img, tags[t_tab] + 24 * k)
                out[finv(off)] = ('r', finv(add)) if info & 0xFFFFFFFF == R_RELATIVE else ('s', info >> 32, add)
    if 36 in tags:
        where = None
        for k in range(tags.get(35, 0) // 8):
            e = struct.unpack_from('<Q', img, tags[36] + 8 * k)[0]
            if not e & 1: locs = [e]; where = e + 8
            elif where is None: raise Error('RELR starts with a bitmap')
            else: locs = [where + 8 * (i - 1) for i in range(1, 64) if e >> i & 1]; where += 63 * 8
            for L in locs: out[finv(L)] = ('r', finv(struct.unpack_from('<Q', img, L)[0]))
    return out


if __name__ == '__main__':
    import nso
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    m = nso.cli_read(sys.argv[1]); F, G = int(sys.argv[2], 0), int(sys.argv[3], 0)
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    N, segs, st = shift(m.image, m.segments, F, G, md)
    print('shifted:', st, 'segments', [(hex(a), hex(b)) for a, b in segs])
    a, b = relocated(m.image, F, 0), relocated(N, F, G)
    print('relocations', len(a), 'same meaning' if a == b else f'DIFFER: {sum(1 for k in a if a.get(k) != b.get(k))} of them')
    f = lambda x: x + G if x >= F else x
    r0, r1 = nso_code.fde_ranges(m.image), nso_code.fde_ranges(N)
    print('functions', len(r0), 'same' if [(f(s), f(e - 4) + 4) for s, e in r0] == r1 else 'DIFFER')   # an end is exclusive: a function ending at F keeps its end
    bad = 0
    for pc, w, kind, t in pcrel_words(m.image, code_ranges(m.image, m.text_end, md)):
        w2 = struct.unpack_from('<I', N, f(pc))[0]; d = decode(w2, f(pc))
        if d is None: bad += 1; continue   # no longer a PC-relative instruction
        if kind == 'adrp': ok = d[1] == (f(t) if t >= F else t) if t != (F & ~0xFFF) else True
        elif kind == 'b':
            t2 = d[1]
            if F <= t2 < F + G:   # through a veneer
                v = decode(struct.unpack_from('<I', N, t2)[0], t2)
                t2 = v[1] if v else None
            ok = t2 == f(t)
        else: ok = d[1] == f(t)
        bad += not ok
    print('pc-relative instructions checked, wrong:', bad)
