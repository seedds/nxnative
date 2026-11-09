#!/usr/bin/env python3
"""HAL Laboratory's frame interval and dynamic-resolution bounds, found in main by the engine's own code, without a build ID: patch_nso.py writes their places into main's fixups
(HAL_FRAME_PTR, HAL_FRAME_FIELD; HAL_DR_PTR, HAL_DR_HOP0..2, HAL_DR_FIELD), and the host writes them for the profile's fps_target (hle_fpslock.c).
- The frame interval: the frame manager switches on an int32 of its frame object, the vsyncs it waits for each frame (ldr wM, [xO, #F]; cmp wM, #2; b.eq; cmp wM, #1; b.eq;
  cbnz wM), and counts the time that passed in 60 Hz vsyncs (16,666,666 ns, movz / movk wK, #0xfe502a, right after). The frame object's constructor clears that field
  (str wzr, [xB, #F]) and stores the address of its interface at +D into a global (add xT, xB, #D; adrp xP; str xT, [xP, #lo]), which no other code sets but to 0. The
  field is at [global] + F - D. The game sets 2 (30 fps); 0 lifts its limit, and the manager then steps the game by the time that really passed.
- The dynamic resolution's GPU frame-time bounds: two floats (max, min, in 60 Hz vsyncs) at [[[[root] + 0x140] + 0x1b0] + 0x20] + 0x34, chosen for 30 fps (1.95, 1.7).
  The root is the last of the engine's three heap roots, each read by a function that looks up which of the root's heaps (count at +0xf0, pointers from +0xf8) holds an
  address (adrp x8; ldr x20, [x8, #lo]; ldrsw x8, [x20, #0xf0]; mov x19, x0; cmp w8, #1), three globals 8 bytes apart. The path after the root goes through the heaps'
  bookkeeping (lists of 0x30-byte heads from +0x110) to the block that holds the bounds: HOPS, as masagrator's FPSLocker config walks it for 1.0.0 and 1.1.0 alike
  (FPSLocker-Warehouse, SaltySD/plugins/FPSLocker/patches/01004D300C5AE000; the interval found by Hazerou). The rewriter finds the root; the path is not read from code.
Found on Kirby and the Forgotten Land 1.1.0 (main a6ce40dc…: the interval at [0xa76728] + 0x50, the root at 0xa767b0) and 1.0.0 (d9ba7db7…: 0xa72628 and 0xa726b0, the
places that config lists for that build). Each place must be found exactly once, or it is not written; a field or root found twice gives a [RED] line.
Usage on its own: hal_fps.py <module>... (what it finds in each)"""
import struct, sys, os

VSYNC_NS = 16666666   # one 60 Hz vsync in nanoseconds: the frame manager's divisor
HOPS, DR_FIELD = (0x140, 0x1b0, 0x20), 0x34
ROOT_TAIL = bytes.fromhex('88f280b9f30300aa1f050071')   # ldrsw x8, [x20, #0xf0]; mov x19, x0; cmp w8, #1

def adrp_page(w, pc):
    imm = ((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)) << 12
    if imm & (1 << 32): imm -= 1 << 33
    return (pc & ~0xFFF) + imm
def is_adrp(w): return (w & 0x9F000000) == 0x90000000

def writes(u):   # the general register an instruction writes, roughly: Rd of anything but stores, branches and FP / SIMD loads (None)
    if (u & 0x0A000000) == 0x08000000:   # loads and stores
        if not (u >> 22) & 1 or (u >> 26) & 1: return None
        return u & 31
    if (u & 0x1C000000) == 0x14000000: return None
    if (u & 0x1F000000) == 0x1E000000 and (u >> 29) & 1 == 0 and (u & 0xFFFFFC00) not in (0x1E260000, 0x9E660000): return None
    return u & 31

def interval_fields(img, text_end):
    """{F: [site]}: the frame manager's switch on the interval at +F of its object, with the vsync count right after"""
    word = lambda a: struct.unpack_from('<I', img, a)[0]
    out = {}
    for i in range(0, text_end - 24, 4):
        u = word(i)
        if (u & 0xFFC00000) != 0xB9400000: continue   # ldr wM, [xO, #F]
        m, f = u & 31, ((u >> 10) & 0xFFF) * 4
        if word(i + 4) != (0x7100081F | m << 5) or (word(i + 8) & 0xFF00001F) != 0x54000000: continue   # cmp wM, #2; b.eq
        if word(i + 12) != (0x7100041F | m << 5) or (word(i + 16) & 0xFF00001F) != 0x54000000: continue   # cmp wM, #1; b.eq
        if (word(i + 20) & 0xFF00001F) != (0x35000000 | m): continue   # cbnz wM
        lo = next((k for k in range(i + 24, min(text_end - 4, i + 24 + 4 * 32), 4) if (word(k) & 0xFFFFFFE0) == 0x52800000 | (VSYNC_NS & 0xFFFF) << 5), None)   # movz wK, #0x502a
        if lo is None or word(lo + 4) != (0x72A00000 | (VSYNC_NS >> 16) << 5 | (word(lo) & 31)): continue   # movk wK, #0xfe, lsl #16
        out.setdefault(f, []).append(i)
    return out

