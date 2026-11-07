"""EA BIG4 / EASF / chunkzip / NXSHADER input for shader_split.

Adapted from Touchline23 (ohxiaoguang), under its MIT source license.
Only format parsing lives here; keys, package export and pairing use the host
pipeline. The EASF digest covers the first 32 plaintext bytes, not the archive.
"""
import hashlib
import struct

from . import chunkzip, fxo, keyfind


def entries(stream, length):
    """Read a BIG4 index without reading its potentially multi-gigabyte payload."""
    head = stream.read(16)
    if len(head) != 16 or head[:4] != b'BIG4':
        raise ValueError('invalid BIG4 header')
    count, end = struct.unpack_from('>II', head, 8)
    if not 16 <= end <= length or count > (end - 16) // 9:
        raise ValueError('BIG4 index exceeds archive')
    table = stream.read(end - 16)
    if len(table) != end - 16:
        raise ValueError('truncated BIG4 index')
    at, result = 0, []
    for _ in range(count):
        if at + 9 > len(table): raise ValueError('truncated BIG4 entry')
        offset, size = struct.unpack_from('>II', table, at)
        z = table.find(b'\0', at + 8)
        if z < 0: raise ValueError('unterminated BIG4 name')
        if offset < end or offset > length or size > length - offset:
            raise ValueError('BIG4 member exceeds archive')
        result.append((offset, size, table[at + 8:z].decode('latin1')))
        at = z + 1
    return result


def decode(data, ring):
    if data.startswith(b'EASF'):
        from Crypto.Cipher import AES
        header = keyfind.parse_header(data)
        key, _ = ring.resolve(data)
        if key is None: raise ValueError('no verified EASF key in the supplied module or optional key file')
        plain = AES.new(key, AES.MODE_CBC, iv=key).decrypt(data[48:])
        size = header['size']
        if not 32 <= size <= len(plain) or hashlib.sha256(plain[:32]).digest() != header['digest']:
            raise ValueError('EASF plaintext prefix verification failed')
        data = plain[:size]
    if data.startswith(b'chunkzip'):
        _, data, _ = chunkzip.decode(data)
    return data


def archive(data, label, ring, depth=0):
    """Shader records in a decoded archive; never return or log key material."""
    import io
    if depth > 8: raise ValueError('EA archive nesting exceeds eight levels')
    data = decode(data, ring)
    if data.startswith(b'NXSHADER'):
        return [(f'{label}#NXSHADER@{off:x}', st, ctl, code)
                for st, _, ctl, code, off in fxo.programs(data)]
    if not data.startswith(b'BIG4'): return []   # a valid compressed non-shader resource (e.g. player heads)
    found = []
    for off, size, name in entries(io.BytesIO(data), len(data)):
        member = data[off:off + size]
        if member.startswith((b'NXSHADER', b'EASF', b'chunkzip', b'BIG4')):
            found += archive(member, f'{label}#{name}', ring, depth + 1)
    return found


def scan(path, module):
    """Recognize EA archives by magic, using the same directory/image IO as other readers.

    For outer BIG4s inspect member magic without loading unrelated payloads,
    which can hold gigabytes of game data. No resource basename is required.
    Returns None for another format, a list (possibly empty) for a BIG4.
    """
    from romfs import open, os
    with open(path, 'rb') as stream:
        head = stream.read(8)
        if head not in (b'NXSHADER', b'chunkzip') and head[:4] not in (b'EASF', b'BIG4'):
            return None
        ring = keyfind.Keyring(modules=(module,), key_file=os.environ.get('DM_EASF_KEY_FILE'),
                              bounded=os.environ.get('DM_EASF_KEY_SCAN') == '1')
        if head[:4] != b'BIG4':
            stream.seek(0); found = archive(stream.read(), str(path), ring)
        else:
            stream.seek(0); table = entries(stream, os.path.getsize(path)); found = []
            for off, size, name in table:
                stream.seek(off); magic = stream.read(min(8, size))
                if magic not in (b'NXSHADER', b'chunkzip') and magic[:4] not in (b'EASF', b'BIG4'): continue
                stream.seek(off)
                found += archive(stream.read(size), f'{os.path.basename(path)}#{name}', ring)
        for note in dict.fromkeys(ring.notes): print(f'EA archive: {note}', flush=True)
        return found
