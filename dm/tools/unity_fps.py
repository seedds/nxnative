#!/usr/bin/env python3
"""Unity's frame-rate settings, found in main through the engine's internal-call table, without a build ID: patch_nso.py writes their places into main's fixups and the host writes
the profile's fps_target there (hle_fpslock.c).
IL2CPP registers the engine's internal calls from two parallel tables in main's data: one of name strings ("UnityEngine.Application::set_targetFrameRate"), and right after it one of
the native functions, in the same order (relative relocations, so each entry is found through the module's relocations). Unity 2019.4 and 2017.4 register them from code instead,
one call per name with the function in x1 and the name in x0 (icall_code). From the functions:
- Application.set_targetFrameRate stores its argument into a global int32 (adrp xN; str w0, [xN, #lo]), possibly behind a branch: UNITY_TARGET_FPS, that global's offset.
- QualitySettings.get_vSyncCount reads the current quality level's count: the QualitySettings object from a global (adrp; ldr xQ, [xP, #lo]) or from the engine's managers array
  (bl to mov w0, #K; b to adrp; add; ldr x0, [x8, w0, sxtw #3]: GetManagerFromContext(K)), the level index (ldrsw xI, [xQ, #IDX]), the levels (ldr xA, [xQ, #ARR]), the level
  (add xL, xA, xI, lsl #S, or mov wT, #SIZE; madd xL, xI, xT, xA), the count (ldr w0, [xL, #VSYNC]): UNITY_QS_PTR (the global), _IDX, _ARR, _SIZE, _VSYNC.
Found on The Rogue Prince of Persia 1.1.0 (Unity 6000.0.64f1): targetFrameRate at main+0x8d33de8, vSyncCount at [[main+0x90957d8]+0x20] + [[main+0x90957d8]+0x64] * 0x100 + 0x88,
which the game sets to 2 (present interval 2) and leaves targetFrameRate at -1. Checked on the Unity mains on the drives (2026-11-08): Donkey Kong Country Returns HD (6000.0.28f1)
has the same layout; Super Mario RPG (2021.3.15f1) reaches QualitySettings through GetManagerFromContext(11) and has levels of 0xa8 bytes with the count at +0x68, its
targetFrameRate setter behind a branch; Fire Emblem Engage (2020.3.18f1) the same as Super Mario RPG. Pokémon Brilliant Diamond (2019.4) registers from code and keeps a frame in
the manager getter; Return of the Obra Dinn (2017.4) registers from code (get_vSyncCount only), its managers-array lookup adding the index before an offset load. Unity 2022.3 (SUPER
ROBOT WARS Y, Super Monkey Ball Banana Rumble, Monster Train 2) keeps no table and registers no name from code that this reads: nothing is found there.
Usage on its own: unity_fps.py <module>... (what it finds in each)"""
import struct, sys, os

TFR, VSYNC = 'UnityEngine.Application::set_targetFrameRate', 'UnityEngine.QualitySettings::get_vSyncCount'
RET = 0xD65F03C0

def adrp_page(w, pc):
    imm = ((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)) << 12
    if imm & (1 << 32): imm -= 1 << 33
    return (pc & ~0xFFF) + imm
def is_adrp(w): return (w & 0x9F000000) == 0x90000000
def branch(w, pc):   # the target of b / bl at pc
    off = (w & 0x3FFFFFF) << 2
    return pc + (off - (1 << 28) if off & (1 << 27) else off)

def relocated(img):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from loader_census import got_values
    return got_values(img)

def icall_table(img, rel, ro_off, data_off):
    """{internal call name: native function offset} from the name table and the function table right after it, or {} when main has neither (not Unity, or another layout)"""
    def name_at(v):
        if not ro_off <= v < data_off: return None
        e = img.find(b'\0', v, v + 256)
        s = img[v:e] if e > v else b''
        return s.decode() if b'::' in s and s.isascii() else None
    key = img.find(b'\0' + TFR.encode() + b'\0')
    if key < 0: return {}
    slot = next((a for a, v in rel.items() if v == key + 1), None)
    if slot is None: return {}
    start = end = slot
    while name_at(rel.get(start - 8, -1)): start -= 8
    while name_at(rel.get(end, -1)): end += 8
    n = (end - start) // 8
    if not all(0 < rel.get(end + 8 * i, 0) < ro_off for i in range(n)): return {}   # the functions right after the names, one per name
    return {name_at(rel[start + 8 * i]): rel[end + 8 * i] for i in range(n)}