def stores_to(img, text_end, g):
    """[(site, value register)]: the 64-bit stores to global g through adrp (adrp xP; ...; str xT, [xP, #lo] with xP written by nothing in between)"""
    word = lambda a: struct.unpack_from('<I', img, a)[0]
    out = []
    for i in range(0, text_end, 4):
        u = word(i)
        if not is_adrp(u) or adrp_page(u, i) != g & ~0xFFF: continue
        p = u & 31
        for k in range(i + 4, min(text_end, i + 4 * 64), 4):
            v = word(k)
            if (v & 0xFFC00000) == 0xF9000000 and (v >> 5) & 31 == p and ((v >> 10) & 0xFFF) * 8 == g & 0xFFF: out.append((k, v & 31))
            if writes(v) == p: break
    return out

def frame_global(img, text_end, f):
    """[(global, D, site)]: the constructors that clear +F of an object and store its address + D into a global"""
    word = lambda a: struct.unpack_from('<I', img, a)[0]
    out = []
    for i in range(0, text_end, 4):
        u = word(i)
        if (u & 0xFFC0001F) != 0xB900001F or ((u >> 10) & 0xFFF) * 4 != f: continue   # str wzr, [xB, #F]
        b = (u >> 5) & 31
        for k in range(max(0, i - 4 * 64), min(text_end, i + 4 * 64), 4):   # add xT, xB, #D, then str xT, [xP, #lo] with xP from adrp
            v = word(k)
            if (v & 0xFFC00000) != 0x91000000 or (v >> 5) & 31 != b: continue
            t, d = v & 31, (v >> 10) & 0xFFF
            if t in (b, 31) or d >= f: continue
            for s in range(k + 4, min(text_end, k + 4 * 96), 4):
                x = word(s)
                if (x & 0xFFC0001F) == (0xF9000000 | t):   # str xT, [xP, #lo]
                    p = (x >> 5) & 31
                    page = next((adrp_page(word(a), a) for a in range(s - 4, max(k, s - 4 * 96), -4) if is_adrp(word(a)) and word(a) & 31 == p), None)
                    if page is not None: out.append((page + ((x >> 10) & 0xFFF) * 8, d, s))
                    break
                if writes(x) == t: break
    return out

def find(img, text_end, ro_off, data_off):
    """({'frame': (global, field), 'dr': (root, hops, field)} with what was found, reason); ({}, why) when main has neither. The reason has '[RED] ' when a place was found
    more than once"""
    out, why = {}, []
    fields = interval_fields(img, text_end)
    found = set()
    for f in fields:
        for g, d, s in frame_global(img, text_end, f):
            if g < data_off: continue
            if all(s2 == s or t == 31 for s2, t in stores_to(img, text_end, g)): found.add((g, f - d))   # nothing else sets the global but to 0
    if len(found) == 1:
        g, f = found.pop(); out['frame'] = (g, f); why.append(f'frame interval at [{g:#x}]+{f:#x}')
    elif found: why.append('[RED] frame interval: ' + ', '.join(f'[{g:#x}]+{f:#x}' for g, f in sorted(found)) + ' qualify')
    elif fields: why.append('[RED] frame interval: the frame manager\'s switch at ' + ', '.join(f'{s[0]:#x}' for s in fields.values()) + ', but no constructor stores its object in a global of its own')
    roots, i = [], 0
    while (i := img.find(ROOT_TAIL, i, text_end)) >= 0:   # adrp x8; ldr x20, [x8, #lo] before it
        if i % 4 == 0 and i >= 8:
            a, l = struct.unpack_from('<2I', img, i - 8)
            if is_adrp(a) and a & 31 == 8 and (l & 0xFFC003FF) == 0xF9400114: roots.append(adrp_page(a, i - 8) + ((l >> 10) & 0xFFF) * 8)
        i += 4
    roots = sorted(set(roots))
    if len(roots) == 3 and roots[1] - roots[0] == 8 and roots[2] - roots[1] == 8 and roots[0] >= data_off:
        out['dr'] = (roots[2], HOPS, DR_FIELD)
        why.append(f'dynamic-resolution bounds at [[[[{roots[2]:#x}]' + ''.join(f'+{h:#x}]' for h in HOPS) + f'+{DR_FIELD:#x}')
    elif roots: why.append('[RED] dynamic-resolution bounds: heap roots ' + ', '.join(f'{r:#x}' for r in roots) + ', not three in a row')
    if not why: return {}, 'no HAL frame interval'
    return out, 'HAL engine: ' + ', '.join(why)

if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nso
    for path in sys.argv[1:]:
        try: m = nso.read(path)
        except nso.Error as e: print(f'{path}\t-\t{e}'); continue
        out, why = find(bytes(m.image), m.text_end, m.segments[1][0], m.segments[2][0])
        print(f'{path}\t{m.build_id[:16].hex()}\t{why}')
