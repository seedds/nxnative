#!/usr/bin/env python3
"""Ubisoft's Scimitar / Anvil .forge archives on the Switch:
· Version 28 (Anvil, Immortals Fenyx Rising): 'scimitar\\0', u32 version, u64 data header offset; the data header holds u32 count at +0 and u64 first index header at +0x24; each chained
  index header {u32 count, u32, u64 index table offset, u64 next or -1} points at 20-byte rows {u64 offset, u64 file ID, u32 size}. Each entry is one or more sequential compressed
  containers: magic 33 AA FB 57 99 FA 04 10, u16 version, u8 compression, u32 chunk size, u32 chunk count, {u32 raw size, u32 stored size} per chunk, then per chunk u32 checksum and stored
  bytes; equal sizes are stored, the others are Oodle. Shaders inside are 'GPUCODEONLY\\0' records (shader_split.py anvil_shaders).
· Versions 25 and 31 (Scimitar, Assassin's Creed: The Ezio Collection: 31 for AC II in AC2/, 25 for Brotherhood in its first DLC's ACB/): 'scimitar\\0', u32 version, i64 file data header
  offset {u32 entry count, u32, i64, i64 -1, u32 most entries (4999 or 5000), u32, i64 first table offset at +0x20}; each chained table {u32 entry count, u32, i64 index offset, i64 next (-1),
  u32 first, u32 last, i64 name table offset, i64 raw data offset} points at 16-byte index rows {i64 offset, u32 id, u32 size: what follows the entry's 0x1b8-byte FILEDATA header} and
  0xbc-byte name records (NUL-terminated name at +0x2c). Each entry's compressed blocks have magic 33 AA FB 57 99 FA 04 10, u16 version 1, u8 type (4 Mermaid, 6 Leviathan), u16 0x8000,
  u16 0x8000, u16 chunk count, {u16 raw size, u16 stored size} per chunk, then per chunk u32 checksum and stored bytes; equal sizes are stored, the others are Oodle. The decoded material
  templates hold NVN control sections (+0x20 its length, 0x880) each followed directly by its code area (length at control +0x704).
Used by shader_split.py (forge_jobs, forge_shaders)."""
import struct

MAGIC, BLOCK = b'scimitar\0', bytes.fromhex('33aafb5799fa0410')
SCIMITAR_VERSIONS = frozenset((25, 31))
ANVIL_VERSIONS = frozenset((28,))
KNOWN_VERSIONS = SCIMITAR_VERSIONS | ANVIL_VERSIONS
OODLE = {4: 'Mermaid', 6: 'Leviathan'}   # v25/v31 block types: Oodle compressors (stored chunks in either)
NAME_REC, NAME_AT = 0xbc, 0x2c
ENTRY_HEAD = 0x1b8   # v25/v31 entry FILEDATA header, which the index's size leaves out
CTL_MAGIC, GFX_MAGIC, CS_MAGIC = bytes.fromhex('34127698'), bytes.fromhex('78563412'), bytes.fromhex('79563412')


def is_forge(head): return head[:len(MAGIC)] == MAGIC


def version(d, what='forge'):
    """The u32 forge format version after the 'scimitar\\0' magic."""
    if not is_forge(d[:len(MAGIC)]): raise ValueError(f'{what}: not a Scimitar/Anvil forge (magic {bytes(d[:9]).hex()})')
    if len(d) < 13: raise ValueError(f'{what}: the header runs past the end')
    return struct.unpack_from('<I', d, 9)[0]


