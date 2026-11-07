#!/usr/bin/env python3
"""Unreal Engine's fixed frame rate, UEngine::FixedFrameRate on the engine object GEngine points to, found in main without a build ID: patch_nso.py writes its place into main's fixups
(UE_FIXED_RATE_PTR, GEngine's offset in main, and UE_FIXED_RATE_FIELD, the property's offset in UEngine), and the host writes the profile's fps_target there (hle_fpslock.c).
With bUseFixedFrameRate on, the engine steps every frame by exactly 1 / FixedFrameRate seconds and holds its tick to that rate, whatever rhi.SyncInterval and t.MaxFPS say
(ue_cvar.py). DRAGON BALL: Sparking! ZERO (UE 5.1.1) and Pikmin 4 (UE 4.26.2) turn it on in their configs (bUseFixedFrameRate=True; 30, set by Sparking! ZERO's config and Unreal's
Switch frame pace, main+0x1e1e904, and Pikmin 4's by the engine's default), so their gameplay stayed at 30 with the present interval raised. The value is the frame rate, so the game's speed
holds at any rate, as far as the game's own code steps by the engine's delta time. Both links hold in any Unreal 4.22 to 5 build, each unique or nothing is written:
- the property's reflection entry: the data word holding the address of its UTF-8 name "FixedFrameRate" (the module's relative relocations applied; the linker may share the string with the
  tail of "bUseFixedFrameRate"), whose generated-property kind is Float (0x0A at +24) and whose array size is 1 (+32). The offset follows the array size (+36, Unreal Engine 4.22 to 5.0:
  Pikmin 4's 0x7a8) or, from Unreal Engine 5.1 on, a setter and a getter pointer after it (+56: Sparking! ZERO's 0x9c8);
- GEngine, where FEngineLoop::Init makes the engine: it reads the engine class from the config key TEXT("GameEngine") (UTF-16; Sparking! ZERO's linker shares it with the tail of
  "UGameEngine"), makes the object and stores it into GEngine (str xV, [xR, #lo]), then loads it back and makes a virtual call on it (ParseCommandline: ldr x0, [xR, #lo], then a blr).
  xR is the global's page (adrp; Pikmin 4: main+0x9fbf000 + 0x880), that plus an offset (adrp + add), or the address a GOT slot holds (adrp + ldr; Sparking! ZERO: the slot main+0x8dbfb40
  → main+0x9aefa30). Every reference with that shape must name the same global. Until 2026-11-08 the rule took the global through which code stores the property, which only Sparking! ZERO's
  frame pace does (main+0x1e1ea04), and before that any global through which code reached the offset: Pikmin 4's one load there (main+0x1689a70) reads another object
  (main+0x9350560, -980 at +0x7a8), and the host's range check refused it.
- the flag bUseFixedFrameRate, a bitfield bool: its reflection entry (kind Bool, 0x0C) holds the function that sets it on an object (ldrb w, [x0, #byte]; orr w, w, #bit; strb; ret:
  Sparking! ZERO +0x9c4, Pikmin 4 and Princess Peach: Showtime! +0x7a4, bit 0x40). The host writes the rate only while the flag is set on the object, so a game whose engine steps by the
  time that passed (Princess Peach: Showtime!, at 60 by the present interval alone) is left as it is, and an object that is not the engine would also need the flag's bit and a frame rate.
Usage on its own: ue_fixed_rate.py <module>... (what it finds in each)"""
import re, struct, sys, os

NAME = b'FixedFrameRate\0'
FLOAT = 0x0A   # EPropertyGenFlags::Float (Unreal Engine 4.22 and later)
KEY = 'GameEngine'.encode('utf-16-le') + b'\0\0'   # FEngineLoop::Init's config key for the engine class
STORE_REACH, RELOAD_REACH, CALL_REACH, BACK_REACH = 0x100, 8, 6, 0x4000   # bytes after the key's reference; instructions to the reload and from it to the call; bytes back to xR's value

def relocated(img):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from loader_census import got_values
    return got_values(img)

def adrp_page(w, pc):
    imm = ((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)) << 12
    if imm & (1 << 32): imm -= 1 << 33
    return (pc & ~0xFFF) + imm