def icall_code(img, rel, text_end, ro_off, data_off, names):
    """{internal call name: native function offset} for those of names that main registers from code (Unity 2019.4, Pokémon Brilliant Diamond: adrp x1; ldr x1, [x1, #slot], the
    function through a relocated GOT slot, or adrp x1; add x1, the function itself; then adrp x0; add x0, the name; then bl the register call), each registered exactly once"""
    import re
    w = lambda a: struct.unpack_from('<I', img, a)[0]
    out = {}
    for name in names:
        s = img.find(b'\0' + name.encode() + b'\0', ro_off, data_off) + 1
        if s <= 0: continue
        lo, page, fns = s & 0xFFF, s & ~0xFFF, set()
        word = 0x91000000 | lo << 10   # add x0, x0, #lo (Rd = Rn = 0)
        for m in re.finditer(re.escape(struct.pack('<I', word)), img[:text_end]):
            j = m.start()
            if j % 4 or j < 16 or not is_adrp(w(j - 4)) or w(j - 4) & 31 or adrp_page(w(j - 4), j - 4) != page: continue
            if (w(j + 4) & 0xFC000000) != 0x94000000: continue                                       # bl the register call
            p, l = w(j - 12), w(j - 8)                                                                 # the function into x1, just before
            if not is_adrp(p) or p & 31 != 1: continue
            if (l & 0xFFC003FF) == 0xF9400021: f = rel.get(adrp_page(p, j - 12) + ((l >> 10) & 0xFFF) * 8)   # ldr x1, [x1, #slot]
            elif (l & 0xFFC003FF) == 0x91000021: f = adrp_page(p, j - 12) + ((l >> 10) & 0xFFF)            # add x1, x1, #lo
            else: continue
            if f is not None and 0 <= f < text_end: fns.add(f)
        if len(fns) == 1: out[name] = fns.pop()
    return out

def follow(img, f, text_end, hops=2):
    """f, or where the unconditional branches it starts with lead (up to hops of them: an internal call may be a thunk), or None outside the text"""
    for _ in range(hops):
        u = struct.unpack_from('<I', img, f)[0]
        if (u & 0xFC000000) != 0x14000000: break
        f = branch(u, f)
        if not 0 <= f < text_end: return None
    return f

def target_fps(img, f, text_end):
    """The global int32 the targetFrameRate setter stores w0 into (adrp xN, then str w0, [xN, #lo] among its first instructions), or None"""
    f = follow(img, f, text_end)
    if f is None: return None
    page = {}
    for i in range(8):
        a = f + 4 * i; u = struct.unpack_from('<I', img, a)[0]
        if is_adrp(u): page[u & 31] = adrp_page(u, a); continue
        if (u & 0xFFC00000) == 0xB9000000 and u & 31 == 0:   # str w0, [xN, #imm]
            n = (u >> 5) & 31
            return page[n] + ((u >> 10) & 0xFFF) * 4 if n in page else None
        if u == RET: return None
    return None

def context_global(img, f, text_end):
    """The managers-array slot a manager getter reads (mov w0, #K; b H, with H: adrp x8; add x8, x8, #lo; ldr x0, [x8, w0, sxtw #3]), or None. Unity 2019.4 (Pokémon Brilliant
    Diamond) keeps a frame: stp x29, x30, [sp, #-16]!; mov x29, sp; mov w0, #K; bl H; ldp x29, x30, [sp], #16; ret"""
    w = lambda a: struct.unpack_from('<I', img, a)[0]
    framed = w(f) == 0xA9BF7BFD and w(f + 4) == 0x910003FD and w(f + 16) == 0xA8C17BFD and w(f + 20) == RET
    if framed: f += 8
    u = w(f)
    if (u & 0xFFE0001F) != 0x52800000: return None   # mov w0, #K
    v = w(f + 4)
    if (v & 0xFC000000) != (0x94000000 if framed else 0x14000000): return None   # b H (bl H in the framed form)
    h = branch(v, f + 4)
    if not 0 <= h < text_end - 12: return None
    a, b, c, d = w(h), w(h + 4), w(h + 8), w(h + 12)
    if not is_adrp(a) or (b & 0xFFC00000) != 0x91000000 or (b >> 5) & 31 != a & 31: return None
    base, r = adrp_page(a, h) + ((b >> 10) & 0xFFF), b & 31
    if (c & 0xFFE0FC00) == 0xF860D800 and (c >> 5) & 31 == r and (c >> 16) & 31 == 0: return base + 8 * ((u >> 5) & 0xFFFF)   # ldr x0, [xR, w0, sxtw #3]
    # Unity 2017.4 (Return of the Obra Dinn): add xR, xR, w0, sxtw #3; ldr x0, [xR, #off]
    if (c & 0xFFE0FC00) == 0x8B20CC00 and (c >> 16) & 31 == 0 and c & 31 == (c >> 5) & 31 == r and (d & 0xFFC003FF) == 0xF9400000 | r << 5:
        return base + ((d >> 10) & 0xFFF) * 8 + 8 * ((u >> 5) & 0xFFFF)
    return None

