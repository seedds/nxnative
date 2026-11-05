#!/usr/bin/env python3
"""Ubisoft's Scimitar (Anvil) archives on the Switch (Assassin's Creed: The Ezio Collection: Assassin's Creed II in the RomFS's AC2/, Brotherhood in its first DLC's ACB/): the .forge files,
their entries' compressed blocks, and the NVN shaders of the material templates inside. Used by shader_split.py (forge_jobs, forge_shaders) and dm/games/profile_guess.py (is_forge).

· A .forge file is 'scimitar\\0', u32 version (31 for AC2's, 25 for Brotherhood's), i64 offset of the file data header {u32 entry count, u32, i64, i64 -1, u32 most entries (4999 or 5000), u32,
  i64 offset of the first table}. A table is {u32 entry count, u32, i64 index offset, i64 next table (-1: none), u32 first, u32 last, i64 name table offset, i64 raw data offset}: an index entry
  is 16 bytes {i64 offset, u32 an id, u32 size: what follows the entry's 0x1b8-byte header}, a name record 0xbc bytes {u32 a size (mostly the index's; 0x1c8 for an entry of 0x1c6 in
  AC2's DataNX_Common.forge, so the index's is the one read), u64 id, …, u32 time at +0x28, the NUL-terminated name at +0x2c}. The index is not in offset order. Most of the file is zero
  padding: AC2's DataNX_TitleScreen.forge holds 160 KB of data in its 1.9 MB.
· An entry is a 0x1b8-byte header ('FILEDATA', its name, padding), then the index's size of bytes: a table of 8-byte items (most often one u32), then compressed blocks: magic
  33 AA FB 57 99 FA 04 10 (0x1004FA9957FBAA33), u16 version 1, u8 type, u16 0x8000, u16 0x8000, u16 chunk count, {u16 raw size, u16 stored size} per chunk, then per chunk a u32 checksum
  and its stored bytes. A chunk whose stored size is its raw size is stored as it is; the others are Oodle, which only the game's own decoder reads (shader_split.py oodle): type 6 Leviathan
  (8C 0C, AC2), type 4 Mermaid (8C 0A, Brotherhood). Two blocks to an entry, a table of its resources and their data; the blocks of all 57,834 entries that have any end exactly at
  +0x1b8 + the index's size (58 have none). Read up to the index's size alone, the blocks of 17,029 small entries begin past it.
· The material templates' data hold NVN shaders compiled offline: a control section (+0x20 its length, 0x880) and right after it the code area (header 0x12345678, its length at control
  +0x704). AC2 1.3's 27 forge files hold 224,124 (20,417 different: 1,514 vertex, 18,903 pixel), Brotherhood's 29 hold 299,174 (26,123 different)."""
import struct

MAGIC, BLOCK = b'scimitar\0', bytes.fromhex('33aafb5799fa0410')
OODLE = {4: 'Mermaid', 6: 'Leviathan'}   # block types: Oodle compressors (stored chunks in either)
NAME_REC, NAME_AT = 0xbc, 0x2c
ENTRY_HEAD = 0x1b8   # an entry's header, which the index's size leaves out
CTL_MAGIC, GFX_MAGIC, CS_MAGIC = bytes.fromhex('34127698'), bytes.fromhex('78563412'), bytes.fromhex('79563412')


def is_forge(head): return head[:len(MAGIC)] == MAGIC


