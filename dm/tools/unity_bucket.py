#!/usr/bin/env python3
"""Unity's ALLOC_BUCKET BucketAllocator, found in main by its name without a build ID: patch_nso.py cuts its block limit when its reservation is larger than an iPhone can spare.
Unity's memory manager setup constructs the bucket allocator with its name in x1, the granularity in w2 (16), the bucket count in w3, the block size in w4 (4 MB) and the most
blocks in w5, then calls the constructor (bl), which reserves (blocks + 1) x block size of address space at once. Unity 2019 passes 255 blocks, a 1 GB region (Subnautica 1.23.83358
and Subnautica: Below Zero 1.23.55063, Unity 2019.4: the opening commits 20 blocks, and AddMoreBuckets returns false at the limit, so the allocation goes to Unity's other allocators:
the limit sizes only the reservation); Unity 2020.3 and later pass 1 (Fire Emblem Engage, Patrick's Parabox, Cult of the Lamb, Super Mario RPG and the others on the drives,
2026-11-09). On a Switch the alias region is 64 GB, on an iPhone 4.375 GB: the limit becomes the most blocks whose reservation fits 256 MB (63 of 4 MB). Platform-limit cut.
Unity 2021 and later pass the sizes in registers from the player's settings (boot.config, one block by default: Super Mario RPG, Monster Train 2, Silksong): no cut, a plain reason.
It fails closed: the name with more than one reference, or a branch before the call, is a [RED] reason and no cut; no name or a small limit is a plain one.
Usage on its own: unity_bucket.py <module>... (what it finds in each)"""
import struct, sys, os

NAME = b'ALLOC_BUCKET'
CAP = 256 << 20   # the reservation an iPhone's alias region can spare for it

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ue_cvar import adrp_page, is_adrp, add_imm, writes

def movz32(u):   # mov wD, #imm (movz, 32-bit): (d, value), or None
    if (u & 0xFF800000) != 0x52800000: return None
    return u & 31, ((u >> 5) & 0xFFFF) << (16 * ((u >> 21) & 3))

def find(img, text_end, ro_off, data_off):
    """((offset, original bytes hex, new bytes hex, reason) of the cut, or None, reason); the reason starts '[RED] ' when the setup is there but not read"""
    word = lambda a: struct.unpack_from('<I', img, a)[0]
    s = img.find(b'\0' + NAME + b'\0', ro_off, data_off) + 1
    if s <= 0: return None, 'no ALLOC_BUCKET'
    refs = []
    for j in range(0, text_end, 4):
        a = add_imm(word(j))
        if not a or a[2] != (s & 0xFFF): continue
        if any(is_adrp(word(i)) and word(i) & 31 == a[1] and adrp_page(word(i), i) == s & ~0xFFF for i in range(max(0, j - 20), j, 4)): refs.append((j, a[0]))
    if len(refs) != 1: return None, f'[RED] ALLOC_BUCKET: {len(refs)} references, not one'
    j, d = refs[0]
    if d != 1: return None, f'[RED] ALLOC_BUCKET at {j:#x}: the name is not loaded into x1'
    last = {}   # register → (offset, value) of its last mov before the call
    for t in range(j + 4, min(text_end, j + 4 + 4 * 16), 4):
        u = word(t)
        if (u & 0xFC000000) == 0x94000000: break                                      # bl the constructor
        if (u & 0x7C000000) == 0x14000000 or (u & 0xFFFFFC1F) in (0xD63F0000, 0xD61F0000) or u == 0xD65F03C0:
            return None, f'[RED] ALLOC_BUCKET at {j:#x}: a branch before the constructor call'
        m = movz32(u)
        for r in (4, 5):
            if writes(u, r): last[r] = (t, m[1]) if m and m[0] == r else (t, None)
    else: return None, f'[RED] ALLOC_BUCKET at {j:#x}: no constructor call'
    if 4 not in last or 5 not in last or last[4][1] is None or last[5][1] is None:   # Unity 2021 and later: the sizes come from the player's settings (boot.config), one block by default
        return None, f'ALLOC_BUCKET at {j:#x}: sizes set at run time, no cut'
    size, (at, blocks) = last[4][1], last[5]
    if not size or size & (size - 1): return None, f'[RED] ALLOC_BUCKET at {j:#x}: block size {size:#x}'
    want = CAP // size - 1
    if blocks <= want: return None, f'ALLOC_BUCKET: {blocks} blocks of {size >> 20} MB, no cut'
    old, new = word(at), 0x52800005 | want << 5
    return ((at, struct.pack('<I', old).hex(), struct.pack('<I', new).hex(),
             f'Unity BucketAllocator ALLOC_BUCKET: at most {blocks} blocks of {size >> 20} MB (a {(blocks + 1) * size >> 20} MB address region) → {want} ({CAP >> 20} MB) (unity_bucket.py)'),
            f'Unity ALLOC_BUCKET at {j:#x}: {blocks} → {want} blocks of {size >> 20} MB at {at:#x}')

if __name__ == '__main__':
    import nso
    for path in sys.argv[1:]:
        try: m = nso.read(path)
        except nso.Error as e: print(f'{path}\t-\t{e}'); continue
        cut, why = find(bytes(m.image), m.text_end, m.segments[1][0], m.segments[2][0])
        print(f'{path}\t{m.build_id[:16].hex()}\t{why}')
