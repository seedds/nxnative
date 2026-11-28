"""Larian LSPK v13 packages and BSHD shader records (Divinity: Original Sin 2).

The v13 package header is at the end, followed by its size and the LSPK magic.
Its LZ4 file table names stored/zlib/LZ4 members, possibly in numbered parts.
The layout agrees with LSLib's PackageFormat.cs and PackageReader.cs:
https://github.com/Norbyte/lslib/tree/master/LSLib/LS
Only member ranges requested by the caller are read; sound/video packs need no scan.
"""
import struct
import zlib
import lz4.block
from romfs import open, os

MAGIC = b'LSPK'
ENTRY_SIZE = 280
MAX_TABLE = 256 << 20
MAX_MEMBER = 256 << 20


def _range(offset, size, end, what):
    if offset < 0 or size < 0 or offset > end or size > end - offset:
        raise ValueError(f'{what}: range {offset:#x}+{size:#x} exceeds {end:#x} bytes')


def _decode(data, size, method, what):
    if size > MAX_MEMBER: raise ValueError(f'{what}: decoded size exceeds {MAX_MEMBER} bytes')
    if method == 0: out = data
    elif method == 1:
        dec = zlib.decompressobj()
        try: out = dec.decompress(data, size + 1)
        except zlib.error as e: raise ValueError(f'{what}: damaged zlib stream: {e}') from e
        if not dec.eof or dec.unused_data or dec.unconsumed_tail: raise ValueError(f'{what}: incomplete or oversized zlib stream')
    elif method == 2:
        try: out = lz4.block.decompress(data, uncompressed_size=size)
        except lz4.block.LZ4BlockError as e: raise ValueError(f'{what}: damaged LZ4 block: {e}') from e
    else: raise ValueError(f'{what}: unsupported compression {method}')
    if len(out) != size: raise ValueError(f'{what}: decoded size {len(out)} differs from {size}')
    return out


def package(path):
    """None for another format, else (part paths, entries).

    An entry is (name, part path, offset, stored length, decoded length, method).
    Footer recognition is independent of file names; recognized invalid packages fail.
    """
    with open(path, 'rb') as f:
        f.seek(0, 2); size = f.tell()
        if size < 8: return None
        f.seek(size - 8); hsize, magic = struct.unpack('<I4s', f.read(8))
        if magic != MAGIC: return None
        if hsize != 40 or size < hsize: raise ValueError(f'{path}: invalid LSPK header size {hsize}')
        header = size - hsize
        f.seek(header); version, offset, stored, parts, flags, _ = struct.unpack('<IIIHBB', f.read(16))
        if version != 13: raise ValueError(f'{path}: unsupported LSPK version {version}')
        if not parts or flags & 4: raise ValueError(f'{path}: invalid part count or unsupported solid LSPK')
        _range(offset, stored, header, f'{path}: LSPK file table')
        if stored < 4 or stored > MAX_TABLE: raise ValueError(f'{path}: invalid LSPK table size {stored}')
        f.seek(offset); count = struct.unpack('<I', f.read(4))[0]
        if count > MAX_TABLE // ENTRY_SIZE: raise ValueError(f'{path}: LSPK file count {count} is too large')
        table = _decode(f.read(stored - 4), count * ENTRY_SIZE, 2, f'{path}: LSPK file table')
    stem, ext = os.path.splitext(path)
    paths = [path] + [f'{stem}_{i}{ext}' for i in range(1, parts)]
    lengths = [offset] + [os.path.getsize(p) for p in paths[1:]]
    out = []
    for i in range(count):
        row = table[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE]
        end = row.find(b'\0', 0, 256)
        if end <= 0: raise ValueError(f'{path}: LSPK member {i} has an invalid name')
        name = row[:end].decode('utf-8')
        at, n, raw, part, comp, _ = struct.unpack_from('<6I', row, 256)
        if part >= parts: raise ValueError(f'{path}/{name}: invalid LSPK part {part}')
        _range(at, n, lengths[part], f'{path}/{name}')
        out.append((name, paths[part], at, n, raw if comp & 15 else n, comp & 15))
    return paths, out


def read(entry):
    name, path, offset, size, raw, method = entry
    if size > MAX_MEMBER: raise ValueError(f'{name}: stored size exceeds {MAX_MEMBER} bytes')
    with open(path, 'rb') as f: f.seek(offset); data = f.read(size)
    if len(data) != size: raise ValueError(f'{name}: truncated LSPK member')
    return _decode(data, raw, method, name)


def bshd_sections(data, label):
    """BSHD v1 → [(engine stage, compiled program)].

    A big-endian length prefixes each record; its top bit selects zlib. The
    decoded record has a version, count, then byte/u32/u32 stage/range entries.
    NX records hold GLSLC; other platforms' records are left to their readers.
    """
    if len(data) < 4: raise ValueError(f'{label}: truncated BSHD length')
    word = int.from_bytes(data[:4], 'big')
    data = _decode(data[4:], word & 0x7fffffff, 1 if word >> 31 else 0, label)
    if len(data) < 6 or data[:5] != b'\xff\xff\xff\xff\x01': raise ValueError(f'{label}: unsupported BSHD header')
    count = data[5]; start = 6 + 9 * count
    if not count or start > len(data): raise ValueError(f'{label}: truncated BSHD stage table')
    out = []
    for i in range(count):
        stage, offset, size = struct.unpack_from('<BII', data, 6 + i * 9)
        if offset < start or not size: raise ValueError(f'{label}: invalid BSHD stage {i} range')
        _range(offset, size, len(data), f'{label}: BSHD stage {i}')
        out.append((stage, data[offset:offset + size]))
    return out
