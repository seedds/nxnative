"""Read Bethesda BSA shader members without loading whole texture archives.

Skyrim's Switch shaders are the .fxp member of an uncompressed version 105 BSA.
Versions 103/104 use 16-byte folder records and zlib; 105 uses 24 bytes and LZ4
frames. Bit 30 of a member size toggles the archive's compression default.
"""
import os
import struct
import zlib


def shader_files(path, *, opener=open):
    """Yield (member path, bytes) for .fxp members; other payloads stay on disk."""
    with opener(path, 'rb') as f:
        # A RomFS-image member's descriptor belongs to the whole image.
        # Bound offsets against this stream, not the backing descriptor.
        f.seek(0, os.SEEK_END)
        length = f.tell()
        f.seek(0)

        def read(n):
            if n < 0 or n > length - f.tell():
                raise ValueError(f'{path}: truncated BSA table or member')
            data = f.read(n)
            if len(data) != n:
                raise ValueError(f'{path}: short BSA read')
            return data

        magic, version, folders_at, flags, folders, files, _, names_size, _ = struct.unpack('<4s8I', read(36))
        if magic != b'BSA\0' or version not in (103, 104, 105) or flags & 3 != 3:
            raise ValueError(f'{path}: unsupported BSA header (version {version}, flags {flags:#x})')
        folder_size = 24 if version == 105 else 16
        if folders_at < 36 or folders_at + folders * folder_size > length:
            raise ValueError(f'{path}: folder table outside BSA')
        f.seek(folders_at)
        counts = []
        for _ in range(folders):
            record = read(folder_size)
            counts.append(struct.unpack_from('<I', record, 8)[0])
        if sum(counts) != files:
            raise ValueError(f'{path}: BSA folder/file counts disagree')
        entries = []
        for count in counts:
            name_length = read(1)[0]
            folder = read(name_length).rstrip(b'\0').decode('utf-8', errors='replace')
            for _ in range(count):
                _, size, offset = struct.unpack('<QII', read(16))
                entries.append((folder, size, offset))
        names = read(names_size).split(b'\0')
        if len(names) != files + 1 or names[-1]:
            raise ValueError(f'{path}: invalid BSA filename table')
        for (folder, stored, offset), name in zip(entries, names):
            name = name.decode('utf-8', errors='replace')
            if not name.lower().endswith('.fxp'):
                continue
            size = stored & 0x3fffffff
            if offset > length or size > length - offset:
                raise ValueError(f'{path}: member {name} outside BSA')
            f.seek(offset)
            data = read(size)
            if flags & 0x100 and version >= 104:
                if not data or data[0] + 1 > len(data):
                    raise ValueError(f'{path}: truncated embedded member name')
                data = data[data[0] + 1:]
            if bool(flags & 4) != bool(stored & 0x40000000):
                if len(data) < 4:
                    raise ValueError(f'{path}: missing uncompressed length')
                wanted = struct.unpack_from('<I', data)[0]
                if version == 105:
                    import lz4.frame
                    data = lz4.frame.decompress(data[4:])
                else:
                    data = zlib.decompress(data[4:])
                if len(data) != wanted:
                    raise ValueError(f'{path}: decompressed member size mismatch')
            yield (folder.replace('\\', '/') + '/' + name).lstrip('/'), data
