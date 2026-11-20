"""KIT archives (Unravel TWO): an index file '<name>.kit' and its data in numbered parts '<name>.kit.0', '<name>.kit.1', ...
Index: magic 'kit!', +0x10 u64 the directory's offset after the 0x25-byte header, +0x18 u32 its record count; 56-byte records of
u32 flags (8: chained LZ4 blocks), u16 part, u32 offset, u16 high offset word, u64 stored size, u64 decoded size, the decoded data's SHA1 and two u32
not read here. Members are named only by hash. A compressed member is a run of {u32 length, LZ4 block} that decode to 64 KB each (the last one
shorter), each block's matches reaching into the 64 KB decoded before it. Format reference: Luigi Auriemma's Unravel KIT reader
(https://aluigi.altervista.org/bms/unravel.bms). Files are read through romfs.open, so archives inside a RomFS image need no extraction.
Every malformed index or member raises ValueError naming it.
"""
import hashlib
import struct
from typing import NamedTuple
from romfs import os, open

MAGIC = b'kit!'
CHUNK = 0x10000
HEADER = 0x25
RECORD = struct.Struct('<IHIHQQ20sII')


class Entry(NamedTuple):
    index: int        # the record's number in the index
    part: int         # the data is in <index path>.<part>
    offset: int       # where in that part
    length: int       # bytes it takes there: the stored size when compressed, else the decoded size
    size: int         # decoded size
    compressed: bool  # chained LZ4 blocks
    sha1: bytes       # of the decoded data


def entries(path):
    """The index's members, each checked to lie inside its part (and the part to exist)"""
    with open(path, 'rb') as f: d = f.read()
    if len(d) < HEADER or d[:4] != MAGIC: raise ValueError(f'{path}: incomplete KIT header')
    base = HEADER + struct.unpack_from('<Q', d, 0x10)[0]
    count = struct.unpack_from('<I', d, 0x18)[0]
    if base > len(d) or count > (len(d) - base) // RECORD.size: raise ValueError(f'{path}: KIT directory is outside the index')
    out, sizes = [], {}
    for i in range(count):
        flags, part, off, high, stored, size, sha, _, _ = RECORD.unpack_from(d, base + RECORD.size * i)
        if high: raise ValueError(f'{path}: KIT entry {i} has an unsupported high offset word')
        if part not in sizes:
            shard = f'{path}.{part}'
            if not os.path.isfile(shard): raise ValueError(f'{path}: KIT entry {i} has no part {part}')
            sizes[part] = os.path.getsize(shard)
        n = stored if flags & 8 else size
        if off > sizes[part] or n > sizes[part] - off: raise ValueError(f'{path}: KIT entry {i} is outside part {part}')
        out.append(Entry(i, part, off, n, size, bool(flags & 8), sha))
    return out


def read(path, entry):
    """One member's decoded data: every LZ4 block decodes to its full length, the stored bytes are used up exactly, and the SHA1 matches"""
    import lz4.block
    i, part, off, n, size, compressed, sha = entry
    label = f'{path}#{i}'
    with open(f'{path}.{part}', 'rb') as f: f.seek(off); d = f.read(n)
    if len(d) != n: raise ValueError(f'{label}: truncated KIT member')
    if compressed:
        out, pos = bytearray(), 0
        while pos + 4 <= len(d) and len(out) < size:
            stored = struct.unpack_from('<I', d, pos)[0]; pos += 4
            want = min(CHUNK, size - len(out))
            if not 0 < stored <= len(d) - pos: raise ValueError(f'{label}: LZ4 block is outside the KIT member')
            try: block = lz4.block.decompress(d[pos:pos + stored], uncompressed_size=want, dict=bytes(out[-CHUNK:]))
            except lz4.block.LZ4BlockError as e: raise ValueError(f'{label}: LZ4 block at {pos - 4:#x}: {e}') from None
            if len(block) != want: raise ValueError(f'{label}: LZ4 block makes {len(block)} of {want} bytes')
            out += block; pos += stored
        if pos != len(d) or len(out) != size: raise ValueError(f'{label}: incomplete KIT LZ4 stream ({len(out)} of {size} bytes, {pos} of {len(d)} stored)')
        d = bytes(out)
    if hashlib.sha1(d).digest() != sha: raise ValueError(f'{label}: KIT SHA1 mismatch')
    return d