def entries(d, what='forge'):
    """[(offset, size with the header, name)] of a .forge file's entries, in index order"""
    if not is_forge(d[:len(MAGIC)]): raise ValueError(f'{what}: not a Scimitar forge (magic {bytes(d[:9]).hex()})')
    if len(d) < 0x15: raise ValueError(f'{what}: the header runs past the end')
    hdr = struct.unpack_from('<q', d, 13)[0]
    if not 0x15 <= hdr <= len(d) - 0x28: raise ValueError(f'{what}: the file data header at {hdr:#x} is outside the file')
    count, tab = struct.unpack_from('<I', d, hdr)[0], struct.unpack_from('<q', d, hdr + 0x20)[0]
    out, seen = [], set()
    while tab != -1:
        if tab in seen or not 0 <= tab <= len(d) - 0x30: raise ValueError(f'{what}: a table at {tab:#x} is outside the file or read twice')
        seen.add(tab)
        n, _, idx, nxt, _, _, names, _ = struct.unpack_from('<IIqqIIqq', d, tab)
        if not (0 <= idx and idx + 16 * n <= len(d) and 0 <= names and names + NAME_REC * n <= len(d)): raise ValueError(f'{what}: the {n} entries of the table at {tab:#x} run past the end')
        for i in range(n):
            off, _, size = struct.unpack_from('<qII', d, idx + 16 * i); size += ENTRY_HEAD
            if not 0 <= off <= len(d) - size: raise ValueError(f'{what}: entry {len(out)} ({off:#x}, {size:#x} bytes) is outside the file')
            rec = d[names + NAME_REC * i:names + NAME_REC * (i + 1)]
            out.append((off, size, bytes(rec[NAME_AT:]).split(b'\0')[0].decode('latin1')))
        tab = nxt
    if len(out) != count: raise ValueError(f'{what}: {len(out)} entries in the tables, the header counts {count}')
    return out


def blocks(d, off, size, what='entry'):
    """The compressed blocks of the entry at off: [(type, [(raw size, stored size, offset of the stored bytes)])]"""
    p, end, out = off, off + size, []
    while (p := d.find(BLOCK, p, end)) >= 0:
        if p + 17 > end: raise ValueError(f'{what}: a block header at {p - off:#x} runs past the entry')
        ver, typ, _, _, n = struct.unpack_from('<HBHHH', d, p + 8)
        if ver != 1 or typ not in OODLE: raise ValueError(f'{what}: a block at {p - off:#x} of version {ver}, type {typ} (known: version 1, types {sorted(OODLE)})')
        q = p + 17 + 4 * n
        if q > end: raise ValueError(f'{what}: the chunk table of the block at {p - off:#x} runs past the entry')
        chunks = []
        for i in range(n):
            raw, stored = struct.unpack_from('<HH', d, p + 17 + 4 * i)
            if q + 4 + stored > end: raise ValueError(f'{what}: chunk {i} of the block at {p - off:#x} runs past the entry')
            chunks.append((raw, stored, q + 4)); q += 4 + stored
        out.append((typ, chunks)); p = q
    return out


def read(d, off, size, decode, what='entry'):
    """The entry's blocks decoded, one bytes each; decode(compressed, size) is the game's Oodle"""
    out = []
    for typ, chunks in blocks(d, off, size, what):
        b = bytearray()
        for raw, stored, at in chunks:
            c = bytes(d[at:at + stored])
            got = c if raw == stored else decode(c, raw)
            if len(got) != raw: raise ValueError(f'{what}: an Oodle {OODLE[typ]} chunk decoded to {len(got):#x} bytes, not {raw:#x}')
            b += got
        out.append(bytes(b))
    return out


def nvn_shaders(b):
    """The NVN shaders in a decoded block: [(offset, control section, code area)] and [(offset, why)] for control sections without their code area. A control magic whose length
    (+0x20) is no control section's (0x760..0x880, shader_split.py ctl_len) is other data and passed over"""
    out, bad, c = [], [], b.find(CTL_MAGIC)
    while c >= 0:
        if c + 0x708 <= len(b) and 0x760 <= (nctl := struct.unpack_from('<I', b, c + 0x20)[0]) <= 0x880:
            n, k = struct.unpack_from('<I', b, c + 0x704)[0], c + nctl
            if b[k:k + 4] in (GFX_MAGIC, CS_MAGIC) and k + n <= len(b): out.append((c, b[c:k], b[k:k + n])); c = b.find(CTL_MAGIC, k + n); continue
            bad.append((c, f'no code area of {n:#x} bytes right after its {nctl:#x}-byte control section'))
        c = b.find(CTL_MAGIC, c + 4)
    return out, bad
