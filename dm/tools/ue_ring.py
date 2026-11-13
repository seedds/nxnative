#!/usr/bin/env python3
"""Unreal Engine 4's NVN dynamic buffers keep 4 copies instead of 3, found by the engine's own code, without a build ID: patch_nso.py makes the load of the copy count in the buffer's
constructor a mov. This is a deliberate workaround for a race in the engine (GitHub issue #1040, docs/reference/rendering.md): each write lock of a BUF_Dynamic buffer moves to the next
copy with no fence, and the render thread can run far enough ahead of the GPU to rewrite a copy that queued draws still read. One more copy widens that margin by one lock; it adds no fence.

The count comes from the console variable nvn.NumBufferedFrames (3), which cannot simply be set to 4: its other readers size fixed tables, and the table of deferred deletes has room for 3.
So only this one reader changes. A buffer has room for 4: its slots run from +0x8 to +0x28, where its current location starts.

The chain, each link unique or nothing changes:
- the variable's name (UTF-16) in ro: Unreal Engine 4.18 has its names in UTF-32 and another RHI, so a main without it has no such ring;
- its registration, adrp + add x1 of the name, then adrp + add x2 of the int it refers to, whose value is 3 (Unreal Engine 5 has 2, and no such reader);
- the lock's rotation: ldr wC, [xB, #0x5c]; cmp wC, #2; b.lo; ldr wI, [xB, #0x60]; add; udiv; msub; str wI, [xB, #0x60]; add xS, xB, wI, uxtw #3; ldr xT, [xS, #8], so +0x5c bounds the slots at +8;
- the constructor's load of the count on the BUF_Dynamic path: tbz wU, #1; adrp xN; then ldr wM, [xN, #lo], or through the GOT ldr xN, [xN, #got]; ldr wM, [xN] (OCTOPATH TRAVELER II); then
  str wM, [xK, #0x5c], or str wM, [xK] with xK made +0x5c by str xzr, [xK, #0x5c]! (Pikmin 4);
- before it in the constructor, the slots' end and the location after them: stur q, [x0, #0x18] and a store to #0x28 that writes its base back.
Checked on the mains of Unreal Engine 4.26 and 4.27 on the game drive: Pikmin 4, OCTOPATH TRAVELER II, DRAGON QUEST I & II HD-2D Remake, DRAGON QUEST VII Reimagined, Princess Peach: Showtime!,
TRIANGLE STRATEGY.
Usage on its own: ue_ring.py <module>... (what the chain finds in each)"""
import re, struct, sys, os

NAME = 'nvn.NumBufferedFrames'
COPIES = 4
BACK = 0x200   # how far before the load the constructor's start of the object can be

