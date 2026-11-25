#!/usr/bin/env python3
"""A target frame rate the game steps by, found in main by the code that reads and sets it, without a build ID: patch_nso.py writes its place into main's fixups (FPS_TARGET_PTR, the
global pointer's offset, and FPS_TARGET_FIELD, the field's offset in the object it points to), and the host writes the profile's fps_target there (hle_fpslock.c).
The field is an int32 frame rate in an object a global points to. Its readers load it into a float register, convert it and divide 60.0 by it: the vsyncs one frame stands for, by which
they step the game (ldr sN, [xB, #F]; scvtf sC, sN; fdiv sD, s60, sC, the 60.0 made by movz wK, #0x4270, lsl #16 and fmov). The game sets it with an immediate 30 or 60 (str wK, [xB, #F]).
The object's address is loaded from the global each time: ldr xB, [xA], xA made by adrp + add (the global) or adrp + ldr (its GOT slot, the module's relative relocations applied).
A field with at least MIN_READERS such readers and a store of 30 is the target; exactly one must be, or nothing is written (two give a [RED] line).
Found on Fire Emblem Fortune's Weave v131072 (Intelligent Systems' engine, its gxc graphics object; 59 readers, 30 set where gameplay starts and 60 for the title and menus, and its present
asks for interval 60 / target). Checked on 100 mains on the drives (2026-11-03): it finds 55 of those 59 readers, and not one reader in any other main, Paper Mario: The Origami King
and The Thousand-Year Door (Intelligent Systems' older engine) among them.
Usage on its own: fps_target.py <module>... (what it finds in each)"""
import struct, sys, os

# A reader is the division of 60.0 by the field. Fortune's Weave has 59; a lone division of 60 by an int field (a rate in a menu, a timer) must not count as the game's step
MIN_READERS = 8
SIXTY = 0x42700000   # 60.0f

def adrp_page(w, pc):
    imm = ((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)) << 12
    if imm & (1 << 32): imm -= 1 << 33
    return (pc & ~0xFFF) + imm
def is_adrp(w): return (w & 0x9F000000) == 0x90000000
def movz_w(w):   # movz wD, #imm{, lsl #16}: (d, value), or None
    if (w & 0xFF800000) != 0x52800000: return None
    return w & 31, ((w >> 5) & 0xFFFF) << (16 * ((w >> 21) & 1))

def writes(u):   # the general register an instruction writes, roughly: Rd of anything but stores, branches and FP / SIMD loads (None)
    if (u & 0x0A000000) == 0x08000000:   # loads and stores
        if not (u >> 22) & 1 or (u >> 26) & 1: return None   # a store, or a load into an FP / SIMD register
        return u & 31
    if (u & 0x1C000000) == 0x14000000: return None   # branches, system instructions
    if (u & 0x1F000000) == 0x1E000000 and (u >> 29) & 1 == 0 and (u & 0xFFFFFC00) not in (0x1E260000, 0x9E660000): return None   # FP data processing (fmov wD, sN writes a general one)
    return u & 31

def relocated(img):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from loader_census import got_values
    return got_values(img)

