#!/usr/bin/env python3
"""Snowdrop's three-pool page allocator in main, found by the engine's own code, without a build ID: patch_nso.py gives its pools the sizes the profile decides on (snowdrop_pools).
The constructor reserves 12 GB in one address region, three adjacent 4 GB ranges: large allocations, then 512 KB pages, then 64 KB pages. Only committed pages use memory, but an
iPhone's address space cannot hold 12 GB, and the failed reservation is unchecked: the first 64 KB page writes to NULL + 0x20000ffff. Each range is sized from the constants in this
one function, so the cut is a platform limit that nothing outside the game's code can make: pointers are real addresses, and the layout comes from these instructions.

The structure, every part checked on each candidate or nothing is cut (Mario + Rabbids Kingdom Battle and Sparks of Hope build it with other encodings, field offsets and callees):
- mov xA, #4 GB, stored to three adjacent fields F, F+8, F+0x10 of the object (x0, or xS after mov xS, x0), the sizes of the 64 KB, 512 KB and large ranges;
- bl to the size of the metadata; then mov w1, wzr; mov x2, xzr; bl R, the reservation of the metadata, whose base goes to [xS, #F+0x28];
- mov xB, #12 GB; mov w1, wzr; mov x0, #12 GB; mov x2, xzr; bl R, the reservation of the ranges, whose base and end go to [xS, #F+0x18], the end being an add of xB;
- mov w5, #0x10000, the large allocator's maximum 64 KB page count, in the arguments of the next bl. It is independent of its range and must shrink with it: otherwise the large
  allocator crosses into the 512 KB pool, and free reads file data as a slab owner.
R takes the size in x0 alone: it overwrites x1, and the region call it makes takes two arguments. So both calls' mov w1, wzr and mov x2, xzr are free for the two sizes that differ
from the shared one: mov x1, #large + str x1, [xS, #F+0x10], and likewise the 64 KB range at F.

A pool that outgrows its range does not fail cleanly: the slab allocator's fallback asks R for 4 GB more, which does not fit, and retries without end; each failed attempt leaks a
48-byte node of the SDK's region manager until its area is full and the SDK aborts (sdk+0x37d1ac on SDK 13.3.5, with main+0x164b40 in the backtrace; GitHub issue #1010). The pool
sizes are the profile's decision, because they are measured against each game's regions; DEFAULT serves a game with no such key.

Usage on its own: snowdrop_pools.py <module>... (what the search finds in each)"""
import os, struct, sys

G4, G12 = 0x100000000, 0x300000000
W1_ZERO, X2_ZERO = 0x2A1F03E1, 0xAA1F03E2   # mov w1, wzr; mov x2, xzr
W5_64K = (0x52A00025, 0x321003E5)           # mov w5, #0x10000: movz, or orr w5, wzr, #0x10000
WINDOW = 48                                 # instructions from the first constant to the last call: 27 and 32 in the two builds
KEY = 'snowdrop_pools'                      # the profile key: the large, 512 KB and 64 KB ranges in MB
DEFAULT = (2048, 512, 1280)


def load_imm(w):
    """(rd, value) of a 64-bit register loaded from zero by one instruction, for the two encodings of these constants, else None"""
    if w & 0xFFE00000 == 0xD2C00000: return w & 31, ((w >> 5) & 0xFFFF) << 32   # movz x, #imm, lsl #32
    if w & 0xFFC003E0 == 0xB24003E0:   # orr x, xzr, #bitmask with a 64-bit element
        immr, imms = (w >> 16) & 63, (w >> 10) & 63
        v = (1 << (imms + 1)) - 1 if imms < 63 else (1 << 64) - 1
        return w & 31, ((v >> immr) | (v << (64 - immr))) & ((1 << 64) - 1)
    return None
def loaded(w, value): return (load_imm(w) or (None, None))[1] == value
def is_bl(w): return w & 0xFC000000 == 0x94000000
def bl_target(w, pc):
    imm = w & 0x3FFFFFF
    return pc + (imm - (1 << 26) if imm & (1 << 25) else imm) * 4

def str_fields(w, rt):
    """(base register, [offsets]) of str xT, [xN, #o] or stp xT, xT, [xN, #o], for rt = T; else None"""
    if w & 0xFFC00000 == 0xF9000000 and w & 31 == rt: return (w >> 5) & 31, [((w >> 10) & 0xFFF) * 8]
    if w & 0xFFC00000 == 0xA9000000 and w & 31 == rt and (w >> 10) & 31 == rt:
        imm = (w >> 15) & 0x7F
        o = (imm - 0x80 if imm & 0x40 else imm) * 8
        return (w >> 5) & 31, [o, o + 8]
    return None