def adrp_page(w, pc):
    if w & 0x9F000000 != 0x90000000: return None
    imm = (((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)
    if imm & (1 << 20): imm -= 1 << 21
    return (pc & ~0xFFF) + (imm << 12)

def registered(img, text_end, name):
    """The address of the int registered under the name at offset `name` of the image, or None: adrp + add x1 of the name, then adrp + add x2 within 8 instructions"""
    add = struct.pack('<I', 0x91000021 | (name & 0xFFF) << 10)
    found, a = set(), -1
    while (a := img.find(add, a + 1, text_end)) >= 0:
        if a % 4 or a < 4 or adrp_page(struct.unpack_from('<I', img, a - 4)[0], a - 4) != name & ~0xFFF: continue
        for k in range(a + 4, min(a + 36, text_end - 4), 4):
            w, x = struct.unpack_from('<2I', img, k)
            page = adrp_page(w, k)
            if page is not None and w & 31 == 2 and x & 0xFFC003FF == 0x91000042:
                found.add(page + ((x >> 10) & 0xFFF))
                break
    return found.pop() if len(found) == 1 else None

def rotations(img, text_end):
    """Offsets of the lock's rotation over the slots at +8, its count at +0x5c and its index at +0x60"""
    out = []
    for m in re.finditer(rb'[\x00-\xff][\x60-\x63]\x00\xb9', img[:text_end], re.S):   # str w, [x, #0x60]
        a = m.start()
        if a % 4 or a < 32 or a + 12 > text_end: continue
        c, cmp, blo, i0, inc, div, msub, st, ad, ld = struct.unpack_from('<10I', img, a - 28)
        b, ri = (st >> 5) & 31, st & 31
        if c & 0xFFFFFC00 != 0xB9405C00 or (c >> 5) & 31 != b: continue
        if cmp != 0x7100081F | (c & 31) << 5 or blo & 0xFF00001F != 0x54000003: continue
        if i0 & 0xFFFFFC00 != 0xB9406000 or (i0 >> 5) & 31 != b: continue
        if inc & 0xFFFFFC00 != 0x11000400 or div & 0xFFE0FC00 != 0x1AC00800 or msub & 0xFFE08000 != 0x1B008000 or msub & 31 != ri: continue
        if ad & 0xFFE0FC00 != 0x8B204C00 or (ad >> 5) & 31 != b or (ad >> 16) & 31 != ri: continue
        if ld & 0xFFC00000 != 0xF9400000 or (ld >> 5) & 31 != ad & 31 or (ld >> 10) & 0xFFF != 1: continue
        out.append(a)
    return out

def layout_holds(img, load, base=None):
    """The constructor before the load ends the slots at +0x28: stur q, [x0, #0x18] and a store to #0x28 with writeback (str xzr or str q). With base, also str xzr, [base, #0x5c]!"""
    q18 = loc = kept = False
    for a in range(max(load - BACK, 0), load, 4):
        w = struct.unpack_from('<I', img, a)[0]
        if w & 0xFFFFFFE0 == 0x3C818000: q18 = True   # stur q, [x0, #0x18]
        if w & 0xFFFFFC1F == 0xF8028C1F or w & 0xFFFFFC00 == 0x3C828C00: loc = True   # str xzr, [xA, #0x28]! / str q, [xA, #0x28]!
        if base is not None and w == 0xF805CC1F | base << 5: kept = True   # str xzr, [base, #0x5c]!
    return q18 and loc and (base is None or kept)

def loads(img, text_end, var):
    """Offsets of the constructor's load of the count: the instruction that gives wM the variable's value, with M, on the path of tbz wU, #1"""
    got = None
    out = []
    for m in re.finditer(rb'[\x00-\xff]{2}[\x08-\x0f]\x36', img[:text_end], re.S):   # tbz w, #1
        a = m.start()
        if a % 4 or a + 20 > text_end: continue
        adrp, w2, w3, w4 = struct.unpack_from('<4I', img, a + 4)
        page = adrp_page(adrp, a + 4)
        if page is None: continue
        n = adrp & 31
        if w2 & 0xFFC00000 == 0xB9400000 and (w2 >> 5) & 31 == n:   # ldr wM, [xN, #lo]
            if page + ((w2 >> 10) & 0xFFF) * 4 != var: continue
            load, st = a + 8, w3
        elif w2 & 0xFFC00000 == 0xF9400000 and (w2 >> 5) & 31 == n and w2 & 31 == n and w3 & 0xFFFFFC00 == 0xB9400000 and (w3 >> 5) & 31 == n:   # ldr xN, [xN, #got]; ldr wM, [xN]
            if got is None:
                sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
                from loader_census import got_values
                got = got_values(img)
            if got.get(page + ((w2 >> 10) & 0xFFF) * 8) != var: continue
            load, st = a + 12, w4
        else: continue
        rm = struct.unpack_from('<I', img, load)[0] & 31
        if st & 0xFFC0001F != 0xB9000000 | rm: continue
        field, base = ((st >> 10) & 0xFFF) * 4, (st >> 5) & 31
        if field == 0x5C and layout_holds(img, load): out.append((load, rm))
        elif field == 0 and layout_holds(img, load, base): out.append((load, rm))
    return out

def find(img, text_end, ro_off, data_off):
    """(offset of the load, its word, the word to put there, reason) when the chain holds; (None, None, None, why not) otherwise, with why starting '[RED] ' when the engine's ring of 3 is
    there but this chain does not find its count (the buffers keep 3 copies)"""
    name = img.find(NAME.encode('utf-16le') + b'\0\0', ro_off, data_off)
    if name < 0: return None, None, None, f'no {NAME}'
    var = registered(img, text_end, name)
    if var is None: return None, None, None, f'[RED] Unreal\'s {NAME} but not one registration of it'
    if var + 4 > len(img): return None, None, None, f'[RED] Unreal\'s {NAME} at {var:#x}, outside the image'
    val = struct.unpack_from('<I', img, var)[0]
    if val != 3: return None, None, None, f'Unreal\'s {NAME} is {val}: no ring of 3'
    rot = rotations(img, text_end)
    if not rot: return None, None, None, f'[RED] Unreal\'s {NAME} is 3 but no lock rotates over a buffer\'s slots'
    found = loads(img, text_end, var)
    if len(found) != 1: return None, None, None, f'[RED] Unreal\'s {NAME} is 3 but {len(found)} loads of a dynamic buffer\'s copy count ({", ".join(f"{a:#x}" for a, _ in found)})'
    load, rm = found[0]
    return load, struct.unpack_from('<I', img, load)[0], 0x52800000 | COPIES << 5 | rm, f'Unreal\'s dynamic buffers: {NAME} at {var:#x} loaded at {load:#x}, {len(rot)} lock rotations ({rot[0]:#x}): {COPIES} copies'

if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nso
    for path in sys.argv[1:]:
        try: m = nso.read(path)
        except nso.Error as e: print(f'{path}\t-\t{e}'); continue
        print(f'{path}\t{m.build_id[:16].hex()}\t{find(bytes(m.image), m.text_end, m.segments[1][0], m.segments[2][0])[3]}')