def globals_of(img):
    """global_of(i, b): the global whose value register xB holds at word offset i (ldr xB, [xA] a few words before, xA the global's address made by adrp + add, or loaded from
    its GOT slot by adrp + ldr, the module's relative relocations applied), or None. Also used by ue_fixed_rate.py (Unreal's GEngine)"""
    word = lambda a: struct.unpack_from('<I', img, a)[0]
    rel = None
    def page_of(i, a):
        """The page adrp xA made a few words before word offset i, with xA written by nothing in between, or None"""
        for k in range(i - 4, max(0, i - 24), -4):
            u = word(k)
            if is_adrp(u) and u & 31 == a: return adrp_page(u, k)
            if writes(u) == a: return None
        return None
    def global_of(i, b):
        """The global whose value xB holds at word offset i (ldr xB, [xA] a few words before, xA the global's address), or None"""
        nonlocal rel
        for k in range(i - 4, max(0, i - 40), -4):
            u = word(k)
            if u & 31 == b and (u & 0xFFC00000) == 0xF9400000:   # ldr xB, [xA, #imm]
                if (u >> 10) & 0xFFF: return None
                a = (u >> 5) & 31
                for k2 in range(k - 4, max(0, k - 32), -4):
                    v = word(k2)
                    if v & 31 != a: continue
                    if (v & 0xFFC00000) == 0x91000000 and (v >> 5) & 31 == a:   # add xA, xA, #lo
                        page = page_of(k2, a)
                        return page + ((v >> 10) & 0xFFF) if page is not None else None
                    if (v & 0xFFC00000) == 0xF9400000 and (v >> 5) & 31 == a:   # ldr xA, [xA, #got]: the GOT slot
                        page = page_of(k2, a)
                        if page is None: return None
                        if rel is None: rel = relocated(img)
                        return rel.get(page + ((v >> 10) & 0xFFF) * 8)
                    return None
                return None
            if writes(u) == b: return None   # xB written by something else
        return None
    return global_of

def find(img, text_end, ro_off, data_off):
    """(offset of the global pointer, offset of the field, reason), or (None, None, why not); why starts '[RED] ' when more than one field qualifies"""
    word = lambda a: struct.unpack_from('<I', img, a)[0]
    global_of = globals_of(img)
    readers, stores = {}, {}
    for i in range(0, text_end, 4):
        u = word(i)
        if (u & 0xFFC00000) == 0xBD400000:   # ldr sT, [xB, #F]
            t, b, f = u & 31, (u >> 5) & 31, ((u >> 10) & 0xFFF) * 4
            conv = next((k for k in range(i + 4, min(text_end, i + 24), 4) if (word(k) & 0xFFFFFFE0) == 0x5E21D800 | t << 5), None)   # scvtf sC, sT
            if conv is None: continue
            c = word(conv) & 31
            div = next((k for k in range(conv + 4, min(text_end, conv + 20), 4) if (word(k) & 0xFFE0FC00) == 0x1E201800 and (word(k) >> 16) & 31 == c), None)   # fdiv sD, sM, sC
            if div is None: continue
            m = (word(div) >> 5) & 31
            fm = next((k for k in range(div - 4, max(0, div - 48), -4) if (word(k) & 0xFFFFFC1F) == 0x1E270000 | m), None)   # fmov sM, wK
            if fm is None or not any((z := movz_w(word(k))) and z == ((word(fm) >> 5) & 31, SIXTY) for k in range(fm - 4, max(0, fm - 32), -4)): continue
            g = global_of(i, b)
            if g is not None and g >= data_off: readers.setdefault((g, f), []).append(i)
        elif (u & 0xFFC00000) == 0xB9000000:   # str wK, [xB, #F]
            k_, b, f = u & 31, (u >> 5) & 31, ((u >> 10) & 0xFFF) * 4
            val = next((z[1] for k in range(i - 4, max(0, i - 32), -4) if (z := movz_w(word(k))) and z[0] == k_), None)
            if val not in (30, 60): continue
            g = global_of(i, b)
            if g is not None and g >= data_off: stores.setdefault((g, f), []).append((i, val))
    found = [(g, f) for (g, f), r in readers.items() if len(r) >= MIN_READERS and any(v == 30 for _, v in stores.get((g, f), ()))]
    if not found:
        best = max(readers.items(), key=lambda x: len(x[1]), default=None)
        return None, None, 'no target frame rate' + (f' (most readers: {len(best[1])} of [{best[0][0]:#x}]+{best[0][1]:#x}, stores {[v for _, v in stores.get(best[0], ())]})' if best else '')
    if len(found) > 1: return None, None, '[RED] target frame rate: ' + ', '.join(f'[{g:#x}]+{f:#x}' for g, f in found) + ' qualify'
    g, f = found[0]
    st = stores[(g, f)]
    return g, f, f'target frame rate at [{g:#x}]+{f:#x}: {len(readers[(g, f)])} readers of 60 / target, set to ' + ', '.join(f'{v} at {a:#x}' for a, v in st)