def stores_pair_at(w, rn, off):
    """stp x?, x?, [xN, #off]"""
    if w & 0xFFC00000 != 0xA9000000 or (w >> 5) & 31 != rn: return False
    imm = (w >> 15) & 0x7F
    return (imm - 0x80 if imm & 0x40 else imm) * 8 == off

def movz_x(rd, value):
    """mov xD, #value for a multiple of 64 KB under 4 GB"""
    assert value & 0xFFFF == 0 and 0 < value < 1 << 32, hex(value)
    return 0xD2A00000 | (value >> 16) << 5 | rd
def str_x(rt, rn, off):
    assert off % 8 == 0 and 0 <= off < 0x8000, hex(off)
    return 0xF9000000 | (off // 8) << 10 | rn << 5 | rt
def movz_w5(n):
    assert 0 < n < 1 << 16, n
    return 0x52800005 | n << 5


def check(W, a, k):
    """The sites of the structure with its first constant at word a and the page count at word k; (None, why not) when a part differs"""
    calls = [j for j in range(a, min(k + 8, len(W))) if is_bl(W[j])]
    if len(calls) < 4 or not calls[2] < k < calls[3]: return None, 'not four calls with the page count before the last'
    b0, b1, b2, b3 = calls[:4]
    xa = W[a] & 31
    g12 = [j for j in range(b1, b2) if loaded(W[j], G12)]
    if len(g12) != 2 or W[g12[1]] & 31 != 0: return None, 'the second reservation is not 12 GB from x0'
    xb = W[g12[0]] & 31
    if not 19 <= xb <= 28: return None, 'the 12 GB end is not in a saved register'
    # the object's register: mov xS, x0, before the first call
    movs = [W[j] & 31 for j in range(max(0, a - 8), b0) if W[j] & 0xFFFFFFE0 == 0xAA0003E0 and 19 <= W[j] & 31 <= 28]
    if len(movs) != 1: return None, f'{len(movs)} moves of the object to a saved register'
    xs = movs[0]
    # the three size fields
    fields = set()
    for j in range(a + 1, b0):
        s = str_fields(W[j], xa)
        if s and s[0] in (0, xs): fields.update(s[1])
    f = min(fields, default=0)
    if fields != {f, f + 8, f + 0x10}: return None, f'the 4 GB goes to fields {sorted(hex(x) for x in fields)}'
    # the two reservations and their free argument moves
    targets = {bl_target(W[b1], b1 * 4), bl_target(W[b2], b2 * 4)}
    if len(targets) != 1: return None, 'the two reservations call different functions'
    args = []
    for lo, hi in ((b0, b1), (b1, b2)):
        w1, x2 = [j for j in range(lo, hi) if W[j] == W1_ZERO], [j for j in range(lo, hi) if W[j] == X2_ZERO]
        if len(w1) != 1 or len(x2) != 1 or w1[0] > x2[0]: return None, 'a reservation without mov w1, wzr then mov x2, xzr'
        args += [w1[0], x2[0]]
    if not any(stores_pair_at(W[j], xs, f + 0x28) for j in range(b1, b2)): return None, "the metadata's base does not go beside the fields"
    if not any(stores_pair_at(W[j], xs, f + 0x18) for j in range(b2, b3)): return None, "the region's base does not go beside the fields"
    if not any(W[j] & 0xFFE0FC00 == 0x8B000000 and (W[j] >> 16) & 31 == xb for j in range(b2, b3)): return None, "the region's end is not an add of the 12 GB register"
    # R overwrites x1 before it calls on: its incoming x1 and x2 do not count
    r = targets.pop() // 4
    if not 0 <= r < len(W) - 12: return None, 'the reservation function is out of the text'
    end = next((j for j in range(r, r + 12) if is_bl(W[j])), None)
    if end is None or not any(W[j] & 0xFFE0FFFF == 0xAA0003E1 for j in range(r, end)): return None, 'the reservation function does not overwrite x1'
    return {'sizes': a, 'end': g12[0], 'total': g12[1], 'pages': k, 'w1_meta': args[0], 'x2_meta': args[1], 'w1': args[2], 'x2': args[3], 'field': f, 'xs': xs}, ''


def find(img, text_end):
    """(sites, why): the instruction offsets of the one constructor in the text and a line for the log; (None, why) otherwise, with why starting '[RED] ' when the shape is there
    but something differs, or is there twice"""
    n = text_end // 4
    W = struct.unpack_from(f'<{n}I', img)
    cands = []
    for pat in W5_64K:
        needle, at = struct.pack('<I', pat), -1
        while (at := img.find(needle, at + 1, text_end)) >= 0:
            if at % 4: continue
            k = at // 4
            for a in range(k - 1, max(-1, k - WINDOW), -1):
                if loaded(W[a], G4):
                    if sum(loaded(W[j], G12) for j in range(a, k)) >= 2: cands.append((a, k))
                    break
    if not cands: return None, 'no Snowdrop three-pool allocator'
    good, bad = [], []
    for a, k in cands:
        site, why = check(W, a, k)
        (good if site else bad).append(site if site else (a * 4, why))
    if len(good) > 1: return None, f'[RED] {len(good)} Snowdrop three-pool allocators: ' + ', '.join(f'{s["sizes"] * 4:#x}' for s in good)
    if not good: return None, '[RED] Snowdrop three-pool allocator shape, but ' + '; '.join(f'{o:#x}: {w}' for o, w in bad)
    return {k: v * 4 if k not in ('field', 'xs') else v for k, v in good[0].items()}, f'Snowdrop three-pool allocator at {good[0]["sizes"] * 4:#x}, size fields at +{good[0]["field"]:#x}'


def parse(words, where):
    """(large, 512 KB, 64 KB) in MB from a profile's words; a key that cannot be written stops the rewrite"""
    try:
        sizes = tuple(int(x, 0) for x in words)
        if len(sizes) != 3 or min(sizes) <= 0 or sum(sizes) >= 4096: raise ValueError
    except ValueError:
        raise SystemExit(f'[RED] {where}: {KEY} takes the large, 512 KB and 64 KB ranges in MB, under 4 GB in all: {" ".join(words)}') from None
    return sizes


def profile_sizes(env=os.environ):
    """(sizes, where they come from): the selected profile's key, else DEFAULT"""
    name = env.get('DM_GAME')
    if not name: return DEFAULT, 'default, no DM_GAME'
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../games'))
    import game
    path = game.find_dir(name) / 'game.tsv'
    if not path.is_file(): return DEFAULT, 'default, no profile'
    words = game.Game(path.parent).words(KEY, '')
    return (parse(words, path), 'profile') if words else (DEFAULT, f'default, the profile has no {KEY}')


def cuts(img, s, sizes):
    """The rewriter's cuts, (offset, old hex, new hex, why), for the sites of find() and the sizes in MB"""
    large, p512, p64 = (x << 20 for x in sizes)
    total = large + p512 + p64
    xa, xb = struct.unpack_from('<I', img, s['sizes'])[0] & 31, struct.unpack_from('<I', img, s['end'])[0] & 31
    new = [(s['sizes'], movz_x(xa, p512), f'each range {sizes[1]} MB; the two that differ are set below'),
           (s['end'], movz_x(xb, total), f'combined range end {total >> 20} MB'),
           (s['w1'], movz_x(1, large), 'the large range; the reservation discards incoming x1'),
           (s['total'], movz_x(0, total), f'combined reservation {total >> 20} MB'),
           (s['x2'], str_x(1, s['xs'], s['field'] + 0x10), f'the large range field {sizes[0]} MB; the reservation never reads x2'),
           (s['pages'], movz_w5(large >> 16), f'the large allocator\'s maximum 64 KB page count, matching its {sizes[0]} MB')]
    if p64 != p512:
        new += [(s['w1_meta'], movz_x(1, p64), 'the 64 KB range; the reservation discards incoming x1'),
                (s['x2_meta'], str_x(1, s['xs'], s['field']), f'the 64 KB range field {sizes[2]} MB; the reservation never reads x2')]
    return [(o, bytes(img[o:o + 4]).hex(), struct.pack('<I', w).hex(), f'Snowdrop allocator: {why}') for o, w, why in sorted(new)]


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nso
    for path in sys.argv[1:]:
        m = nso.cli_read(path)
        sites, why = find(bytes(m.image), m.segments[0][0] + m.segments[0][1])
        print(f'{path}: {why}')
        if sites:
            for o, ob, nb, r in cuts(m.image, sites, DEFAULT): print(f'  {o:#x} {ob} -> {nb}  {r}')