def entries(d, what='forge'):
    """A .forge file's entries in index order:
    · versions 25 / 31: [(offset, size including the 0x1b8 FILEDATA header, name)]
    · version 28: [(offset, size, file ID)]
    Raises ValueError on any unknown version or out-of-bounds table/entry."""
    ver = version(d, what)
    if len(d) < 0x15: raise ValueError(f'{what}: the header runs past the end')
    if ver in SCIMITAR_VERSIONS:
        hdr = struct.unpack_from('<q', d, 13)[0]
        if not 0x15 <= hdr <= len(d) - 0x28: raise ValueError(f'{what}: the file data header at {hdr:#x} is outside the file')
        count, tab = struct.unpack_from('<I', d, hdr)[0], struct.unpack_from('<q', d, hdr + 0x20)[0]
        out, seen = [], set()
        while tab != -1:
            if tab in seen or not 0 <= tab <= len(d) - 0x30: raise ValueError(f'{what}: a table at {tab:#x} is outside the file or read twice')
            seen.add(tab)
            n, _, idx, nxt, _, _, names, _ = struct.unpack_from('<IIqqIIqq', d, tab)
            if not (0 <= idx and idx + 16 * n <= len(d) and 0 <= names and names + NAME_REC * n <= len(d)):
                raise ValueError(f'{what}: the {n} entries of the table at {tab:#x} run past the end')
            for i in range(n):
                off, _, size = struct.unpack_from('<qII', d, idx + 16 * i); size += ENTRY_HEAD
                if not 0 <= off <= len(d) - size: raise ValueError(f'{what}: entry {len(out)} ({off:#x}, {size:#x} bytes) is outside the file')
                rec = d[names + NAME_REC * i:names + NAME_REC * (i + 1)]
                out.append((off, size, bytes(rec[NAME_AT:]).split(b'\0')[0].decode('latin1')))
            tab = nxt
        if len(out) != count: raise ValueError(f'{what}: {len(out)} entries in the tables, the header counts {count}')
        return out
    if ver in ANVIL_VERSIONS:
        hdr = struct.unpack_from('<Q', d, 13)[0]
        if not 0x15 <= hdr <= len(d) - 0x2c: raise ValueError(f'{what}: the data header at {hdr:#x} is outside the file')
        count, ih = struct.unpack_from('<I', d, hdr)[0], struct.unpack_from('<Q', d, hdr + 0x24)[0]
        out, seen = [], set()
        while ih != 0xffffffffffffffff:
            if ih in seen or not 0 <= ih <= len(d) - 0x18: raise ValueError(f'{what}: index header {ih:#x} is outside the file or chained twice')
            seen.add(ih)
            cnt, _, tab, nxt = struct.unpack_from('<IIQQ', d, ih)
            if not (0 <= tab and tab + 20 * cnt <= len(d)): raise ValueError(f'{what}: index table at {tab:#x} runs past the end')
            for i in range(cnt):
                off, fid, size = struct.unpack_from('<QQI', d, tab + 20 * i)
                if not (0 <= off and size <= len(d) - off): raise ValueError(f'{what}: entry {len(out)} ({off:#x}, {size:#x} bytes) is outside the file')
                out.append((off, size, fid))
            ih = nxt
        if len(out) != count: raise ValueError(f'{what}: the index tables list {len(out)} files, the data header {count}')
        return out
    raise ValueError(f'{what}: forge version {ver} is not known (known: {sorted(KNOWN_VERSIONS)})')


def blocks(d, off, size, what='entry'):
    """The v25/v31 compressed blocks of the entry at off: [(type, [(raw size, stored size, offset of the stored bytes)])]"""
    if not (0 <= off and 0 <= size <= len(d) - off): raise ValueError(f'{what}: {size:#x} bytes at {off:#x} run past the forge')
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


def read(d, off, size, decode, what='entry', version=31):
    """The entry's compressed blocks/containers decoded, one bytes each; decode(compressed, size) is the game's Oodle."""
    if version in ANVIL_VERSIONS:
        if not (0 <= off and 0 <= size <= len(d) - off): raise ValueError(f'{what}: {size} bytes at {off:#x} run past the forge')
        p, end, out = off, off + size, []
        while p < end and d[p:p + 8] == BLOCK:
            if p + 19 > end: raise ValueError(f'{what}: a data container header at {p - off:#x} runs past the entry')
            n = struct.unpack_from('<I', d, p + 15)[0]
            q = p + 19 + 8 * n
            if q > end: raise ValueError(f'{what}: the chunk table of the container at {p - off:#x} runs past the entry')
            b = bytearray()
            for i in range(n):
                raw, stored = struct.unpack_from('<II', d, p + 19 + 8 * i)
                if q + 4 + stored > end: raise ValueError(f'{what}: chunk {i} of the container at {p - off:#x} runs past the entry')
                c = bytes(d[q + 4:q + 4 + stored]); q += 4 + stored
                got = c if raw == stored else decode(c, raw)
                if len(got) != raw: raise ValueError(f'{what}: a container chunk decoded to {len(got):#x} bytes, not {raw:#x}')
                b += got
            out.append(bytes(b)); p = q
        return out
    if version not in SCIMITAR_VERSIONS:
        raise ValueError(f'{what}: forge version {version} is not known (known: {sorted(KNOWN_VERSIONS)})')
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
    """The NVN shaders in a decoded Scimitar block: [(offset, control section, code area)] and [(offset, why)] for control sections without their code area. A control magic whose length
    (+0x20) is no control section's (0x760..0x880, shader_split.py ctl_len) is other data and passed over"""
    out, bad, c = [], [], b.find(CTL_MAGIC)
    while c >= 0:
        if c + 0x708 <= len(b) and 0x760 <= (nctl := struct.unpack_from('<I', b, c + 0x20)[0]) <= 0x880:
            n, k = struct.unpack_from('<I', b, c + 0x704)[0], c + nctl
            if k <= len(b) and b[k:k + 4] in (GFX_MAGIC, CS_MAGIC) and k + n <= len(b):
                out.append((c, b[c:k], b[k:k + n])); c = b.find(CTL_MAGIC, k + n); continue
            bad.append((c, f'no code area of {n:#x} bytes right after its {nctl:#x}-byte control section'))
        c = b.find(CTL_MAGIC, c + 4)
    return out, bad