def find_re_engine(img, text_end, data_off):
    """Find the float MaxFps of via.Application through its timing loop and native accessors."""
    if not all(name in img for name in (b'via.Application\0', b'get_MaxFps\0', b'set_MaxFps\0')):
        return None, None, ''
    # The reviewed GameThread loop compares elapsed frame units with base / MaxFps.
    # Its fixed-step branch divides the same fields. The two paths supply the
    # step without changing the base rate, which the game uses as a time unit.
    wait = struct.pack('<3I', 0x1e2119af, 0xbd041e60, 0x1e2021e0)  # fdiv s15,s13,s1; str s0,[x19,#0x41c]; fcmp s15,s0
    fixed = struct.pack('<2I', 0xf9000bf9, 0x1e2019a1)              # str x25,[sp,#0x10]; fdiv s1,s13,s0
    from nso_code import fde_ranges
    import bisect
    ranges = fde_ranges(img)
    starts = [start for start, _ in ranges]
    words = struct.Struct('<I')
    fields = set()
    for at in _sites(img, wait, text_end):
        i = bisect.bisect_right(starts, at) - 1
        if i < 0: continue
        start, end = ranges[i]
        if at - 4 < start or at + len(wait) > end: continue
        cap = words.unpack_from(img, at - 4)[0]
        if cap & 0xffc003ff != 0xbd400261: continue   # ldr s1,[x19,#MaxFps]
        limit = (cap >> 10 & 0xfff) * 4
        if limit < 4: continue
        base = struct.pack('<I', (cap & 0x003ffc00) - 0x400 | 0xbd40026d)  # ldr s13,[x19,#BaseFps]
        same_limit = struct.pack('<I', cap - 1) + fixed                    # ldr s0,[x19,#MaxFps]
        if img.rfind(base, start, at - 4) != -1 and img.find(same_limit, at + len(wait), end) != -1:
            fields.add(limit)
    if not fields: return None, None, ''

    # Four neighboring accessors get/set the base and get/set MaxFps through
    # the same singleton. Their GOT relocations yield the pointer slot.
    rel = relocated(img)
    candidates = set()
    for limit in fields:
        base = limit - 4
        getter = struct.pack('<3I', 0xf9400108, 0xbd400100 | base << 8, 0xd65f03c0)
        for at in _sites(img, getter, text_end):
            first = at - 8
            if first < 0 or first + 80 > text_end: continue
            slots = set()
            for j, field in enumerate((base, base, limit, limit)):
                pc = first + j * 20
                page, got, obj, access, ret = struct.unpack_from('<5I', img, pc)
                op = 0xbd400100 if j % 2 == 0 else 0xbd000100
                if (page & 0x9f00001f != 0x90000008 or got & 0xffc003ff != 0xf9400108
                        or obj != 0xf9400108 or access != op | field << 8 or ret != 0xd65f03c0):
                    break
                slots.add(rel.get(adrp_page(page, pc) + (got >> 10 & 0xfff) * 8))
            else:
                if len(slots) != 1: continue
                ptr = slots.pop()
                if ptr is not None and data_off <= ptr and ptr % 8 == 0:
                    candidates.add((ptr, limit))
    if len(candidates) > 1:
        return None, None, '[RED] multiple via.Application MaxFps candidates'
    if not candidates: return None, None, ''
    ptr, field = candidates.pop()
    return ptr, field, f'RE Engine MaxFps at [{ptr:#x}]+{field:#x}'


def _sites(img, pattern, end):
    at = img.find(pattern, 0, end)
    while at >= 0:
        if at % 4 == 0: yield at
        at = img.find(pattern, at + 1, end)


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nso
    for path in sys.argv[1:]:
        try: m = nso.read(path)
        except nso.Error as e: print(f'{path}\t-\t{e}'); continue
        img = bytes(m.image)
        g, f, why = find_re_engine(img, m.text_end, m.segments[2][0])
        if not why: g, f, why = find(img, m.text_end, m.segments[1][0], m.segments[2][0])
        print(f'{path}\t{m.build_id[:16].hex()}\t{why}')