def is_adrp(w): return (w & 0x9F000000) == 0x90000000

def fields(img, ro_off, data_off, rel):
    """{reflection entry offset: the property's offset in its class} of the float properties named FixedFrameRate"""
    names, s = [], img.find(NAME, ro_off, data_off)
    while s >= 0: names.append(s); s = img.find(NAME, s + 1, data_off)
    out = {}
    for o, v in rel.items():
        if v not in names or o < data_off or o + 64 > len(img): continue
        kind, dim = img[o + 24], struct.unpack_from('<i', img, o + 32)[0]
        if kind != FLOAT or dim != 1: continue
        f = struct.unpack_from('<I', img, o + 36)[0] or struct.unpack_from('<I', img, o + 56)[0]   # UE 4.22-5.0, else UE 5.1+ (a setter and a getter before it)
        if f and f % 4 == 0 and f < 0x4000: out[o] = f
    return out

def flag(img, text_end, ro_off, data_off, rel):
    """(byte offset, bit mask) of bUseFixedFrameRate in UEngine, from the set-bit function of its reflection entry, or None"""
    s = img.find(b'bUseFixedFrameRate\0', ro_off, data_off)
    word = lambda a: struct.unpack_from('<I', img, a)[0]
    for o, v in rel.items():
        if v != s or o < data_off or img[o + 24] != 0x0C: continue
        fn = next((rel[k] for k in range(o + 8, o + 0x50, 8) if 0 < rel.get(k, 0) < text_end), None)
        if fn is None: continue
        ld, orr, st, ret = (word(fn + 4 * k) for k in range(4))
        if (ld & 0xFFC003E0) != 0x39400000 or st != (ld & ~0x00400000) or ret != 0xD65F03C0: continue   # ldrb wA, [x0, #off] / strb wA, [x0, #off] / ret
        a = ld & 31
        if (orr & 0xFFC003FF) != (0x32000000 | a << 5 | a) or (orr >> 10) & 0x3F: continue   # orr wA, wA, #(one bit): N 0, imms 0
        mask = 1 << ((32 - ((orr >> 16) & 0x3F)) % 32)
        if mask < 0x100: return (ld >> 10) & 0xFFF, mask
    return None

def key_refs(img, text_end, ro_off, data_off):
    """Word offsets of the add xD, xN, #lo that make the address of a UTF-16 "GameEngine" (alone or a longer string's tail) after an adrp xN of its page"""
    word = lambda a: struct.unpack_from('<I', img, a)[0]
    out = []
    s = img.find(KEY, ro_off, data_off)
    while s >= 0:
        if s % 2 == 0:
            lo = s & 0xFFF
            b1 = b''.join(re.escape(bytes([(lo & 0x3F) << 2 | r])) for r in range(4))   # imm bits 0-5 and the top two bits of Xn (ue_cvar.py)
            for m in re.finditer(rb'(?=[' + b1 + rb']' + re.escape(bytes([lo >> 6])) + rb'\x91)', img[:text_end]):
                j = m.start() - 1
                if j % 4 or j < 0: continue
                w = word(j)
                if (w & 0xFFC00000) != 0x91000000 or (w >> 10) & 0xFFF != lo: continue
                n = (w >> 5) & 31
                if any(is_adrp(word(i)) and word(i) & 31 == n and adrp_page(word(i), i) == s & ~0xFFF for i in range(max(0, j - 20), j, 4)): out.append(j)
        s = img.find(KEY, s + 2, data_off)
    return sorted(set(out))

