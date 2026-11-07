#!/usr/bin/env python3
"""Unreal Engine's fixed frame rate, UEngine::FixedFrameRate on the engine object GEngine points to, found in main without a build ID: patch_nso.py writes its place into main's fixups
(UE_FIXED_RATE_PTR, GEngine's offset in main, and UE_FIXED_RATE_FIELD, the property's offset in UEngine), and the host writes the profile's fps_target there (hle_fpslock.c).
With bUseFixedFrameRate on, the engine steps every frame by exactly 1 / FixedFrameRate seconds and holds its tick to that rate, whatever rhi.SyncInterval and t.MaxFPS say
(ue_cvar.py). Unreal's Switch frame pace sets both: DRAGON BALL: Sparking! ZERO (UE 5.1.1, main+0x1e1e904) caches rhi.SyncInterval, sets it to 1 for its 60 fps mode and 2 for the others,
then stores {30, 24, 60}[mode] into GEngine->FixedFrameRate; its config turns the fixed frame rate on (bUseFixedFrameRate=True, FixedFrameRate=30), so its battles stayed at 30 with
the present interval raised. The value is the frame rate, so the game's speed holds at any rate, as far as the game's own code steps by the engine's delta time.
The chain, each link unique or nothing is written:
- the property's reflection entry: the data word holding the address of its UTF-8 name "FixedFrameRate" (the module's relative relocations applied; the linker may share the string with the
  tail of "bUseFixedFrameRate"), whose generated-property kind is Float (0x0A at +24) and whose array size is 1 (+32). The offset follows the array size (+36, Unreal Engine 4.22 to 5.0)
  or, from Unreal Engine 5.1 on, a setter and a getter pointer after it (+56): Sparking! ZERO's is 0x9c8, at +56;
- GEngine: the one global through which main's code stores a float at that offset (str sT, [xB, #field], xB loaded from the global: fps_target.globals_of), the engine or the game
  setting the rate (Sparking! ZERO's frame pace, main+0x1e1ea04). A load alone proves nothing: Pikmin 4 (UE 4.26, the property at +0x7a8) has one through main+0x9350560, whose object
  holds -980 there (a gravity), not its engine's 30; the offset only coincides. Such a main gets no fixups, and the host writes nothing.
Usage on its own: ue_fixed_rate.py <module>... (what it finds in each)"""
import struct, sys, os

NAME = b'FixedFrameRate\0'
FLOAT = 0x0A   # EPropertyGenFlags::Float (Unreal Engine 4.22 and later)

def relocated(img):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from loader_census import got_values
    return got_values(img)

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

def find(img, text_end, ro_off, data_off):
    """(GEngine's offset in main, FixedFrameRate's offset in UEngine, reason), or (None, None, why not); why starts '[RED] ' when more than one global stores the property. A main whose
    code stores it through no global (Princess Peach: Showtime!, Pikmin 4) gets no fixups, and the host has nothing to write"""
    rel = relocated(img)
    fs = fields(img, ro_off, data_off, rel)
    if not fs: return None, None, 'no UEngine::FixedFrameRate'
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from fps_target import globals_of
    global_of = globals_of(img)
    want = set(fs.values()); word = lambda a: struct.unpack_from('<I', img, a)[0]
    users = {}
    for i in range(0, text_end, 4):
        u = word(i)
        if (u & 0xFFC00000) != 0xBD000000: continue   # str sT, [xB, #imm * 4]
        f = ((u >> 10) & 0xFFF) * 4
        if f not in want: continue
        g = global_of(i, (u >> 5) & 31)
        if g is not None and g >= data_off: users.setdefault((g, f), []).append(i)
    if not users: return None, None, f'UEngine::FixedFrameRate at +{min(want):#x}: no code stores it through a global (GEngine), so the object is not known'
    if len(users) > 1: return None, None, '[RED] UEngine::FixedFrameRate: ' + ', '.join(f'[{g:#x}]+{f:#x} ({len(a)})' for (g, f), a in sorted(users.items())) + ': stored through more than one global'
    (g, f), at = users.popitem()
    return g, f, f'UEngine::FixedFrameRate at [{g:#x}]+{f:#x} (GEngine; reflection entry {min(o for o, v in fs.items() if v == f):#x}; stored at ' + ', '.join(f'{a:#x}' for a in at[:4]) + ')'

if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nso
    for path in sys.argv[1:]:
        try: m = nso.read(path)
        except nso.Error as e: print(f'{path}\t-\t{e}'); continue
        g, f, why = find(bytes(m.image), m.text_end, m.segments[1][0], m.segments[2][0])
        print(f'{path}\t{m.build_id[:16].hex()}\t{why}')