def vsync_count(img, f, text_end):
    """(QualitySettings global, level index offset, levels offset, level size, count offset) from the vSyncCount getter, read straight-line, or None"""
    f = follow(img, f, text_end)
    if f is None: return None
    regs: dict[int, tuple] = {}   # register → ('page', address) / ('q',) / ('idx',) / ('arr',) / ('imm', value) / ('lvl', size)
    q = idx = arr = None
    for i in range(24):
        a = f + 4 * i; u = struct.unpack_from('<I', img, a)[0]
        d, n, m = u & 31, (u >> 5) & 31, (u >> 16) & 31
        if u == RET: return None   # returned before the count was read
        if is_adrp(u): regs[d] = ('page', adrp_page(u, a))
        elif (u & 0xFC000000) == 0x94000000:   # bl: a manager getter, its result in x0
            t = branch(u, a)
            q = context_global(img, t, text_end) if 0 <= t < text_end - 8 else None
            if q is None: return None
            regs = {0: ('q',)}
        elif (u & 0xFFC00000) == 0xF9400000:   # ldr xD, [xN, #imm]
            r = regs.get(n); imm = ((u >> 10) & 0xFFF) * 8
            if r and r[0] == 'page' and q is None: q = r[1] + imm; regs[d] = ('q',)
            elif r and r[0] == 'q': arr = imm; regs[d] = ('arr',)
            else: regs.pop(d, None)
        elif (u & 0xFFC00000) == 0xB9800000:   # ldrsw xD, [xN, #imm]
            if regs.get(n, ('',))[0] == 'q': idx = ((u >> 10) & 0xFFF) * 4; regs[d] = ('idx',)
            else: regs.pop(d, None)
        elif (u & 0x7FE00000) == 0x52800000:   # mov wD / xD, #imm
            regs[d] = ('imm', (u >> 5) & 0xFFFF)
        elif (u & 0xFFE00000) == 0x8B000000 and (u >> 22) & 3 == 0:   # add xD, xN, xM, lsl #s
            if regs.get(n, ('',))[0] == 'arr' and regs.get(m, ('',))[0] == 'idx': regs[d] = ('lvl', 1 << ((u >> 10) & 63))
            else: regs.pop(d, None)
        elif (u & 0xFFE08000) == 0x9B000000:   # madd xD, xN, xM, xA
            ri, rt, ra = regs.get(n, ('',)), regs.get(m, ('',)), regs.get((u >> 10) & 31, ('',))
            if ri[0] == 'idx' and rt[0] == 'imm' and ra[0] == 'arr': regs[d] = ('lvl', rt[1])
            else: regs.pop(d, None)
        elif (u & 0xFFC00000) == 0xB9400000:   # ldr wD, [xN, #imm]: the count, from the level
            r = regs.get(n, ('',))
            return (q, idx, arr, r[1], ((u >> 10) & 0xFFF) * 4) if r[0] == 'lvl' and d == 0 and None not in (q, idx, arr) else None
        elif (u & 0x0A000000) != 0x08000000 or (u >> 22) & 1: regs.pop(d, None)   # anything else that may write a register (stores and FP loads do not)
    return None

def find(img, text_end, ro_off, data_off):
    """({'target': global offset, 'qs': (global, idx, arr, size, vsync)} with what was found, reason); ({}, why) when main has no table. The reason has '[RED] ' when the table is
    there but a function's code was not read"""
    rel = relocated(img)
    calls = icall_table(img, rel, ro_off, data_off) or icall_code(img, rel, text_end, ro_off, data_off, (TFR, VSYNC))
    if not calls: return {}, 'no Unity internal-call table'
    out, why = {}, []
    if TFR in calls:
        g = target_fps(img, calls[TFR], text_end)
        if g is not None and g >= data_off: out['target'] = g; why.append(f'targetFrameRate at {g:#x}')
        else: why.append(f'[RED] targetFrameRate setter at {calls[TFR]:#x} not read')
    if VSYNC in calls:
        v = vsync_count(img, calls[VSYNC], text_end)
        if v is not None and v[0] >= data_off: out['qs'] = v; why.append(f'vSyncCount at [[{v[0]:#x}]+{v[2]:#x}] + [[{v[0]:#x}]+{v[1]:#x}] * {v[3]:#x} + {v[4]:#x}')
        else: why.append(f'[RED] vSyncCount getter at {calls[VSYNC]:#x} not read')
    return out, f'Unity internal calls ({len(calls)}): ' + ', '.join(why)

if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nso
    for path in sys.argv[1:]:
        try: m = nso.read(path)
        except nso.Error as e: print(f'{path}\t-\t{e}'); continue
        out, why = find(bytes(m.image), m.text_end, m.segments[1][0], m.segments[2][0])
        print(f'{path}\t{m.build_id[:16].hex()}\t{why}')