def engine_global(img, text_end, ro_off, data_off, rel):
    """({GEngine candidate: [store offsets]}): at each reference to the key, the first store of a register to [xR, #lo] after which [xR, #lo] is loaded into x0 and a blr follows; the
    candidate is xR's value (resolved back: adrp, adrp + add, a GOT slot's address) plus lo"""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from fps_target import writes
    word = lambda a: struct.unpack_from('<I', img, a)[0]
    def value(i, r, depth=0):
        """The address register r holds at word offset i, from the last instruction before it that writes r (up to BACK_REACH bytes back), or None"""
        if depth > 3: return None
        for k in range(i - 4, max(0, i - BACK_REACH), -4):
            u = word(k)
            if is_adrp(u) and u & 31 == r: return adrp_page(u, k)
            if writes(u) != r: continue
            n = (u >> 5) & 31
            if (u & 0xFFC00000) == 0x91000000:   # add xr, xn, #imm
                b = value(k, n, depth + 1); return b + ((u >> 10) & 0xFFF) if b is not None else None
            if (u & 0xFFC00000) == 0xF9400000:   # ldr xr, [xn, #imm]: a GOT slot holds the address
                b = value(k, n, depth + 1)
                return rel.get(b + ((u >> 10) & 0xFFF) * 8) if b is not None else None
            if (u & 0xFFE0FFE0) == 0xAA0003E0: return value(k, (u >> 16) & 31, depth + 1)   # mov xr, xs
            return None
        return None
    found = {}
    for ref in key_refs(img, text_end, ro_off, data_off):
        for k in range(ref + 4, min(text_end, ref + STORE_REACH), 4):
            u = word(k)
            if (u & 0xFFC00000) != 0xF9000000: continue   # str xV, [xR, #lo]
            r, lo = (u >> 5) & 31, ((u >> 10) & 0xFFF) * 8
            reload = 0xF9400000 | (lo // 8) << 10 | r << 5   # ldr x0, [xR, #lo]
            at = next((q for q in range(k + 4, min(text_end, k + 4 * RELOAD_REACH), 4) if word(q) == reload), None)
            if at is None or any(writes(word(q)) == r for q in range(k + 4, at, 4)): continue
            if not any((word(q) & 0xFFFFFC1F) == 0xD63F0000 for q in range(at + 4, min(text_end, at + 4 * CALL_REACH), 4)): continue   # blr
            b = value(k, r)
            if b is not None and b + lo >= data_off and (b + lo) % 8 == 0: found.setdefault(b + lo, []).append(k)   # in data or the bss (past the file image)
            break
    return found

def find(img, text_end, ro_off, data_off):
    """(GEngine's offset in main, FixedFrameRate's offset in UEngine, bUseFixedFrameRate as byte offset << 8 | bit mask, reason), or (None, None, None, why not); why starts
    '[RED] ' when the property is there and the engine's creation names more than one global, the property has more than one offset or its flag is not found"""
    rel = relocated(img)
    fs = fields(img, ro_off, data_off, rel)
    if not fs: return None, None, None, 'no UEngine::FixedFrameRate'
    offs = sorted(set(fs.values()))
    if len(offs) > 1: return None, None, None, '[RED] UEngine::FixedFrameRate: float properties of that name at ' + ', '.join(f'+{f:#x}' for f in offs)
    f = offs[0]
    fl = flag(img, text_end, ro_off, data_off, rel)
    if fl is None: return None, None, None, f'[RED] UEngine::FixedFrameRate at +{f:#x}: no set-bit function of bUseFixedFrameRate of the known form'
    gs = engine_global(img, text_end, ro_off, data_off, rel)
    if not gs: return None, None, None, f'UEngine::FixedFrameRate at +{f:#x}: no engine creation (the "GameEngine" key, a store, its reload and a virtual call) names GEngine'
    if len(gs) > 1: return None, None, None, '[RED] UEngine::FixedFrameRate: the engine creation names ' + ', '.join(f'{g:#x} (at {", ".join(f"{a:#x}" for a in at)})' for g, at in sorted(gs.items())) + ': more than one global'
    g, at = gs.popitem()
    return g, f, fl[0] << 8 | fl[1], (f'UEngine::FixedFrameRate at [{g:#x}]+{f:#x}, used while bit {fl[1]:#x} of +{fl[0]:#x} is set (GEngine, stored where the engine is made at '
                                      + ', '.join(f'{a:#x}' for a in at) + f'; reflection entry {min(fs):#x})')

if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nso
    for path in sys.argv[1:]:
        try: m = nso.read(path)
        except nso.Error as e: print(f'{path}\t-\t{e}'); continue
        g, f, fl, why = find(bytes(m.image), m.text_end, m.segments[1][0], m.segments[2][0])
        print(f'{path}\t{m.build_id[:16].hex()}\t{why}')
