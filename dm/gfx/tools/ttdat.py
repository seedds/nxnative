"""TT Games' CC4 v2 DAT index and OODL members (Skywalker Saga).

The DAT's little-endian pointer names a big-endian .CC40TAD index. Name
records form a folder tree, while FNV-1a hashes map full names to the sorted
file records. Only requested members are read. Layout reference:
https://github.com/linterniGamer/Tt-Games-quickbms-scripts/blob/main/files/ttgames_TSS.bms
"""
import struct
from romfs import open

MAGIC = b'.CC40TAD'
MAX_TABLE = 64 << 20
MAX_MEMBER = 64 << 20


def _range(offset, size, end, what):
    if offset < 0 or size < 0 or offset > end or size > end - offset:
        raise ValueError(f'{what}: range {offset:#x}+{size:#x} exceeds {end:#x} bytes')


def index_header(f):
    """(index offset, length, header), or None for a different DAT format."""
    f.seek(0, 2); size = f.tell(); f.seek(0); h = f.read(8)
    if len(h) < 8: return None
    offset, length = struct.unpack('<II', h)
    if offset & 0x80000000: offset = ((offset ^ 0xffffffff) + 1) << 8
    if offset < 8 or offset > size - 12: return None
    f.seek(offset); header = f.read(32)
    if header[4:12] != MAGIC: return None
    if len(header) < 32: raise ValueError('CC4 index: truncated header')
    _range(offset, length, size, 'CC4 index')
    if not 32 <= length <= MAX_TABLE: raise ValueError(f'CC4 index: invalid length {length}')
    return offset, length, header


def needs_oodle(path):
    """Probe shader members for OODL framing. The splitter reports damaged archives."""
    try:
        shaders = entries(path)
        if shaders:
            with open(path, 'rb') as f:
                for _, offset, stored, _ in shaders:
                    if stored >= 4:
                        f.seek(offset)
                        if f.read(4) == b'OODL': return True
    except (OSError, ValueError):
        pass
    return False


def name_hash(name):
    value = 0xcbf29ce484222325
    for byte in name.replace('/', '\\').lstrip('\\').upper().encode('utf-8'):
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def entries(path):
    """Shader members as (name, offset, stored length, decoded length).
    Unrecognized archives return None. Recognized invalid indices fail before
    any member is decoded. No archive path is materialized on the filesystem.
    """
    with open(path, 'rb') as f:
        header = index_header(f)
        if header is None: return None
        offset, length, h = header
        hlen, _, kind, version, count, names, strings = struct.unpack('>I8si4I', h)
        if version != 2 or kind != -13: raise ValueError(f'{path}: unsupported CC4 version {version}, layout {kind}')
        if hlen + 4 != length: raise ValueError(f'{path}: CC4 index length disagrees with its header')
        names_at = 32 + strings + 4
        files_at = names_at + 12 * names + 8
        hashes_at = files_at + 16 * count
        _range(32, strings, length, 'CC4 name strings')
        _range(names_at, 12 * names + 8, length, 'CC4 name records')
        _range(files_at, 24 * count, length, 'CC4 files and hashes')
        f.seek(offset); data = f.read(length)
    if len(data) != length: raise ValueError(f'{path}: truncated CC4 index')
    if struct.unpack_from('>iI', data, files_at - 8) != (kind, count): raise ValueError(f'{path}: CC4 file counts disagree')
    hashes = {}
    for i in range(count):
        key = struct.unpack_from('>Q', data, hashes_at + 8 * i)[0]
        if key in hashes: raise ValueError(f'{path}: duplicate CC4 file hash')
        hashes[key] = i
    folders, out = {0: ''}, []
    for i in range(names):
        no, parent, _, _, file_id = struct.unpack_from('>I4H', data, names_at + 12 * i)
        if no == 0xffffffff: continue
        _range(no, 1, strings, 'CC4 name')
        end = data.find(b'\0', 32 + no, 32 + strings)
        if end < 0: raise ValueError(f'{path}: unterminated CC4 name')
        if parent not in folders: raise ValueError(f'{path}: CC4 name {i} has an unknown parent {parent}')
        name = data[32 + no:end].decode('utf-8')
        full = folders[parent] + name
        if len(full) > 4096: raise ValueError(f'{path}: CC4 path exceeds 4096 characters')
        if not file_id and i != names - 1:
            folders[i] = full + '/'; continue
        if not full.lower().endswith('.nnx_shaders'): continue
        key = name_hash(full)
        if key not in hashes: raise ValueError(f'{path}/{full}: no matching CC4 file hash')
        encoded, stored, raw = struct.unpack_from('>QII', data, files_at + 16 * hashes[key])
        at = encoded & 0x00ffffffffffffff   # top byte carries the packing flags, not the file address
        raw &= 0x7fffffff
        _range(at, stored, offset, f'{path}/{full}')
        if not 0 < raw <= MAX_MEMBER: raise ValueError(f'{path}/{full}: invalid shader member length {raw}')
        if not 0 < stored <= MAX_MEMBER * 2: raise ValueError(f'{path}/{full}: invalid stored shader length {stored}')
        out.append((full, at, stored, raw))
    return out


def read(f, entry, decode):
    """Decode a bounded member, using the game's own Oodle for OODL chunks."""
    name, offset, stored, size = entry
    f.seek(offset); magic = f.read(4); f.seek(offset)
    if magic != b'OODL':
        if stored != size: raise ValueError(f'{name}: unsupported TT Games compression {magic!r}')
        data = f.read(size)
        if len(data) != size: raise ValueError(f'{name}: truncated stored member')
        return data
    remaining, out = stored, bytearray()
    while remaining:
        if remaining < 12: raise ValueError(f'{name}: truncated OODL header')
        header = f.read(12)
        if len(header) != 12: raise ValueError(f'{name}: truncated OODL header')
        magic, packed, raw = struct.unpack('<4sII', header)
        if magic != b'OODL' or not packed or not raw or packed > min(remaining - 12, MAX_MEMBER) or raw > size - len(out):
            raise ValueError(f'{name}: invalid OODL chunk lengths')
        data = f.read(packed)
        if len(data) != packed: raise ValueError(f'{name}: truncated OODL chunk')
        chunk = data if packed == raw else decode(data, raw)
        if len(chunk) != raw: raise ValueError(f'{name}: Oodle returned the wrong length')
        out += chunk; remaining -= 12 + packed
    if len(out) != size: raise ValueError(f'{name}: OODL decoded length disagrees with the index')
    return bytes(out)
